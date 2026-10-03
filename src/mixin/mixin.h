#pragma once

#include "../mixin.h"

typedef struct StructType StructType;

PyObject * Struct_copy(PyObject * self, PyObject * noargs);
PyObject * Struct_reduce_ex(PyObject * self, PyObject * protocol);
PyObject * Struct_setstate(PyObject * self, PyObject * state);
PyObject * co_base_override(PyObject * self, PyObject * name);
int defines_reduce_hooks(PyTypeObject * cls);
PyObject * Struct_deepcopy(PyObject * self, PyObject * memo);
PyObject * copy_dispatch_prologue(
	PyObject * self,
	char const * name,
	PyObject * argument,
	bool dispatch_truthy,
	PyObject * * copy_module,
	PyObject * * copier
);
PyObject * copy_delegate(
	PyObject * self,
	PyObject * argument,
	PyObject * memo,
	char const * name,
	char const * uncopyable,
	bool dispatch_truthy
);
PyObject * copy_reconstruct(
	PyObject * self,
	PyObject * reduced,
	PyObject * copy_module,
	PyObject * memo
);
PyObject * interned_copy(StructType const * type);
