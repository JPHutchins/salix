/*
 * Runner for the in-file tests. The module is registered before the
 * interpreter starts so a test can `import salix` and get this build rather
 * than whatever is installed.
 */
#include <Python.h>
#include <unity.h>

#include "../../src/testing.h"

PyMODINIT_FUNC PyInit_salix(void);

void setUp(void) {}

void tearDown(void) {
	/* A test that leaves an exception set would otherwise fail the next one. */
	if (PyErr_Occurred() != NULL) {
		PyErr_Print();
		TEST_FAIL_MESSAGE("the test left an exception set");
	}
}

int main(void) {
	if (PyImport_AppendInittab("salix", PyInit_salix) < 0) {
		return 1;
	}

	Py_Initialize();

	UNITY_BEGIN();

	construct_tests();
	fields_tests();
	meta_tests();
	class_tests();
	options_tests();
	owned_tests();
	repr_tests();

	int const failures = UNITY_END();

	Py_Finalize();

	return failures;
}
