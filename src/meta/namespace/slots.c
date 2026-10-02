#include <Python.h>
#include <stdbool.h>

#include "namespace.h"
#include "../../owned.h"

static char const * weakref_slot_name(void) {
	return "__weakref__";
}

static char const * instance_dict_slot_name(void) {
	return "__dict__";
}

enum slot_name_owner slot_name_owner_of(PyObject * const name) {
	if (PyUnicode_CompareWithASCIIString(name, weakref_slot_name()) == 0) {
		return SLOT_NAME_WEAKREF;
	}

	if (PyUnicode_CompareWithASCIIString(name, instance_dict_slot_name()) == 0) {
		return SLOT_NAME_INSTANCE_DICT;
	}

	return SLOT_NAME_NONE;
}
PyObject * build_slots(PyObject * const new_names, bool const adds_weakref_slot) {
	PY_OWNED(names, PySequence_List(new_names));

	if (names == NULL) {
		return NULL;
	}

	if (adds_weakref_slot) {
		PY_OWNED(weakref_name, PyUnicode_FromString(weakref_slot_name()));

		if (weakref_name == NULL || PyList_Append(names, weakref_name) < 0) {
			return NULL;
		}
	}

	return PyList_AsTuple(names);
}

#ifdef TESTING

#	include "../../testing.h"

static void test_the_slot_names_answer_for_each_shape(void) {
	PyObject * const weakref_name = PyUnicode_FromFormat("%s%s", "__weak", "ref__");
	PyObject * const dict_name = PyUnicode_FromFormat("%s%s", "__di", "ct__");
	PyObject * const plain_name = PyUnicode_FromString("x");

	TEST_ASSERT_NOT_NULL(weakref_name);
	TEST_ASSERT_NOT_NULL(dict_name);
	TEST_ASSERT_NOT_NULL(plain_name);
	TEST_ASSERT_EQUAL_INT(SLOT_NAME_WEAKREF, slot_name_owner_of(weakref_name));
	TEST_ASSERT_EQUAL_INT(SLOT_NAME_INSTANCE_DICT, slot_name_owner_of(dict_name));
	TEST_ASSERT_EQUAL_INT(SLOT_NAME_NONE, slot_name_owner_of(plain_name));

	Py_DECREF(plain_name);
	Py_DECREF(dict_name);
	Py_DECREF(weakref_name);
}

static void test_build_slots_appends_the_weakref_name_only_when_asked(void) {
	PyObject * const names = PyList_New(0);
	PyObject * const x_name = PyUnicode_FromString("x");
	PyObject * const y_name = PyUnicode_FromString("y");

	TEST_ASSERT_NOT_NULL(names);
	TEST_ASSERT_NOT_NULL(x_name);
	TEST_ASSERT_NOT_NULL(y_name);
	TEST_ASSERT_EQUAL_INT(0, PyList_Append(names, x_name));
	TEST_ASSERT_EQUAL_INT(0, PyList_Append(names, y_name));

	PyObject * const without = build_slots(names, false);
	PyObject * const with_weakref = build_slots(names, true);

	TEST_ASSERT_NOT_NULL(without);
	TEST_ASSERT_NOT_NULL(with_weakref);
	TEST_ASSERT_EQUAL_INT(2, PyTuple_GET_SIZE(without));
	TEST_ASSERT_EQUAL_INT(3, PyTuple_GET_SIZE(with_weakref));
	TEST_ASSERT_EQUAL_INT(
		0,
		PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(with_weakref, 2), "__weakref__")
	);

	Py_DECREF(with_weakref);
	Py_DECREF(without);
	Py_DECREF(y_name);
	Py_DECREF(x_name);
	Py_DECREF(names);
}

void slots_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_the_slot_names_answer_for_each_shape);
	RUN_TEST(test_build_slots_appends_the_weakref_name_only_when_asked);
}

#endif
