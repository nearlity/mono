/*
 * sre.c: Routines for creating an image at runtime
 *   and related System.Reflection.Emit icalls
 *   
 * 
 * Author:
 *   Paolo Molaro (lupus@ximian.com)
 *
 * Copyright 2001-2003 Ximian, Inc (http://www.ximian.com)
 * Copyright 2004-2009 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Rodrigo Kumpera
 * Copyright 2016 Microsoft
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include <config.h>
#include <glib.h>
#include "mono/metadata/assembly.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/dynamic-image-internals.h"
#include "mono/metadata/dynamic-stream-internals.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/gc-internals.h"
#include "mono/metadata/mono-ptr-array.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/profiler-private.h"
#include "mono/metadata/reflection-internals.h"
#include "mono/metadata/reflection-cache.h"
#include "mono/metadata/sre-internals.h"
#include "mono/metadata/custom-attrs-internals.h"
#include "mono/metadata/security-manager.h"
#include "mono/metadata/security-core-clr.h"
#include "mono/metadata/tabledefs.h"
#include "mono/metadata/tokentype.h"
#include "mono/utils/checked-build.h"
#include "mono/utils/mono-digest.h"

void
mono_sre_generic_param_table_entry_free (GenericParamTableEntry *entry)
{
	MONO_GC_UNREGISTER_ROOT_IF_MOVING (entry->gparam);
	g_free (entry);
}

static GENERATE_GET_CLASS_WITH_CACHE (marshal_as_attribute, System.Runtime.InteropServices, MarshalAsAttribute);

#ifndef DISABLE_REFLECTION_EMIT
static guint32 mono_image_get_methodref_token (MonoDynamicImage *assembly, MonoMethod *method, gboolean create_typespec);
static guint32 mono_image_get_methodbuilder_token (MonoDynamicImage *assembly, MonoReflectionMethodBuilder *mb, gboolean create_open_instance, MonoError *error);
static guint32 mono_image_get_ctorbuilder_token (MonoDynamicImage *assembly, MonoReflectionCtorBuilder *cb, MonoError *error);
static guint32 mono_image_get_sighelper_token (MonoDynamicImage *assembly, MonoReflectionSigHelper *helper, MonoError *error);
static gboolean ensure_runtime_vtable (MonoClass *klass, MonoError  *error);
static guint32 mono_image_get_methodref_token_for_methodbuilder (MonoDynamicImage *assembly, MonoReflectionMethodBuilder *method, MonoError *error);
static void reflection_methodbuilder_from_dynamic_method (ReflectionMethodBuilder *rmb, MonoReflectionDynamicMethod *mb);

static gpointer register_assembly (MonoDomain *domain, MonoReflectionAssembly *res, MonoAssembly *assembly);
#endif

static char*   type_get_qualified_name (MonoType *type, MonoAssembly *ass);
static MonoReflectionType *mono_reflection_type_get_underlying_system_type (MonoReflectionType* t, MonoError *error);
static gboolean is_sre_array (MonoClass *klass);
static gboolean is_sre_byref (MonoClass *klass);
static gboolean is_sre_pointer (MonoClass *klass);
static gboolean is_sre_type_builder (MonoClass *klass);
static gboolean is_sre_method_builder (MonoClass *klass);
static gboolean is_sre_field_builder (MonoClass *klass);
static gboolean is_sr_mono_method (MonoClass *klass);
static gboolean is_sr_mono_generic_method (MonoClass *klass);
static gboolean is_sr_mono_generic_cmethod (MonoClass *klass);
static gboolean is_sr_mono_field (MonoClass *klass);

static guint32 mono_image_get_methodspec_token (MonoDynamicImage *assembly, MonoMethod *method);
static guint32 mono_image_get_inflated_method_token (MonoDynamicImage *assembly, MonoMethod *m);
static MonoMethod * inflate_method (MonoReflectionType *type, MonoObject *obj, MonoError *error);

#define mono_type_array_get_and_resolve(array, index, error) mono_reflection_type_get_handle ((MonoReflectionType*)mono_array_get (array, gpointer, index), error)

static void mono_image_module_basic_init (MonoReflectionModuleBuilder *module);

void
mono_reflection_emit_init (void)
{
	mono_dynamic_images_init ();
}

static char*
type_get_fully_qualified_name (MonoType *type)
{
	MONO_REQ_GC_NEUTRAL_MODE;

	return mono_type_get_name_full (type, MONO_TYPE_NAME_FORMAT_ASSEMBLY_QUALIFIED);
}

static char*
type_get_qualified_name (MonoType *type, MonoAssembly *ass)
{
	MONO_REQ_GC_UNSAFE_MODE;

	MonoClass *klass;
	MonoAssembly *ta;

	klass = mono_class_from_mono_type (type);
	if (!klass) 
		return mono_type_get_name_full (type, MONO_TYPE_NAME_FORMAT_REFLECTION);
	ta = klass->image->assembly;
	if (assembly_is_dynamic (ta) || (ta == ass)) {
		if (klass->generic_class || klass->generic_container)
			/* For generic type definitions, we want T, while REFLECTION returns T<K> */
			return mono_type_get_name_full (type, MONO_TYPE_NAME_FORMAT_FULL_NAME);
		else
			return mono_type_get_name_full (type, MONO_TYPE_NAME_FORMAT_REFLECTION);
	}

	return mono_type_get_name_full (type, MONO_TYPE_NAME_FORMAT_ASSEMBLY_QUALIFIED);
}

#ifndef DISABLE_REFLECTION_EMIT
/**
 * mp_g_alloc:
 *
 * Allocate memory from the @image mempool if it is non-NULL. Otherwise, allocate memory
 * from the C heap.
 */
static gpointer
image_g_malloc (MonoImage *image, guint size)
{
	MONO_REQ_GC_NEUTRAL_MODE;

	if (image)
		return mono_image_alloc (image, size);
	else
		return g_malloc (size);
}
#endif /* !DISABLE_REFLECTION_EMIT */

/**
 * image_g_alloc0:
 *
 * Allocate memory from the @image mempool if it is non-NULL. Otherwise, allocate memory
 * from the C heap.
 */
gpointer
mono_image_g_malloc0 (MonoImage *image, guint size)
{
	MONO_REQ_GC_NEUTRAL_MODE;

	if (image)
		return mono_image_alloc0 (image, size);
	else
		return g_malloc0 (size);
}

/**
 * image_g_free:
 * @image: a MonoImage
 * @ptr: pointer
 *
 * If @image is NULL, free @ptr, otherwise do nothing.
 */
static void
image_g_free (MonoImage *image, gpointer ptr)
{
	if (image == NULL)
		g_free (ptr);
}

#ifndef DISABLE_REFLECTION_EMIT
static char*
image_strdup (MonoImage *image, const char *s)
{
	MONO_REQ_GC_NEUTRAL_MODE;

	if (image)
		return mono_image_strdup (image, s);
	else
		return g_strdup (s);
}
#endif

#define image_g_new(image,struct_type, n_structs)		\
    ((struct_type *) image_g_malloc (image, ((gsize) sizeof (struct_type)) * ((gsize) (n_structs))))

#define image_g_new0(image,struct_type, n_structs)		\
    ((struct_type *) mono_image_g_malloc0 (image, ((gsize) sizeof (struct_type)) * ((gsize) (n_structs))))


static void
alloc_table (MonoDynamicTable *table, guint nrows)
{
	mono_dynimage_alloc_table (table, nrows);
}

static guint32
string_heap_insert (MonoDynamicStream *sh, const char *str)
{
	return mono_dynstream_insert_string (sh, str);
}

static guint32
mono_image_add_stream_data (MonoDynamicStream *stream, const char *data, guint32 len)
{
	return mono_dynstream_add_data (stream, data, len);
}

/*
 * Despite the name, we handle also TypeSpec (with the above helper).
 */
static guint32
mono_image_typedef_or_ref (MonoDynamicImage *assembly, MonoType *type)
{
	return mono_dynimage_encode_typedef_or_ref_full (assembly, type, TRUE);
}

/*
 * Copy len * nelem bytes from val to dest, swapping bytes to LE if necessary.
 * dest may be misaligned.
 */
static void
swap_with_size (char *dest, const char* val, int len, int nelem) {
	MONO_REQ_GC_NEUTRAL_MODE;
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
	int elem;

	for (elem = 0; elem < nelem; ++elem) {
		switch (len) {
		case 1:
			*dest = *val;
			break;
		case 2:
			dest [0] = val [1];
			dest [1] = val [0];
			break;
		case 4:
			dest [0] = val [3];
			dest [1] = val [2];
			dest [2] = val [1];
			dest [3] = val [0];
			break;
		case 8:
			dest [0] = val [7];
			dest [1] = val [6];
			dest [2] = val [5];
			dest [3] = val [4];
			dest [4] = val [3];
			dest [5] = val [2];
			dest [6] = val [1];
			dest [7] = val [0];
			break;
		default:
			g_assert_not_reached ();
		}
		dest += len;
		val += len;
	}
#else
	memcpy (dest, val, len * nelem);
#endif
}

guint32
mono_reflection_method_count_clauses (MonoReflectionILGen *ilgen)
{
	MONO_REQ_GC_UNSAFE_MODE;

	guint32 num_clauses = 0;
	int i;

	MonoILExceptionInfo *ex_info;
	for (i = 0; i < mono_array_length (ilgen->ex_handlers); ++i) {
		ex_info = (MonoILExceptionInfo*)mono_array_addr (ilgen->ex_handlers, MonoILExceptionInfo, i);
		if (ex_info->handlers)
			num_clauses += mono_array_length (ex_info->handlers);
		else
			num_clauses++;
	}

	return num_clauses;
}

#ifndef DISABLE_REFLECTION_EMIT
static MonoExceptionClause*
method_encode_clauses (MonoImage *image, MonoDynamicImage *assembly, MonoReflectionILGen *ilgen, guint32 num_clauses, MonoError *error)
{
	MONO_REQ_GC_UNSAFE_MODE;

	mono_error_init (error);

	MonoExceptionClause *clauses;
	MonoExceptionClause *clause;
	MonoILExceptionInfo *ex_info;
	MonoILExceptionBlock *ex_block;
	guint32 finally_start;
	int i, j, clause_index;;

	clauses = image_g_new0 (image, MonoExceptionClause, num_clauses);

	clause_index = 0;
	for (i = mono_array_length (ilgen->ex_handlers) - 1; i >= 0; --i) {
		ex_info = (MonoILExceptionInfo*)mono_array_addr (ilgen->ex_handlers, MonoILExceptionInfo, i);
		finally_start = ex_info->start + ex_info->len;
		if (!ex_info->handlers)
			continue;
		for (j = 0; j < mono_array_length (ex_info->handlers); ++j) {
			ex_block = (MonoILExceptionBlock*)mono_array_addr (ex_info->handlers, MonoILExceptionBlock, j);
			clause = &(clauses [clause_index]);

			clause->flags = ex_block->type;
			clause->try_offset = ex_info->start;

			if (ex_block->type == MONO_EXCEPTION_CLAUSE_FINALLY)
				clause->try_len = finally_start - ex_info->start;
			else
				clause->try_len = ex_info->len;
			clause->handler_offset = ex_block->start;
			clause->handler_len = ex_block->len;
			if (ex_block->extype) {
				MonoType *extype = mono_reflection_type_get_handle ((MonoReflectionType*)ex_block->extype, error);

				if (!is_ok (error)) {
					image_g_free (image, clauses);
					return NULL;
				}
				clause->data.catch_class = mono_class_from_mono_type (extype);
			} else {
				if (ex_block->type == MONO_EXCEPTION_CLAUSE_FILTER)
					clause->data.filter_offset = ex_block->filter_offset;
				else
					clause->data.filter_offset = 0;
			}
			finally_start = ex_block->start + ex_block->len;

			clause_index ++;
		}
	}

	return clauses;
}
#endif /* !DISABLE_REFLECTION_EMIT */

#ifndef DISABLE_REFLECTION_EMIT
/*
 * LOCKING: Acquires the loader lock. 
 */
static void
mono_save_custom_attrs (MonoImage *image, void *obj, MonoArray *cattrs)
{
	MONO_REQ_GC_UNSAFE_MODE;

	MonoCustomAttrInfo *ainfo, *tmp;

	if (!cattrs || !mono_array_length (cattrs))
		return;

	ainfo = mono_custom_attrs_from_builders (image, image, cattrs);

	mono_loader_lock ();
	tmp = (MonoCustomAttrInfo *)mono_image_property_lookup (image, obj, MONO_PROP_DYNAMIC_CATTR);
	if (tmp)
		mono_custom_attrs_free (tmp);
	mono_image_property_insert (image, obj, MONO_PROP_DYNAMIC_CATTR, ainfo);
	mono_loader_unlock ();

}
#endif


guint32
mono_reflection_resolution_scope_from_image (MonoDynamicImage *assembly, MonoImage *image)
{
	MONO_REQ_GC_UNSAFE_MODE;

	MonoDynamicTable *table;
	guint32 token;
	guint32 *values;
	guint32 cols [MONO_ASSEMBLY_SIZE];
	const char *pubkey;
	guint32 publen;

	if ((token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, image))))
		return token;

	if (assembly_is_dynamic (image->assembly) && (image->assembly == assembly->image.assembly)) {
		table = &assembly->tables [MONO_TABLE_MODULEREF];
		token = table->next_idx ++;
		table->rows ++;
		alloc_table (table, table->rows);
		values = table->values + token * MONO_MODULEREF_SIZE;
		values [MONO_MODULEREF_NAME] = string_heap_insert (&assembly->sheap, image->module_name);

		token <<= MONO_RESOLUTION_SCOPE_BITS;
		token |= MONO_RESOLUTION_SCOPE_MODULEREF;
		g_hash_table_insert (assembly->handleref, image, GUINT_TO_POINTER (token));

		return token;
	}
	
	if (assembly_is_dynamic (image->assembly))
		/* FIXME: */
		memset (cols, 0, sizeof (cols));
	else {
		/* image->assembly->image is the manifest module */
		image = image->assembly->image;
		mono_metadata_decode_row (&image->tables [MONO_TABLE_ASSEMBLY], 0, cols, MONO_ASSEMBLY_SIZE);
	}

	table = &assembly->tables [MONO_TABLE_ASSEMBLYREF];
	token = table->next_idx ++;
	table->rows ++;
	alloc_table (table, table->rows);
	values = table->values + token * MONO_ASSEMBLYREF_SIZE;
	values [MONO_ASSEMBLYREF_NAME] = string_heap_insert (&assembly->sheap, image->assembly_name);
	values [MONO_ASSEMBLYREF_MAJOR_VERSION] = cols [MONO_ASSEMBLY_MAJOR_VERSION];
	values [MONO_ASSEMBLYREF_MINOR_VERSION] = cols [MONO_ASSEMBLY_MINOR_VERSION];
	values [MONO_ASSEMBLYREF_BUILD_NUMBER] = cols [MONO_ASSEMBLY_BUILD_NUMBER];
	values [MONO_ASSEMBLYREF_REV_NUMBER] = cols [MONO_ASSEMBLY_REV_NUMBER];
	values [MONO_ASSEMBLYREF_FLAGS] = 0;
	values [MONO_ASSEMBLYREF_CULTURE] = 0;
	values [MONO_ASSEMBLYREF_HASH_VALUE] = 0;

	if (strcmp ("", image->assembly->aname.culture)) {
		values [MONO_ASSEMBLYREF_CULTURE] = string_heap_insert (&assembly->sheap,
				image->assembly->aname.culture);
	}

	if ((pubkey = mono_image_get_public_key (image, &publen))) {
		guchar pubtoken [9];
		pubtoken [0] = 8;
		mono_digest_get_public_token (pubtoken + 1, (guchar*)pubkey, publen);
		values [MONO_ASSEMBLYREF_PUBLIC_KEY] = mono_image_add_stream_data (&assembly->blob, (char*)pubtoken, 9);
	} else {
		values [MONO_ASSEMBLYREF_PUBLIC_KEY] = 0;
	}
	token <<= MONO_RESOLUTION_SCOPE_BITS;
	token |= MONO_RESOLUTION_SCOPE_ASSEMBLYREF;
	g_hash_table_insert (assembly->handleref, image, GUINT_TO_POINTER (token));
	return token;
}

#ifndef DISABLE_REFLECTION_EMIT
gboolean
mono_reflection_methodbuilder_from_method_builder (ReflectionMethodBuilder *rmb, MonoReflectionMethodBuilder *mb, MonoError *error)
{
	MONO_REQ_GC_UNSAFE_MODE;

	mono_error_init (error);
	memset (rmb, 0, sizeof (ReflectionMethodBuilder));

	rmb->ilgen = mb->ilgen;
	rmb->rtype = (MonoReflectionType*)mb->rtype;
	return_val_if_nok (error, FALSE);
	rmb->parameters = mb->parameters;
	rmb->generic_params = mb->generic_params;
	rmb->generic_container = mb->generic_container;
	rmb->opt_types = NULL;
	rmb->pinfo = mb->pinfo;
	rmb->attrs = mb->attrs;
	rmb->iattrs = mb->iattrs;
	rmb->call_conv = mb->call_conv;
	rmb->code = mb->code;
	rmb->type = mb->type;
	rmb->name = mb->name;
	rmb->table_idx = &mb->table_idx;
	rmb->init_locals = mb->init_locals;
	rmb->skip_visibility = FALSE;
	rmb->return_modreq = mb->return_modreq;
	rmb->return_modopt = mb->return_modopt;
	rmb->param_modreq = mb->param_modreq;
	rmb->param_modopt = mb->param_modopt;
	rmb->permissions = mb->permissions;
	rmb->mhandle = mb->mhandle;
	rmb->nrefs = 0;
	rmb->refs = NULL;

	if (mb->dll) {
		rmb->charset = mb->charset;
		rmb->extra_flags = mb->extra_flags;
		rmb->native_cc = mb->native_cc;
		rmb->dllentry = mb->dllentry;
		rmb->dll = mb->dll;
	}

	return TRUE;
}

gboolean
mono_reflection_methodbuilder_from_ctor_builder (ReflectionMethodBuilder *rmb, MonoReflectionCtorBuilder *mb, MonoError *error)
{
	MONO_REQ_GC_UNSAFE_MODE;

	const char *name = mb->attrs & METHOD_ATTRIBUTE_STATIC ? ".cctor": ".ctor";

	mono_error_init (error);

	memset (rmb, 0, sizeof (ReflectionMethodBuilder));

	rmb->ilgen = mb->ilgen;
	rmb->rtype = mono_type_get_object_checked (mono_domain_get (), &mono_defaults.void_class->byval_arg, error);
	return_val_if_nok (error, FALSE);
	rmb->parameters = mb->parameters;
	rmb->generic_params = NULL;
	rmb->generic_container = NULL;
	rmb->opt_types = NULL;
	rmb->pinfo = mb->pinfo;
	rmb->attrs = mb->attrs;
	rmb->iattrs = mb->iattrs;
	rmb->call_conv = mb->call_conv;
	rmb->code = NULL;
	rmb->type = mb->type;
	rmb->name = mono_string_new (mono_domain_get (), name);
	rmb->table_idx = &mb->table_idx;
	rmb->init_locals = mb->init_locals;
	rmb->skip_visibility = FALSE;
	rmb->return_modreq = NULL;
	rmb->return_modopt = NULL;
	rmb->param_modreq = mb->param_modreq;
	rmb->param_modopt = mb->param_modopt;
	rmb->permissions = mb->permissions;
	rmb->mhandle = mb->mhandle;
	rmb->nrefs = 0;
	rmb->refs = NULL;

	return TRUE;
}

static void
reflection_methodbuilder_from_dynamic_method (ReflectionMethodBuilder *rmb, MonoReflectionDynamicMethod *mb)
{
	MONO_REQ_GC_UNSAFE_MODE;

	memset (rmb, 0, sizeof (ReflectionMethodBuilder));

	rmb->ilgen = mb->ilgen;
	rmb->rtype = mb->rtype;
	rmb->parameters = mb->parameters;
	rmb->generic_params = NULL;
	rmb->generic_container = NULL;
	rmb->opt_types = NULL;
	rmb->pinfo = NULL;
	rmb->attrs = mb->attrs;
	rmb->iattrs = 0;
	rmb->call_conv = mb->call_conv;
	rmb->code = NULL;
	rmb->type = (MonoObject *) mb->owner;
	rmb->name = mb->name;
	rmb->table_idx = NULL;
	rmb->init_locals = mb->init_locals;
	rmb->skip_visibility = mb->skip_visibility;
	rmb->return_modreq = NULL;
	rmb->return_modopt = NULL;
	rmb->param_modreq = NULL;
	rmb->param_modopt = NULL;
	rmb->permissions = NULL;
	rmb->mhandle = mb->mhandle;
	rmb->nrefs = 0;
	rmb->refs = NULL;
}	
#else /* DISABLE_REFLECTION_EMIT */
gboolean
mono_reflection_methodbuilder_from_method_builder (ReflectionMethodBuilder *rmb, MonoReflectionMethodBuilder *mb, MonoError *error) {
	g_assert_not_reached ();
	return FALSE;
}
gboolean
mono_reflection_methodbuilder_from_ctor_builder (ReflectionMethodBuilder *rmb, MonoReflectionCtorBuilder *mb, MonoError *error)
{
	g_assert_not_reached ();
	return FALSE;
}
#endif /* DISABLE_REFLECTION_EMIT */

#ifndef DISABLE_REFLECTION_EMIT
static guint32
mono_image_add_memberef_row (MonoDynamicImage *assembly, guint32 parent, const char *name, guint32 sig)
{
	MONO_REQ_GC_NEUTRAL_MODE;

	MonoDynamicTable *table;
	guint32 *values;
	guint32 token, pclass;

	switch (parent & MONO_TYPEDEFORREF_MASK) {
	case MONO_TYPEDEFORREF_TYPEREF:
		pclass = MONO_MEMBERREF_PARENT_TYPEREF;
		break;
	case MONO_TYPEDEFORREF_TYPESPEC:
		pclass = MONO_MEMBERREF_PARENT_TYPESPEC;
		break;
	case MONO_TYPEDEFORREF_TYPEDEF:
		pclass = MONO_MEMBERREF_PARENT_TYPEDEF;
		break;
	default:
		g_warning ("unknown typeref or def token 0x%08x for %s", parent, name);
		return 0;
	}
	/* extract the index */
	parent >>= MONO_TYPEDEFORREF_BITS;

	table = &assembly->tables [MONO_TABLE_MEMBERREF];

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_MEMBERREF_SIZE;
		values [MONO_MEMBERREF_CLASS] = pclass | (parent << MONO_MEMBERREF_PARENT_BITS);
		values [MONO_MEMBERREF_NAME] = string_heap_insert (&assembly->sheap, name);
		values [MONO_MEMBERREF_SIGNATURE] = sig;
	}

	token = MONO_TOKEN_MEMBER_REF | table->next_idx;
	table->next_idx ++;

	return token;
}

/*
 * Insert a memberef row into the metadata: the token that point to the memberref
 * is returned. Caching is done in the caller (mono_image_get_methodref_token() or
 * mono_image_get_fieldref_token()).
 * The sig param is an index to an already built signature.
 */
static guint32
mono_image_get_memberref_token (MonoDynamicImage *assembly, MonoType *type, const char *name, guint32 sig)
{
	MONO_REQ_GC_NEUTRAL_MODE;

	guint32 parent = mono_image_typedef_or_ref (assembly, type);
	return mono_image_add_memberef_row (assembly, parent, name, sig);
}


