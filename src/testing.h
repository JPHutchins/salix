#pragma once

#ifdef TESTING

#	include <Python.h>
#	include <unity.h>

PyObject * testing_evaluate(char const * source);
PyObject * testing_entry(PyObject * fixtures, char const * name);
PyObject * testing_two_field_instance(void);
PyObject * testing_frozen_empty_instance(void);

void class_tests(void);
void comparison_tests(void);
void construct_tests(void);
void copy_tests(void);
void deepcopy_tests(void);
void exceptions_tests(void);
void fields_tests(void);
void forms_tests(void);
void handoff_tests(void);
void init_owner_tests(void);
void meta_tests(void);
void mixin_tests(void);
void mro_tests(void);
void options_tests(void);
void owned_tests(void);
void replace_tests(void);
void repr_tests(void);
void restore_tests(void);
void slots_tests(void);

#endif
