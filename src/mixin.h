#pragma once

#include <Python.h>

extern PyTypeObject StructMixin_Type;

PyObject * Struct_get_signature(PyObject * self, void * closure);
int Struct_set_signature(PyObject * self, PyObject * value, void * closure);
