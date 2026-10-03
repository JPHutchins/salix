#pragma once

#include <Python.h>
#include <stdbool.h>

#include "result.h"
#include "types.h"

struct field_plan {
	PyObject * all_names;
	PyObject * new_names;
	PyObject * defaults;
	PyObject * annotations;
	PyObject * metadata;
	PyObject * parameter_names;
	PyObject * init_var_flags;
	PyObject * init_var_names;
	PyObject * init_var_defaults;
	PyObject * init_var_annotations;
	PyObject * declared_names;
	PyObject * class_var_positions;
	PyObject * mro_default_names;
};

struct field_plan field_plan_build(StructType const * base, PyObject * bases, PyObject * namespace);

enum result field_plan_resolve_mro_defaults(struct field_plan * plan, PyTypeObject * created);

void field_plan_clear(struct field_plan * plan);

extern char const * const reserved_metadata_names[];
char const * reserved_metadata_name_of(PyObject * name);

static inline bool field_plan_failed(struct field_plan const * const plan) {
	return plan->all_names == NULL || plan->declared_names == NULL;
}
