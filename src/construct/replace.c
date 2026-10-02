#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

PyObject * Struct_replace(
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const nargs,
	PyObject * const keyword_names
) {
	if (nargs != 0) {
		PyErr_Format(
			PyExc_TypeError,
			"%s.__replace__() takes exactly one positional argument (%zd given)",
			Py_TYPE(self)->tp_name,
			nargs
		);

		return NULL;
	}

	if (!is_struct(self)) {
		PyErr_Format(PyExc_TypeError, "%s object is not replaceable", Py_TYPE(self)->tp_name);

		return NULL;
	}

	StructType * const type = struct_type_of(self);
	Py_ssize_t const change_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;

	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", struct_type_name(type));

		return NULL;
	}

	if (change_count == 0 && type->struct_options.frozen) {
		return Py_NewRef(self);
	}

	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (type->struct_own_init) {
		for (Py_ssize_t i = 0; i < change_count; ++i) {
			if (named_field(type, PyTuple_GET_ITEM(keyword_names, i)).tag != FIELD_LOOKUP_FOUND) {
				return NULL;
			}
		}

		PY_OWNED(values, PyTuple_New(type->struct_field_count));

		if (values == NULL) {
			return NULL;
		}

		struct_slots_ref_into(type, self, values, NULL);

		PY_MOVABLE(replaced, NULL);

		if (type->struct_family_owned || type->struct_group_family) {
			PY_MOVABLE(positionals, NULL);

			STRUCT_BEGIN_CRITICAL_SECTION(self);
			positionals = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
			STRUCT_END_CRITICAL_SECTION();

			if (positionals == NULL) {
				positionals = PyTuple_New(0);
			}

			if (positionals == NULL) {
				return NULL;
			}

			bool allocated_route = false;

			if (PyTuple_GET_SIZE(positionals) > 0) {
				replaced = cls->tp_new(cls, positionals, NULL);

				if (
					replaced == NULL &&
					PyErr_ExceptionMatches(PyExc_TypeError) &&
					!type->struct_author_new
				) {
					PyErr_Clear();
					allocated_route = true;
					replaced = cls->tp_alloc(cls, 0);
				} else if (replaced != NULL && !PyObject_TypeCheck(replaced, cls)) {
					PyErr_Format(
						PyExc_TypeError,
						"%s.__new__(%s) is not safe, use %s.__new__()",
						Py_TYPE(replaced)->tp_name,
						cls->tp_name,
						cls->tp_name
					);
					Py_CLEAR(replaced);
				} else if (replaced != NULL) {
					if (cls->tp_init != NULL && cls->tp_init(replaced, positionals, NULL) < 0) {
						Py_CLEAR(replaced);
					}
				}
			} else {
				allocated_route = true;
				replaced = cls->tp_alloc(cls, 0);
			}

			if (replaced != NULL) {
				for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
					PyObject * const value = PyTuple_GET_ITEM(values, i);

					if (value != NULL) {
						Py_XSETREF(*struct_slot(type, replaced, i), Py_NewRef(value));
					}
				}

				for (Py_ssize_t i = 0; i < change_count; ++i) {
					struct field_lookup const found = find_field(
						type,
						PyTuple_GET_ITEM(keyword_names, i)
					);

					if (found.tag != FIELD_LOOKUP_FOUND) {
						return NULL;
					}

					Py_XSETREF(
						*struct_slot(type, replaced, found.index),
						Py_NewRef(arguments[nargs + i])
					);
				}
#if PY_VERSION_HEX >= 0x030B0000
				if (type->struct_group_family) {
					PyBaseExceptionGroupObject * const source_group =
						(PyBaseExceptionGroupObject *) self;
					int const touched = change_names_touch(
						type,
						keyword_names,
						change_count,
						type->struct_exceptions_index
					);

					if (touched < 0) {
						return NULL;
					}

					if (touched == 1) {
						if (
							group_members_from_fields(
								type,
								replaced,
								type->struct_message_index >= 0 ? NULL :
								Py_XNewRef(source_group->msg)
							) != RESULT_OK
						) {
							return NULL;
						}

						if (store_group_args(type, replaced, false) != RESULT_OK) {
							return NULL;
						}
					} else if (allocated_route) {
						if (
							carry_group_members(
								type,
								replaced,
								source_group->msg,
								source_group->excs,
								group_excs_str(self),
								NULL,
								NULL
							) != RESULT_OK
						) {
							return NULL;
						}

						if (
							set_exception_args_from_original(
								type,
								replaced,
								self,
								NULL,
								NULL
							) != RESULT_OK
						) {
							return NULL;
						}
					}
				} else
#endif
				if (allocated_route) {
					if (
						set_exception_args_from_original(
							type,
							replaced,
							self,
							NULL,
							NULL
						) != RESULT_OK
					) {
						return NULL;
					}
				}
			}
		} else {
			PY_OWNED(changed, PyDict_New());

			if (changed == NULL) {
				return NULL;
			}

			for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
				PyObject * const value = PyTuple_GET_ITEM(values, i);

				if (value == NULL) {
					continue;
				}

				PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, i);
				int const present = PyDict_Contains(changed, name);

				if (present < 0) {
					return NULL;
				}

				if (present == 0 && PyDict_SetItem(changed, name, value) < 0) {
					return NULL;
				}
			}

			for (Py_ssize_t i = 0; i < change_count; ++i) {
				if (
					PyDict_SetItem(
						changed,
						PyTuple_GET_ITEM(keyword_names, i),
						arguments[nargs + i]
					) < 0
				) {
					return NULL;
				}
			}

			PY_OWNED(no_arguments, PyTuple_New(0));

			if (no_arguments == NULL) {
				return NULL;
			}

			replaced = PyObject_Call((PyObject *) cls, no_arguments, changed);
		}

		if (replaced == NULL) {
			return NULL;
		}

		if (!PyObject_TypeCheck(replaced, cls)) {
			PyErr_SetString(
				PyExc_SystemError,
				"salix internal error: the replace construction returned a different type"
			);

			return NULL;
		}

		if (!type->struct_family_owned) {
			for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
				PyObject * const value = PyTuple_GET_ITEM(values, i);
				PyObject * * const slot = struct_slot(type, replaced, i);

				if (value != NULL && *slot == NULL) {
					*slot = Py_NewRef(value);
				}
			}
		}

		PY_MOVABLE(source_dict, NULL);
		struct_slots_copy_into(type, self, replaced, &source_dict);

		if (source_dict != NULL && struct_dict_copy_merged(source_dict, replaced) < 0) {
			return NULL;
		}

		return py_move(&replaced);
	}

	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (copy == NULL) {
		return NULL;
	}

	if (bind_keywords(type, copy, arguments, 0, keyword_names) != RESULT_OK) {
		return NULL;
	}

	PY_MOVABLE(source_dict, NULL);
	struct_slots_copy_into(type, self, copy, &source_dict);

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyBaseExceptionGroupObject * const source_group = (PyBaseExceptionGroupObject *) self;
		int const touched = change_names_touch(
			type,
			keyword_names,
			change_count,
			type->struct_exceptions_index
		);

		if (touched < 0) {
			return NULL;
		}

		if (touched == 1) {
			if (
				group_members_from_fields(
					type,
					copy,
					type->struct_message_index >= 0 ? NULL : Py_XNewRef(source_group->msg)
				) != RESULT_OK
			) {
				return NULL;
			}
		} else if (
			carry_group_members(
				type,
				copy,
				source_group->msg,
				source_group->excs,
				group_excs_str(self),
				NULL,
				NULL
			) != RESULT_OK
		) {
			return NULL;
		}
	}
