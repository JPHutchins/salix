#pragma once

#include <Python.h>
#include <stdbool.h>

#include "result.h"
#include "types.h"

struct field_plan {
	PyObject * all_names;
	PyObject * new_names;
	PyObject * defaults;
	Py_ssize_t * field_default_indices;
	PyObject * annotations;
	PyObject * metadata;
	PyObject * parameter_names;
	PyObject * init_var_flags;
	PyObject * init_var_names;
	PyObject * init_var_defaults;
	Py_ssize_t * init_var_default_indices;
	PyObject * init_var_annotations;
	PyObject * declared_names;
	PyObject * declared_annotations;
	PyObject * class_var_positions;
	PyObject * mro_default_names;
	PyObject * default_by_name;
	PyObject * keyword_only_names;
	PyObject * missing;
};

enum keyword_only_choice : unsigned char {
	KEYWORD_ONLY_UNSPECIFIED,
	KEYWORD_ONLY_CHOSEN,
	KEYWORD_ONLY_DECLINED,
};

struct field_specifier {
	enum { FIELD_SPECIFIER_ABSENT, FIELD_SPECIFIER_PRESENT } tag;
	PyObject * default_value;
	enum keyword_only_choice keyword_only;
};

struct field_plan field_plan_build(
	StructType const * base,
	PyObject * bases,
	PyObject * namespace,
	bool class_keyword_only,
	PyObject * missing
);

enum result field_plan_resolve_mro_defaults(struct field_plan * plan, PyTypeObject * created);

void field_plan_clear(struct field_plan * plan);

PyObject * Struct_field_specifier(PyObject * module, PyObject * arguments, PyObject * keywords);
struct field_specifier field_specifier_of(PyObject * value);
enum result field_specifier_install(void);
PyObject * positional_parameter_names(struct field_plan const * plan);

extern char const * const reserved_metadata_names[];
char const * reserved_metadata_name_of(PyObject * name);

static inline bool field_plan_failed(struct field_plan const * const plan) {
	return (
		plan->all_names == NULL ||
		plan->declared_names == NULL ||
		plan->declared_annotations == NULL ||
		plan->keyword_only_names == NULL
	);
}
