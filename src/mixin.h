#pragma once

#include <Python.h>

#include "result.h"

/* The base every struct inherits the dunders from.  It holds no fields of its
 * own, so `isinstance(x, Struct)` is the same question as "is this a struct".
 *
 * Not const: PyType_Ready fills in tp_dict, tp_mro, tp_bases and the inherited
 * slots at import time, and PyObject_TypeCheck takes a mutable pointer. */
extern PyTypeObject StructMixin_Type;

/* Resolved once at import time under the import lock, so the static cache is
 * safe on a free-threaded build. */
enum result Struct_mixin_init(void);
