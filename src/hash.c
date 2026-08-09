#include <Python.h>

#include "hash.h"
#include "owned.h"
#include "types.h"

/* CPython reserves -1: a real hash of -1 is remapped to -2. */
enum : Py_hash_t {
	HASH_ERR = -1,
};

Py_hash_t Struct_hash(PyObject * const self) {
	if (!is_struct(self)) {
		PyErr_Format(PyExc_TypeError, "unhashable type: '%.200s'", Py_TYPE(self)->tp_name);

		return HASH_ERR;
	}

	StructType const * const type = struct_type_of(self);
	PY_OWNED(values, PyTuple_New(type->struct_field_count));

	if (values == NULL) {
		return HASH_ERR;
	}

	struct_slots_ref_or_none_into(type, self, values);

	if (Py_EnterRecursiveCall(" while hashing a struct") != 0) {
		return HASH_ERR;
	}

	Py_hash_t const hash = PyObject_Hash(values);

	Py_LeaveRecursiveCall();

	return hash;
}
