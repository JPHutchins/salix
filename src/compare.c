#include <Python.h>

#include "compare.h"
#include "owned.h"
#include "types.h"

enum comparison {
	COMPARISON_ERROR = -1,
	COMPARISON_UNEQUAL = 0,
	COMPARISON_EQUAL = 1,
};

static PyObject * equality_result(PyObject * self, PyObject * other, int op);
static PyObject * ordering_result(PyObject * self, PyObject * other, int op);
static enum comparison structs_equal(PyObject * self, PyObject * other);
static enum comparison values_equal(StructType const * type, PyObject * self, PyObject * other);

PyObject * Struct_rich_compare(PyObject * const self, PyObject * const other, int const op) {
	if (!is_struct(self) || Py_TYPE(other) != Py_TYPE(self)) {
		Py_RETURN_NOTIMPLEMENTED;
	}

	return (
		op == Py_EQ || op == Py_NE ? equality_result(self, other, op) :
		ordering_result(self, other, op)
	);
}

static PyObject * equality_result(PyObject * const self, PyObject * const other, int const op) {
	switch (structs_equal(self, other)) {
		case COMPARISON_ERROR:
			return NULL;
		case COMPARISON_UNEQUAL:
			return PyBool_FromLong(op == Py_NE);
		case COMPARISON_EQUAL:
			return PyBool_FromLong(op == Py_EQ);
	}

	Py_UNREACHABLE();
}

static PyObject * ordering_result(PyObject * const self, PyObject * const other, int const op) {
	StructType const * const type = struct_type_of(self);

	if (!type->struct_options.order) {
		Py_RETURN_NOTIMPLEMENTED;
	}

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		struct slot_pair const pair = struct_slot_pair_ref(type, self, other, i);
		PY_OWNED(mine, pair.mine);
		PY_OWNED(theirs, pair.theirs);
		int const equal = PyObject_RichCompareBool(mine, theirs, Py_EQ);

		if (equal == COMPARISON_ERROR) {
			return NULL;
		}

		if (equal == COMPARISON_UNEQUAL) {
			return PyObject_RichCompare(mine, theirs, op);
		}
	}

	return PyBool_FromLong(op == Py_LE || op == Py_GE);
}

static enum comparison structs_equal(PyObject * const self, PyObject * const other) {
	return values_equal(struct_type_of(self), self, other);
}

static enum comparison values_equal(
	StructType const * const type,
	PyObject * const self,
	PyObject * const other
) {
	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		struct slot_pair const pair = struct_slot_pair_ref(type, self, other, i);
		PY_OWNED(mine, pair.mine);
		PY_OWNED(theirs, pair.theirs);
		int const equal = PyObject_RichCompareBool(mine, theirs, Py_EQ);

		if (equal != COMPARISON_EQUAL) {
			return equal;
		}
	}

	return COMPARISON_EQUAL;
}