static guint32
mono_image_get_methodref_token (MonoDynamicImage *assembly, MonoMethod *method, gboolean create_typespec)
{
	MONO_REQ_GC_NEUTRAL_MODE;

	guint32 token;
	MonoMethodSignature *sig;
	
	create_typespec = create_typespec && method->is_generic && method->klass->image != &assembly->image;

	if (create_typespec) {
		token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, GUINT_TO_POINTER (GPOINTER_TO_UINT (method) + 1)));
		if (token)
			return token;
	} 

	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, method));
	if (token && !create_typespec)
		return token;

	g_assert (!method->is_inflated);
	if (!token) {
		/*
		 * A methodref signature can't contain an unmanaged calling convention.
		 */
		sig = mono_metadata_signature_dup (mono_method_signature (method));
		if ((sig->call_convention != MONO_CALL_DEFAULT) && (sig->call_convention != MONO_CALL_VARARG))
			sig->call_convention = MONO_CALL_DEFAULT;
		token = mono_image_get_memberref_token (assembly, &method->klass->byval_arg,
			method->name,  mono_dynimage_encode_method_signature (assembly, sig));
		g_free (sig);
		g_hash_table_insert (assembly->handleref, method, GUINT_TO_POINTER(token));
	}

	if (create_typespec) {
		MonoDynamicTable *table = &assembly->tables [MONO_TABLE_METHODSPEC];
		g_assert (mono_metadata_token_table (token) == MONO_TABLE_MEMBERREF);
		token = (mono_metadata_token_index (token) << MONO_METHODDEFORREF_BITS) | MONO_METHODDEFORREF_METHODREF;

		if (assembly->save) {
			guint32 *values;

			alloc_table (table, table->rows + 1);
			values = table->values + table->next_idx * MONO_METHODSPEC_SIZE;
			values [MONO_METHODSPEC_METHOD] = token;
			values [MONO_METHODSPEC_SIGNATURE] = mono_dynimage_encode_generic_method_sig (assembly, &mono_method_get_generic_container (method)->context);
		}

		token = MONO_TOKEN_METHOD_SPEC | table->next_idx;
		table->next_idx ++;
		/*methodspec and memberef tokens are diferent, */
		g_hash_table_insert (assembly->handleref, GUINT_TO_POINTER (GPOINTER_TO_UINT (method) + 1), GUINT_TO_POINTER (token));
		return token;
	}
	return token;
}

static guint32
mono_image_get_methodref_token_for_methodbuilder (MonoDynamicImage *assembly, MonoReflectionMethodBuilder *method, MonoError *error)
{
	guint32 token, parent, sig;
	ReflectionMethodBuilder rmb;
	MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)method->type;
	
	mono_error_init (error);
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, method));
	if (token)
		return token;

	if (!mono_reflection_methodbuilder_from_method_builder (&rmb, method, error))
		return 0;

	/*
	 * A methodref signature can't contain an unmanaged calling convention.
	 * Since some flags are encoded as part of call_conv, we need to check against it.
	*/
	if ((rmb.call_conv & ~0x60) != MONO_CALL_DEFAULT && (rmb.call_conv & ~0x60) != MONO_CALL_VARARG)
		rmb.call_conv = (rmb.call_conv & 0x60) | MONO_CALL_DEFAULT;

	sig = mono_dynimage_encode_method_builder_signature (assembly, &rmb, error);
	return_val_if_nok (error, 0);

	if (tb->generic_params) {
		parent = mono_dynimage_encode_generic_typespec (assembly, tb, error);
		return_val_if_nok (error, 0);
	} else {
		MonoType *t = mono_reflection_type_get_handle ((MonoReflectionType*)rmb.type, error);
		return_val_if_nok (error, 0);

		parent = mono_image_typedef_or_ref (assembly, t);
	}

	char *name = mono_string_to_utf8_checked (method->name, error);
	return_val_if_nok (error, 0);

	token = mono_image_add_memberef_row (assembly, parent, name, sig);
	g_free (name);

	g_hash_table_insert (assembly->handleref, method, GUINT_TO_POINTER(token));

	return token;
}

static guint32
mono_image_get_varargs_method_token (MonoDynamicImage *assembly, guint32 original,
				     const gchar *name, guint32 sig)
{
	MonoDynamicTable *table;
	guint32 token;
	guint32 *values;
	
	table = &assembly->tables [MONO_TABLE_MEMBERREF];

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_MEMBERREF_SIZE;
		values [MONO_MEMBERREF_CLASS] = original;
		values [MONO_MEMBERREF_NAME] = string_heap_insert (&assembly->sheap, name);
		values [MONO_MEMBERREF_SIGNATURE] = sig;
	}

	token = MONO_TOKEN_MEMBER_REF | table->next_idx;
	table->next_idx ++;

	return token;
}

static guint32
mono_image_get_methodspec_token_for_generic_method_definition (MonoDynamicImage *assembly, MonoReflectionMethodBuilder *mb, MonoError *error)
{
	MonoDynamicTable *table;
	guint32 *values;
	guint32 token, mtoken = 0;

	mono_error_init (error);
	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->methodspec, mb));
	if (token)
		return token;

	table = &assembly->tables [MONO_TABLE_METHODSPEC];

	mtoken = mono_image_get_methodref_token_for_methodbuilder (assembly, mb, error);
	if (!mono_error_ok (error))
		return 0;

	switch (mono_metadata_token_table (mtoken)) {
	case MONO_TABLE_MEMBERREF:
		mtoken = (mono_metadata_token_index (mtoken) << MONO_METHODDEFORREF_BITS) | MONO_METHODDEFORREF_METHODREF;
		break;
	case MONO_TABLE_METHOD:
		mtoken = (mono_metadata_token_index (mtoken) << MONO_METHODDEFORREF_BITS) | MONO_METHODDEFORREF_METHODDEF;
		break;
	default:
		g_assert_not_reached ();
	}

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_METHODSPEC_SIZE;
		values [MONO_METHODSPEC_METHOD] = mtoken;
		values [MONO_METHODSPEC_SIGNATURE] = mono_dynimage_encode_generic_method_definition_sig (assembly, mb);
	}

	token = MONO_TOKEN_METHOD_SPEC | table->next_idx;
	table->next_idx ++;

	mono_g_hash_table_insert (assembly->methodspec, mb, GUINT_TO_POINTER(token));
	return token;
}

static guint32
mono_image_get_methodbuilder_token (MonoDynamicImage *assembly, MonoReflectionMethodBuilder *mb, gboolean create_methodspec, MonoError *error)
{
	guint32 token;

	mono_error_init (error);

	if (mb->generic_params && create_methodspec) 
		return mono_image_get_methodspec_token_for_generic_method_definition (assembly, mb, error);

	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->handleref_managed, mb));
	if (token)
		return token;

	token = mono_image_get_methodref_token_for_methodbuilder (assembly, mb, error);
	if (!mono_error_ok (error))
		return 0;
	mono_g_hash_table_insert (assembly->handleref_managed, mb, GUINT_TO_POINTER(token));
	return token;
}

static guint32
mono_image_get_ctorbuilder_token (MonoDynamicImage *assembly, MonoReflectionCtorBuilder *mb, MonoError *error)
{
	guint32 token, parent, sig;
	ReflectionMethodBuilder rmb;
	char *name;
	MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)mb->type;
	
	mono_error_init (error);
	
	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->handleref_managed, mb));
	if (token)
		return token;

	if (!mono_reflection_methodbuilder_from_ctor_builder (&rmb, mb, error))
		return 0;

	if (tb->generic_params) {
		parent = mono_dynimage_encode_generic_typespec (assembly, tb, error);
		return_val_if_nok (error, 0);
	} else {
		MonoType * type = mono_reflection_type_get_handle ((MonoReflectionType*)tb, error);
		return_val_if_nok (error, 0);
		parent = mono_image_typedef_or_ref (assembly, type);
	}
	
	name = mono_string_to_utf8_checked (rmb.name, error);
	return_val_if_nok (error, 0);
	sig = mono_dynimage_encode_method_builder_signature (assembly, &rmb, error);
	return_val_if_nok (error, 0);

	token = mono_image_add_memberef_row (assembly, parent, name, sig);

	g_free (name);
	mono_g_hash_table_insert (assembly->handleref_managed, mb, GUINT_TO_POINTER(token));
	return token;
}
#endif

static gboolean
is_field_on_inst (MonoClassField *field)
{
	return field->parent->generic_class && field->parent->generic_class->is_dynamic;
}

#ifndef DISABLE_REFLECTION_EMIT
static guint32
mono_image_get_fieldref_token (MonoDynamicImage *assembly, MonoObject *f, MonoClassField *field)
{
	MonoType *type;
	guint32 token;

	g_assert (field);
	g_assert (field->parent);

	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->handleref_managed, f));
	if (token)
		return token;

	if (field->parent->generic_class && field->parent->generic_class->container_class && field->parent->generic_class->container_class->fields) {
		int index = field - field->parent->fields;
		type = mono_field_get_type (&field->parent->generic_class->container_class->fields [index]);
	} else {
		type = mono_field_get_type (field);
	}
	token = mono_image_get_memberref_token (assembly, &field->parent->byval_arg,
											mono_field_get_name (field),
											mono_dynimage_encode_fieldref_signature (assembly, field->parent->image, type));
	mono_g_hash_table_insert (assembly->handleref_managed, f, GUINT_TO_POINTER(token));
	return token;
}

static guint32
mono_image_get_field_on_inst_token (MonoDynamicImage *assembly, MonoReflectionFieldOnTypeBuilderInst *f, MonoError *error)
{
	guint32 token;
	MonoClass *klass;
	MonoGenericClass *gclass;
	MonoType *type;
	char *name;

	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->handleref_managed, f));
	if (token)
		return token;
	if (is_sre_field_builder (mono_object_class (f->fb))) {
		MonoReflectionFieldBuilder *fb = (MonoReflectionFieldBuilder *)f->fb;
		type = mono_reflection_type_get_handle ((MonoReflectionType*)f->inst, error);
		return_val_if_nok (error, 0);
		klass = mono_class_from_mono_type (type);
		gclass = type->data.generic_class;
		g_assert (gclass->is_dynamic);

		guint32 sig_token = mono_dynimage_encode_field_signature (assembly, fb, error);
		return_val_if_nok (error, 0);
		name = mono_string_to_utf8_checked (fb->name, error);
		return_val_if_nok (error, 0);
		token = mono_image_get_memberref_token (assembly, &klass->byval_arg, name, sig_token);
		g_free (name);		
	} else if (is_sr_mono_field (mono_object_class (f->fb))) {
		guint32 sig;
		MonoClassField *field = ((MonoReflectionField *)f->fb)->field;

		type = mono_reflection_type_get_handle ((MonoReflectionType*)f->inst, error);
		return_val_if_nok (error, 0);
		klass = mono_class_from_mono_type (type);

		sig = mono_dynimage_encode_fieldref_signature (assembly, field->parent->image, field->type);
		token = mono_image_get_memberref_token (assembly, &klass->byval_arg, field->name, sig);
	} else {
		char *name = mono_type_get_full_name (mono_object_class (f->fb));
		g_error ("mono_image_get_field_on_inst_token: don't know how to handle %s", name);
	}

	mono_g_hash_table_insert (assembly->handleref_managed, f, GUINT_TO_POINTER (token));
	return token;
}

static guint32
mono_image_get_ctor_on_inst_token (MonoDynamicImage *assembly, MonoReflectionCtorOnTypeBuilderInst *c, gboolean create_methodspec, MonoError *error)
{
	guint32 sig, token;
	MonoClass *klass;
	MonoGenericClass *gclass;
	MonoType *type;

	mono_error_init (error);

	/* A ctor cannot be a generic method, so we can ignore create_methodspec */

	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->handleref_managed, c));
	if (token)
		return token;

	if (mono_is_sre_ctor_builder (mono_object_class (c->cb))) {
		MonoReflectionCtorBuilder *cb = (MonoReflectionCtorBuilder *)c->cb;
		ReflectionMethodBuilder rmb;
		char *name;

		type = mono_reflection_type_get_handle ((MonoReflectionType*)c->inst, error);
		return_val_if_nok (error, 0);
		klass = mono_class_from_mono_type (type);

		gclass = type->data.generic_class;
		g_assert (gclass->is_dynamic);

		if (!mono_reflection_methodbuilder_from_ctor_builder (&rmb, cb, error))
			return 0;

		sig = mono_dynimage_encode_method_builder_signature (assembly, &rmb, error);
		return_val_if_nok (error, 0);

		name = mono_string_to_utf8_checked (rmb.name, error);
		return_val_if_nok (error, 0);

		token = mono_image_get_memberref_token (assembly, &klass->byval_arg, name, sig);
		g_free (name);
	} else if (mono_is_sr_mono_cmethod (mono_object_class (c->cb))) {
		MonoMethod *mm = ((MonoReflectionMethod *)c->cb)->method;

		type = mono_reflection_type_get_handle ((MonoReflectionType*)c->inst, error);
		return_val_if_nok (error, 0);
		klass = mono_class_from_mono_type (type);

		sig = mono_dynimage_encode_method_signature (assembly, mono_method_signature (mm));
		token = mono_image_get_memberref_token (assembly, &klass->byval_arg, mm->name, sig);
	} else {
		char *name = mono_type_get_full_name (mono_object_class (c->cb));
		g_error ("mono_image_get_method_on_inst_token: don't know how to handle %s", name);
	}


	mono_g_hash_table_insert (assembly->handleref_managed, c, GUINT_TO_POINTER (token));
	return token;
}

MonoMethod*
mono_reflection_method_on_tb_inst_get_handle (MonoReflectionMethodOnTypeBuilderInst *m, MonoError *error)
{
	MonoClass *klass;
	MonoGenericContext tmp_context;
	MonoType **type_argv;
	MonoGenericInst *ginst;
	MonoMethod *method, *inflated;
	int count, i;

	mono_error_init (error);

	mono_reflection_init_type_builder_generics ((MonoObject*)m->inst, error);
	return_val_if_nok (error, NULL);

	method = inflate_method (m->inst, (MonoObject*)m->mb, error);
	return_val_if_nok (error, NULL);

	klass = method->klass;

	if (m->method_args == NULL)
		return method;

	if (method->is_inflated)
		method = ((MonoMethodInflated *) method)->declaring;

	count = mono_array_length (m->method_args);

	type_argv = g_new0 (MonoType *, count);
	for (i = 0; i < count; i++) {
		MonoReflectionType *garg = (MonoReflectionType *)mono_array_get (m->method_args, gpointer, i);
		type_argv [i] = mono_reflection_type_get_handle (garg, error);
		return_val_if_nok (error, NULL);
	}
	ginst = mono_metadata_get_generic_inst (count, type_argv);
	g_free (type_argv);

	tmp_context.class_inst = klass->generic_class ? klass->generic_class->context.class_inst : NULL;
	tmp_context.method_inst = ginst;

	inflated = mono_class_inflate_generic_method_checked (method, &tmp_context, error);
	mono_error_assert_ok (error);
	return inflated;
}

static guint32
mono_image_get_method_on_inst_token (MonoDynamicImage *assembly, MonoReflectionMethodOnTypeBuilderInst *m, gboolean create_methodspec, MonoError *error)
{
	guint32 sig, token = 0;
	MonoType *type;
	MonoClass *klass;

	mono_error_init (error);

	if (m->method_args) {
		MonoMethod *inflated;

		inflated = mono_reflection_method_on_tb_inst_get_handle (m, error);
		return_val_if_nok (error, 0);

		if (create_methodspec)
			token = mono_image_get_methodspec_token (assembly, inflated);
		else
			token = mono_image_get_inflated_method_token (assembly, inflated);
		return token;
	}

	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->handleref_managed, m));
	if (token)
		return token;

	if (is_sre_method_builder (mono_object_class (m->mb))) {
		MonoReflectionMethodBuilder *mb = (MonoReflectionMethodBuilder *)m->mb;
		MonoGenericClass *gclass;
		ReflectionMethodBuilder rmb;
		char *name;

		type = mono_reflection_type_get_handle ((MonoReflectionType*)m->inst, error);
		return_val_if_nok (error, 0);
		klass = mono_class_from_mono_type (type);
		gclass = type->data.generic_class;
		g_assert (gclass->is_dynamic);

		if (!mono_reflection_methodbuilder_from_method_builder (&rmb, mb, error))
			return 0;

		sig = mono_dynimage_encode_method_builder_signature (assembly, &rmb, error);
		return_val_if_nok (error, 0);

		name = mono_string_to_utf8_checked (rmb.name, error);
		return_val_if_nok (error, 0);

		token = mono_image_get_memberref_token (assembly, &klass->byval_arg, name, sig);
		g_free (name);		
	} else if (is_sr_mono_method (mono_object_class (m->mb))) {
		MonoMethod *mm = ((MonoReflectionMethod *)m->mb)->method;

		type = mono_reflection_type_get_handle ((MonoReflectionType*)m->inst, error);
		return_val_if_nok (error, 0);
		klass = mono_class_from_mono_type (type);

		sig = mono_dynimage_encode_method_signature (assembly, mono_method_signature (mm));
		token = mono_image_get_memberref_token (assembly, &klass->byval_arg, mm->name, sig);
	} else {
		char *name = mono_type_get_full_name (mono_object_class (m->mb));
		g_error ("mono_image_get_method_on_inst_token: don't know how to handle %s", name);
	}

	mono_g_hash_table_insert (assembly->handleref_managed, m, GUINT_TO_POINTER (token));
	return token;
}

static guint32
method_encode_methodspec (MonoDynamicImage *assembly, MonoMethod *method)
{
	MonoDynamicTable *table;
	guint32 *values;
	guint32 token, mtoken = 0, sig;
	MonoMethodInflated *imethod;
	MonoMethod *declaring;

	table = &assembly->tables [MONO_TABLE_METHODSPEC];

	g_assert (method->is_inflated);
	imethod = (MonoMethodInflated *) method;
	declaring = imethod->declaring;

	sig = mono_dynimage_encode_method_signature (assembly, mono_method_signature (declaring));
	mtoken = mono_image_get_memberref_token (assembly, &method->klass->byval_arg, declaring->name, sig);

	if (!mono_method_signature (declaring)->generic_param_count)
		return mtoken;

	switch (mono_metadata_token_table (mtoken)) {
	case MONO_TABLE_MEMBERREF:
		mtoken = (mono_metadata_token_index (mtoken) << MONO_METHODDEFORREF_BITS) | MONO_METHODDEFORREF_METHODREF;
		break;
	case MONO_TABLE_METHOD:
		mtoken = (mono_metadata_token_index (mtoken) << MONO_METHODDEFORREF_BITS) | MONO_METHODDEFORREF_METHODDEF;
		break;
	default:
		g_assert_not_reached ();
	}

	sig = mono_dynimage_encode_generic_method_sig (assembly, mono_method_get_context (method));

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_METHODSPEC_SIZE;
		values [MONO_METHODSPEC_METHOD] = mtoken;
		values [MONO_METHODSPEC_SIGNATURE] = sig;
	}

	token = MONO_TOKEN_METHOD_SPEC | table->next_idx;
	table->next_idx ++;

	return token;
}

static guint32
mono_image_get_methodspec_token (MonoDynamicImage *assembly, MonoMethod *method)
{
	MonoMethodInflated *imethod;
	guint32 token;
	
	token = GPOINTER_TO_UINT (g_hash_table_lookup (assembly->handleref, method));
	if (token)
		return token;

	g_assert (method->is_inflated);
	imethod = (MonoMethodInflated *) method;

	if (mono_method_signature (imethod->declaring)->generic_param_count) {
		token = method_encode_methodspec (assembly, method);
	} else {
		guint32 sig = mono_dynimage_encode_method_signature (
			assembly, mono_method_signature (imethod->declaring));
		token = mono_image_get_memberref_token (
			assembly, &method->klass->byval_arg, method->name, sig);
	}

	g_hash_table_insert (assembly->handleref, method, GUINT_TO_POINTER(token));
	return token;
}

static guint32
mono_image_get_inflated_method_token (MonoDynamicImage *assembly, MonoMethod *m)
{
	MonoMethodInflated *imethod = (MonoMethodInflated *) m;
	guint32 sig, token;

	sig = mono_dynimage_encode_method_signature (assembly, mono_method_signature (imethod->declaring));
	token = mono_image_get_memberref_token (
		assembly, &m->klass->byval_arg, m->name, sig);

	return token;
}

/*
 * Return a copy of TYPE, adding the custom modifiers in MODREQ and MODOPT.
 */
static MonoType*
add_custom_modifiers (MonoDynamicImage *assembly, MonoType *type, MonoArray *modreq, MonoArray *modopt, MonoError *error)
{
	int i, count, len, pos;
	MonoType *t;

	mono_error_init (error);

	count = 0;
	if (modreq)
		count += mono_array_length (modreq);
	if (modopt)
		count += mono_array_length (modopt);

	if (count == 0)
		return mono_metadata_type_dup (NULL, type);

	len = MONO_SIZEOF_TYPE + ((gint32)count) * sizeof (MonoCustomMod);
	t = (MonoType *)g_malloc (len);
	memcpy (t, type, MONO_SIZEOF_TYPE);

	t->num_mods = count;
	pos = 0;
	if (modreq) {
		for (i = 0; i < mono_array_length (modreq); ++i) {
			MonoType *mod = mono_type_array_get_and_resolve (modreq, i, error);
			if (!is_ok (error))
				goto fail;
			t->modifiers [pos].required = 1;
			t->modifiers [pos].token = mono_image_typedef_or_ref (assembly, mod);
			pos ++;
		}
	}
	if (modopt) {
		for (i = 0; i < mono_array_length (modopt); ++i) {
			MonoType *mod = mono_type_array_get_and_resolve (modopt, i, error);
			if (!is_ok (error))
				goto fail;
			t->modifiers [pos].required = 0;
			t->modifiers [pos].token = mono_image_typedef_or_ref (assembly, mod);
			pos ++;
		}
	}

	return t;
fail:
	g_free (t);
	return NULL;
}

void
mono_reflection_init_type_builder_generics (MonoObject *type, MonoError *error)
{
	MonoReflectionTypeBuilder *tb;

	mono_error_init (error);

	if (!is_sre_type_builder(mono_object_class (type)))
		return;
	tb = (MonoReflectionTypeBuilder *)type;

	if (tb && tb->generic_container)
		mono_reflection_create_generic_class (tb, error);
}

static guint32
mono_image_get_generic_field_token (MonoDynamicImage *assembly, MonoReflectionFieldBuilder *fb, MonoError *error)
{
	MonoDynamicTable *table;
	MonoType *custom = NULL, *type;
	guint32 *values;
	guint32 token, pclass, parent, sig;
	gchar *name;

	mono_error_init (error);

	token = GPOINTER_TO_UINT (mono_g_hash_table_lookup (assembly->handleref_managed, fb));
	if (token)
		return token;

	MonoType *typeb = mono_reflection_type_get_handle (fb->typeb, error);
	return_val_if_nok (error, 0);
	/* FIXME: is this call necessary? */
	mono_class_from_mono_type (typeb);

	/*FIXME this is one more layer of ugliness due how types are created.*/
	mono_reflection_init_type_builder_generics (fb->type, error);
	return_val_if_nok (error, 0);

	/* fb->type does not include the custom modifiers */
	/* FIXME: We should do this in one place when a fieldbuilder is created */
	type = mono_reflection_type_get_handle ((MonoReflectionType*)fb->type, error);
	return_val_if_nok (error, 0);

	if (fb->modreq || fb->modopt) {
		type = custom = add_custom_modifiers (assembly, type, fb->modreq, fb->modopt, error);
		return_val_if_nok (error, 0);
	}

	sig = mono_dynimage_encode_fieldref_signature (assembly, NULL, type);
	g_free (custom);

	parent = mono_dynimage_encode_generic_typespec (assembly, (MonoReflectionTypeBuilder *) fb->typeb, error);
	return_val_if_nok (error, 0);
	g_assert ((parent & MONO_TYPEDEFORREF_MASK) == MONO_TYPEDEFORREF_TYPESPEC);
	
	pclass = MONO_MEMBERREF_PARENT_TYPESPEC;
	parent >>= MONO_TYPEDEFORREF_BITS;

	table = &assembly->tables [MONO_TABLE_MEMBERREF];

	name = mono_string_to_utf8_checked (fb->name, error);
	return_val_if_nok (error, 0);

	if (assembly->save) {
		alloc_table (table, table->rows + 1);
		values = table->values + table->next_idx * MONO_MEMBERREF_SIZE;
		values [MONO_MEMBERREF_CLASS] = pclass | (parent << MONO_MEMBERREF_PARENT_BITS);
		values [MONO_MEMBERREF_NAME] = string_heap_insert (&assembly->sheap, name);
		values [MONO_MEMBERREF_SIGNATURE] = sig;
	}

	token = MONO_TOKEN_MEMBER_REF | table->next_idx;
	table->next_idx ++;
	mono_g_hash_table_insert (assembly->handleref_managed, fb, GUINT_TO_POINTER(token));
	g_free (name);
	return token;
}

