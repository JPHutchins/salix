#ifdef TESTING

#	include <Python.h>
#	include <unity.h>

#	include "owned.h"
#	include "testing.h"

static void test_a_moved_reference_leaves_nothing_behind(void) {
	PyObject * const value = PyList_New(0);
	Py_ssize_t const before = Py_REFCNT(value);
	PyObject * source = value;
	PyObject * const moved = py_move(&source);

	TEST_ASSERT_EQUAL_PTR(value, moved);
	TEST_ASSERT_NULL(source);
	TEST_ASSERT_EQUAL_INT(before, Py_REFCNT(value));

	Py_DECREF(moved);
}

static void test_a_scope_releases_what_it_owns(void) {
	PyObject * const value = PyList_New(0);
	Py_ssize_t const before = Py_REFCNT(value);

	{
		PY_OWNED(owned, Py_NewRef(value));

		TEST_ASSERT_EQUAL_INT(before + 1, Py_REFCNT(owned));
	}

	TEST_ASSERT_EQUAL_INT(before, Py_REFCNT(value));

	Py_DECREF(value);
}

static void test_a_scope_releases_nothing_after_a_move(void) {
	PyObject * const value = PyList_New(0);
	Py_ssize_t const before = Py_REFCNT(value);
	PyObject * escaped = NULL;

	{
		PY_MOVABLE(movable, Py_NewRef(value));

		escaped = py_move(&movable);
	}

	TEST_ASSERT_EQUAL_INT(before + 1, Py_REFCNT(value));

	Py_DECREF(escaped);
	Py_DECREF(value);
}

void owned_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_moved_reference_leaves_nothing_behind);
	RUN_TEST(test_a_scope_releases_what_it_owns);
	RUN_TEST(test_a_scope_releases_nothing_after_a_move);
}

PyObject * testing_evaluate(char const * const source) {
	PyObject * const globals = PyDict_New();

	if (globals == NULL) {
		TEST_FAIL_MESSAGE("could not allocate a namespace");
	}

	PyObject * const prelude = PyRun_String(
		"import salix\nfrom salix import Struct\n",
		Py_file_input,
		globals,
		globals
	);

	if (prelude == NULL) {
		PyErr_Print();
		TEST_FAIL_MESSAGE("could not import salix");
	}

	Py_DECREF(prelude);

	PyObject * const body = PyRun_String(source, Py_file_input, globals, globals);

	if (body == NULL) {
		PyErr_Print();
		TEST_FAIL_MESSAGE(source);
	}

	Py_DECREF(body);

	PyObject * const result = PyDict_GetItemString(globals, "result");

	if (result == NULL) {
		TEST_FAIL_MESSAGE("the source bound no `result`");
	}

	Py_INCREF(result);
	Py_DECREF(globals);

	return result;
}

#endif
