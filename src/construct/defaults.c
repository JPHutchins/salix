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

static PyObject * copy_declared(PyObject * const declared, PyObject * const seed) {
	/* The type's own constructor preserves the subclass, which runs the
	 * subclass's __init__ (and __len__, at the class-statement emptiness
	 * gate) on every copy. The seed is a base copy of the declared value, so
	 * the constructor cannot retain or mutate the class-body object or the
	 * class's stored default. A constructor whose signature is not the
	 * iterable one (a defaultdict takes a factory) raises TypeError;
	 * copy_or_base falls back to the base copy, dropping the subclass and
	 * its extra state. Any TypeError raised by the constructor's own code is
	 * swallowed the same way -- the fallback cannot tell the two apart. */
	return PyObject_CallOneArg((PyObject *) Py_TYPE(declared), seed);
}

static PyObject * copy_or_base(
	PyObject * const declared,
	PyObject * (* const base_copy) (PyObject *)
) {
	/* #172: a non-empty value cannot be copied shallowly without sharing its
	 * contents, so the deep path carries it; the empty one copies through the
	 * declared type's constructor, with the base copy for constructors whose
	 * signature is not the iterable one. Emptiness therefore decides the
	 * copy: an empty defaultdict falls back and loses its factory, while a
	 * seeded one is deep-copied with the factory intact. */
	Py_ssize_t const size = PyObject_Size(declared);

	if (size < 0) {
		return NULL;
	}

	if (size > 0) {
		PY_OWNED(deepcopy, deepcopy_function());

		if (deepcopy == NULL) {
			return NULL;
		}

		PY_MOVABLE(deep_copied, PyObject_CallOneArg(deepcopy, declared));

		if (deep_copied != NULL) {
			return py_move(&deep_copied);
		}

		/* A TypeError is the deepcopy refusal shape (a memoryview); the old
		 * rule shared those, so share them still. Anything else propagates.
		 * A __deepcopy__ that returns its argument comes back through the
		 * call itself: copy.deepcopy hands it over as the copy. */
		if (!PyErr_ExceptionMatches(PyExc_TypeError)) {
			return NULL;
		}

		PyErr_Clear();

		return Py_NewRef(declared);
	}

	PY_MOVABLE(seed, base_copy(declared));

	if (seed == NULL) {
		return NULL;
	}

	PY_MOVABLE(copied, copy_declared(declared, seed));

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

/* The module does not support multiple interpreters, so the static cache
 * is one per process, cleared by defaults_free at teardown. */
static PyObject * cached_deepcopy = NULL;

static PyObject * deepcopy_function(void) {
	if (cached_deepcopy != NULL) {
		return Py_XNewRef(cached_deepcopy);
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
	 * loser's reference drops with its scope. The caller gets its own
	 * reference, so a concurrent defaults_free cannot free the object the
	 * caller is about to invoke. */
	STRUCT_BEGIN_CRITICAL_SECTION(module);
		if (cached_deepcopy == NULL) {
			cached_deepcopy = Py_NewRef(resolved);
		}
	STRUCT_END_CRITICAL_SECTION();

	return Py_XNewRef(cached_deepcopy);
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

	PY_OWNED(deepcopy, deepcopy_function());

	if (deepcopy == NULL) {
		return NULL;
	}

	/* A fresh memo per field: two fields declaring the same object get
	 * disjoint copies. */
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