static guint32 
mono_image_get_sighelper_token (MonoDynamicImage *assembly, MonoReflectionSigHelper *helper, MonoError *error)
{
	guint32 idx;
	MonoDynamicTable *table;
	guint32 *values;

	mono_error_init (error);

	table = &assembly->tables [MONO_TABLE_STANDALONESIG];
	idx = table->next_idx ++;
	table->rows ++;
	alloc_table (table, table->rows);
	values = table->values + idx * MONO_STAND_ALONE_SIGNATURE_SIZE;

	values [MONO_STAND_ALONE_SIGNATURE] =
		mono_dynimage_encode_reflection_sighelper (assembly, helper, error);
	return_val_if_nok (error, 0);
	
	return idx;
}

static int
reflection_cc_to_file (int call_conv) {
	switch (call_conv & 0x3) {
	case 0:
	case 1: return MONO_CALL_DEFAULT;
	case 2: return MONO_CALL_VARARG;
	default:
		g_assert_not_reached ();
	}
	return 0;
}
#endif /* !DISABLE_REFLECTION_EMIT */

struct _ArrayMethod {
	MonoType *parent;
	MonoMethodSignature *sig;
	char *name;
	guint32 token;
};

void
mono_sre_array_method_free (ArrayMethod *am)
{
	g_free (am->sig);
	g_free (am->name);
	g_free (am);
}

#ifndef DISABLE_REFLECTION_EMIT
static guint32
mono_image_get_array_token (MonoDynamicImage *assembly, MonoReflectionArrayMethod *m, MonoError *error)
{
	guint32 nparams, i;
	GList *tmp;
	char *name = NULL;
	MonoMethodSignature *sig;
	ArrayMethod *am = NULL;
	MonoType *mtype;

	mono_error_init (error);

	nparams = mono_array_length (m->parameters);
	sig = (MonoMethodSignature *)g_malloc0 (MONO_SIZEOF_METHOD_SIGNATURE + sizeof (MonoType*) * nparams);
	sig->hasthis = 1;
	sig->sentinelpos = -1;
	sig->call_convention = reflection_cc_to_file (m->call_conv);
	sig->param_count = nparams;
	if (m->ret) {
		sig->ret = mono_reflection_type_get_handle (m->ret, error);
		if (!is_ok (error))
			goto fail;
	} else
		sig->ret = &mono_defaults.void_class->byval_arg;

	mtype = mono_reflection_type_get_handle (m->parent, error);
	if (!is_ok (error))
		goto fail;

	for (i = 0; i < nparams; ++i) {
		sig->params [i] = mono_type_array_get_and_resolve (m->parameters, i, error);
		if (!is_ok (error))
			goto fail;
	}

	name = mono_string_to_utf8_checked (m->name, error);
	if (!is_ok (error))
		goto fail;
	for (tmp = assembly->array_methods; tmp; tmp = tmp->next) {
		am = (ArrayMethod *)tmp->data;
		if (strcmp (name, am->name) == 0 && 
				mono_metadata_type_equal (am->parent, mtype) &&
				mono_metadata_signature_equal (am->sig, sig)) {
			g_free (name);
			g_free (sig);
			m->table_idx = am->token & 0xffffff;
			return am->token;
		}
	}
	am = g_new0 (ArrayMethod, 1);
	am->name = name;
	am->sig = sig;
	am->parent = mtype;
	am->token = mono_image_get_memberref_token (assembly, am->parent, name,
		mono_dynimage_encode_method_signature (assembly, sig));
	assembly->array_methods = g_list_prepend (assembly->array_methods, am);
	m->table_idx = am->token & 0xffffff;
	return am->token;
fail:
	g_free (am);
	g_free (name);
	g_free (sig);
	return 0;

}
#endif

#ifndef DISABLE_REFLECTION_EMIT

/*
 * mono_image_insert_string:
 * @module: module builder object
 * @str: a string
 *
 * Insert @str into the user string stream of @module.
 */
guint32
mono_image_insert_string (MonoReflectionModuleBuilder *module, MonoString *str)
{
	MonoDynamicImage *assembly;
	guint32 idx;
	char buf [16];
	char *b = buf;
	
	if (!module->dynamic_image)
		mono_image_module_basic_init (module);

	assembly = module->dynamic_image;
	
	if (assembly->save) {
		mono_metadata_encode_value (1 | (str->length * 2), b, &b);
		idx = mono_image_add_stream_data (&assembly->us, buf, b-buf);
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
	{
		char *swapped = g_malloc (2 * mono_string_length (str));
		const char *p = (const char*)mono_string_chars (str);

		swap_with_size (swapped, p, 2, mono_string_length (str));
		mono_image_add_stream_data (&assembly->us, swapped, str->length * 2);
		g_free (swapped);
	}
#else
		mono_image_add_stream_data (&assembly->us, (const char*)mono_string_chars (str), str->length * 2);
#endif
		mono_image_add_stream_data (&assembly->us, "", 1);
	} else {
		idx = assembly->us.index ++;
	}

	mono_dynamic_image_register_token (assembly, MONO_TOKEN_STRING | idx, (MonoObject*)str);

	return MONO_TOKEN_STRING | idx;
}

guint32
mono_image_create_method_token (MonoDynamicImage *assembly, MonoObject *obj, MonoArray *opt_param_types, MonoError *error)
{
	MonoClass *klass;
	guint32 token = 0;
	MonoMethodSignature *sig;

	mono_error_init (error);

	klass = obj->vtable->klass;
	if (strcmp (klass->name, "MonoMethod") == 0 || strcmp (klass->name, "MonoCMethod") == 0) {
		MonoMethod *method = ((MonoReflectionMethod *)obj)->method;
		MonoMethodSignature *old;
		guint32 sig_token, parent;
		int nargs, i;

		g_assert (opt_param_types && (mono_method_signature (method)->sentinelpos >= 0));

		nargs = mono_array_length (opt_param_types);
		old = mono_method_signature (method);
		sig = mono_metadata_signature_alloc ( &assembly->image, old->param_count + nargs);

		sig->hasthis = old->hasthis;
		sig->explicit_this = old->explicit_this;
		sig->call_convention = old->call_convention;
		sig->generic_param_count = old->generic_param_count;
		sig->param_count = old->param_count + nargs;
		sig->sentinelpos = old->param_count;
		sig->ret = old->ret;

		for (i = 0; i < old->param_count; i++)
			sig->params [i] = old->params [i];

		for (i = 0; i < nargs; i++) {
			MonoReflectionType *rt = mono_array_get (opt_param_types, MonoReflectionType *, i);
			sig->params [old->param_count + i] = mono_reflection_type_get_handle (rt, error);
			if (!is_ok (error)) goto fail;
		}

		parent = mono_image_typedef_or_ref (assembly, &method->klass->byval_arg);
		g_assert ((parent & MONO_TYPEDEFORREF_MASK) == MONO_MEMBERREF_PARENT_TYPEREF);
		parent >>= MONO_TYPEDEFORREF_BITS;

		parent <<= MONO_MEMBERREF_PARENT_BITS;
		parent |= MONO_MEMBERREF_PARENT_TYPEREF;

		sig_token = mono_dynimage_encode_method_signature (assembly, sig);
		token = mono_image_get_varargs_method_token (assembly, parent, method->name, sig_token);
	} else if (strcmp (klass->name, "MethodBuilder") == 0) {
		MonoReflectionMethodBuilder *mb = (MonoReflectionMethodBuilder *)obj;
		ReflectionMethodBuilder rmb;
		guint32 parent, sig_token;
		int nopt_args, nparams, ngparams, i;

		if (!mono_reflection_methodbuilder_from_method_builder (&rmb, mb, error))
			goto fail;
		
		rmb.opt_types = opt_param_types;
		nopt_args = mono_array_length (opt_param_types);

		nparams = rmb.parameters ? mono_array_length (rmb.parameters): 0;
		ngparams = rmb.generic_params ? mono_array_length (rmb.generic_params): 0;
		sig = mono_metadata_signature_alloc (&assembly->image, nparams + nopt_args);

		sig->hasthis = !(rmb.attrs & METHOD_ATTRIBUTE_STATIC);
		sig->explicit_this = (rmb.call_conv & 0x40) == 0x40;
		sig->call_convention = rmb.call_conv;
		sig->generic_param_count = ngparams;
		sig->param_count = nparams + nopt_args;
		sig->sentinelpos = nparams;
		sig->ret = mono_reflection_type_get_handle (rmb.rtype, error);
		if (!is_ok (error)) goto fail;

		for (i = 0; i < nparams; i++) {
			MonoReflectionType *rt = mono_array_get (rmb.parameters, MonoReflectionType *, i);
			sig->params [i] = mono_reflection_type_get_handle (rt, error);
			if (!is_ok (error)) goto fail;
		}

		for (i = 0; i < nopt_args; i++) {
			MonoReflectionType *rt = mono_array_get (opt_param_types, MonoReflectionType *, i);
			sig->params [nparams + i] = mono_reflection_type_get_handle (rt, error);
			if (!is_ok (error)) goto fail;
		}

		sig_token = mono_dynimage_encode_method_builder_signature (assembly, &rmb, error);
		if (!is_ok (error))
			goto fail;

		parent = mono_image_create_token (assembly, obj, TRUE, TRUE, error);
		if (!mono_error_ok (error))
			goto fail;
		g_assert (mono_metadata_token_table (parent) == MONO_TABLE_METHOD);

		parent = mono_metadata_token_index (parent) << MONO_MEMBERREF_PARENT_BITS;
		parent |= MONO_MEMBERREF_PARENT_METHODDEF;

		char *name = mono_string_to_utf8_checked (rmb.name, error);
		if (!is_ok (error)) goto fail;
		token = mono_image_get_varargs_method_token (
			assembly, parent, name, sig_token);
		g_free (name);
	} else {
		g_error ("requested method token for %s\n", klass->name);
	}

	g_hash_table_insert (assembly->vararg_aux_hash, GUINT_TO_POINTER (token), sig);
	mono_dynamic_image_register_token (assembly, token, obj);
	return token;
fail:
	g_assert (!mono_error_ok (error));
	return 0;
}

/*
 * mono_image_create_token:
 * @assembly: a dynamic assembly
 * @obj:
 * @register_token: Whenever to register the token in the assembly->tokens hash. 
 *
 * Get a token to insert in the IL code stream for the given MemberInfo.
 * The metadata emission routines need to pass FALSE as REGISTER_TOKEN, since by that time, 
 * the table_idx-es were recomputed, so registering the token would overwrite an existing 
 * entry.
 */
guint32
mono_image_create_token (MonoDynamicImage *assembly, MonoObject *obj, 
			 gboolean create_open_instance, gboolean register_token,
			 MonoError *error)
{
	MonoClass *klass;
	guint32 token = 0;

	mono_error_init (error);

	klass = obj->vtable->klass;

	/* Check for user defined reflection objects */
	/* TypeDelegator is the only corlib type which doesn't look like a MonoReflectionType */
	if (klass->image != mono_defaults.corlib || (strcmp (klass->name, "TypeDelegator") == 0)) {
		mono_error_set_not_supported (error, "User defined subclasses of System.Type are not yet supported");
		return 0;
	}

	if (strcmp (klass->name, "MethodBuilder") == 0) {
		MonoReflectionMethodBuilder *mb = (MonoReflectionMethodBuilder *)obj;
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)mb->type;

		if (tb->module->dynamic_image == assembly && !tb->generic_params && !mb->generic_params)
			token = mb->table_idx | MONO_TOKEN_METHOD_DEF;
		else {
			token = mono_image_get_methodbuilder_token (assembly, mb, create_open_instance, error);
			if (!mono_error_ok (error))
				return 0;
		}
		/*g_print ("got token 0x%08x for %s\n", token, mono_string_to_utf8 (mb->name));*/
	} else if (strcmp (klass->name, "ConstructorBuilder") == 0) {
		MonoReflectionCtorBuilder *mb = (MonoReflectionCtorBuilder *)obj;
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)mb->type;

		if (tb->module->dynamic_image == assembly && !tb->generic_params)
			token = mb->table_idx | MONO_TOKEN_METHOD_DEF;
		else {
			token = mono_image_get_ctorbuilder_token (assembly, mb, error);
			if (!mono_error_ok (error))
				return 0;
		}
		/*g_print ("got token 0x%08x for %s\n", token, mono_string_to_utf8 (mb->name));*/
	} else if (strcmp (klass->name, "FieldBuilder") == 0) {
		MonoReflectionFieldBuilder *fb = (MonoReflectionFieldBuilder *)obj;
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)fb->typeb;
		if (tb->generic_params) {
			token = mono_image_get_generic_field_token (assembly, fb, error);
			return_val_if_nok (error, 0);
		} else {
			if (tb->module->dynamic_image == assembly) {
				token = fb->table_idx | MONO_TOKEN_FIELD_DEF;
			} else {
				token = mono_image_get_fieldref_token (assembly, (MonoObject*)fb, fb->handle);
			}
		}
	} else if (strcmp (klass->name, "TypeBuilder") == 0) {
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)obj;
		if (create_open_instance && tb->generic_params) {
			MonoType *type;
			mono_reflection_init_type_builder_generics (obj, error);
			return_val_if_nok (error, 0);
			type = mono_reflection_type_get_handle ((MonoReflectionType *)obj, error);
			return_val_if_nok (error, 0);
			token = mono_dynimage_encode_typedef_or_ref_full (assembly, type, TRUE);
			token = mono_metadata_token_from_dor (token);
		} else if (tb->module->dynamic_image == assembly) {
			token = tb->table_idx | MONO_TOKEN_TYPE_DEF;
		} else {
			MonoType *type;
			type = mono_reflection_type_get_handle ((MonoReflectionType *)obj, error);
			return_val_if_nok (error, 0);
			token = mono_metadata_token_from_dor (mono_image_typedef_or_ref (assembly, type));
		}
	} else if (strcmp (klass->name, "RuntimeType") == 0) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType *)obj, error);
		return_val_if_nok (error, 0);
		MonoClass *mc = mono_class_from_mono_type (type);
		token = mono_metadata_token_from_dor (
			mono_dynimage_encode_typedef_or_ref_full (assembly, type, mc->generic_container == NULL || create_open_instance));
	} else if (strcmp (klass->name, "GenericTypeParameterBuilder") == 0) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType *)obj, error);
		return_val_if_nok (error, 0);
		token = mono_metadata_token_from_dor (
			mono_image_typedef_or_ref (assembly, type));
	} else if (strcmp (klass->name, "MonoGenericClass") == 0) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType *)obj, error);
		return_val_if_nok (error, 0);
		token = mono_metadata_token_from_dor (
			mono_image_typedef_or_ref (assembly, type));
	} else if (strcmp (klass->name, "MonoCMethod") == 0 ||
		   strcmp (klass->name, "MonoMethod") == 0 ||
		   strcmp (klass->name, "MonoGenericMethod") == 0 ||
		   strcmp (klass->name, "MonoGenericCMethod") == 0) {
		MonoReflectionMethod *m = (MonoReflectionMethod *)obj;
		if (m->method->is_inflated) {
			if (create_open_instance)
				token = mono_image_get_methodspec_token (assembly, m->method);
			else
				token = mono_image_get_inflated_method_token (assembly, m->method);
		} else if ((m->method->klass->image == &assembly->image) &&
			 !m->method->klass->generic_class) {
			static guint32 method_table_idx = 0xffffff;
			if (m->method->klass->wastypebuilder) {
				/* we use the same token as the one that was assigned
				 * to the Methodbuilder.
				 * FIXME: do the equivalent for Fields.
				 */
				token = m->method->token;
			} else {
				/*
				 * Each token should have a unique index, but the indexes are
				 * assigned by managed code, so we don't know about them. An
				 * easy solution is to count backwards...
				 */
				method_table_idx --;
				token = MONO_TOKEN_METHOD_DEF | method_table_idx;
			}
		} else {
			token = mono_image_get_methodref_token (assembly, m->method, create_open_instance);
		}
		/*g_print ("got token 0x%08x for %s\n", token, m->method->name);*/
	} else if (strcmp (klass->name, "MonoField") == 0) {
		MonoReflectionField *f = (MonoReflectionField *)obj;
		if ((f->field->parent->image == &assembly->image) && !is_field_on_inst (f->field)) {
			static guint32 field_table_idx = 0xffffff;
			field_table_idx --;
			token = MONO_TOKEN_FIELD_DEF | field_table_idx;
		} else {
			token = mono_image_get_fieldref_token (assembly, (MonoObject*)f, f->field);
		}
		/*g_print ("got token 0x%08x for %s\n", token, f->field->name);*/
	} else if (strcmp (klass->name, "MonoArrayMethod") == 0) {
		MonoReflectionArrayMethod *m = (MonoReflectionArrayMethod *)obj;
		token = mono_image_get_array_token (assembly, m, error);
		return_val_if_nok (error, 0);
	} else if (strcmp (klass->name, "SignatureHelper") == 0) {
		MonoReflectionSigHelper *s = (MonoReflectionSigHelper*)obj;
		token = MONO_TOKEN_SIGNATURE | mono_image_get_sighelper_token (assembly, s, error);
		return_val_if_nok (error, 0);
	} else if (strcmp (klass->name, "EnumBuilder") == 0) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType *)obj, error);
		return_val_if_nok (error, 0);
		token = mono_metadata_token_from_dor (
			mono_image_typedef_or_ref (assembly, type));
	} else if (strcmp (klass->name, "FieldOnTypeBuilderInst") == 0) {
		MonoReflectionFieldOnTypeBuilderInst *f = (MonoReflectionFieldOnTypeBuilderInst*)obj;
		token = mono_image_get_field_on_inst_token (assembly, f, error);
		return_val_if_nok (error, 0);
	} else if (strcmp (klass->name, "ConstructorOnTypeBuilderInst") == 0) {
		MonoReflectionCtorOnTypeBuilderInst *c = (MonoReflectionCtorOnTypeBuilderInst*)obj;
		token = mono_image_get_ctor_on_inst_token (assembly, c, create_open_instance, error);
		if (!mono_error_ok (error))
			return 0;
	} else if (strcmp (klass->name, "MethodOnTypeBuilderInst") == 0) {
		MonoReflectionMethodOnTypeBuilderInst *m = (MonoReflectionMethodOnTypeBuilderInst*)obj;
		token = mono_image_get_method_on_inst_token (assembly, m, create_open_instance, error);
		if (!mono_error_ok (error))
			return 0;
	} else if (is_sre_array (klass) || is_sre_byref (klass) || is_sre_pointer (klass)) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType *)obj, error);
		return_val_if_nok (error, 0);
		token = mono_metadata_token_from_dor (
				mono_image_typedef_or_ref (assembly, type));
	} else {
		g_error ("requested token for %s\n", klass->name);
	}

	if (register_token)
		mono_image_register_token (assembly, token, obj);

	return token;
}


#endif

#ifndef DISABLE_REFLECTION_EMIT

/*
 * mono_reflection_dynimage_basic_init:
 * @assembly: an assembly builder object
 *
 * Create the MonoImage that represents the assembly builder and setup some
 * of the helper hash table and the basic metadata streams.
 */
void
mono_reflection_dynimage_basic_init (MonoReflectionAssemblyBuilder *assemblyb)
{
	MonoError error;
	MonoDynamicAssembly *assembly;
	MonoDynamicImage *image;
	MonoDomain *domain = mono_object_domain (assemblyb);
	
	if (assemblyb->dynamic_assembly)
		return;

	assembly = assemblyb->dynamic_assembly = g_new0 (MonoDynamicAssembly, 1);

	mono_profiler_assembly_event (&assembly->assembly, MONO_PROFILE_START_LOAD);
	
	assembly->assembly.ref_count = 1;
	assembly->assembly.dynamic = TRUE;
	assembly->assembly.corlib_internal = assemblyb->corlib_internal;
	assemblyb->assembly.assembly = (MonoAssembly*)assembly;
	assembly->assembly.basedir = mono_string_to_utf8_checked (assemblyb->dir, &error);
	if (mono_error_set_pending_exception (&error))
		return;
	if (assemblyb->culture) {
		assembly->assembly.aname.culture = mono_string_to_utf8_checked (assemblyb->culture, &error);
		if (mono_error_set_pending_exception (&error))
			return;
	} else
		assembly->assembly.aname.culture = g_strdup ("");

        if (assemblyb->version) {
			char *vstr = mono_string_to_utf8_checked (assemblyb->version, &error);
			if (mono_error_set_pending_exception (&error))
				return;
			char **version = g_strsplit (vstr, ".", 4);
			char **parts = version;
			assembly->assembly.aname.major = atoi (*parts++);
			assembly->assembly.aname.minor = atoi (*parts++);
			assembly->assembly.aname.build = *parts != NULL ? atoi (*parts++) : 0;
			assembly->assembly.aname.revision = *parts != NULL ? atoi (*parts) : 0;

			g_strfreev (version);
			g_free (vstr);
        } else {
			assembly->assembly.aname.major = 0;
			assembly->assembly.aname.minor = 0;
			assembly->assembly.aname.build = 0;
			assembly->assembly.aname.revision = 0;
        }

	assembly->run = assemblyb->access != 2;
	assembly->save = assemblyb->access != 1;
	assembly->domain = domain;

	char *assembly_name = mono_string_to_utf8_checked (assemblyb->name, &error);
	if (mono_error_set_pending_exception (&error))
		return;
	image = mono_dynamic_image_create (assembly, assembly_name, g_strdup ("RefEmit_YouForgotToDefineAModule"));
	image->initial_image = TRUE;
	assembly->assembly.aname.name = image->image.name;
	assembly->assembly.image = &image->image;
	if (assemblyb->pktoken && assemblyb->pktoken->max_length) {
		/* -1 to correct for the trailing NULL byte */
		if (assemblyb->pktoken->max_length != MONO_PUBLIC_KEY_TOKEN_LENGTH - 1) {
			g_error ("Public key token length invalid for assembly %s: %i", assembly->assembly.aname.name, assemblyb->pktoken->max_length);
		}
		memcpy (&assembly->assembly.aname.public_key_token, mono_array_addr (assemblyb->pktoken, guint8, 0), assemblyb->pktoken->max_length);		
	}

	mono_domain_assemblies_lock (domain);
	domain->domain_assemblies = g_slist_append (domain->domain_assemblies, assembly);
	mono_domain_assemblies_unlock (domain);

	register_assembly (mono_object_domain (assemblyb), &assemblyb->assembly, &assembly->assembly);
	
	mono_profiler_assembly_loaded (&assembly->assembly, MONO_PROFILE_OK);
	
	mono_assembly_invoke_load_hook ((MonoAssembly*)assembly);
}

#endif /* !DISABLE_REFLECTION_EMIT */

#ifndef DISABLE_REFLECTION_EMIT
static gpointer
register_assembly (MonoDomain *domain, MonoReflectionAssembly *res, MonoAssembly *assembly)
{
	CACHE_OBJECT (MonoReflectionAssembly *, assembly, res, NULL);
}

