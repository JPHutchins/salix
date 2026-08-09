#include <Python.h>

#include "owned.h"
#include "repr.h"
#include "types.h"

static PyObject * fields_repr(StructType const * type, PyObject * self);
static PyObject * field_repr(StructType const * type, PyObject * self, Py_ssize_t index);

PyObject * Struct_repr(PyObject * const self) {
	if (!is_struct(self)) {
		return PyBaseObject_Type.tp_repr(self);
	}

	int const recursive = Py_ReprEnter(self);

	if (recursive != 0) {
		return recursive < 0 ? NULL : PyUnicode_FromString("...");
	}

	PY_OWNED(inner, fields_repr(struct_type_of(self), self));
	Py_ReprLeave(self);

	if (inner == NULL) {
		return NULL;
	}

	return PyUnicode_FromFormat("%s(%U)", Py_TYPE(self)->tp_name, inner);
}

static PyObject * fields_repr(StructType const * const type, PyObject * const self) {
	PY_OWNED(pieces, PyList_New(type->struct_field_count));

	if (pieces == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * const piece = field_repr(type, self, i);

		if (piece == NULL) {
			return NULL;
		}

		PyList_SET_ITEM(pieces, i, piece);
	}

	PY_OWNED(separator, PyUnicode_FromString(", "));

	return separator != NULL ? PyUnicode_Join(separator, pieces) : NULL;
}

static PyObject * field_repr(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index
) {
	PY_OWNED(value, struct_slot_ref(type, self, index));

	PY_OWNED(rendered, value != NULL ? PyObject_Repr(value) : PyUnicode_FromString("<unset>"));

	if (rendered == NULL) {
		return NULL;
	}

	return PyUnicode_FromFormat(
		"%U=%U",
		PyTuple_GET_ITEM(type->struct_field_names, index),
		rendered
	);
}

#ifdef TESTING

#	include "testing.h"

static void test_an_unwritten_slot_renders_as_unset(void) {
	PyObject * const instance =
		testing_evaluate("class P(Struct):\n    x: int\n    y: int\nresult = P(1, 2)\n");
	StructType const * const type = struct_type_of(instance);
	PyObject * * const slot = struct_slot(type, instance, 1);
	PyObject * const written = *slot;

	*slot = NULL;

	PyObject * const rendered = field_repr(type, instance, 1);

	*slot = written;

	TEST_ASSERT_NOT_NULL(rendered);
	TEST_ASSERT_EQUAL_STRING("y=<unset>", PyUnicode_AsUTF8(rendered));

	Py_DECREF(rendered);
	Py_DECREF(instance);
}

static void test_a_written_slot_renders_its_repr(void) {
	PyObject * const instance =
		testing_evaluate("class P(Struct):\n    x: int\nresult = P('value')\n");
	PyObject * const rendered = field_repr(struct_type_of(instance), instance, 0);

	TEST_ASSERT_NOT_NULL(rendered);
	TEST_ASSERT_EQUAL_STRING("x='value'", PyUnicode_AsUTF8(rendered));

	Py_DECREF(rendered);
	Py_DECREF(instance);
}

void repr_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_unwritten_slot_renders_as_unset);
	RUN_TEST(test_a_written_slot_renders_its_repr);
}

#endif
