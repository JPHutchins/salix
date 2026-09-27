#pragma once

#include "../mixin.h"

typedef struct StructType StructType;

PyObject * Struct_copy(PyObject * self, PyObject * noargs);
PyObject * Struct_deepcopy(PyObject * self, PyObject * memo);
PyObject * frozen_instance_error(StructType const * type);
