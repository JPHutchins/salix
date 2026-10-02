#pragma once

#include "../meta.h"
#include "../../types.h"

bool group_family_in_mro(PyTypeObject * cls);
bool family_owns_in_mro(PyTypeObject * cls);
bool defines_own_init(StructType * struct_class, PyObject * namespace);