static gpointer
register_module (MonoDomain *domain, MonoReflectionModuleBuilder *res, MonoDynamicImage *module)
{
	CACHE_OBJECT (MonoReflectionModuleBuilder *, module, res, NULL);
}

static gboolean
image_module_basic_init (MonoReflectionModuleBuilder *moduleb, MonoError *error)
{
	MonoDynamicImage *image = moduleb->dynamic_image;
	MonoReflectionAssemblyBuilder *ab = moduleb->assemblyb;
	mono_error_init (error);
	if (!image) {
		int module_count;
		MonoImage **new_modules;
		MonoImage *ass;
		char *name, *fqname;
		/*
		 * FIXME: we already created an image in mono_reflection_dynimage_basic_init (), but
		 * we don't know which module it belongs to, since that is only 
		 * determined at assembly save time.
		 */
		/*image = (MonoDynamicImage*)ab->dynamic_assembly->assembly.image; */
		name = mono_string_to_utf8_checked (ab->name, error);
		return_val_if_nok (error, FALSE);
		fqname = mono_string_to_utf8_checked (moduleb->module.fqname, error);
		if (!is_ok (error)) {
			g_free (name);
			return FALSE;
		}
		image = mono_dynamic_image_create (ab->dynamic_assembly, name, fqname);

		moduleb->module.image = &image->image;
		moduleb->dynamic_image = image;
		register_module (mono_object_domain (moduleb), moduleb, image);

		/* register the module with the assembly */
		ass = ab->dynamic_assembly->assembly.image;
		module_count = ass->module_count;
		new_modules = g_new0 (MonoImage *, module_count + 1);

		if (ass->modules)
			memcpy (new_modules, ass->modules, module_count * sizeof (MonoImage *));
		new_modules [module_count] = &image->image;
		mono_image_addref (&image->image);

		g_free (ass->modules);
		ass->modules = new_modules;
		ass->module_count ++;
	}
	return TRUE;
}

static void
mono_image_module_basic_init (MonoReflectionModuleBuilder *moduleb)
{
	MonoError error;
	(void) image_module_basic_init (moduleb, &error);
	mono_error_set_pending_exception (&error);
}

#endif

static gboolean
is_corlib_type (MonoClass *klass)
{
	return klass->image == mono_defaults.corlib;
}

#define check_corlib_type_cached(_class, _namespace, _name) do { \
	static MonoClass *cached_class; \
	if (cached_class) \
		return cached_class == _class; \
	if (is_corlib_type (_class) && !strcmp (_name, _class->name) && !strcmp (_namespace, _class->name_space)) { \
		cached_class = _class; \
		return TRUE; \
	} \
	return FALSE; \
} while (0) \



#ifndef DISABLE_REFLECTION_EMIT
static gboolean
is_sre_array (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "ArrayType");
}

static gboolean
is_sre_byref (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "ByRefType");
}

static gboolean
is_sre_pointer (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "PointerType");
}

static gboolean
is_sre_generic_instance (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection", "MonoGenericClass");
}

static gboolean
is_sre_type_builder (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "TypeBuilder");
}

static gboolean
is_sre_method_builder (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "MethodBuilder");
}

gboolean
mono_is_sre_ctor_builder (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "ConstructorBuilder");
}

static gboolean
is_sre_field_builder (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "FieldBuilder");
}

gboolean
mono_is_sre_method_on_tb_inst (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "MethodOnTypeBuilderInst");
}

gboolean
mono_is_sre_ctor_on_tb_inst (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection.Emit", "ConstructorOnTypeBuilderInst");
}

static MonoReflectionType*
mono_reflection_type_get_underlying_system_type (MonoReflectionType* t, MonoError *error)
{
	static MonoMethod *method_get_underlying_system_type = NULL;
	MonoReflectionType *rt;
	MonoMethod *usertype_method;

	mono_error_init (error);

	if (!method_get_underlying_system_type)
		method_get_underlying_system_type = mono_class_get_method_from_name (mono_defaults.systemtype_class, "get_UnderlyingSystemType", 0);

	usertype_method = mono_object_get_virtual_method ((MonoObject *) t, method_get_underlying_system_type);

	rt = (MonoReflectionType *) mono_runtime_invoke_checked (usertype_method, t, NULL, error);

	return rt;
}

MonoType*
mono_reflection_type_get_handle (MonoReflectionType* ref, MonoError *error)
{
	MonoClass *klass;
	mono_error_init (error);

	if (!ref)
		return NULL;
	if (ref->type)
		return ref->type;

	if (mono_reflection_is_usertype (ref)) {
		ref = mono_reflection_type_get_underlying_system_type (ref, error);
		if (ref == NULL || mono_reflection_is_usertype (ref) || !is_ok (error))
			return NULL;
		if (ref->type)
			return ref->type;
	}

	klass = mono_object_class (ref);

	if (is_sre_array (klass)) {
		MonoType *res;
		MonoReflectionArrayType *sre_array = (MonoReflectionArrayType*)ref;
		MonoType *base = mono_reflection_type_get_handle (sre_array->element_type, error);
		return_val_if_nok (error, NULL);
		g_assert (base);
		if (sre_array->rank == 0) //single dimentional array
			res = &mono_array_class_get (mono_class_from_mono_type (base), 1)->byval_arg;
		else
			res = &mono_bounded_array_class_get (mono_class_from_mono_type (base), sre_array->rank, TRUE)->byval_arg;
		sre_array->type.type = res;
		return res;
	} else if (is_sre_byref (klass)) {
		MonoType *res;
		MonoReflectionDerivedType *sre_byref = (MonoReflectionDerivedType*)ref;
		MonoType *base = mono_reflection_type_get_handle (sre_byref->element_type, error);
		return_val_if_nok (error, NULL);
		g_assert (base);
		res = &mono_class_from_mono_type (base)->this_arg;
		sre_byref->type.type = res;
		return res;
	} else if (is_sre_pointer (klass)) {
		MonoType *res;
		MonoReflectionDerivedType *sre_pointer = (MonoReflectionDerivedType*)ref;
		MonoType *base = mono_reflection_type_get_handle (sre_pointer->element_type, error);
		return_val_if_nok (error, NULL);
		g_assert (base);
		res = &mono_ptr_class_get (base)->byval_arg;
		sre_pointer->type.type = res;
		return res;
	} else if (is_sre_generic_instance (klass)) {
		MonoType *res, **types;
		MonoReflectionGenericClass *gclass = (MonoReflectionGenericClass*)ref;
		int i, count;

		count = mono_array_length (gclass->type_arguments);
		types = g_new0 (MonoType*, count);
		for (i = 0; i < count; ++i) {
			MonoReflectionType *t = (MonoReflectionType *)mono_array_get (gclass->type_arguments, gpointer, i);
			types [i] = mono_reflection_type_get_handle (t, error);
			if (!types[i] || !is_ok (error)) {
				g_free (types);
				return NULL;
			}
		}

		res = mono_reflection_bind_generic_parameters (gclass->generic_type, count, types, error);
		g_free (types);
		g_assert (res);
		gclass->type.type = res;
		return res;
	}

	g_error ("Cannot handle corlib user type %s", mono_type_full_name (&mono_object_class(ref)->byval_arg));
	return NULL;
}

void
ves_icall_SymbolType_create_unmanaged_type (MonoReflectionType *type)
{
	MonoError error;
	mono_reflection_type_get_handle (type, &error);
	mono_error_set_pending_exception (&error);
}

static gboolean
reflection_register_with_runtime (MonoReflectionType *type, MonoError *error)
{
	MonoDomain *domain = mono_object_domain ((MonoObject*)type);
	MonoClass *klass;

	mono_error_init (error);

	MonoType *res = mono_reflection_type_get_handle (type, error);

	if (!res && is_ok (error)) {
		mono_error_set_argument (error, NULL, "Invalid generic instantiation, one or more arguments are not proper user types");
	}
	return_val_if_nok (error, FALSE);

	klass = mono_class_from_mono_type (res);

	mono_loader_lock (); /*same locking as mono_type_get_object_checked */
	mono_domain_lock (domain);

	if (!image_is_dynamic (klass->image)) {
		mono_class_setup_supertypes (klass);
	} else {
		if (!domain->type_hash)
			domain->type_hash = mono_g_hash_table_new_type ((GHashFunc)mono_metadata_type_hash, 
					(GCompareFunc)mono_metadata_type_equal, MONO_HASH_VALUE_GC, MONO_ROOT_SOURCE_DOMAIN, "domain reflection types table");
		mono_g_hash_table_insert (domain->type_hash, res, type);
	}
	mono_domain_unlock (domain);
	mono_loader_unlock ();

	return TRUE;
}

void
mono_reflection_register_with_runtime (MonoReflectionType *type)
{
	MonoError error;
	(void) reflection_register_with_runtime (type, &error);
	mono_error_set_pending_exception (&error);
}

/**
 * LOCKING: Assumes the loader lock is held.
 */
static MonoMethodSignature*
parameters_to_signature (MonoImage *image, MonoArray *parameters, MonoError *error) {
	MonoMethodSignature *sig;
	int count, i;

	mono_error_init (error);

	count = parameters? mono_array_length (parameters): 0;

	sig = (MonoMethodSignature *)mono_image_g_malloc0 (image, MONO_SIZEOF_METHOD_SIGNATURE + sizeof (MonoType*) * count);
	sig->param_count = count;
	sig->sentinelpos = -1; /* FIXME */
	for (i = 0; i < count; ++i) {
		sig->params [i] = mono_type_array_get_and_resolve (parameters, i, error);
		if (!is_ok (error)) {
			image_g_free (image, sig);
			return NULL;
		}
	}
	return sig;
}

/**
 * LOCKING: Assumes the loader lock is held.
 */
static MonoMethodSignature*
ctor_builder_to_signature (MonoImage *image, MonoReflectionCtorBuilder *ctor, MonoError *error) {
	MonoMethodSignature *sig;

	mono_error_init (error);

	sig = parameters_to_signature (image, ctor->parameters, error);
	return_val_if_nok (error, NULL);
	sig->hasthis = ctor->attrs & METHOD_ATTRIBUTE_STATIC? 0: 1;
	sig->ret = &mono_defaults.void_class->byval_arg;
	return sig;
}

/**
 * LOCKING: Assumes the loader lock is held.
 */
static MonoMethodSignature*
method_builder_to_signature (MonoImage *image, MonoReflectionMethodBuilder *method, MonoError *error) {
	MonoMethodSignature *sig;

	mono_error_init (error);

	sig = parameters_to_signature (image, method->parameters, error);
	return_val_if_nok (error, NULL);
	sig->hasthis = method->attrs & METHOD_ATTRIBUTE_STATIC? 0: 1;
	if (method->rtype) {
		sig->ret = mono_reflection_type_get_handle ((MonoReflectionType*)method->rtype, error);
		if (!is_ok (error)) {
			image_g_free (image, sig);
			return NULL;
		}
	} else {
		sig->ret = &mono_defaults.void_class->byval_arg;
	}
	sig->generic_param_count = method->generic_params ? mono_array_length (method->generic_params) : 0;
	return sig;
}

static MonoMethodSignature*
dynamic_method_to_signature (MonoReflectionDynamicMethod *method, MonoError *error) {
	MonoMethodSignature *sig;

	mono_error_init (error);

	sig = parameters_to_signature (NULL, method->parameters, error);
	return_val_if_nok (error, NULL);
	sig->hasthis = method->attrs & METHOD_ATTRIBUTE_STATIC? 0: 1;
	if (method->rtype) {
		sig->ret = mono_reflection_type_get_handle (method->rtype, error);
		if (!is_ok (error)) {
			g_free (sig);
			return NULL;
		}
	} else {
		sig->ret = &mono_defaults.void_class->byval_arg;
	}
	sig->generic_param_count = 0;
	return sig;
}

static void
get_prop_name_and_type (MonoObject *prop, char **name, MonoType **type, MonoError *error)
{
	mono_error_init (error);
	MonoClass *klass = mono_object_class (prop);
	if (strcmp (klass->name, "PropertyBuilder") == 0) {
		MonoReflectionPropertyBuilder *pb = (MonoReflectionPropertyBuilder *)prop;
		*name = mono_string_to_utf8_checked (pb->name, error);
		return_if_nok (error);
		*type = mono_reflection_type_get_handle ((MonoReflectionType*)pb->type, error);
	} else {
		MonoReflectionProperty *p = (MonoReflectionProperty *)prop;
		*name = g_strdup (p->property->name);
		if (p->property->get)
			*type = mono_method_signature (p->property->get)->ret;
		else
			*type = mono_method_signature (p->property->set)->params [mono_method_signature (p->property->set)->param_count - 1];
	}
}

static void
get_field_name_and_type (MonoObject *field, char **name, MonoType **type, MonoError *error)
{
	mono_error_init (error);
	MonoClass *klass = mono_object_class (field);
	if (strcmp (klass->name, "FieldBuilder") == 0) {
		MonoReflectionFieldBuilder *fb = (MonoReflectionFieldBuilder *)field;
		*name = mono_string_to_utf8_checked (fb->name, error);
		return_if_nok (error);
		*type = mono_reflection_type_get_handle ((MonoReflectionType*)fb->type, error);
	} else {
		MonoReflectionField *f = (MonoReflectionField *)field;
		*name = g_strdup (mono_field_get_name (f->field));
		*type = f->field->type;
	}
}

#else /* DISABLE_REFLECTION_EMIT */

void
mono_reflection_register_with_runtime (MonoReflectionType *type)
{
	/* This is empty */
}

static gboolean
is_sre_type_builder (MonoClass *klass)
{
	return FALSE;
}

static gboolean
is_sre_generic_instance (MonoClass *klass)
{
	return FALSE;
}

gboolean
mono_is_sre_ctor_builder (MonoClass *klass)
{
	return FALSE;
}

gboolean
mono_is_sre_method_on_tb_inst (MonoClass *klass)
{
	return FALSE;
}

gboolean
mono_is_sre_ctor_on_tb_inst (MonoClass *klass)
{
	return FALSE;
}

void
mono_reflection_init_type_builder_generics (MonoObject *type, MonoError *error)
{
	mono_error_init (error);
}

#endif /* !DISABLE_REFLECTION_EMIT */


static gboolean
is_sr_mono_field (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection", "MonoField");
}

gboolean
mono_is_sr_mono_property (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection", "MonoProperty");
}

static gboolean
is_sr_mono_method (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection", "MonoMethod");
}

gboolean
mono_is_sr_mono_cmethod (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection", "MonoCMethod");
}

static gboolean
is_sr_mono_generic_method (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection", "MonoGenericMethod");
}

static gboolean
is_sr_mono_generic_cmethod (MonoClass *klass)
{
	check_corlib_type_cached (klass, "System.Reflection", "MonoGenericCMethod");
}

gboolean
mono_class_is_reflection_method_or_constructor (MonoClass *klass)
{
	return is_sr_mono_method (klass) || mono_is_sr_mono_cmethod (klass) || is_sr_mono_generic_method (klass) || is_sr_mono_generic_cmethod (klass);
}

gboolean
mono_is_sre_type_builder (MonoClass *klass)
{
	return is_sre_type_builder (klass);
}

gboolean
mono_is_sre_generic_instance (MonoClass *klass)
{
	return is_sre_generic_instance (klass);
}



/**
 * encode_cattr_value:
 * Encode a value in a custom attribute stream of bytes.
 * The value to encode is either supplied as an object in argument val
 * (valuetypes are boxed), or as a pointer to the data in the
 * argument argval.
 * @type represents the type of the value
 * @buffer is the start of the buffer
 * @p the current position in the buffer
 * @buflen contains the size of the buffer and is used to return the new buffer size
 * if this needs to be realloced.
 * @retbuffer and @retp return the start and the position of the buffer
 * @error set on error.
 */
static void
encode_cattr_value (MonoAssembly *assembly, char *buffer, char *p, char **retbuffer, char **retp, guint32 *buflen, MonoType *type, MonoObject *arg, char *argval, MonoError *error)
{
	MonoTypeEnum simple_type;
	
	mono_error_init (error);
	if ((p-buffer) + 10 >= *buflen) {
		char *newbuf;
		*buflen *= 2;
		newbuf = (char *)g_realloc (buffer, *buflen);
		p = newbuf + (p-buffer);
		buffer = newbuf;
	}
	if (!argval)
		argval = ((char*)arg + sizeof (MonoObject));
	simple_type = type->type;
handle_enum:
	switch (simple_type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
		*p++ = *argval;
		break;
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
		swap_with_size (p, argval, 2, 1);
		p += 2;
		break;
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
	case MONO_TYPE_R4:
		swap_with_size (p, argval, 4, 1);
		p += 4;
		break;
	case MONO_TYPE_R8:
		swap_with_size (p, argval, 8, 1);
		p += 8;
		break;
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
		swap_with_size (p, argval, 8, 1);
		p += 8;
		break;
	case MONO_TYPE_VALUETYPE:
		if (type->data.klass->enumtype) {
			simple_type = mono_class_enum_basetype (type->data.klass)->type;
			goto handle_enum;
		} else {
			g_warning ("generic valutype %s not handled in custom attr value decoding", type->data.klass->name);
		}
		break;
	case MONO_TYPE_STRING: {
		char *str;
		guint32 slen;
		if (!arg) {
			*p++ = 0xFF;
			break;
		}
		str = mono_string_to_utf8_checked ((MonoString*)arg, error);
		return_if_nok (error);
		slen = strlen (str);
		if ((p-buffer) + 10 + slen >= *buflen) {
			char *newbuf;
			*buflen *= 2;
			*buflen += slen;
			newbuf = (char *)g_realloc (buffer, *buflen);
			p = newbuf + (p-buffer);
			buffer = newbuf;
		}
		mono_metadata_encode_value (slen, p, &p);
		memcpy (p, str, slen);
		p += slen;
		g_free (str);
		break;
	}
	case MONO_TYPE_CLASS: {
		char *str;
		guint32 slen;
		MonoType *arg_type;
		if (!arg) {
			*p++ = 0xFF;
			break;
		}
handle_type:
		arg_type = mono_reflection_type_get_handle ((MonoReflectionType*)arg, error);
		return_if_nok (error);

		str = type_get_qualified_name (arg_type, NULL);
		slen = strlen (str);
		if ((p-buffer) + 10 + slen >= *buflen) {
			char *newbuf;
			*buflen *= 2;
			*buflen += slen;
			newbuf = (char *)g_realloc (buffer, *buflen);
			p = newbuf + (p-buffer);
			buffer = newbuf;
		}
		mono_metadata_encode_value (slen, p, &p);
		memcpy (p, str, slen);
		p += slen;
		g_free (str);
		break;
	}
	case MONO_TYPE_SZARRAY: {
		int len, i;
		MonoClass *eclass, *arg_eclass;

		if (!arg) {
			*p++ = 0xff; *p++ = 0xff; *p++ = 0xff; *p++ = 0xff;
			break;
		}
		len = mono_array_length ((MonoArray*)arg);
		*p++ = len & 0xff;
		*p++ = (len >> 8) & 0xff;
		*p++ = (len >> 16) & 0xff;
		*p++ = (len >> 24) & 0xff;
		*retp = p;
		*retbuffer = buffer;
		eclass = type->data.klass;
		arg_eclass = mono_object_class (arg)->element_class;

		if (!eclass) {
			/* Happens when we are called from the MONO_TYPE_OBJECT case below */
			eclass = mono_defaults.object_class;
		}
		if (eclass == mono_defaults.object_class && arg_eclass->valuetype) {
			char *elptr = mono_array_addr ((MonoArray*)arg, char, 0);
			int elsize = mono_class_array_element_size (arg_eclass);
			for (i = 0; i < len; ++i) {
				encode_cattr_value (assembly, buffer, p, &buffer, &p, buflen, &arg_eclass->byval_arg, NULL, elptr, error);
				return_if_nok (error);
				elptr += elsize;
			}
		} else if (eclass->valuetype && arg_eclass->valuetype) {
			char *elptr = mono_array_addr ((MonoArray*)arg, char, 0);
			int elsize = mono_class_array_element_size (eclass);
			for (i = 0; i < len; ++i) {
				encode_cattr_value (assembly, buffer, p, &buffer, &p, buflen, &eclass->byval_arg, NULL, elptr, error);
				return_if_nok (error);
				elptr += elsize;
			}
		} else {
			for (i = 0; i < len; ++i) {
				encode_cattr_value (assembly, buffer, p, &buffer, &p, buflen, &eclass->byval_arg, mono_array_get ((MonoArray*)arg, MonoObject*, i), NULL, error);
				return_if_nok (error);
			}
		}
		break;
	}
	case MONO_TYPE_OBJECT: {
		MonoClass *klass;
		char *str;
		guint32 slen;

		/*
		 * The parameter type is 'object' but the type of the actual
		 * argument is not. So we have to add type information to the blob
		 * too. This is completely undocumented in the spec.
		 */

		if (arg == NULL) {
			*p++ = MONO_TYPE_STRING;	// It's same hack as MS uses
			*p++ = 0xFF;
			break;
		}
		
		klass = mono_object_class (arg);

		if (mono_object_isinst_checked (arg, mono_defaults.systemtype_class, error)) {
			*p++ = 0x50;
			goto handle_type;
		} else {
			return_if_nok (error);
		}

		if (klass->enumtype) {
			*p++ = 0x55;
		} else if (klass == mono_defaults.string_class) {
			simple_type = MONO_TYPE_STRING;
			*p++ = 0x0E;
			goto handle_enum;
		} else if (klass->rank == 1) {
			*p++ = 0x1D;
			if (klass->element_class->byval_arg.type == MONO_TYPE_OBJECT)
				/* See Partition II, Appendix B3 */
				*p++ = 0x51;
			else
				*p++ = klass->element_class->byval_arg.type;
			encode_cattr_value (assembly, buffer, p, &buffer, &p, buflen, &klass->byval_arg, arg, NULL, error);
			return_if_nok (error);
			break;
		} else if (klass->byval_arg.type >= MONO_TYPE_BOOLEAN && klass->byval_arg.type <= MONO_TYPE_R8) {
			*p++ = simple_type = klass->byval_arg.type;
			goto handle_enum;
		} else {
			g_error ("unhandled type in custom attr");
		}
		str = type_get_qualified_name (mono_class_get_type(klass), NULL);
		slen = strlen (str);
		if ((p-buffer) + 10 + slen >= *buflen) {
			char *newbuf;
			*buflen *= 2;
			*buflen += slen;
			newbuf = (char *)g_realloc (buffer, *buflen);
			p = newbuf + (p-buffer);
			buffer = newbuf;
		}
		mono_metadata_encode_value (slen, p, &p);
		memcpy (p, str, slen);
		p += slen;
		g_free (str);
		simple_type = mono_class_enum_basetype (klass)->type;
		goto handle_enum;
	}
	default:
		g_error ("type 0x%02x not yet supported in custom attr encoder", simple_type);
	}
	*retp = p;
	*retbuffer = buffer;
}

static void
encode_field_or_prop_type (MonoType *type, char *p, char **retp)
{
	if (type->type == MONO_TYPE_VALUETYPE && type->data.klass->enumtype) {
		char *str = type_get_qualified_name (type, NULL);
		int slen = strlen (str);

		*p++ = 0x55;
		/*
		 * This seems to be optional...
		 * *p++ = 0x80;
		 */
		mono_metadata_encode_value (slen, p, &p);
		memcpy (p, str, slen);
		p += slen;
		g_free (str);
	} else if (type->type == MONO_TYPE_OBJECT) {
		*p++ = 0x51;
	} else if (type->type == MONO_TYPE_CLASS) {
		/* it should be a type: encode_cattr_value () has the check */
		*p++ = 0x50;
	} else {
		mono_metadata_encode_value (type->type, p, &p);
		if (type->type == MONO_TYPE_SZARRAY)
			/* See the examples in Partition VI, Annex B */
			encode_field_or_prop_type (&type->data.klass->byval_arg, p, &p);
	}

	*retp = p;
}