#endif

	Py_ssize_t const explicit_count = (
		is_exception_struct(
			cls
		) ? explicit_field_prefix(type, 0, keyword_names, carried_payload_count(type, self)) :
		0
	);

	if (explicit_count < 0) {
		return NULL;
	}

	if (
		set_exception_args_from_fields(type, copy, explicit_count) != RESULT_OK ||
		run_post_init(type, copy) != RESULT_OK ||
		(
			type->struct_post_init != NULL &&
			set_exception_args_from_fields(type, copy, explicit_count) != RESULT_OK
		)
	) {
		return NULL;
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		if (store_group_args(type, copy, false) != RESULT_OK) {
			return NULL;
		}
	}
#endif

	if (source_dict != NULL && struct_dict_copy_merged(source_dict, copy) < 0) {
		return NULL;
	}

	return py_move(&copy);
}

#ifdef TESTING

#	include "../testing.h"

static char const replace_source[] = {
#	embed "../../tests/c/fixtures/construct/replace.py" suffix(, '\0')
};

static void test_a_replace_changes_only_the_named_field(void) {
	PyObject * const fixtures = testing_evaluate(replace_source);
	PyObject * const replaced = Struct_replace(
		testing_entry(fixtures, "point"),
		(PyObject * const []){testing_entry(fixtures, "value")},
		0,
		testing_entry(fixtures, "keyword_y")
	);

	TEST_ASSERT_NOT_NULL(replaced);
	TEST_ASSERT_NOT_EQUAL(testing_entry(fixtures, "point"), replaced);
	TEST_ASSERT_EQUAL_PTR(
		*struct_slot(struct_type_of(replaced), testing_entry(fixtures, "point"), 0),
		*struct_slot(struct_type_of(replaced), replaced, 0)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "value"),
		*struct_slot(struct_type_of(replaced), replaced, 1)
	);
	TEST_ASSERT_EQUAL_INT(
		2,
		PyLong_AsLong(*struct_slot(struct_type_of(replaced), testing_entry(fixtures, "point"), 1))
	);

	Py_DECREF(replaced);
	Py_DECREF(fixtures);
}

static void test_a_replace_refuses_a_positional_argument(void) {
	PyObject * const fixtures = testing_evaluate(replace_source);

	TEST_ASSERT_NULL(
		Struct_replace(
			testing_entry(fixtures, "point"),
			(PyObject * const []){testing_entry(fixtures, "value")},
			1,
			NULL
		)
	);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();

	Py_DECREF(fixtures);
}

static void test_a_replace_refuses_a_name_that_is_not_a_field(void) {
	PyObject * const fixtures = testing_evaluate(replace_source);

	TEST_ASSERT_NULL(
		Struct_replace(
			testing_entry(fixtures, "point"),
			(PyObject * const []){testing_entry(fixtures, "value")},
			0,
			testing_entry(fixtures, "keyword_z")
		)
	);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();

	Py_DECREF(fixtures);
}

static void test_a_replace_refuses_a_receiver_that_is_not_a_struct(void) {
	PyObject * const fixtures = testing_evaluate(replace_source);

	TEST_ASSERT_NULL(Struct_replace(testing_entry(fixtures, "not_a_struct"), NULL, 0, NULL));
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();

	Py_DECREF(fixtures);
}

void replace_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_replace_changes_only_the_named_field);
	RUN_TEST(test_a_replace_refuses_a_positional_argument);
	RUN_TEST(test_a_replace_refuses_a_name_that_is_not_a_field);
	RUN_TEST(test_a_replace_refuses_a_receiver_that_is_not_a_struct);
}

#endif
