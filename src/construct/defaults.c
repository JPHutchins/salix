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

	return NULL;
}

bool struct_copies_default(PyTypeObject * const kind) {
	return copies_default(kind) != NULL;
}

/* The module does not support multiple interpreters, so the static cache
 * is one per process, cleared by defaults_free at teardown. */
static PyObject * cached_deepcopy = NULL;

static PyObject * deepcopy_function(void) {
	if (cached_deepcopy != NULL) {
		return cached_deepcopy;
	}

	PY_OWNED(module, PyImport_ImportModule("copy"));

	if (module == NULL) {
		return NULL;
	}

	PY_OWNED(resolved, PyObject_GetAttrString(module, "deepcopy"));

	if (resolved == NULL) {
		return NULL;
	}

	/* Both racers hold the same module, so the critical section is on it; the
	 * loser's reference drops with its scope. */
	STRUCT_BEGIN_CRITICAL_SECTION(module);
		if (cached_deepcopy == NULL) {
			cached_deepcopy = Py_NewRef(resolved);
		}
	STRUCT_END_CRITICAL_SECTION();

	return cached_deepcopy;
}

void defaults_free(void) {
	Py_CLEAR(cached_deepcopy);
}

enum default_probe { DEFAULT_PROBE_SHARE, DEFAULT_PROBE_DEEPCOPY, DEFAULT_PROBE_ERROR };

static enum default_probe probe_default(PyObject * const declared) {
	PyTypeObject * const kind = Py_TYPE(declared);

	if (kind->tp_hash == NULL || kind->tp_hash == PyObject_HashNotImplemented) {
		return DEFAULT_PROBE_SHARE;
	}

	if (PyObject_Hash(declared) != -1 || !PyErr_Occurred()) {
		return DEFAULT_PROBE_SHARE;
	}

	if (PyErr_ExceptionMatches(PyExc_RecursionError)) {
		/* A frozen struct pointing at itself hashes out of stack; sharing it
		 * is safe, exactly as the old refusal ruled. */
		PyErr_Clear();

		return DEFAULT_PROBE_SHARE;
	}

	if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_ValueError)) {
		/* The author's own exception propagates unchanged. */
		return DEFAULT_PROBE_ERROR;
	}

	PyErr_Clear();

	return DEFAULT_PROBE_DEEPCOPY;
}

PyObject * struct_default_copy(PyObject * const declared) {
	default_copier const copy = copies_default(Py_TYPE(declared));

	if (copy != NULL) {
		return copy(declared);
	}

	switch (probe_default(declared)) {
		case DEFAULT_PROBE_SHARE:
			return Py_NewRef(declared);
		case DEFAULT_PROBE_DEEPCOPY:
			break;
		case DEFAULT_PROBE_ERROR:
			return NULL;
	}

	PyObject * const deepcopy = deepcopy_function();

	if (deepcopy == NULL) {
		return NULL;
	}

	/* A fresh memo per field: two fields declaring the same object get
	 * disjoint copies, and the copies never are the declared object. */
	PY_MOVABLE(copied, PyObject_CallOneArg(deepcopy, declared));

	if (copied != NULL) {
		return py_move(&copied);
	}

	/* A TypeError is the deepcopy refusal shape (a memoryview); the old rule
	 * shared those, so share them still. Anything else propagates. */
	if (!PyErr_ExceptionMatches(PyExc_TypeError)) {
		return NULL;
	}

	PyErr_Clear();

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
