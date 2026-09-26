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

static PyObject * deepcopy_function(void);

static PyObject * copy_list(PyObject * const declared) {
	return PyList_GetSlice(declared, 0, PyList_GET_SIZE(declared));
}

static PyObject * copy_declared(PyObject * const declared) {
	/* The type's own constructor preserves the subclass. A constructor whose
	 * signature is not the iterable one (a defaultdict takes a factory)
	 * raises; copy_or_base falls back to the base copy. */
	return PyObject_CallOneArg((PyObject *) Py_TYPE(declared), declared);
}

static PyObject * copy_or_base(
	PyObject * const declared,
	PyObject * (* const base_copy) (PyObject *)
) {
	/* #172: a non-empty value cannot be copied shallowly without sharing its
	 * contents, so the deep path carries it; the empty one copies through the
	 * declared type's constructor, with the base copy for constructors whose
	 * signature is not the iterable one. */
	Py_ssize_t const size = PyObject_Size(declared);

	if (size < 0) {
		return NULL;
	}

	if (size > 0) {
		PyObject * const deepcopy = deepcopy_function();

		return deepcopy != NULL ? PyObject_CallOneArg(deepcopy, declared) : NULL;
	}

	PY_MOVABLE(copied, copy_declared(declared));

	if (copied != NULL) {
		return py_move(&copied);
	}

	if (!PyErr_ExceptionMatches(PyExc_TypeError)) {
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

static PyObject * deepcopy_function(void) {
	/* The module does not support multiple interpreters, so the static cache
	 * is one per process. */
	static PyObject * cached = NULL;

	if (cached != NULL) {
		return cached;
	}

	PY_OWNED(module, PyImport_ImportModule("copy"));

	if (module == NULL) {
		return NULL;
	}

	cached = PyObject_GetAttrString(module, "deepcopy");

	return cached;
}

static bool hashes_unhashable_value(PyObject * const declared) {
	PyTypeObject * const kind = Py_TYPE(declared);

	if (kind->tp_hash == NULL || kind->tp_hash == PyObject_HashNotImplemented) {
		return false;
	}

	if (PyObject_Hash(declared) != -1 || !PyErr_Occurred()) {
		return false;
	}

	if (PyErr_ExceptionMatches(PyExc_RecursionError)) {
		/* A frozen struct pointing at itself hashes out of stack; sharing it
		 * is safe, exactly as the old refusal ruled. */
		PyErr_Clear();

		return false;
	}

	if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_ValueError)) {
		/* The author's own exception propagates unchanged. */
		return false;
	}

	PyErr_Clear();

	return true;
}

PyObject * struct_default_copy(PyObject * const declared) {
	default_copier const copy = copies_default(Py_TYPE(declared));

	if (copy != NULL) {
		return copy(declared);
	}

	if (hashes_unhashable_value(declared)) {
		PyObject * const deepcopy = deepcopy_function();

		if (deepcopy == NULL) {
			return NULL;
		}

		PY_MOVABLE(copied, PyObject_CallOneArg(deepcopy, declared));

		if (copied != NULL) {
			return py_move(&copied);
		}

		/* Some values a deepcopy cannot carry (a memoryview); the old rule
		 * shared those, so share them still. */
		PyErr_Clear();
	}

	return Py_NewRef(declared);
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