#ifndef DISABLE_REFLECTION_EMIT
static void
encode_named_val (MonoReflectionAssembly *assembly, char *buffer, char *p, char **retbuffer, char **retp, guint32 *buflen, MonoType *type, char *name, MonoObject *value, MonoError *error)
{
	int len;

	mono_error_init (error);

	/* Preallocate a large enough buffer */
	if (type->type == MONO_TYPE_VALUETYPE && type->data.klass->enumtype) {
		char *str = type_get_qualified_name (type, NULL);
		len = strlen (str);
		g_free (str);
	} else if (type->type == MONO_TYPE_SZARRAY && type->data.klass->enumtype) {
		char *str = type_get_qualified_name (&type->data.klass->byval_arg, NULL);
		len = strlen (str);
		g_free (str);
	} else {
		len = 0;
	}
	len += strlen (name);

	if ((p-buffer) + 20 + len >= *buflen) {
		char *newbuf;
		*buflen *= 2;
		*buflen += len;
		newbuf = (char *)g_realloc (buffer, *buflen);
		p = newbuf + (p-buffer);
		buffer = newbuf;
	}

	encode_field_or_prop_type (type, p, &p);

	len = strlen (name);
	mono_metadata_encode_value (len, p, &p);
	memcpy (p, name, len);
	p += len;
	encode_cattr_value (assembly->assembly, buffer, p, &buffer, &p, buflen, type, value, NULL, error);
	return_if_nok (error);
	*retp = p;
	*retbuffer = buffer;
}

/**
 * mono_reflection_get_custom_attrs_blob:
 * @ctor: custom attribute constructor
 * @ctorArgs: arguments o the constructor
 * @properties:
 * @propValues:
 * @fields:
 * @fieldValues:
 * 
 * Creates the blob of data that needs to be saved in the metadata and that represents
 * the custom attributed described by @ctor, @ctorArgs etc.
 * Returns: a Byte array representing the blob of data.
 */
MonoArray*
mono_reflection_get_custom_attrs_blob (MonoReflectionAssembly *assembly, MonoObject *ctor, MonoArray *ctorArgs, MonoArray *properties, MonoArray *propValues, MonoArray *fields, MonoArray* fieldValues) 
{
	MonoError error;
	MonoArray *result = mono_reflection_get_custom_attrs_blob_checked (assembly, ctor, ctorArgs, properties, propValues, fields, fieldValues, &error);
	mono_error_cleanup (&error);
	return result;
}

/**
 * mono_reflection_get_custom_attrs_blob_checked:
 * @ctor: custom attribute constructor
 * @ctorArgs: arguments o the constructor
 * @properties:
 * @propValues:
 * @fields:
 * @fieldValues:
 * @error: set on error
 * 
 * Creates the blob of data that needs to be saved in the metadata and that represents
 * the custom attributed described by @ctor, @ctorArgs etc.
 * Returns: a Byte array representing the blob of data.  On failure returns NULL and sets @error.
 */
MonoArray*
mono_reflection_get_custom_attrs_blob_checked (MonoReflectionAssembly *assembly, MonoObject *ctor, MonoArray *ctorArgs, MonoArray *properties, MonoArray *propValues, MonoArray *fields, MonoArray* fieldValues, MonoError *error) 
{
	MonoArray *result = NULL;
	MonoMethodSignature *sig;
	MonoObject *arg;
	char *buffer, *p;
	guint32 buflen, i;

	mono_error_init (error);

	if (strcmp (ctor->vtable->klass->name, "MonoCMethod")) {
		/* sig is freed later so allocate it in the heap */
		sig = ctor_builder_to_signature (NULL, (MonoReflectionCtorBuilder*)ctor, error);
		if (!is_ok (error)) {
			g_free (sig);
			return NULL;
		}
	} else {
		sig = mono_method_signature (((MonoReflectionMethod*)ctor)->method);
	}

	g_assert (mono_array_length (ctorArgs) == sig->param_count);
	buflen = 256;
	p = buffer = (char *)g_malloc (buflen);
	/* write the prolog */
	*p++ = 1;
	*p++ = 0;
	for (i = 0; i < sig->param_count; ++i) {
		arg = mono_array_get (ctorArgs, MonoObject*, i);
		encode_cattr_value (assembly->assembly, buffer, p, &buffer, &p, &buflen, sig->params [i], arg, NULL, error);
		if (!is_ok (error)) goto leave;
	}
	i = 0;
	if (properties)
		i += mono_array_length (properties);
	if (fields)
		i += mono_array_length (fields);
	*p++ = i & 0xff;
	*p++ = (i >> 8) & 0xff;
	if (properties) {
		MonoObject *prop;
		for (i = 0; i < mono_array_length (properties); ++i) {
			MonoType *ptype;
			char *pname;

			prop = (MonoObject *)mono_array_get (properties, gpointer, i);
			get_prop_name_and_type (prop, &pname, &ptype, error);
			if (!is_ok (error)) goto leave;
			*p++ = 0x54; /* PROPERTY signature */
			encode_named_val (assembly, buffer, p, &buffer, &p, &buflen, ptype, pname, (MonoObject*)mono_array_get (propValues, gpointer, i), error);
			g_free (pname);
			if (!is_ok (error)) goto leave;
		}
	}

	if (fields) {
		MonoObject *field;
		for (i = 0; i < mono_array_length (fields); ++i) {
			MonoType *ftype;
			char *fname;

			field = (MonoObject *)mono_array_get (fields, gpointer, i);
			get_field_name_and_type (field, &fname, &ftype, error);
			if (!is_ok (error)) goto leave;
			*p++ = 0x53; /* FIELD signature */
			encode_named_val (assembly, buffer, p, &buffer, &p, &buflen, ftype, fname, (MonoObject*)mono_array_get (fieldValues, gpointer, i), error);
			g_free (fname);
			if (!is_ok (error)) goto leave;
		}
	}

	g_assert (p - buffer <= buflen);
	buflen = p - buffer;
	result = mono_array_new_checked (mono_domain_get (), mono_defaults.byte_class, buflen, error);
	if (!is_ok (error))
		goto leave;
	p = mono_array_addr (result, char, 0);
	memcpy (p, buffer, buflen);
leave:
	g_free (buffer);
	if (strcmp (ctor->vtable->klass->name, "MonoCMethod"))
		g_free (sig);
	return result;
}

/**
 * reflection_setup_internal_class:
 * @tb: a TypeBuilder object
 * @error: set on error
 *
 * Creates a MonoClass that represents the TypeBuilder.
 * This is a trick that lets us simplify a lot of reflection code
 * (and will allow us to support Build and Run assemblies easier).
 *
 * Returns TRUE on success. On failure, returns FALSE and sets @error.
 */
static gboolean
reflection_setup_internal_class (MonoReflectionTypeBuilder *tb, MonoError *error)
{
	MonoClass *klass, *parent;

	mono_error_init (error);

	mono_loader_lock ();

	if (tb->parent) {
		MonoType *parent_type = mono_reflection_type_get_handle ((MonoReflectionType*)tb->parent, error);
		if (!is_ok (error)) {
			mono_loader_unlock ();
			return FALSE;
		}
		/* check so we can compile corlib correctly */
		if (strcmp (mono_object_class (tb->parent)->name, "TypeBuilder") == 0) {
			/* mono_class_setup_mono_type () guaranteess type->data.klass is valid */
			parent = parent_type->data.klass;
		} else {
			parent = mono_class_from_mono_type (parent_type);
		}
	} else {
		parent = NULL;
	}
	
	/* the type has already being created: it means we just have to change the parent */
	if (tb->type.type) {
		klass = mono_class_from_mono_type (tb->type.type);
		klass->parent = NULL;
		/* fool mono_class_setup_parent */
		klass->supertypes = NULL;
		mono_class_setup_parent (klass, parent);
		mono_class_setup_mono_type (klass);
		mono_loader_unlock ();
		return TRUE;
	}

	klass = (MonoClass *)mono_image_alloc0 (&tb->module->dynamic_image->image, sizeof (MonoClass));

	klass->image = &tb->module->dynamic_image->image;

	klass->inited = 1; /* we lie to the runtime */
	klass->name = mono_string_to_utf8_image (klass->image, tb->name, error);
	if (!is_ok (error))
		goto failure;
	klass->name_space = mono_string_to_utf8_image (klass->image, tb->nspace, error);
	if (!is_ok (error))
		goto failure;
	klass->type_token = MONO_TOKEN_TYPE_DEF | tb->table_idx;
	klass->flags = tb->attrs;
	
	mono_profiler_class_event (klass, MONO_PROFILE_START_LOAD);

	klass->element_class = klass;

	if (mono_class_get_ref_info (klass) == NULL) {
		mono_class_set_ref_info (klass, tb);

		/* Put into cache so mono_class_get_checked () will find it.
		Skip nested types as those should not be available on the global scope. */
		if (!tb->nesting_type)
			mono_image_add_to_name_cache (klass->image, klass->name_space, klass->name, tb->table_idx);

		/*
		We must register all types as we cannot rely on the name_cache hashtable since we find the class
		by performing a mono_class_get which does the full resolution.

		Working around this semantics would require us to write a lot of code for no clear advantage.
		*/
		mono_image_append_class_to_reflection_info_set (klass);
	} else {
		g_assert (mono_class_get_ref_info (klass) == tb);
	}

	mono_dynamic_image_register_token (tb->module->dynamic_image, MONO_TOKEN_TYPE_DEF | tb->table_idx, (MonoObject*)tb);

	if (parent != NULL) {
		mono_class_setup_parent (klass, parent);
	} else if (strcmp (klass->name, "Object") == 0 && strcmp (klass->name_space, "System") == 0) {
		const char *old_n = klass->name;
		/* trick to get relative numbering right when compiling corlib */
		klass->name = "BuildingObject";
		mono_class_setup_parent (klass, mono_defaults.object_class);
		klass->name = old_n;
	}

	if ((!strcmp (klass->name, "ValueType") && !strcmp (klass->name_space, "System")) ||
			(!strcmp (klass->name, "Object") && !strcmp (klass->name_space, "System")) ||
			(!strcmp (klass->name, "Enum") && !strcmp (klass->name_space, "System"))) {
		klass->instance_size = sizeof (MonoObject);
		klass->size_inited = 1;
		mono_class_setup_vtable_general (klass, NULL, 0, NULL);
	}

	mono_class_setup_mono_type (klass);

	mono_class_setup_supertypes (klass);

	/*
	 * FIXME: handle interfaces.
	 */

	tb->type.type = &klass->byval_arg;

	if (tb->nesting_type) {
		g_assert (tb->nesting_type->type);
		MonoType *nesting_type = mono_reflection_type_get_handle (tb->nesting_type, error);
		if (!is_ok (error)) goto failure;
		klass->nested_in = mono_class_from_mono_type (nesting_type);
	}

	/*g_print ("setup %s as %s (%p)\n", klass->name, ((MonoObject*)tb)->vtable->klass->name, tb);*/

	mono_profiler_class_loaded (klass, MONO_PROFILE_OK);
	
	mono_loader_unlock ();
	return TRUE;

failure:
	mono_loader_unlock ();
	return FALSE;
}

/**
 * ves_icall_TypeBuilder_setup_internal_class:
 * @tb: a TypeBuilder object
 *
 * (icall)
 * Creates a MonoClass that represents the TypeBuilder.
 * This is a trick that lets us simplify a lot of reflection code
 * (and will allow us to support Build and Run assemblies easier).
 *
 */
void
ves_icall_TypeBuilder_setup_internal_class (MonoReflectionTypeBuilder *tb)
{
	MonoError error;
	(void) reflection_setup_internal_class (tb, &error);
	mono_error_set_pending_exception (&error);
}

/**
 * mono_reflection_create_generic_class:
 * @tb: a TypeBuilder object
 * @error: set on error
 *
 * Creates the generic class after all generic parameters have been added.
 * On success returns TRUE, on failure returns FALSE and sets @error.
 * 
 */
gboolean
mono_reflection_create_generic_class (MonoReflectionTypeBuilder *tb, MonoError *error)
{

	MonoClass *klass;
	int count, i;

	mono_error_init (error);

	klass = mono_class_from_mono_type (tb->type.type);

	count = tb->generic_params ? mono_array_length (tb->generic_params) : 0;

	if (klass->generic_container || (count == 0))
		return TRUE;

	g_assert (tb->generic_container && (tb->generic_container->owner.klass == klass));

	klass->generic_container = (MonoGenericContainer *)mono_image_alloc0 (klass->image, sizeof (MonoGenericContainer));

	klass->generic_container->owner.klass = klass;
	klass->generic_container->type_argc = count;
	klass->generic_container->type_params = (MonoGenericParamFull *)mono_image_alloc0 (klass->image, sizeof (MonoGenericParamFull) * count);

	klass->is_generic = 1;

	for (i = 0; i < count; i++) {
		MonoReflectionGenericParam *gparam = (MonoReflectionGenericParam *)mono_array_get (tb->generic_params, gpointer, i);
		MonoType *param_type = mono_reflection_type_get_handle ((MonoReflectionType*)gparam, error);
		return_val_if_nok (error, FALSE);
		MonoGenericParamFull *param = (MonoGenericParamFull *) param_type->data.generic_param;
		klass->generic_container->type_params [i] = *param;
		/*Make sure we are a diferent type instance */
		klass->generic_container->type_params [i].param.owner = klass->generic_container;
		klass->generic_container->type_params [i].info.pklass = NULL;
		klass->generic_container->type_params [i].info.flags = gparam->attrs;

		g_assert (klass->generic_container->type_params [i].param.owner);
	}

	klass->generic_container->context.class_inst = mono_get_shared_generic_inst (klass->generic_container);
	return TRUE;
}

static MonoMarshalSpec*
mono_marshal_spec_from_builder (MonoImage *image, MonoAssembly *assembly,
				MonoReflectionMarshal *minfo, MonoError *error)
{
	MonoMarshalSpec *res;

	mono_error_init (error);

	res = image_g_new0 (image, MonoMarshalSpec, 1);
	res->native = (MonoMarshalNative)minfo->type;

	switch (minfo->type) {
	case MONO_NATIVE_LPARRAY:
		res->data.array_data.elem_type = (MonoMarshalNative)minfo->eltype;
		if (minfo->has_size) {
			res->data.array_data.param_num = minfo->param_num;
			res->data.array_data.num_elem = minfo->count;
			res->data.array_data.elem_mult = minfo->param_num == -1 ? 0 : 1;
		}
		else {
			res->data.array_data.param_num = -1;
			res->data.array_data.num_elem = -1;
			res->data.array_data.elem_mult = -1;
		}
		break;

	case MONO_NATIVE_BYVALTSTR:
	case MONO_NATIVE_BYVALARRAY:
		res->data.array_data.num_elem = minfo->count;
		break;

	case MONO_NATIVE_CUSTOM:
		if (minfo->marshaltyperef) {
			MonoType *marshaltyperef = mono_reflection_type_get_handle ((MonoReflectionType*)minfo->marshaltyperef, error);
			if (!is_ok (error)) {
				image_g_free (image, res);
				return NULL;
			}
			res->data.custom_data.custom_name =
				type_get_fully_qualified_name (marshaltyperef);
		}
		if (minfo->mcookie) {
			res->data.custom_data.cookie = mono_string_to_utf8_checked (minfo->mcookie, error);
			if (!is_ok (error)) {
				image_g_free (image, res);
				return NULL;
			}
		}
		break;

	default:
		break;
	}

	return res;
}
#endif /* !DISABLE_REFLECTION_EMIT */

MonoReflectionMarshalAsAttribute*
mono_reflection_marshal_as_attribute_from_marshal_spec (MonoDomain *domain, MonoClass *klass,
							MonoMarshalSpec *spec, MonoError *error)
{
	MonoReflectionType *rt;
	MonoReflectionMarshalAsAttribute *minfo;
	MonoType *mtype;

	mono_error_init (error);
	
	minfo = (MonoReflectionMarshalAsAttribute*)mono_object_new_checked (domain, mono_class_get_marshal_as_attribute_class (), error);
	if (!minfo)
		return NULL;
	minfo->utype = spec->native;

	switch (minfo->utype) {
	case MONO_NATIVE_LPARRAY:
		minfo->array_subtype = spec->data.array_data.elem_type;
		minfo->size_const = spec->data.array_data.num_elem;
		if (spec->data.array_data.param_num != -1)
			minfo->size_param_index = spec->data.array_data.param_num;
		break;

	case MONO_NATIVE_BYVALTSTR:
	case MONO_NATIVE_BYVALARRAY:
		minfo->size_const = spec->data.array_data.num_elem;
		break;

	case MONO_NATIVE_CUSTOM:
		if (spec->data.custom_data.custom_name) {
			mtype = mono_reflection_type_from_name_checked (spec->data.custom_data.custom_name, klass->image, error);
			return_val_if_nok  (error, NULL);

			if (mtype) {
				rt = mono_type_get_object_checked (domain, mtype, error);
				if (!rt)
					return NULL;

				MONO_OBJECT_SETREF (minfo, marshal_type_ref, rt);
			}

			MONO_OBJECT_SETREF (minfo, marshal_type, mono_string_new (domain, spec->data.custom_data.custom_name));
		}
		if (spec->data.custom_data.cookie)
			MONO_OBJECT_SETREF (minfo, marshal_cookie, mono_string_new (domain, spec->data.custom_data.cookie));
		break;

	default:
		break;
	}

	return minfo;
}

#ifndef DISABLE_REFLECTION_EMIT
static MonoMethod*
reflection_methodbuilder_to_mono_method (MonoClass *klass,
					 ReflectionMethodBuilder *rmb,
					 MonoMethodSignature *sig,
					 MonoError *error)
{
	MonoMethod *m;
	MonoMethodWrapper *wrapperm;
	MonoMarshalSpec **specs;
	MonoReflectionMethodAux *method_aux;
	MonoImage *image;
	gboolean dynamic;
	int i;

	mono_error_init (error);
	/*
	 * Methods created using a MethodBuilder should have their memory allocated
	 * inside the image mempool, while dynamic methods should have their memory
	 * malloc'd.
	 */
	dynamic = rmb->refs != NULL;
	image = dynamic ? NULL : klass->image;

	if (!dynamic)
		g_assert (!klass->generic_class);

	mono_loader_lock ();

	if ((rmb->attrs & METHOD_ATTRIBUTE_PINVOKE_IMPL) ||
			(rmb->iattrs & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL))
		m = (MonoMethod *)image_g_new0 (image, MonoMethodPInvoke, 1);
	else
		m = (MonoMethod *)image_g_new0 (image, MonoMethodWrapper, 1);

	wrapperm = (MonoMethodWrapper*)m;

	m->dynamic = dynamic;
	m->slot = -1;
	m->flags = rmb->attrs;
	m->iflags = rmb->iattrs;
	m->name = mono_string_to_utf8_image_ignore (image, rmb->name);
	m->klass = klass;
	m->signature = sig;
	m->sre_method = TRUE;
	m->skip_visibility = rmb->skip_visibility;
	if (rmb->table_idx)
		m->token = MONO_TOKEN_METHOD_DEF | (*rmb->table_idx);

	if (m->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
		if (klass == mono_defaults.string_class && !strcmp (m->name, ".ctor"))
			m->string_ctor = 1;

		m->signature->pinvoke = 1;
	} else if (m->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
		m->signature->pinvoke = 1;

		method_aux = image_g_new0 (image, MonoReflectionMethodAux, 1);

		method_aux->dllentry = rmb->dllentry ? mono_string_to_utf8_image (image, rmb->dllentry, error) : image_strdup (image, m->name);
		mono_error_assert_ok (error);
		method_aux->dll = mono_string_to_utf8_image (image, rmb->dll, error);
		mono_error_assert_ok (error);
		
		((MonoMethodPInvoke*)m)->piflags = (rmb->native_cc << 8) | (rmb->charset ? (rmb->charset - 1) * 2 : 0) | rmb->extra_flags;

		if (image_is_dynamic (klass->image))
			g_hash_table_insert (((MonoDynamicImage*)klass->image)->method_aux_hash, m, method_aux);

		mono_loader_unlock ();

		return m;
	} else if (!(m->flags & METHOD_ATTRIBUTE_ABSTRACT) &&
			   !(m->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME)) {
		MonoMethodHeader *header;
		guint32 code_size;
		gint32 max_stack, i;
		gint32 num_locals = 0;
		gint32 num_clauses = 0;
		guint8 *code;

		if (rmb->ilgen) {
			code = mono_array_addr (rmb->ilgen->code, guint8, 0);
			code_size = rmb->ilgen->code_len;
			max_stack = rmb->ilgen->max_stack;
			num_locals = rmb->ilgen->locals ? mono_array_length (rmb->ilgen->locals) : 0;
			if (rmb->ilgen->ex_handlers)
				num_clauses = mono_reflection_method_count_clauses (rmb->ilgen);
		} else {
			if (rmb->code) {
				code = mono_array_addr (rmb->code, guint8, 0);
				code_size = mono_array_length (rmb->code);
				/* we probably need to run a verifier on the code... */
				max_stack = 8; 
			}
			else {
				code = NULL;
				code_size = 0;
				max_stack = 8;
			}
		}

		header = (MonoMethodHeader *)mono_image_g_malloc0 (image, MONO_SIZEOF_METHOD_HEADER + num_locals * sizeof (MonoType*));
		header->code_size = code_size;
		header->code = (const unsigned char *)image_g_malloc (image, code_size);
		memcpy ((char*)header->code, code, code_size);
		header->max_stack = max_stack;
		header->init_locals = rmb->init_locals;
		header->num_locals = num_locals;

		for (i = 0; i < num_locals; ++i) {
			MonoReflectionLocalBuilder *lb = 
				mono_array_get (rmb->ilgen->locals, MonoReflectionLocalBuilder*, i);

			header->locals [i] = image_g_new0 (image, MonoType, 1);
			MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)lb->type, error);
			mono_error_assert_ok (error);
			memcpy (header->locals [i], type, MONO_SIZEOF_TYPE);
		}

		header->num_clauses = num_clauses;
		if (num_clauses) {
			header->clauses = method_encode_clauses (image, (MonoDynamicImage*)klass->image,
								 rmb->ilgen, num_clauses, error);
			mono_error_assert_ok (error);
		}

		wrapperm->header = header;
	}

	if (rmb->generic_params) {
		int count = mono_array_length (rmb->generic_params);
		MonoGenericContainer *container = rmb->generic_container;

		g_assert (container);

		container->type_argc = count;
		container->type_params = image_g_new0 (image, MonoGenericParamFull, count);
		container->owner.method = m;
		container->is_anonymous = FALSE; // Method is now known, container is no longer anonymous

		m->is_generic = TRUE;
		mono_method_set_generic_container (m, container);

		for (i = 0; i < count; i++) {
			MonoReflectionGenericParam *gp =
				mono_array_get (rmb->generic_params, MonoReflectionGenericParam*, i);
			MonoType *gp_type = mono_reflection_type_get_handle ((MonoReflectionType*)gp, error);
			mono_error_assert_ok (error);
			MonoGenericParamFull *param = (MonoGenericParamFull *) gp_type->data.generic_param;
			container->type_params [i] = *param;
		}

		/*
		 * The method signature might have pointers to generic parameters that belong to other methods.
		 * This is a valid SRE case, but the resulting method signature must be encoded using the proper
		 * generic parameters.
		 */
		for (i = 0; i < m->signature->param_count; ++i) {
			MonoType *t = m->signature->params [i];
			if (t->type == MONO_TYPE_MVAR) {
				MonoGenericParam *gparam =  t->data.generic_param;
				if (gparam->num < count) {
					m->signature->params [i] = mono_metadata_type_dup (image, m->signature->params [i]);
					m->signature->params [i]->data.generic_param = mono_generic_container_get_param (container, gparam->num);
				}

			}
		}

		if (klass->generic_container) {
			container->parent = klass->generic_container;
			container->context.class_inst = klass->generic_container->context.class_inst;
		}
		container->context.method_inst = mono_get_shared_generic_inst (container);
	}

	if (rmb->refs) {
		MonoMethodWrapper *mw = (MonoMethodWrapper*)m;
		int i;
		void **data;

		m->wrapper_type = MONO_WRAPPER_DYNAMIC_METHOD;

		mw->method_data = data = image_g_new (image, gpointer, rmb->nrefs + 1);
		data [0] = GUINT_TO_POINTER (rmb->nrefs);
		for (i = 0; i < rmb->nrefs; ++i)
			data [i + 1] = rmb->refs [i];
	}

	method_aux = NULL;

	/* Parameter info */
	if (rmb->pinfo) {
		if (!method_aux)
			method_aux = image_g_new0 (image, MonoReflectionMethodAux, 1);
		method_aux->param_names = image_g_new0 (image, char *, mono_method_signature (m)->param_count + 1);
		for (i = 0; i <= m->signature->param_count; ++i) {
			MonoReflectionParamBuilder *pb;
			if ((pb = mono_array_get (rmb->pinfo, MonoReflectionParamBuilder*, i))) {
				if ((i > 0) && (pb->attrs)) {
					/* Make a copy since it might point to a shared type structure */
					m->signature->params [i - 1] = mono_metadata_type_dup (klass->image, m->signature->params [i - 1]);
					m->signature->params [i - 1]->attrs = pb->attrs;
				}

				if (pb->attrs & PARAM_ATTRIBUTE_HAS_DEFAULT) {
					MonoDynamicImage *assembly;
					guint32 idx, len;
					MonoTypeEnum def_type;
					char *p;
					const char *p2;

					if (!method_aux->param_defaults) {
						method_aux->param_defaults = image_g_new0 (image, guint8*, m->signature->param_count + 1);
						method_aux->param_default_types = image_g_new0 (image, guint32, m->signature->param_count + 1);
					}
					assembly = (MonoDynamicImage*)klass->image;
					idx = mono_dynimage_encode_constant (assembly, pb->def_value, &def_type);
					/* Copy the data from the blob since it might get realloc-ed */
					p = assembly->blob.data + idx;
					len = mono_metadata_decode_blob_size (p, &p2);
					len += p2 - p;
					method_aux->param_defaults [i] = (uint8_t *)image_g_malloc (image, len);
					method_aux->param_default_types [i] = def_type;
					memcpy ((gpointer)method_aux->param_defaults [i], p, len);
				}

				if (pb->name) {
					method_aux->param_names [i] = mono_string_to_utf8_image (image, pb->name, error);
					mono_error_assert_ok (error);
				}
				if (pb->cattrs) {
					if (!method_aux->param_cattr)
						method_aux->param_cattr = image_g_new0 (image, MonoCustomAttrInfo*, m->signature->param_count + 1);
					method_aux->param_cattr [i] = mono_custom_attrs_from_builders (image, klass->image, pb->cattrs);
				}
			}
		}
	}

	/* Parameter marshalling */
	specs = NULL;
	if (rmb->pinfo)		
		for (i = 0; i < mono_array_length (rmb->pinfo); ++i) {
			MonoReflectionParamBuilder *pb;
			if ((pb = mono_array_get (rmb->pinfo, MonoReflectionParamBuilder*, i))) {
				if (pb->marshal_info) {
					if (specs == NULL)
						specs = image_g_new0 (image, MonoMarshalSpec*, sig->param_count + 1);
					specs [pb->position] = 
						mono_marshal_spec_from_builder (image, klass->image->assembly, pb->marshal_info, error);
					if (!is_ok (error)) {
						mono_loader_unlock ();
						image_g_free (image, specs);
						/* FIXME: if image is NULL, this leaks all the other stuff we alloc'd in this function */
						return NULL;
					}
				}
			}
		}
	if (specs != NULL) {
		if (!method_aux)
			method_aux = image_g_new0 (image, MonoReflectionMethodAux, 1);
		method_aux->param_marshall = specs;
	}

	if (image_is_dynamic (klass->image) && method_aux)
		g_hash_table_insert (((MonoDynamicImage*)klass->image)->method_aux_hash, m, method_aux);

	mono_loader_unlock ();

	return m;
}	

