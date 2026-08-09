#pragma once

#ifdef TESTING

#	include <Python.h>
#	include <unity.h>

/* Run `source` as a module body with `salix` already imported, and return
 * the value bound to `result`. Aborts the test on any Python error. */
PyObject * testing_evaluate(char const * source);

void construct_tests(void);
void fields_tests(void);
void meta_tests(void);
void options_tests(void);
void owned_tests(void);
void repr_tests(void);

#endif
