#pragma once

#include <Python.h>

#include "meta.h"
#include "result.h"

extern PyTypeObject Field_Type;
extern PyTypeObject Missing_Type;

enum result introspection_install(PyObject * module, struct salix_state * state);
PyObject * Struct_fields(PyObject * module, PyObject * class_or_instance);
PyObject * Struct_to_dict(PyObject * module, PyObject * instance);
