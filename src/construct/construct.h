#pragma once

#include <Python.h>
#include <stdbool.h>

#include "../result.h"
#include "../types.h"

struct field_lookup {
	enum { FIELD_LOOKUP_FOUND, FIELD_LOOKUP_MISSING, FIELD_LOOKUP_ERROR } tag;
	Py_ssize_t index;
};

PyObject * Struct_vectorcall(
	PyObject * struct_class,
	PyObject * const * arguments,
	size_t argument_count_and_flags,
	PyObject * keyword_names
);

PyObject * Struct_new(PyTypeObject * struct_class, PyObject * arguments, PyObject * keywords);

PyObject * Struct_set_field(PyObject * module, PyObject * arguments);

bool struct_copies_default(PyTypeObject const * kind);

PyObject * struct_default_copy(PyObject * declared);

void bind_positional(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count
);
enum result bind_keywords(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count,
	PyObject * keyword_names
);
enum result fill_defaults(StructType const * type, PyObject * self, Py_ssize_t positional_count);
struct field_lookup find_field(StructType const * type, PyObject * name);
enum result write_slot(
	StructType const * type,
	PyObject * self,
	Py_ssize_t index,
	PyObject * value
);
