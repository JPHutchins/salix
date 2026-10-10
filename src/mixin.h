#pragma once

#include <Python.h>

extern PyTypeObject StructMixin_Type;

enum setter_source {
	SETTER_SOURCE_ERROR,
	SETTER_SOURCE_OBJECT,
	SETTER_SOURCE_STRUCT,
	SETTER_SOURCE_OTHER,
};

enum setter_source setter_source_of(PyTypeObject const * type, char const * name);

PyObject * Struct_get_signature(PyObject * self, void * closure);
int Struct_set_signature(PyObject * self, PyObject * value, void * closure);
