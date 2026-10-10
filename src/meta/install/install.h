#pragma once

#include "../meta.h"
#include "../../types.h"

bool group_family_in_mro(PyTypeObject * cls);
bool family_owns_in_mro(PyTypeObject * cls);
struct init_source {
	bool own;
	PyTypeObject * builtin_owner;
};

struct init_source init_source_of(StructType * struct_class, PyObject * namespace);
