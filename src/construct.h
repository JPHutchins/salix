#pragma once

#include <Python.h>
#include <stdbool.h>

PyObject * Struct_vectorcall(
	PyObject * struct_class,
	PyObject * const * arguments,
	size_t argument_count_and_flags,
	PyObject * keyword_names
);

PyObject * Struct_new(PyTypeObject * struct_class, PyObject * arguments, PyObject * keywords);

PyObject * Struct_set_field(PyObject * module, PyObject * arguments);

PyObject * Struct_from_mapping(PyObject * module, PyObject * arguments);

bool struct_copies_default(PyTypeObject const * kind);

PyObject * struct_default_copy(PyObject * declared);
