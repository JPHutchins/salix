#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

/* A list of statements rather than a static array of {type, constructor}: on
 * Windows a `PyTypeObject` is imported from python3.dll, and the address of a
 * dllimport symbol is not a compile-time constant, so the array version
 * compiles everywhere except the platform half the wheels are cross-built for.
 */
typedef PyObject * (*default_copier)(PyObject * declared);

static PyObject * copy_list(PyObject * const declared) {
	return PyList_GetSlice(declared, 0, PyList_GET_SIZE(declared));
}

static PyObject * copy_declared(PyObject * const declared) {
	/* The type's own constructor preserves the subclass, which runs the
	 * subclass's __init__ (and __len__, at the class-statement emptiness
	 * gate) on every copy. A constructor whose signature is not the iterable
	 * one (a defaultdict takes a factory) raises TypeError; the caller falls
	 * back to the base copy, dropping the subclass and its extra state. Any
	 * TypeError raised by the constructor's own code is swallowed the same
	 * way — the fallback cannot tell the two apart. */
	return PyObject_CallOneArg((PyObject *) Py_TYPE(declared), declared);
}

static PyObject * copy_or_base(
	PyObject * const declared,
	PyObject * (* const base_copy) (PyObject *)
) {
	PY_MOVABLE(copied, copy_declared(declared));

	/* A constructor that returns its argument is a caching or delegating
	 * __new__; the result would re-alias the declared default, so it takes
	 * the base-copy path like a rejecting constructor. */
	if (copied != NULL && copied != declared) {
		return py_move(&copied);
	}

	if (copied == NULL && !PyErr_ExceptionMatches(PyExc_TypeError)) {
		return NULL;
	}

	PyErr_Clear();

	return base_copy(declared);
}

static PyObject * copy_list_or_base(PyObject * const declared) {
	return copy_or_base(declared, copy_list);
}

static PyObject * copy_dict_or_base(PyObject * const declared) {
	return copy_or_base(declared, PyDict_Copy);
}

static PyObject * copy_set_or_base(PyObject * const declared) {
	return copy_or_base(declared, PySet_New);
}

static PyObject * copy_bytearray_or_base(PyObject * const declared) {
	return copy_or_base(declared, PyByteArray_FromObject);
}

static default_copier copies_default(PyTypeObject * const kind) {
	if (kind == &PyList_Type) {
		return copy_list;
	}

	if (kind == &PyDict_Type) {
		return PyDict_Copy;
	}

	if (kind == &PySet_Type) {
		return PySet_New;
	}

	if (kind == &PyByteArray_Type) {
		return PyByteArray_FromObject;
	}

	if (PyType_IsSubtype(kind, &PyList_Type)) {
		return copy_list_or_base;
	}

	if (PyType_IsSubtype(kind, &PyDict_Type)) {
		return copy_dict_or_base;
	}

	if (PyType_IsSubtype(kind, &PySet_Type)) {
		return copy_set_or_base;
	}

	if (PyType_IsSubtype(kind, &PyByteArray_Type)) {
		return copy_bytearray_or_base;
	}

	return NULL;
}

bool struct_copies_default(PyTypeObject * const kind) {
	return copies_default(kind) != NULL;
}

PyObject * struct_default_copy(PyObject * const declared) {
	default_copier const copy = copies_default(Py_TYPE(declared));

	return copy != NULL ? copy(declared) : Py_NewRef(declared);
}

PyObject * Struct_set_field(PyObject * const module, PyObject * const arguments) {
	PyObject * self = NULL;
	PyObject * name = NULL;
	PyObject * value = NULL;

	if (!PyArg_UnpackTuple(arguments, "set_field", 3, 3, &self, &name, &value)) {
		return NULL;
	}

	if (!is_struct(self)) {
		PyErr_Format(
			PyExc_TypeError,
			"set_field() expects a struct, not %.200s",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	if (!PyUnicode_Check(name)) {
		PyErr_Format(
			PyExc_TypeError,
			"set_field() field name must be str, not %.200s",
			Py_TYPE(name)->tp_name
		);

		return NULL;
	}

	StructType * const type = struct_type_of(self);
	struct field_lookup const found = find_field(type, name);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
			return NULL;
		case FIELD_LOOKUP_MISSING:
			PyErr_Format(
				PyExc_AttributeError,
				"%.200s has no field '%U'",
				struct_type_name(type),
				name
			);

			return NULL;
		case FIELD_LOOKUP_FOUND:
			if (write_slot(type, self, found.index, value) != RESULT_OK) {
				return NULL;
			}
	}

	Py_RETURN_NONE;
}