static MonoMethod*
ctorbuilder_to_mono_method (MonoClass *klass, MonoReflectionCtorBuilder* mb, MonoError *error)
{
	ReflectionMethodBuilder rmb;
	MonoMethodSignature *sig;

	mono_loader_lock ();
	g_assert (klass->image != NULL);
	sig = ctor_builder_to_signature (klass->image, mb, error);
	mono_loader_unlock ();
	return_val_if_nok (error, NULL);

	if (!mono_reflection_methodbuilder_from_ctor_builder (&rmb, mb, error))
		return NULL;

	mb->mhandle = reflection_methodbuilder_to_mono_method (klass, &rmb, sig, error);
	return_val_if_nok (error, NULL);
	mono_save_custom_attrs (klass->image, mb->mhandle, mb->cattrs);

	/* If we are in a generic class, we might be called multiple times from inflate_method */
	if (!((MonoDynamicImage*)(MonoDynamicImage*)klass->image)->save && !klass->generic_container) {
		/* ilgen is no longer needed */
		mb->ilgen = NULL;
	}

	return mb->mhandle;
}

static MonoMethod*
methodbuilder_to_mono_method (MonoClass *klass, MonoReflectionMethodBuilder* mb, MonoError *error)
{
	ReflectionMethodBuilder rmb;
	MonoMethodSignature *sig;

	mono_error_init (error);

	mono_loader_lock ();
	g_assert (klass->image != NULL);
	sig = method_builder_to_signature (klass->image, mb, error);
	mono_loader_unlock ();
	return_val_if_nok (error, NULL);

	if (!mono_reflection_methodbuilder_from_method_builder (&rmb, mb, error))
		return NULL;

	mb->mhandle = reflection_methodbuilder_to_mono_method (klass, &rmb, sig, error);
	return_val_if_nok (error, NULL);
	mono_save_custom_attrs (klass->image, mb->mhandle, mb->cattrs);

	/* If we are in a generic class, we might be called multiple times from inflate_method */
	if (!((MonoDynamicImage*)(MonoDynamicImage*)klass->image)->save && !klass->generic_container) {
		/* ilgen is no longer needed */
		mb->ilgen = NULL;
	}
	return mb->mhandle;
}
#endif

#ifndef DISABLE_REFLECTION_EMIT

static MonoMethod *
inflate_mono_method (MonoClass *klass, MonoMethod *method, MonoObject *obj)
{
	MonoMethodInflated *imethod;
	MonoGenericContext *context;
	int i;

	/*
	 * With generic code sharing the klass might not be inflated.
	 * This can happen because classes inflated with their own
	 * type arguments are "normalized" to the uninflated class.
	 */
	if (!klass->generic_class)
		return method;

	context = mono_class_get_context (klass);

	if (klass->method.count && klass->methods) {
		/* Find the already created inflated method */
		for (i = 0; i < klass->method.count; ++i) {
			g_assert (klass->methods [i]->is_inflated);
			if (((MonoMethodInflated*)klass->methods [i])->declaring == method)
				break;
		}
		g_assert (i < klass->method.count);
		imethod = (MonoMethodInflated*)klass->methods [i];
	} else {
		MonoError error;
		imethod = (MonoMethodInflated *) mono_class_inflate_generic_method_full_checked (method, klass, context, &error);
		mono_error_assert_ok (&error);
	}

	if (method->is_generic && image_is_dynamic (method->klass->image)) {
		MonoDynamicImage *image = (MonoDynamicImage*)method->klass->image;

		mono_image_lock ((MonoImage*)image);
		mono_g_hash_table_insert (image->generic_def_objects, imethod, obj);
		mono_image_unlock ((MonoImage*)image);
	}
	return (MonoMethod *) imethod;
}

static MonoMethod *
inflate_method (MonoReflectionType *type, MonoObject *obj, MonoError *error)
{
	MonoMethod *method;
	MonoClass *gklass;

	mono_error_init (error);

	MonoClass *type_class = mono_object_class (type);

	if (is_sre_generic_instance (type_class)) {
		MonoReflectionGenericClass *mgc = (MonoReflectionGenericClass*)type;
		MonoType *generic_type = mono_reflection_type_get_handle ((MonoReflectionType*)mgc->generic_type, error);
		return_val_if_nok (error, NULL);
		gklass = mono_class_from_mono_type (generic_type);
	} else if (is_sre_type_builder (type_class)) {
		MonoType *t = mono_reflection_type_get_handle (type, error);
		return_val_if_nok (error, NULL);
		gklass = mono_class_from_mono_type (t);
	} else if (type->type) {
		gklass = mono_class_from_mono_type (type->type);
		gklass = mono_class_get_generic_type_definition (gklass);
	} else {
		g_error ("Can't handle type %s", mono_type_get_full_name (mono_object_class (type)));
	}

	if (!strcmp (obj->vtable->klass->name, "MethodBuilder"))
		if (((MonoReflectionMethodBuilder*)obj)->mhandle)
			method = ((MonoReflectionMethodBuilder*)obj)->mhandle;
		else {
			method = methodbuilder_to_mono_method (gklass, (MonoReflectionMethodBuilder *) obj, error);
			if (!method)
				return NULL;
		}
	else if (!strcmp (obj->vtable->klass->name, "ConstructorBuilder")) {
		method = ctorbuilder_to_mono_method (gklass, (MonoReflectionCtorBuilder *) obj, error);
		if (!method)
			return NULL;
	} else if (!strcmp (obj->vtable->klass->name, "MonoMethod") || !strcmp (obj->vtable->klass->name, "MonoCMethod"))
		method = ((MonoReflectionMethod *) obj)->method;
	else {
		method = NULL; /* prevent compiler warning */
		g_error ("can't handle type %s", obj->vtable->klass->name);
	}

	MonoType *t = mono_reflection_type_get_handle (type, error);
	return_val_if_nok (error, NULL);
	return inflate_mono_method (mono_class_from_mono_type (t), method, obj);
}

static void
reflection_generic_class_initialize (MonoReflectionGenericClass *type, MonoError *error)
{
	MonoGenericClass *gclass;
	MonoClass *klass, *gklass;
	MonoType *gtype;

	mono_error_init (error);

	gtype = mono_reflection_type_get_handle ((MonoReflectionType*)type, error);
	return_if_nok (error);
	klass = mono_class_from_mono_type (gtype);
	g_assert (gtype->type == MONO_TYPE_GENERICINST);
	gclass = gtype->data.generic_class;

	if (!gclass->is_dynamic)
		return;

	gklass = gclass->container_class;
	mono_class_init (gklass);

	/* Mark this as needing synchronization with its generic container */
	gclass->need_sync = TRUE;
}

void
mono_reflection_generic_class_initialize (MonoReflectionGenericClass *type, MonoArray *fields)
{
	MonoError error;
	reflection_generic_class_initialize (type, &error);
	mono_error_set_pending_exception (&error);
}

/**
 * fix_partial_generic_class:
 * @klass: a generic instantiation MonoClass
 * @error: set on error
 *
 * Assumes that the generic container of @klass has its vtable
 * initialized, and updates the parent class, interfaces, methods and
 * fields of @klass by inflating the types using the generic context.
 *
 * On success returns TRUE, on failure returns FALSE and sets @error.
 *
 */
static gboolean
fix_partial_generic_class (MonoClass *klass, MonoError *error)
{
	MonoClass *gklass = klass->generic_class->container_class;
	int i;

	mono_error_init (error);

	if (klass->wastypebuilder)
		return TRUE;

	if (klass->parent != gklass->parent) {
		MonoType *parent_type = mono_class_inflate_generic_type_checked (&gklass->parent->byval_arg, &klass->generic_class->context, error);
		if (mono_error_ok (error)) {
			MonoClass *parent = mono_class_from_mono_type (parent_type);
			mono_metadata_free_type (parent_type);
			if (parent != klass->parent) {
				/*fool mono_class_setup_parent*/
				klass->supertypes = NULL;
				mono_class_setup_parent (klass, parent);
			}
		} else {
			if (gklass->wastypebuilder)
				klass->wastypebuilder = TRUE;
			return FALSE;
		}
	}

	if (!klass->generic_class->need_sync)
		return TRUE;

	if (klass->method.count != gklass->method.count) {
		klass->method.count = gklass->method.count;
		klass->methods = (MonoMethod **)mono_image_alloc (klass->image, sizeof (MonoMethod*) * (klass->method.count + 1));

		for (i = 0; i < klass->method.count; i++) {
			klass->methods [i] = mono_class_inflate_generic_method_full_checked (
				gklass->methods [i], klass, mono_class_get_context (klass), error);
			mono_error_assert_ok (error);
		}
	}

	if (klass->interface_count && klass->interface_count != gklass->interface_count) {
		klass->interface_count = gklass->interface_count;
		klass->interfaces = (MonoClass **)mono_image_alloc (klass->image, sizeof (MonoClass*) * gklass->interface_count);
		klass->interfaces_packed = NULL; /*make setup_interface_offsets happy*/

		for (i = 0; i < gklass->interface_count; ++i) {
			MonoType *iface_type = mono_class_inflate_generic_type_checked (&gklass->interfaces [i]->byval_arg, mono_class_get_context (klass), error);
			return_val_if_nok (error, FALSE);

			klass->interfaces [i] = mono_class_from_mono_type (iface_type);
			mono_metadata_free_type (iface_type);

			if (!ensure_runtime_vtable (klass->interfaces [i], error))
				return FALSE;
		}
		klass->interfaces_inited = 1;
	}

	if (klass->field.count != gklass->field.count) {
		klass->field.count = gklass->field.count;
		klass->fields = image_g_new0 (klass->image, MonoClassField, klass->field.count);

		for (i = 0; i < klass->field.count; i++) {
			klass->fields [i] = gklass->fields [i];
			klass->fields [i].parent = klass;
			klass->fields [i].type = mono_class_inflate_generic_type_checked (gklass->fields [i].type, mono_class_get_context (klass), error);
			return_val_if_nok (error, FALSE);
		}
	}

	/*We can only finish with this klass once it's parent has as well*/
	if (gklass->wastypebuilder)
		klass->wastypebuilder = TRUE;
	return TRUE;
}

/**
 * ensure_generic_class_runtime_vtable:
 * @klass a generic class
 * @error set on error
 *
 * Ensures that the generic container of @klass has a vtable and
 * returns TRUE on success.  On error returns FALSE and sets @error.
 */
static gboolean
ensure_generic_class_runtime_vtable (MonoClass *klass, MonoError *error)
{
	MonoClass *gklass = klass->generic_class->container_class;

	mono_error_init (error);

	if (!ensure_runtime_vtable (gklass, error))
		return FALSE;

	return fix_partial_generic_class (klass, error);
}

/**
 * ensure_runtime_vtable:
 * @klass the class
 * @error set on error
 *
 * Ensures that @klass has a vtable and returns TRUE on success. On
 * error returns FALSE and sets @error.
 */
static gboolean
ensure_runtime_vtable (MonoClass *klass, MonoError *error)
{
	MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)mono_class_get_ref_info (klass);
	int i, num, j;

	mono_error_init (error);

	if (!image_is_dynamic (klass->image) || (!tb && !klass->generic_class) || klass->wastypebuilder)
		return TRUE;
	if (klass->parent)
		if (!ensure_runtime_vtable (klass->parent, error))
			return FALSE;

	if (tb) {
		num = tb->ctors? mono_array_length (tb->ctors): 0;
		num += tb->num_methods;
		klass->method.count = num;
		klass->methods = (MonoMethod **)mono_image_alloc (klass->image, sizeof (MonoMethod*) * num);
		num = tb->ctors? mono_array_length (tb->ctors): 0;
		for (i = 0; i < num; ++i) {
			MonoMethod *ctor = ctorbuilder_to_mono_method (klass, mono_array_get (tb->ctors, MonoReflectionCtorBuilder*, i), error);
			if (!ctor)
				return FALSE;
			klass->methods [i] = ctor;
		}
		num = tb->num_methods;
		j = i;
		for (i = 0; i < num; ++i) {
			MonoMethod *meth = methodbuilder_to_mono_method (klass, mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i), error);
			if (!meth)
				return FALSE;
			klass->methods [j++] = meth;
		}
	
		if (tb->interfaces) {
			klass->interface_count = mono_array_length (tb->interfaces);
			klass->interfaces = (MonoClass **)mono_image_alloc (klass->image, sizeof (MonoClass*) * klass->interface_count);
			for (i = 0; i < klass->interface_count; ++i) {
				MonoType *iface = mono_type_array_get_and_resolve (tb->interfaces, i, error);
				return_val_if_nok (error, FALSE);
				klass->interfaces [i] = mono_class_from_mono_type (iface);
				if (!ensure_runtime_vtable (klass->interfaces [i], error))
					return FALSE;
			}
			klass->interfaces_inited = 1;
		}
	} else if (klass->generic_class){
		if (!ensure_generic_class_runtime_vtable (klass, error)) {
			mono_class_set_failure (klass, MONO_EXCEPTION_TYPE_LOAD, NULL);
			return FALSE;
		}
	}

	if (klass->flags & TYPE_ATTRIBUTE_INTERFACE) {
		int slot_num = 0;
		for (i = 0; i < klass->method.count; ++i) {
			MonoMethod *im = klass->methods [i];
			if (!(im->flags & METHOD_ATTRIBUTE_STATIC))
				im->slot = slot_num++;
		}
		
		klass->interfaces_packed = NULL; /*make setup_interface_offsets happy*/
		mono_class_setup_interface_offsets (klass);
		mono_class_setup_interface_id (klass);
	}

	/*
	 * The generic vtable is needed even if image->run is not set since some
	 * runtime code like ves_icall_Type_GetMethodsByName depends on 
	 * method->slot being defined.
	 */

	/* 
	 * tb->methods could not be freed since it is used for determining 
	 * overrides during dynamic vtable construction.
	 */

	return TRUE;
}

static MonoMethod*
mono_reflection_method_get_handle (MonoObject *method, MonoError *error)
{
	mono_error_init (error);
	MonoClass *klass = mono_object_class (method);
	if (is_sr_mono_method (klass) || is_sr_mono_generic_method (klass)) {
		MonoReflectionMethod *sr_method = (MonoReflectionMethod*)method;
		return sr_method->method;
	}
	if (is_sre_method_builder (klass)) {
		MonoReflectionMethodBuilder *mb = (MonoReflectionMethodBuilder*)method;
		return mb->mhandle;
	}
	if (mono_is_sre_method_on_tb_inst (klass)) {
		MonoReflectionMethodOnTypeBuilderInst *m = (MonoReflectionMethodOnTypeBuilderInst*)method;
		MonoMethod *result;
		/*FIXME move this to a proper method and unify with resolve_object*/
		if (m->method_args) {
			result = mono_reflection_method_on_tb_inst_get_handle (m, error);
		} else {
			MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)m->inst, error);
			return_val_if_nok (error, NULL);
			MonoClass *inflated_klass = mono_class_from_mono_type (type);
			MonoMethod *mono_method;

			if (is_sre_method_builder (mono_object_class (m->mb)))
				mono_method = ((MonoReflectionMethodBuilder *)m->mb)->mhandle;
 			else if (is_sr_mono_method (mono_object_class (m->mb)))
				mono_method = ((MonoReflectionMethod *)m->mb)->method;
			else
				g_error ("resolve_object:: can't handle a MTBI with base_method of type %s", mono_type_get_full_name (mono_object_class (m->mb)));

			result = inflate_mono_method (inflated_klass, mono_method, (MonoObject*)m->mb);
		}
		return result;
	}

	g_error ("Can't handle methods of type %s:%s", klass->name_space, klass->name);
	return NULL;
}

void
mono_reflection_get_dynamic_overrides (MonoClass *klass, MonoMethod ***overrides, int *num_overrides, MonoError *error)
{
	MonoReflectionTypeBuilder *tb;
	int i, j, onum;
	MonoReflectionMethod *m;

	mono_error_init (error);
	*overrides = NULL;
	*num_overrides = 0;

	g_assert (image_is_dynamic (klass->image));

	if (!mono_class_get_ref_info (klass))
		return;

	g_assert (strcmp (((MonoObject*)mono_class_get_ref_info (klass))->vtable->klass->name, "TypeBuilder") == 0);

	tb = (MonoReflectionTypeBuilder*)mono_class_get_ref_info (klass);

	onum = 0;
	if (tb->methods) {
		for (i = 0; i < tb->num_methods; ++i) {
			MonoReflectionMethodBuilder *mb = 
				mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i);
			if (mb->override_methods)
				onum += mono_array_length (mb->override_methods);
		}
	}

	if (onum) {
		*overrides = g_new0 (MonoMethod*, onum * 2);

		onum = 0;
		for (i = 0; i < tb->num_methods; ++i) {
			MonoReflectionMethodBuilder *mb = 
				mono_array_get (tb->methods, MonoReflectionMethodBuilder*, i);
			if (mb->override_methods) {
				for (j = 0; j < mono_array_length (mb->override_methods); ++j) {
					m = mono_array_get (mb->override_methods, MonoReflectionMethod*, j);

					(*overrides) [onum * 2] = mono_reflection_method_get_handle ((MonoObject*)m, error);
					return_if_nok (error);
					(*overrides) [onum * 2 + 1] = mb->mhandle;

					g_assert (mb->mhandle);

					onum ++;
				}
			}
		}
	}

	*num_overrides = onum;
}

static void
typebuilder_setup_fields (MonoClass *klass, MonoError *error)
{
	MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)mono_class_get_ref_info (klass);
	MonoReflectionFieldBuilder *fb;
	MonoClassField *field;
	MonoImage *image = klass->image;
	const char *p, *p2;
	int i;
	guint32 len, idx, real_size = 0;

	klass->field.count = tb->num_fields;
	klass->field.first = 0;

	mono_error_init (error);

	if (tb->class_size) {
		if ((tb->packing_size & 0xffffff00) != 0) {
			char *err_msg = g_strdup_printf ("Could not load struct '%s' with packing size %d >= 256", klass->name, tb->packing_size);
			mono_class_set_failure (klass, MONO_EXCEPTION_TYPE_LOAD, err_msg);
			return;
		}
		klass->packing_size = tb->packing_size;
		real_size = klass->instance_size + tb->class_size;
	}

	if (!klass->field.count) {
		klass->instance_size = MAX (klass->instance_size, real_size);
		return;
	}
	
	klass->fields = image_g_new0 (image, MonoClassField, klass->field.count);
	mono_class_alloc_ext (klass);
	klass->ext->field_def_values = image_g_new0 (image, MonoFieldDefaultValue, klass->field.count);
	/*
	This is, guess what, a hack.
	The issue is that the runtime doesn't know how to setup the fields of a typebuider and crash.
	On the static path no field class is resolved, only types are built. This is the right thing to do
	but we suck.
	Setting size_inited is harmless because we're doing the same job as mono_class_setup_fields anyway.
	*/
	klass->size_inited = 1;

	for (i = 0; i < klass->field.count; ++i) {
		MonoArray *rva_data;
		fb = (MonoReflectionFieldBuilder *)mono_array_get (tb->fields, gpointer, i);
		field = &klass->fields [i];
		field->name = mono_string_to_utf8_image (image, fb->name, error);
		if (!mono_error_ok (error))
			return;
		if (fb->attrs) {
			MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)fb->type, error);
			return_if_nok (error);
			field->type = mono_metadata_type_dup (klass->image, type);
			field->type->attrs = fb->attrs;
		} else {
			field->type = mono_reflection_type_get_handle ((MonoReflectionType*)fb->type, error);
			return_if_nok (error);
		}

		if ((fb->attrs & FIELD_ATTRIBUTE_HAS_FIELD_RVA) && (rva_data = fb->rva_data)) {
			char *base = mono_array_addr (rva_data, char, 0);
			size_t size = mono_array_length (rva_data);
			char *data = (char *)mono_image_alloc (klass->image, size);
			memcpy (data, base, size);
			klass->ext->field_def_values [i].data = data;
		}
		if (fb->offset != -1)
			field->offset = fb->offset;
		field->parent = klass;
		fb->handle = field;
		mono_save_custom_attrs (klass->image, field, fb->cattrs);

		if (klass->enumtype && !(field->type->attrs & FIELD_ATTRIBUTE_STATIC)) {
			klass->cast_class = klass->element_class = mono_class_from_mono_type (field->type);
		}
		if (fb->def_value) {
			MonoDynamicImage *assembly = (MonoDynamicImage*)klass->image;
			field->type->attrs |= FIELD_ATTRIBUTE_HAS_DEFAULT;
			idx = mono_dynimage_encode_constant (assembly, fb->def_value, &klass->ext->field_def_values [i].def_type);
			/* Copy the data from the blob since it might get realloc-ed */
			p = assembly->blob.data + idx;
			len = mono_metadata_decode_blob_size (p, &p2);
			len += p2 - p;
			klass->ext->field_def_values [i].data = (const char *)mono_image_alloc (image, len);
			memcpy ((gpointer)klass->ext->field_def_values [i].data, p, len);
		}
	}

	klass->instance_size = MAX (klass->instance_size, real_size);
	mono_class_layout_fields (klass, klass->instance_size);
}

