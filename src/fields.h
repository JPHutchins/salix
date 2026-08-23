#pragma once

#include <Python.h>
#include <stdbool.h>

#include "types.h"

struct field_plan {
	PyObject * all_names;        /* list[str] */
	PyObject * new_names;        /* list[str] */
	PyObject * class_var_names;  /* list[str] */
	PyObject * defaults;         /* tuple */
	PyObject * annotations;      /* tuple[Any], aligned with all_names */
	PyObject * metadata;         /* tuple[tuple[Any, ...]], aligned with all_names */
};

/* Owns its six references.  On failure every member is NULL and an
 * exception is set. */
struct field_plan field_plan_build(StructType const * base, PyObject * namespace);

void field_plan_clear(struct field_plan * plan);

extern char const * const reserved_metadata_names[];
char const * reserved_metadata_name_of(PyObject * name);

static inline bool field_plan_failed(struct field_plan const * const plan) {
	return plan->all_names == NULL;
}
