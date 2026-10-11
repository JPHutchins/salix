#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../hash.h"
#include "../types.h"
#include "../mixin/mixin.h"

typedef PyObject * (*default_copier)(struct salix_state * state, PyObject * declared);

static PyObject * deepcopy_function(void);

static PyObject * copy_list(PyObject * const declared) {
	return PyList_GetSlice(declared, 0, PyList_GET_SIZE(declared));
}

static PyObject * copy_declared(PyObject * const declared, PyObject * const seed) {
	return PyObject_CallOneArg((PyObject *) Py_TYPE(declared), seed);
}

static PyObject * deep_copy_or_share(struct salix_state * const state, PyObject * const declared) {
	PY_OWNED(deepcopy, deepcopy_function());

	if (deepcopy == NULL) {
		return NULL;
	}

	PY_MOVABLE(mirrored, state != NULL ? fresh_deepcopy(state, deepcopy, declared) : NULL);
	PY_MOVABLE(
		deep_copied,
		mirrored != NULL || PyErr_Occurred() ? py_move(&mirrored) :
		PyObject_CallOneArg(deepcopy, declared)
	);

	if (deep_copied != NULL) {
		return py_move(&deep_copied);
	}

	if (!PyErr_ExceptionMatches(PyExc_TypeError)) {
		return NULL;
	}

	PyErr_Clear();

	return Py_NewRef(declared);
}

static PyObject * copy_or_base(
	struct salix_state * const state,
	PyObject * const declared,
	PyObject * (* const base_copy) (PyObject *)
) {
	Py_ssize_t const size = PyObject_Size(declared);

	if (size < 0) {
		return NULL;
	}

	if (size > 0) {
		return deep_copy_or_share(state, declared);
	}

	PY_MOVABLE(seed, base_copy(declared));

	if (seed == NULL) {
		return NULL;
	}

	PY_MOVABLE(copied, copy_declared(declared, seed));

	if (copied != NULL && copied != declared) {
		return py_move(&copied);
	}

	if (copied == NULL && !PyErr_ExceptionMatches(PyExc_TypeError)) {
		return NULL;
	}

	PyErr_Clear();

	return base_copy(declared);
}

static PyObject * copy_list_or_base(struct salix_state * const state, PyObject * const declared) {
	return copy_or_base(state, declared, copy_list);
}

static PyObject * copy_dict_or_base(struct salix_state * const state, PyObject * const declared) {
	return copy_or_base(state, declared, PyDict_Copy);
}

static PyObject * copy_set_or_base(struct salix_state * const state, PyObject * const declared) {
	return copy_or_base(state, declared, PySet_New);
}

static PyObject * copy_bytearray_or_base(
	struct salix_state * const state,
	PyObject * const declared
) {
	return copy_or_base(state, declared, PyByteArray_FromObject);
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

enum probe_prediction { PROBE_PREDICTED_SHARE, PROBE_PREDICTED_DEEPCOPY, PROBE_UNPREDICTED };

static bool hashes_without_user_code(PyTypeObject * const kind) {
	return (
		kind == Py_TYPE(Py_None) ||
		kind == &PyBool_Type ||
		kind == &PyLong_Type ||
		kind == &PyFloat_Type ||
		kind == &PyComplex_Type ||
		kind == &PyUnicode_Type ||
		kind == &PyBytes_Type
	);
}

static bool declines_to_hash(PyTypeObject * const kind) {
	return (
		kind == &PyList_Type ||
		kind == &PyDict_Type ||
		kind == &PySet_Type ||
		kind == &PyByteArray_Type
	);
}

static enum probe_prediction predicted_struct_probe(PyObject * const declared) {
	if (Py_TYPE(declared)->tp_hash != Struct_hash || !is_struct(declared)) {
		return PROBE_UNPREDICTED;
	}

	if (Py_EnterRecursiveCall(" while hashing a struct") != 0) {
		PyErr_Clear();

		return PROBE_PREDICTED_SHARE;
	}

	StructType const * const type = struct_type_of(declared);
	enum probe_prediction predicted = PROBE_PREDICTED_SHARE;

	STRUCT_BEGIN_CRITICAL_SECTION(declared);

	for (
		Py_ssize_t i = 0;
		predicted == PROBE_PREDICTED_SHARE && i < type->struct_field_count;
		i += 1
	) {
		PyObject * const value = *struct_slot(type, declared, i);
		PyTypeObject * const kind = value != NULL ? Py_TYPE(value) : Py_TYPE(Py_None);

		predicted = (
			hashes_without_user_code(kind) ? PROBE_PREDICTED_SHARE :
			declines_to_hash(kind) ? PROBE_PREDICTED_DEEPCOPY :
			PROBE_UNPREDICTED
		);
	}

	STRUCT_END_CRITICAL_SECTION();
	Py_LeaveRecursiveCall();

	return predicted;
}

static enum default_probe probe_default(PyObject * const declared) {
	PyTypeObject * const kind = Py_TYPE(declared);

	if (kind->tp_hash == NULL || kind->tp_hash == PyObject_HashNotImplemented) {
		return DEFAULT_PROBE_SHARE;
	}

	if (PyObject_Hash(declared) != -1 || !PyErr_Occurred()) {
		return DEFAULT_PROBE_SHARE;
	}

	if (PyErr_ExceptionMatches(PyExc_RecursionError)) {
		PyErr_Clear();

		return DEFAULT_PROBE_SHARE;
	}

	if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_ValueError)) {
		return DEFAULT_PROBE_ERROR;
	}

	PyErr_Clear();

	return DEFAULT_PROBE_DEEPCOPY;
}

PyObject * struct_default_copy(struct salix_state * const state, PyObject * const declared) {
	default_copier const copy = copies_default(Py_TYPE(declared));

	if (copy != NULL) {
		return copy(state, declared);
	}

	switch (predicted_struct_probe(declared)) {
		case PROBE_PREDICTED_SHARE:
			return Py_NewRef(declared);
		case PROBE_PREDICTED_DEEPCOPY:
			return deep_copy_or_share(state, declared);
		case PROBE_UNPREDICTED:
			break;
	}

	switch (probe_default(declared)) {
		case DEFAULT_PROBE_SHARE:
			return Py_NewRef(declared);
		case DEFAULT_PROBE_DEEPCOPY:
			break;
		case DEFAULT_PROBE_ERROR:
			return NULL;
	}

	return deep_copy_or_share(state, declared);
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