static void
typebuilder_setup_properties (MonoClass *klass, MonoError *error)
{
	MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)mono_class_get_ref_info (klass);
	MonoReflectionPropertyBuilder *pb;
	MonoImage *image = klass->image;
	MonoProperty *properties;
	int i;

	mono_error_init (error);

	if (!klass->ext)
		klass->ext = image_g_new0 (image, MonoClassExt, 1);

	klass->ext->property.count = tb->properties ? mono_array_length (tb->properties) : 0;
	klass->ext->property.first = 0;

	properties = image_g_new0 (image, MonoProperty, klass->ext->property.count);
	klass->ext->properties = properties;
	for (i = 0; i < klass->ext->property.count; ++i) {
		pb = mono_array_get (tb->properties, MonoReflectionPropertyBuilder*, i);
		properties [i].parent = klass;
		properties [i].attrs = pb->attrs;
		properties [i].name = mono_string_to_utf8_image (image, pb->name, error);
		if (!mono_error_ok (error))
			return;
		if (pb->get_method)
			properties [i].get = pb->get_method->mhandle;
		if (pb->set_method)
			properties [i].set = pb->set_method->mhandle;

		mono_save_custom_attrs (klass->image, &properties [i], pb->cattrs);
		if (pb->def_value) {
			guint32 len, idx;
			const char *p, *p2;
			MonoDynamicImage *assembly = (MonoDynamicImage*)klass->image;
			if (!klass->ext->prop_def_values)
				klass->ext->prop_def_values = image_g_new0 (image, MonoFieldDefaultValue, klass->ext->property.count);
			properties [i].attrs |= PROPERTY_ATTRIBUTE_HAS_DEFAULT;
			idx = mono_dynimage_encode_constant (assembly, pb->def_value, &klass->ext->prop_def_values [i].def_type);
			/* Copy the data from the blob since it might get realloc-ed */
			p = assembly->blob.data + idx;
			len = mono_metadata_decode_blob_size (p, &p2);
			len += p2 - p;
			klass->ext->prop_def_values [i].data = (const char *)mono_image_alloc (image, len);
			memcpy ((gpointer)klass->ext->prop_def_values [i].data, p, len);
		}
	}
}

static MonoReflectionEvent *
reflection_event_builder_get_event_info (MonoReflectionTypeBuilder *tb, MonoReflectionEventBuilder *eb, MonoError *error)
{
	mono_error_init (error);

	MonoEvent *event = g_new0 (MonoEvent, 1);
	MonoClass *klass;

	MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)tb, error);
	if (!is_ok (error)) {
		g_free (event);
		return NULL;
	}
	klass = mono_class_from_mono_type (type);

	event->parent = klass;
	event->attrs = eb->attrs;
	event->name = mono_string_to_utf8_checked (eb->name, error);
	if (!is_ok (error)) {
		g_free (event);
		return NULL;
	}
	if (eb->add_method)
		event->add = eb->add_method->mhandle;
	if (eb->remove_method)
		event->remove = eb->remove_method->mhandle;
	if (eb->raise_method)
		event->raise = eb->raise_method->mhandle;

#ifndef MONO_SMALL_CONFIG
	if (eb->other_methods) {
		int j;
		event->other = g_new0 (MonoMethod*, mono_array_length (eb->other_methods) + 1);
		for (j = 0; j < mono_array_length (eb->other_methods); ++j) {
			MonoReflectionMethodBuilder *mb = 
				mono_array_get (eb->other_methods,
						MonoReflectionMethodBuilder*, j);
			event->other [j] = mb->mhandle;
		}
	}
#endif

	MonoReflectionEvent *ev_obj = mono_event_get_object_checked (mono_object_domain (tb), klass, event, error);
	if (!is_ok (error)) {
#ifndef MONO_SMALL_CONFIG
		g_free (event->other);
#endif
		g_free (event);
		return NULL;
	}
	return ev_obj;
}

MonoReflectionEvent *
ves_icall_TypeBuilder_get_event_info (MonoReflectionTypeBuilder *tb, MonoReflectionEventBuilder *eb)
{
	MonoError error;
	MonoReflectionEvent *result = reflection_event_builder_get_event_info (tb, eb, &error);
	mono_error_set_pending_exception (&error);
	return result;
}

static void
typebuilder_setup_events (MonoClass *klass, MonoError *error)
{
	MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)mono_class_get_ref_info (klass);
	MonoReflectionEventBuilder *eb;
	MonoImage *image = klass->image;
	MonoEvent *events;
	int i;

	mono_error_init (error);

	if (!klass->ext)
		klass->ext = image_g_new0 (image, MonoClassExt, 1);

	klass->ext->event.count = tb->events ? mono_array_length (tb->events) : 0;
	klass->ext->event.first = 0;

	events = image_g_new0 (image, MonoEvent, klass->ext->event.count);
	klass->ext->events = events;
	for (i = 0; i < klass->ext->event.count; ++i) {
		eb = mono_array_get (tb->events, MonoReflectionEventBuilder*, i);
		events [i].parent = klass;
		events [i].attrs = eb->attrs;
		events [i].name = mono_string_to_utf8_image (image, eb->name, error);
		if (!mono_error_ok (error))
			return;
		if (eb->add_method)
			events [i].add = eb->add_method->mhandle;
		if (eb->remove_method)
			events [i].remove = eb->remove_method->mhandle;
		if (eb->raise_method)
			events [i].raise = eb->raise_method->mhandle;

#ifndef MONO_SMALL_CONFIG
		if (eb->other_methods) {
			int j;
			events [i].other = image_g_new0 (image, MonoMethod*, mono_array_length (eb->other_methods) + 1);
			for (j = 0; j < mono_array_length (eb->other_methods); ++j) {
				MonoReflectionMethodBuilder *mb = 
					mono_array_get (eb->other_methods,
									MonoReflectionMethodBuilder*, j);
				events [i].other [j] = mb->mhandle;
			}
		}
#endif
		mono_save_custom_attrs (klass->image, &events [i], eb->cattrs);
	}
}

struct remove_instantiations_user_data
{
	MonoClass *klass;
	MonoError *error;
};

static gboolean
remove_instantiations_of_and_ensure_contents (gpointer key,
						  gpointer value,
						  gpointer user_data)
{
	struct remove_instantiations_user_data *data = (struct remove_instantiations_user_data*)user_data;
	MonoType *type = (MonoType*)key;
	MonoClass *klass = data->klass;
	gboolean already_failed = !is_ok (data->error);
	MonoError lerror;
	MonoError *error = already_failed ? &lerror : data->error;

	if ((type->type == MONO_TYPE_GENERICINST) && (type->data.generic_class->container_class == klass)) {
		MonoClass *inst_klass = mono_class_from_mono_type (type);
		//Ensure it's safe to use it.
		if (!fix_partial_generic_class (inst_klass, error)) {
			mono_class_set_failure (inst_klass, MONO_EXCEPTION_TYPE_LOAD, NULL);
			// Marked the class with failure, but since some other instantiation already failed,
			// just report that one, and swallow the error from this one.
			if (already_failed)
				mono_error_cleanup (error);
		}
		return TRUE;
	} else
		return FALSE;
}

MonoReflectionType*
ves_icall_TypeBuilder_create_runtime_class (MonoReflectionTypeBuilder *tb)
{
	MonoError error;
	MonoClass *klass;
	MonoDomain* domain;
	MonoReflectionType* res;
	int i;

	mono_error_init (&error);

	domain = mono_object_domain (tb);
	klass = mono_class_from_mono_type (tb->type.type);

	mono_save_custom_attrs (klass->image, klass, tb->cattrs);

	/* 
	 * we need to lock the domain because the lock will be taken inside
	 * So, we need to keep the locking order correct.
	 */
	mono_loader_lock ();
	mono_domain_lock (domain);
	if (klass->wastypebuilder) {
		mono_domain_unlock (domain);
		mono_loader_unlock ();

		res = mono_type_get_object_checked (mono_object_domain (tb), &klass->byval_arg, &error);
		mono_error_set_pending_exception (&error);

		return res;
	}
	/*
	 * Fields to set in klass:
	 * the various flags: delegate/unicode/contextbound etc.
	 */
	klass->flags = tb->attrs;
	klass->has_cctor = 1;

	mono_class_setup_parent (klass, klass->parent);
	/* fool mono_class_setup_supertypes */
	klass->supertypes = NULL;
	mono_class_setup_supertypes (klass);
	mono_class_setup_mono_type (klass);

#if 0
	if (!((MonoDynamicImage*)klass->image)->run) {
		if (klass->generic_container) {
			/* FIXME: The code below can't handle generic classes */
			klass->wastypebuilder = TRUE;
			mono_loader_unlock ();
			mono_domain_unlock (domain);

			res = mono_type_get_object_checked (mono_object_domain (tb), &klass->byval_arg, &error);
			mono_error_set_pending_exception (&error);

			return res;
		}
	}
#endif

	/* enums are done right away */
	if (!klass->enumtype)
		if (!ensure_runtime_vtable (klass, &error))
			goto failure;

	if (tb->subtypes) {
		for (i = 0; i < mono_array_length (tb->subtypes); ++i) {
			MonoReflectionTypeBuilder *subtb = mono_array_get (tb->subtypes, MonoReflectionTypeBuilder*, i);
			mono_class_alloc_ext (klass);
			MonoType *subtype = mono_reflection_type_get_handle ((MonoReflectionType*)subtb, &error);
			if (!is_ok (&error)) goto failure;
			klass->ext->nested_classes = g_list_prepend_image (klass->image, klass->ext->nested_classes, mono_class_from_mono_type (subtype));
		}
	}

	klass->nested_classes_inited = TRUE;

	/* fields and object layout */
	if (klass->parent) {
		if (!klass->parent->size_inited)
			mono_class_init (klass->parent);
		klass->instance_size = klass->parent->instance_size;
		klass->sizes.class_size = 0;
		klass->min_align = klass->parent->min_align;
		/* if the type has no fields we won't call the field_setup
		 * routine which sets up klass->has_references.
		 */
		klass->has_references |= klass->parent->has_references;
	} else {
		klass->instance_size = sizeof (MonoObject);
		klass->min_align = 1;
	}

	/* FIXME: handle packing_size and instance_size */
	typebuilder_setup_fields (klass, &error);
	if (!mono_error_ok (&error))
		goto failure;
	typebuilder_setup_properties (klass, &error);
	if (!mono_error_ok (&error))
		goto failure;

	typebuilder_setup_events (klass, &error);
	if (!mono_error_ok (&error))
		goto failure;

	klass->wastypebuilder = TRUE;

	/* 
	 * If we are a generic TypeBuilder, there might be instantiations in the type cache
	 * which have type System.Reflection.MonoGenericClass, but after the type is created, 
	 * we want to return normal System.MonoType objects, so clear these out from the cache.
	 *
	 * Together with this we must ensure the contents of all instances to match the created type.
	 */
	if (domain->type_hash && klass->generic_container) {
		struct remove_instantiations_user_data data;
		data.klass = klass;
		data.error = &error;
		mono_error_assert_ok (&error);
		mono_g_hash_table_foreach_remove (domain->type_hash, remove_instantiations_of_and_ensure_contents, &data);
		if (!is_ok (&error))
			goto failure;
	}

	mono_domain_unlock (domain);
	mono_loader_unlock ();

	if (klass->enumtype && !mono_class_is_valid_enum (klass)) {
		mono_class_set_failure (klass, MONO_EXCEPTION_TYPE_LOAD, NULL);
		mono_error_set_type_load_class (&error, klass, "Not a valid enumeration");
		goto failure_unlocked;
	}

	res = mono_type_get_object_checked (mono_object_domain (tb), &klass->byval_arg, &error);
	if (!is_ok (&error))
		goto failure_unlocked;

	g_assert (res != (MonoReflectionType*)tb);

	return res;

failure:
	mono_class_set_failure (klass, MONO_EXCEPTION_TYPE_LOAD, NULL);
	klass->wastypebuilder = TRUE;
	mono_domain_unlock (domain);
	mono_loader_unlock ();
failure_unlocked:
	mono_error_set_pending_exception (&error);
	return NULL;
}

static gboolean
reflection_initialize_generic_parameter (MonoReflectionGenericParam *gparam, MonoError *error)
{
	MonoGenericParamFull *param;
	MonoImage *image;
	MonoClass *pklass;

	mono_error_init (error);

	image = &gparam->tbuilder->module->dynamic_image->image;

	param = mono_image_new0 (image, MonoGenericParamFull, 1);

	param->info.name = mono_string_to_utf8_image (image, gparam->name, error);
	mono_error_assert_ok (error);
	param->param.num = gparam->index;

	if (gparam->mbuilder) {
		if (!gparam->mbuilder->generic_container) {
			MonoType *tb = mono_reflection_type_get_handle ((MonoReflectionType*)gparam->mbuilder->type, error);
			return_val_if_nok (error, FALSE);

			MonoClass *klass = mono_class_from_mono_type (tb);
			gparam->mbuilder->generic_container = (MonoGenericContainer *)mono_image_alloc0 (klass->image, sizeof (MonoGenericContainer));
			gparam->mbuilder->generic_container->is_method = TRUE;
			/* 
			 * Cannot set owner.method, since the MonoMethod is not created yet.
			 * Set the image field instead, so type_in_image () works.
			 */
			gparam->mbuilder->generic_container->is_anonymous = TRUE;
			gparam->mbuilder->generic_container->owner.image = klass->image;
		}
		param->param.owner = gparam->mbuilder->generic_container;
	} else if (gparam->tbuilder) {
		if (!gparam->tbuilder->generic_container) {
			MonoType *tb = mono_reflection_type_get_handle ((MonoReflectionType*)gparam->tbuilder, error);
			return_val_if_nok (error, FALSE);
			MonoClass *klass = mono_class_from_mono_type (tb);
			gparam->tbuilder->generic_container = (MonoGenericContainer *)mono_image_alloc0 (klass->image, sizeof (MonoGenericContainer));
			gparam->tbuilder->generic_container->owner.klass = klass;
		}
		param->param.owner = gparam->tbuilder->generic_container;
	}

	pklass = mono_class_from_generic_parameter_internal ((MonoGenericParam *) param);

	gparam->type.type = &pklass->byval_arg;

	mono_class_set_ref_info (pklass, gparam);
	mono_image_append_class_to_reflection_info_set (pklass);

	return TRUE;
}

void
ves_icall_GenericTypeParameterBuilder_initialize_generic_parameter (MonoReflectionGenericParam *gparam)
{
	MonoError error;
	(void) reflection_initialize_generic_parameter (gparam, &error);
	mono_error_set_pending_exception (&error);
}


typedef struct {
	MonoMethod *handle;
	MonoDomain *domain;
} DynamicMethodReleaseData;

/*
 * The runtime automatically clean up those after finalization.
*/	
static MonoReferenceQueue *dynamic_method_queue;

static void
free_dynamic_method (void *dynamic_method)
{
	DynamicMethodReleaseData *data = (DynamicMethodReleaseData *)dynamic_method;
	MonoDomain *domain = data->domain;
	MonoMethod *method = data->handle;
	guint32 dis_link;

	mono_domain_lock (domain);
	dis_link = (guint32)(size_t)g_hash_table_lookup (domain->method_to_dyn_method, method);
	g_hash_table_remove (domain->method_to_dyn_method, method);
	mono_domain_unlock (domain);
	g_assert (dis_link);
	mono_gchandle_free (dis_link);

	mono_runtime_free_method (domain, method);
	g_free (data);
}

static gboolean
reflection_create_dynamic_method (MonoReflectionDynamicMethod *mb, MonoError *error)
{
	MonoReferenceQueue *queue;
	MonoMethod *handle;
	DynamicMethodReleaseData *release_data;
	ReflectionMethodBuilder rmb;
	MonoMethodSignature *sig;
	MonoClass *klass;
	MonoDomain *domain;
	GSList *l;
	int i;

	mono_error_init (error);

	if (mono_runtime_is_shutting_down ()) {
		mono_error_set_generic_error (error, "System", "InvalidOperationException", "");
		return FALSE;
	}

	if (!(queue = dynamic_method_queue)) {
		mono_loader_lock ();
		if (!(queue = dynamic_method_queue))
			queue = dynamic_method_queue = mono_gc_reference_queue_new (free_dynamic_method);
		mono_loader_unlock ();
	}

	sig = dynamic_method_to_signature (mb, error);
	return_val_if_nok (error, FALSE);

	reflection_methodbuilder_from_dynamic_method (&rmb, mb);

	/*
	 * Resolve references.
	 */
	/* 
	 * Every second entry in the refs array is reserved for storing handle_class,
	 * which is needed by the ldtoken implementation in the JIT.
	 */
	rmb.nrefs = mb->nrefs;
	rmb.refs = g_new0 (gpointer, mb->nrefs + 1);
	for (i = 0; i < mb->nrefs; i += 2) {
		MonoClass *handle_class;
		gpointer ref;
		MonoObject *obj = mono_array_get (mb->refs, MonoObject*, i);

		if (strcmp (obj->vtable->klass->name, "DynamicMethod") == 0) {
			MonoReflectionDynamicMethod *method = (MonoReflectionDynamicMethod*)obj;
			/*
			 * The referenced DynamicMethod should already be created by the managed
			 * code, except in the case of circular references. In that case, we store
			 * method in the refs array, and fix it up later when the referenced 
			 * DynamicMethod is created.
			 */
			if (method->mhandle) {
				ref = method->mhandle;
			} else {
				/* FIXME: GC object stored in unmanaged memory */
				ref = method;

				/* FIXME: GC object stored in unmanaged memory */
				method->referenced_by = g_slist_append (method->referenced_by, mb);
			}
			handle_class = mono_defaults.methodhandle_class;
		} else {
			MonoException *ex = NULL;
			ref = mono_reflection_resolve_object (mb->module->image, obj, &handle_class, NULL, error);
			if (!is_ok  (error)) {
				g_free (rmb.refs);
				return FALSE;
			}
			if (!ref)
				ex = mono_get_exception_type_load (NULL, NULL);
			else if (mono_security_core_clr_enabled ())
				ex = mono_security_core_clr_ensure_dynamic_method_resolved_object (ref, handle_class);

			if (ex) {
				g_free (rmb.refs);
				mono_error_set_exception_instance (error, ex);
				return FALSE;
			}
		}

		rmb.refs [i] = ref; /* FIXME: GC object stored in unmanaged memory (change also resolve_object() signature) */
		rmb.refs [i + 1] = handle_class;
	}		

	if (mb->owner) {
		MonoType *owner_type = mono_reflection_type_get_handle ((MonoReflectionType*)mb->owner, error);
		if (!is_ok (error)) {
			g_free (rmb.refs);
			return FALSE;
		}
		klass = mono_class_from_mono_type (owner_type);
	} else {
		klass = mono_defaults.object_class;
	}

	mb->mhandle = handle = reflection_methodbuilder_to_mono_method (klass, &rmb, sig, error);
	g_free (rmb.refs);
	return_val_if_nok (error, FALSE);

	release_data = g_new (DynamicMethodReleaseData, 1);
	release_data->handle = handle;
	release_data->domain = mono_object_get_domain ((MonoObject*)mb);
	if (!mono_gc_reference_queue_add (queue, (MonoObject*)mb, release_data))
		g_free (release_data);

	/* Fix up refs entries pointing at us */
	for (l = mb->referenced_by; l; l = l->next) {
		MonoReflectionDynamicMethod *method = (MonoReflectionDynamicMethod*)l->data;
		MonoMethodWrapper *wrapper = (MonoMethodWrapper*)method->mhandle;
		gpointer *data;
		
		g_assert (method->mhandle);

		data = (gpointer*)wrapper->method_data;
		for (i = 0; i < GPOINTER_TO_UINT (data [0]); i += 2) {
			if ((data [i + 1] == mb) && (data [i + 1 + 1] == mono_defaults.methodhandle_class))
				data [i + 1] = mb->mhandle;
		}
	}
	g_slist_free (mb->referenced_by);

	/* ilgen is no longer needed */
	mb->ilgen = NULL;

	domain = mono_domain_get ();
	mono_domain_lock (domain);
	if (!domain->method_to_dyn_method)
		domain->method_to_dyn_method = g_hash_table_new (NULL, NULL);
	g_hash_table_insert (domain->method_to_dyn_method, handle, (gpointer)(size_t)mono_gchandle_new_weakref ((MonoObject *)mb, TRUE));
	mono_domain_unlock (domain);

	return TRUE;
}

void
ves_icall_DynamicMethod_create_dynamic_method (MonoReflectionDynamicMethod *mb)
{
	MonoError error;
	(void) reflection_create_dynamic_method (mb, &error);
	mono_error_set_pending_exception (&error);
}

#endif /* DISABLE_REFLECTION_EMIT */

MonoMethodSignature *
mono_reflection_lookup_signature (MonoImage *image, MonoMethod *method, guint32 token, MonoError *error)
{
	MonoMethodSignature *sig;
	g_assert (image_is_dynamic (image));

	mono_error_init (error);

	sig = (MonoMethodSignature *)g_hash_table_lookup (((MonoDynamicImage*)image)->vararg_aux_hash, GUINT_TO_POINTER (token));
	if (sig)
		return sig;

	return mono_method_signature_checked (method, error);
}

#ifndef DISABLE_REFLECTION_EMIT

/*
 * ensure_complete_type:
 *
 *   Ensure that KLASS is completed if it is a dynamic type, or references
 * dynamic types.
 */
static void
ensure_complete_type (MonoClass *klass, MonoError *error)
{
	mono_error_init (error);

	if (image_is_dynamic (klass->image) && !klass->wastypebuilder && mono_class_get_ref_info (klass)) {
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder *)mono_class_get_ref_info (klass);

		mono_domain_try_type_resolve_checked (mono_domain_get (), NULL, (MonoObject*)tb, error);
		return_if_nok (error);

		// Asserting here could break a lot of code
		//g_assert (klass->wastypebuilder);
	}

	if (klass->generic_class) {
		MonoGenericInst *inst = klass->generic_class->context.class_inst;
		int i;

		for (i = 0; i < inst->type_argc; ++i) {
			ensure_complete_type (mono_class_from_mono_type (inst->type_argv [i]), error);
			return_if_nok (error);
		}
	}
}

