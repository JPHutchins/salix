#pragma once

#include "../construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

struct field_lookup {
	enum { FIELD_LOOKUP_FOUND, FIELD_LOOKUP_MISSING, FIELD_LOOKUP_ERROR } tag;
	Py_ssize_t index;
};

struct field_lookup find_field(StructType const * type, PyObject * name);
struct field_lookup find_init_var(StructType const * type, PyObject * name);
enum result write_slot(
	StructType const * type,
	PyObject * self,
	Py_ssize_t index,
	PyObject * value
);

void bind_positional(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count,
	bool family_constructed
);
enum result bind_keywords(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count,
	PyObject * keyword_names
);
enum result bind_named(StructType const * type, PyObject * self, PyObject * name, PyObject * value);
struct field_lookup named_field(StructType const * type, PyObject * name);
enum result fill_defaults(StructType const * type, PyObject * self, bool require_all);
enum result refuse_a_builtin_init(StructType const * type, char const * operation);
enum result run_post_init(StructType const * type, PyObject * self);
PyObject * post_init_arguments_for(StructType const * type, PyObject * self);
enum result run_post_init_with(StructType const * type, PyObject * post_init_arguments);
enum result bind_parameter_named(
	StructType const * type,
	PyObject * self,
	PyObject * post_init_arguments,
	PyObject * name,
	PyObject * value
);
enum result bind_parameters(
	StructType const * type,
	PyObject * self,
	PyObject * post_init_arguments,
	PyObject * const * arguments,
	Py_ssize_t positional_count,
	PyObject * keyword_names
);
enum result fill_init_var_defaults(StructType const * type, PyObject * post_init_arguments);
enum result refuse_missing_parameters(
	StructType const * type,
	PyObject * self,
	PyObject * post_init_arguments
);

int change_names_touch(
	StructType * type,
	PyObject * keyword_names,
	Py_ssize_t change_count,
	Py_ssize_t field_index
);

enum result install_empty_exception_payload(StructType * type, PyObject * self);

static inline PyObject * allocated_struct_copy(StructType * const type) {
	PyTypeObject * const cls = &type->heap_type.ht_type;
	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (
		copy != NULL &&
		is_exception_struct(cls) &&
		install_empty_exception_payload(type, copy) != RESULT_OK
	) {
		return NULL;
	}

	return py_move(&copy);
}

PyObject * family_rebuilt_copy(
	StructType * type,
	PyObject * self,
	PyObject * deepcopier,
	PyObject * memo
);

enum result carry_group_members(
	StructType * type,
	PyObject * self,
	PyObject * msg,
	PyObject * excs,
	PyObject * source,
	PyObject * deepcopier,
	PyObject * memo
);

#if PY_VERSION_HEX >= 0x030B0000
enum result group_members_from_fields(StructType * type, PyObject * self, PyObject * msg_fallback);

enum result store_group_args(StructType * type, PyObject * self, bool from_fields);
#endif

PyObject * exception_args_of(PyObject * self);

enum result set_exception_args_from_fields(
	StructType * type,
	PyObject * self,
	Py_ssize_t field_count
);

enum result set_exception_args_from_original(
	StructType * type,
	PyObject * copy,
	PyObject * original,
	PyObject * deepcopier,
	PyObject * memo
);

enum result set_exception_args_from_positionals(
	PyTypeObject * cls,
	PyObject * self,
	PyObject * positionals
);

Py_ssize_t explicit_field_prefix(
	StructType const * type,
	Py_ssize_t positional_count,
	PyObject * keyword_names,
	Py_ssize_t carried_count
);

Py_ssize_t carried_payload_count(StructType * type, PyObject * self);

Py_ssize_t explicit_dict_prefix(StructType const * type, PyObject * values);

Py_ssize_t explicit_items_prefix(StructType const * type, PyObject * items, Py_ssize_t entry_count);
