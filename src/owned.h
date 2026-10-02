#pragma once

#include <Python.h>

#define PY_OWNED(name, initializer) \
	__attribute__((cleanup(py_release))) PyObject * const name = (initializer)

#define PY_MOVABLE(name, initializer) \
	__attribute__((cleanup(py_release))) PyObject * name = (initializer)

static inline void py_release(PyObject * const * const reference) {
	Py_XDECREF(*reference);
}

static inline PyObject * py_move(PyObject * * const reference) {
	PyObject * const moved = *reference;
	*reference = NULL;

	return moved;
}

static inline PyObject * optional_attribute(PyObject * const object, char const * const name) {
	PyObject * const value = PyObject_GetAttrString(object, name);

	if (value == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
		PyErr_Clear();
	}

	return value;
}