gpointer
mono_reflection_resolve_object (MonoImage *image, MonoObject *obj, MonoClass **handle_class, MonoGenericContext *context, MonoError *error)
{
	gpointer result = NULL;

	mono_error_init (error);

	if (strcmp (obj->vtable->klass->name, "String") == 0) {
		result = mono_string_intern_checked ((MonoString*)obj, error);
		return_val_if_nok (error, NULL);
		*handle_class = mono_defaults.string_class;
		g_assert (result);
	} else if (strcmp (obj->vtable->klass->name, "RuntimeType") == 0) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)obj, error);
		return_val_if_nok (error, NULL);
		MonoClass *mc = mono_class_from_mono_type (type);
		if (!mono_class_init (mc)) {
			mono_error_set_for_class_failure (error, mc);
			return NULL;
		}

		if (context) {
			MonoType *inflated = mono_class_inflate_generic_type_checked (type, context, error);
			return_val_if_nok (error, NULL);

			result = mono_class_from_mono_type (inflated);
			mono_metadata_free_type (inflated);
		} else {
			result = mono_class_from_mono_type (type);
		}
		*handle_class = mono_defaults.typehandle_class;
		g_assert (result);
	} else if (strcmp (obj->vtable->klass->name, "MonoMethod") == 0 ||
		   strcmp (obj->vtable->klass->name, "MonoCMethod") == 0 ||
		   strcmp (obj->vtable->klass->name, "MonoGenericCMethod") == 0 ||
		   strcmp (obj->vtable->klass->name, "MonoGenericMethod") == 0) {
		result = ((MonoReflectionMethod*)obj)->method;
		if (context) {
			result = mono_class_inflate_generic_method_checked ((MonoMethod *)result, context, error);
			mono_error_assert_ok (error);
		}
		*handle_class = mono_defaults.methodhandle_class;
		g_assert (result);
	} else if (strcmp (obj->vtable->klass->name, "MethodBuilder") == 0) {
		MonoReflectionMethodBuilder *mb = (MonoReflectionMethodBuilder*)obj;
		result = mb->mhandle;
		if (!result) {
			/* Type is not yet created */
			MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)mb->type;

			mono_domain_try_type_resolve_checked (mono_domain_get (), NULL, (MonoObject*)tb, error);
			return_val_if_nok (error, NULL);

			/*
			 * Hopefully this has been filled in by calling CreateType() on the
			 * TypeBuilder.
			 */
			/*
			 * TODO: This won't work if the application finishes another 
			 * TypeBuilder instance instead of this one.
			 */
			result = mb->mhandle;
		}
		if (context) {
			result = mono_class_inflate_generic_method_checked ((MonoMethod *)result, context, error);
			mono_error_assert_ok (error);
		}
		*handle_class = mono_defaults.methodhandle_class;
	} else if (strcmp (obj->vtable->klass->name, "ConstructorBuilder") == 0) {
		MonoReflectionCtorBuilder *cb = (MonoReflectionCtorBuilder*)obj;

		result = cb->mhandle;
		if (!result) {
			MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)cb->type;

			mono_domain_try_type_resolve_checked (mono_domain_get (), NULL, (MonoObject*)tb, error);
			return_val_if_nok (error, NULL);
			result = cb->mhandle;
		}
		if (context) {
			result = mono_class_inflate_generic_method_checked ((MonoMethod *)result, context, error);
			mono_error_assert_ok (error);
		}
		*handle_class = mono_defaults.methodhandle_class;
	} else if (strcmp (obj->vtable->klass->name, "MonoField") == 0) {
		MonoClassField *field = ((MonoReflectionField*)obj)->field;

		ensure_complete_type (field->parent, error);
		return_val_if_nok (error, NULL);

		if (context) {
			MonoType *inflated = mono_class_inflate_generic_type_checked (&field->parent->byval_arg, context, error);
			return_val_if_nok (error, NULL);

			MonoClass *klass = mono_class_from_mono_type (inflated);
			MonoClassField *inflated_field;
			gpointer iter = NULL;
			mono_metadata_free_type (inflated);
			while ((inflated_field = mono_class_get_fields (klass, &iter))) {
				if (!strcmp (field->name, inflated_field->name))
					break;
			}
			g_assert (inflated_field && !strcmp (field->name, inflated_field->name));
			result = inflated_field;
		} else {
			result = field;
		}
		*handle_class = mono_defaults.fieldhandle_class;
		g_assert (result);
	} else if (strcmp (obj->vtable->klass->name, "FieldBuilder") == 0) {
		MonoReflectionFieldBuilder *fb = (MonoReflectionFieldBuilder*)obj;
		result = fb->handle;

		if (!result) {
			MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)fb->typeb;

			mono_domain_try_type_resolve_checked (mono_domain_get (), NULL, (MonoObject*)tb, error);
			return_val_if_nok (error, NULL);
			result = fb->handle;
		}

		if (fb->handle && fb->handle->parent->generic_container) {
			MonoClass *klass = fb->handle->parent;
			MonoType *type = mono_class_inflate_generic_type_checked (&klass->byval_arg, context, error);
			return_val_if_nok (error, NULL);

			MonoClass *inflated = mono_class_from_mono_type (type);

			result = mono_class_get_field_from_name (inflated, mono_field_get_name (fb->handle));
			g_assert (result);
			mono_metadata_free_type (type);
		}
		*handle_class = mono_defaults.fieldhandle_class;
	} else if (strcmp (obj->vtable->klass->name, "TypeBuilder") == 0) {
		MonoReflectionTypeBuilder *tb = (MonoReflectionTypeBuilder*)obj;
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)tb, error);
		return_val_if_nok (error, NULL);
		MonoClass *klass;

		klass = type->data.klass;
		if (klass->wastypebuilder) {
			/* Already created */
			result = klass;
		}
		else {
			mono_domain_try_type_resolve_checked (mono_domain_get (), NULL, (MonoObject*)tb, error);
			return_val_if_nok (error, NULL);
			result = type->data.klass;
			g_assert (result);
		}
		*handle_class = mono_defaults.typehandle_class;
	} else if (strcmp (obj->vtable->klass->name, "SignatureHelper") == 0) {
		MonoReflectionSigHelper *helper = (MonoReflectionSigHelper*)obj;
		MonoMethodSignature *sig;
		int nargs, i;

		if (helper->arguments)
			nargs = mono_array_length (helper->arguments);
		else
			nargs = 0;

		sig = mono_metadata_signature_alloc (image, nargs);
		sig->explicit_this = helper->call_conv & 64 ? 1 : 0;
		sig->hasthis = helper->call_conv & 32 ? 1 : 0;

		if (helper->unmanaged_call_conv) { /* unmanaged */
			sig->call_convention = helper->unmanaged_call_conv - 1;
			sig->pinvoke = TRUE;
		} else if (helper->call_conv & 0x02) {
			sig->call_convention = MONO_CALL_VARARG;
		} else {
			sig->call_convention = MONO_CALL_DEFAULT;
		}

		sig->param_count = nargs;
		/* TODO: Copy type ? */
		sig->ret = helper->return_type->type;
		for (i = 0; i < nargs; ++i) {
			sig->params [i] = mono_type_array_get_and_resolve (helper->arguments, i, error);
			if (!is_ok (error)) {
				image_g_free (image, sig);
				return NULL;
			}
		}

		result = sig;
		*handle_class = NULL;
	} else if (strcmp (obj->vtable->klass->name, "DynamicMethod") == 0) {
		MonoReflectionDynamicMethod *method = (MonoReflectionDynamicMethod*)obj;
		/* Already created by the managed code */
		g_assert (method->mhandle);
		result = method->mhandle;
		*handle_class = mono_defaults.methodhandle_class;
	} else if (strcmp (obj->vtable->klass->name, "GenericTypeParameterBuilder") == 0) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)obj, error);
		return_val_if_nok (error, NULL);
		type = mono_class_inflate_generic_type_checked (type, context, error);
		return_val_if_nok (error, NULL);

		result = mono_class_from_mono_type (type);
		*handle_class = mono_defaults.typehandle_class;
		g_assert (result);
		mono_metadata_free_type (type);
	} else if (strcmp (obj->vtable->klass->name, "MonoGenericClass") == 0) {
		MonoType *type = mono_reflection_type_get_handle ((MonoReflectionType*)obj, error);
		return_val_if_nok (error, NULL);
		type = mono_class_inflate_generic_type_checked (type, context, error);
		return_val_if_nok (error, NULL);

		result = mono_class_from_mono_type (type);
		*handle_class = mono_defaults.typehandle_class;
		g_assert (result);
		mono_metadata_free_type (type);
	} else if (strcmp (obj->vtable->klass->name, "FieldOnTypeBuilderInst") == 0) {
		MonoReflectionFieldOnTypeBuilderInst *f = (MonoReflectionFieldOnTypeBuilderInst*)obj;
		MonoClass *inflated;
		MonoType *type;
		MonoClassField *field;

		if (is_sre_field_builder (mono_object_class (f->fb)))
			field = ((MonoReflectionFieldBuilder*)f->fb)->handle;
		else if (is_sr_mono_field (mono_object_class (f->fb)))
			field = ((MonoReflectionField*)f->fb)->field;
		else
			g_error ("resolve_object:: can't handle a FTBI with base_method of type %s", mono_type_get_full_name (mono_object_class (f->fb)));

		MonoType *finst = mono_reflection_type_get_handle ((MonoReflectionType*)f->inst, error);
		return_val_if_nok (error, NULL);
		type = mono_class_inflate_generic_type_checked (finst, context, error);
		return_val_if_nok (error, NULL);

		inflated = mono_class_from_mono_type (type);

		result = field = mono_class_get_field_from_name (inflated, mono_field_get_name (field));
		ensure_complete_type (field->parent, error);
		if (!is_ok (error)) {
			mono_metadata_free_type (type);
			return NULL;
		}

		g_assert (result);
		mono_metadata_free_type (type);
		*handle_class = mono_defaults.fieldhandle_class;
	} else if (strcmp (obj->vtable->klass->name, "ConstructorOnTypeBuilderInst") == 0) {
		MonoReflectionCtorOnTypeBuilderInst *c = (MonoReflectionCtorOnTypeBuilderInst*)obj;
		MonoType *cinst = mono_reflection_type_get_handle ((MonoReflectionType*)c->inst, error);
		return_val_if_nok (error, NULL);
		MonoType *type = mono_class_inflate_generic_type_checked (cinst, context, error);
		return_val_if_nok (error, NULL);

		MonoClass *inflated_klass = mono_class_from_mono_type (type);
		MonoMethod *method;

		if (mono_is_sre_ctor_builder (mono_object_class (c->cb)))
			method = ((MonoReflectionCtorBuilder *)c->cb)->mhandle;
		else if (mono_is_sr_mono_cmethod (mono_object_class (c->cb)))
			method = ((MonoReflectionMethod *)c->cb)->method;
		else
			g_error ("resolve_object:: can't handle a CTBI with base_method of type %s", mono_type_get_full_name (mono_object_class (c->cb)));

		result = inflate_mono_method (inflated_klass, method, (MonoObject*)c->cb);
		*handle_class = mono_defaults.methodhandle_class;
		mono_metadata_free_type (type);
	} else if (strcmp (obj->vtable->klass->name, "MethodOnTypeBuilderInst") == 0) {
		MonoReflectionMethodOnTypeBuilderInst *m = (MonoReflectionMethodOnTypeBuilderInst*)obj;
		if (m->method_args) {
			result = mono_reflection_method_on_tb_inst_get_handle (m, error);
			return_val_if_nok (error, NULL);
			if (context) {
				result = mono_class_inflate_generic_method_checked ((MonoMethod *)result, context, error);
				mono_error_assert_ok (error);
			}
		} else {
			MonoType *minst = mono_reflection_type_get_handle ((MonoReflectionType*)m->inst, error);
			return_val_if_nok (error, NULL);
			MonoType *type = mono_class_inflate_generic_type_checked (minst, context, error);
			return_val_if_nok (error, NULL);

			MonoClass *inflated_klass = mono_class_from_mono_type (type);
			MonoMethod *method;

			if (is_sre_method_builder (mono_object_class (m->mb)))
				method = ((MonoReflectionMethodBuilder *)m->mb)->mhandle;
 			else if (is_sr_mono_method (mono_object_class (m->mb)))
				method = ((MonoReflectionMethod *)m->mb)->method;
			else
				g_error ("resolve_object:: can't handle a MTBI with base_method of type %s", mono_type_get_full_name (mono_object_class (m->mb)));

			result = inflate_mono_method (inflated_klass, method, (MonoObject*)m->mb);
			mono_metadata_free_type (type);
		}
		*handle_class = mono_defaults.methodhandle_class;
	} else if (strcmp (obj->vtable->klass->name, "MonoArrayMethod") == 0) {
		MonoReflectionArrayMethod *m = (MonoReflectionArrayMethod*)obj;
		MonoType *mtype;
		MonoClass *klass;
		MonoMethod *method;
		gpointer iter;
		char *name;

		mtype = mono_reflection_type_get_handle (m->parent, error);
		return_val_if_nok (error, NULL);
		klass = mono_class_from_mono_type (mtype);

		/* Find the method */

		name = mono_string_to_utf8_checked (m->name, error);
		return_val_if_nok (error, NULL);
		iter = NULL;
		while ((method = mono_class_get_methods (klass, &iter))) {
			if (!strcmp (method->name, name))
				break;
		}
		g_free (name);

		// FIXME:
		g_assert (method);
		// FIXME: Check parameters/return value etc. match

		result = method;
		*handle_class = mono_defaults.methodhandle_class;
	} else if (is_sre_array (mono_object_get_class(obj)) ||
				is_sre_byref (mono_object_get_class(obj)) ||
				is_sre_pointer (mono_object_get_class(obj))) {
		MonoReflectionType *ref_type = (MonoReflectionType *)obj;
		MonoType *type = mono_reflection_type_get_handle (ref_type, error);
		return_val_if_nok (error, NULL);

		if (context) {
			MonoType *inflated = mono_class_inflate_generic_type_checked (type, context, error);
			return_val_if_nok (error, NULL);

			result = mono_class_from_mono_type (inflated);
			mono_metadata_free_type (inflated);
		} else {
			result = mono_class_from_mono_type (type);
		}
		*handle_class = mono_defaults.typehandle_class;
	} else {
		g_print ("%s\n", obj->vtable->klass->name);
		g_assert_not_reached ();
	}
	return result;
}

#else /* DISABLE_REFLECTION_EMIT */

MonoArray*
mono_reflection_get_custom_attrs_blob (MonoReflectionAssembly *assembly, MonoObject *ctor, MonoArray *ctorArgs, MonoArray *properties, MonoArray *propValues, MonoArray *fields, MonoArray* fieldValues) 
{
	g_assert_not_reached ();
	return NULL;
}

void
ves_icall_TypeBuilder_setup_internal_class (MonoReflectionTypeBuilder *tb)
{
	g_assert_not_reached ();
}

gboolean
mono_reflection_create_generic_class (MonoReflectionTypeBuilder *tb, MonoError *error)
{
	g_assert_not_reached ();
	return FALSE;
}

void
mono_reflection_dynimage_basic_init (MonoReflectionAssemblyBuilder *assemblyb)
{
	g_error ("This mono runtime was configured with --enable-minimal=reflection_emit, so System.Reflection.Emit is not supported.");
}

static void
mono_image_module_basic_init (MonoReflectionModuleBuilder *moduleb)
{
	g_assert_not_reached ();
}

guint32
mono_image_insert_string (MonoReflectionModuleBuilder *module, MonoString *str)
{
	g_assert_not_reached ();
	return 0;
}

guint32
mono_image_create_method_token (MonoDynamicImage *assembly, MonoObject *obj, MonoArray *opt_param_types, MonoError *error)
{
	g_assert_not_reached ();
	return 0;
}

guint32
mono_image_create_token (MonoDynamicImage *assembly, MonoObject *obj, 
			 gboolean create_open_instance, gboolean register_token, MonoError *error)
{
	g_assert_not_reached ();
	return 0;
}

void
mono_reflection_generic_class_initialize (MonoReflectionGenericClass *type, MonoArray *fields)
{
	g_assert_not_reached ();
}

void
mono_reflection_get_dynamic_overrides (MonoClass *klass, MonoMethod ***overrides, int *num_overrides, MonoError *error)
{
	mono_error_init (error);
	*overrides = NULL;
	*num_overrides = 0;
}

MonoReflectionEvent *
ves_icall_TypeBuilder_get_event_info (MonoReflectionTypeBuilder *tb, MonoReflectionEventBuilder *eb)
{
	g_assert_not_reached ();
	return NULL;
}

MonoReflectionType*
ves_icall_TypeBuilder_create_runtime_class (MonoReflectionTypeBuilder *tb)
{
	g_assert_not_reached ();
	return NULL;
}

void
ves_icall_GenericTypeParameterBuilder_initialize_generic_parameter (MonoReflectionGenericParam *gparam)
{
	g_assert_not_reached ();
}

void 
ves_icall_DynamicMethod_create_dynamic_method (MonoReflectionDynamicMethod *mb)
{
}

MonoType*
mono_reflection_type_get_handle (MonoReflectionType* ref, MonoError *error)
{
	mono_error_init (error);
	if (!ref)
		return NULL;
	return ref->type;
}

#endif /* DISABLE_REFLECTION_EMIT */

#ifndef DISABLE_REFLECTION_EMIT
MonoMethod*
mono_reflection_method_builder_to_mono_method (MonoReflectionMethodBuilder *mb, MonoError *error)
{
	MonoType *tb;
	MonoClass *klass;

	tb = mono_reflection_type_get_handle ((MonoReflectionType*)mb->type, error);
	return_val_if_nok (error, NULL);
	klass = mono_class_from_mono_type (tb);

	return methodbuilder_to_mono_method (klass, mb, error);
}
#else /* DISABLE_REFLECTION_EMIT */
MonoMethod*
mono_reflection_method_builder_to_mono_method (MonoReflectionMethodBuilder *mb, MonoError *error)
{
	g_assert_not_reached ();
	return NULL;
}
#endif /* DISABLE_REFLECTION_EMIT */

gint32
ves_icall_ModuleBuilder_getToken (MonoReflectionModuleBuilder *mb, MonoObject *obj, gboolean create_open_instance)
{
	MONO_CHECK_ARG_NULL (obj, 0);

	MonoError error;
	gint32 result = mono_image_create_token (mb->dynamic_image, obj, create_open_instance, TRUE, &error);
	mono_error_set_pending_exception (&error);
	return result;
}

gint32
ves_icall_ModuleBuilder_getMethodToken (MonoReflectionModuleBuilder *mb,
					MonoReflectionMethod *method,
					MonoArray *opt_param_types)
{
	MONO_CHECK_ARG_NULL (method, 0);

	MonoError error;
	gint32 result = mono_image_create_method_token (
		mb->dynamic_image, (MonoObject *) method, opt_param_types, &error);
	mono_error_set_pending_exception (&error);
	return result;
}

void
ves_icall_ModuleBuilder_WriteToFile (MonoReflectionModuleBuilder *mb, HANDLE file)
{
	MonoError error;
	mono_image_create_pefile (mb, file, &error);
	mono_error_set_pending_exception (&error);
}

void
ves_icall_ModuleBuilder_build_metadata (MonoReflectionModuleBuilder *mb)
{
	MonoError error;
	mono_image_build_metadata (mb, &error);
	mono_error_set_pending_exception (&error);
}

void
ves_icall_ModuleBuilder_RegisterToken (MonoReflectionModuleBuilder *mb, MonoObject *obj, guint32 token)
{
	mono_image_register_token (mb->dynamic_image, token, obj);
}

MonoObject*
ves_icall_ModuleBuilder_GetRegisteredToken (MonoReflectionModuleBuilder *mb, guint32 token)
{
	MonoObject *obj;

	mono_loader_lock ();
	obj = (MonoObject *)mono_g_hash_table_lookup (mb->dynamic_image->tokens, GUINT_TO_POINTER (token));
	mono_loader_unlock ();

	return obj;
}

/**
 * ves_icall_TypeBuilder_create_generic_class:
 * @tb: a TypeBuilder object
 *
 * (icall)
 * Creates the generic class after all generic parameters have been added.
 */
void
ves_icall_TypeBuilder_create_generic_class (MonoReflectionTypeBuilder *tb)
{
	MonoError error;
	(void) mono_reflection_create_generic_class (tb, &error);
	mono_error_set_pending_exception (&error);
}

#ifndef DISABLE_REFLECTION_EMIT
MonoArray*
ves_icall_CustomAttributeBuilder_GetBlob (MonoReflectionAssembly *assembly, MonoObject *ctor, MonoArray *ctorArgs, MonoArray *properties, MonoArray *propValues, MonoArray *fields, MonoArray* fieldValues)
{
	MonoError error;
	MonoArray *result = mono_reflection_get_custom_attrs_blob_checked (assembly, ctor, ctorArgs, properties, propValues, fields, fieldValues, &error);
	mono_error_set_pending_exception (&error);
	return result;
}
#endif

void
ves_icall_AssemblyBuilder_basic_init (MonoReflectionAssemblyBuilder *assemblyb)
{
	mono_reflection_dynimage_basic_init (assemblyb);
}

MonoBoolean
ves_icall_TypeBuilder_get_IsGenericParameter (MonoReflectionTypeBuilder *tb)
{
	return mono_type_is_generic_parameter (tb->type.type);
}

void
ves_icall_EnumBuilder_setup_enum_type (MonoReflectionType *enumtype,
									   MonoReflectionType *t)
{
	enumtype->type = t->type;
}

MonoReflectionType*
ves_icall_ModuleBuilder_create_modified_type (MonoReflectionTypeBuilder *tb, MonoString *smodifiers)
{
	MonoError error;
	MonoReflectionType *ret;
	MonoClass *klass;
	int isbyref = 0, rank;
	char *p;
	char *str = mono_string_to_utf8_checked (smodifiers, &error);
	if (mono_error_set_pending_exception (&error))
		return NULL;

	klass = mono_class_from_mono_type (tb->type.type);
	p = str;
	/* logic taken from mono_reflection_parse_type(): keep in sync */
	while (*p) {
		switch (*p) {
		case '&':
			if (isbyref) { /* only one level allowed by the spec */
				g_free (str);
				return NULL;
			}
			isbyref = 1;
			p++;

			g_free (str);

			ret = mono_type_get_object_checked (mono_object_domain (tb), &klass->this_arg, &error);
			mono_error_set_pending_exception (&error);

			return ret;
		case '*':
			klass = mono_ptr_class_get (&klass->byval_arg);
			mono_class_init (klass);
			p++;
			break;
		case '[':
			rank = 1;
			p++;
			while (*p) {
				if (*p == ']')
					break;
				if (*p == ',')
					rank++;
				else if (*p != '*') { /* '*' means unknown lower bound */
					g_free (str);
					return NULL;
				}
				++p;
			}
			if (*p != ']') {
				g_free (str);
				return NULL;
			}
			p++;
			klass = mono_array_class_get (klass, rank);
			mono_class_init (klass);
			break;
		default:
			break;
		}
	}

	g_free (str);

	ret = mono_type_get_object_checked (mono_object_domain (tb), &klass->byval_arg, &error);
	mono_error_set_pending_exception (&error);

	return ret;
}

void
ves_icall_ModuleBuilder_basic_init (MonoReflectionModuleBuilder *moduleb)
{
	mono_image_module_basic_init (moduleb);
}

guint32
ves_icall_ModuleBuilder_getUSIndex (MonoReflectionModuleBuilder *module, MonoString *str)
{
	return mono_image_insert_string (module, str);
}

void
ves_icall_ModuleBuilder_set_wrappers_type (MonoReflectionModuleBuilder *moduleb, MonoReflectionType *type)
{
	MonoDynamicImage *image = moduleb->dynamic_image;

	g_assert (type->type);
	image->wrappers_type = mono_class_from_mono_type (type->type);
}
