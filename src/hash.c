#include <Python.h>

#include "hash.h"
#include "owned.h"
#include "types.h"

/* CPython reserves -1: a real hash of -1 is remapped to -2. */
enum : Py_hash_t {
	HASH_ERR = -1,
};

/*
 * A struct hashes as the tuple of its values, so `hash(p) == hash((1.0, 2.0))`
 * and structs interoperate with tuple keys. Building the tuple is the whole
 * cost; PyObject_Hash does the rest.
 *
 * A struct that holds itself re-enters here through tuplehash without pushing
 * a Python frame, so the interpreter's own depth limit never sees the descent.
 */
Py_hash_t Struct_hash(PyObject * const self) {
	/* Unhashable rather than hashed by pointer, which is the one fallback in
	 * this file that could not be a local decision. A mixin subclass over a
	 * value type is compared by that type -- rich_compare answers
	 * NotImplemented and the co-base's reflected __eq__ says yes -- so an
	 * identity hash made it equal to a value it could not be looked up beside.
	 * Refusing to hash is what puts equality and hashing back in one place, and
	 * being unhashable is an ordinary thing for an object to be.
	 *
	 * Unhashable to the operator and not to the ABC: collections.abc.Hashable
	 * asks whether tp_hash is non-NULL, and the mixin's is, so an impostor is an
	 * instance of it and raises when hashed. Setting tp_hash to NULL is not
	 * available -- it is the same slot every struct hashes through. CPython's
	 * own writable memoryview diverges the same way. */
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
