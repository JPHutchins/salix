#pragma once

#include "../construct.h"
#include "../result.h"

typedef struct StructType StructType;

struct field_lookup {
	enum { FIELD_LOOKUP_FOUND, FIELD_LOOKUP_MISSING, FIELD_LOOKUP_ERROR } tag;
	Py_ssize_t index;
};

struct field_lookup find_field(StructType const * type, PyObject * name);
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
enum result run_post_init(StructType const * type, PyObject * self);

int change_names_touch(
	StructType * type,
	PyObject * keyword_names,
	Py_ssize_t change_count,
	Py_ssize_t field_index
);

enum result carry_group_members(
	StructType * type,
	PyObject * self,
	PyObject * msg,
	PyObject * excs,
	PyObject * excs_str,
	PyObject * deepcopier,
	PyObject * memo
);

#if PY_VERSION_HEX >= 0x030B0000
enum result group_members_from_fields(StructType * type, PyObject * self, PyObject * msg_fallback);

enum result store_group_args(StructType * type, PyObject * self, bool from_fields);
#endif

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

void set_exception_args_from_positionals(
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
