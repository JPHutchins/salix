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
