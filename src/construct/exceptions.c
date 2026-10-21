#include <Python.h>

#include "construct.h"
#include "../meta/meta.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static enum result derive_members_string(PyObject * self);
#if PY_VERSION_HEX >= 0x030B0000
static enum result store_args_from_member_fields(
	StructType * type,
	PyObject * self,
	PyObject * msg
);
#endif

static PyObject * shared_or_deepcopied(
	PyObject * const value,
	PyObject * const deepcopier,
	PyObject * const memo
) {
	return (
		deepcopier != NULL ? PyObject_CallFunctionObjArgs(deepcopier, value, memo, NULL) :
		Py_NewRef(value)
	);
}

int change_names_touch(
	StructType * const type,
	PyObject * const keyword_names,
	Py_ssize_t const change_count,
	Py_ssize_t const field_index
) {
	if (field_index < 0 || keyword_names == NULL) {
		return 0;
	}

	PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, field_index);

	for (Py_ssize_t i = 0; i < change_count; ++i) {
		int const matches = PyObject_RichCompareBool(
			PyTuple_GET_ITEM(keyword_names, i),
			name,
			Py_EQ
		);

		if (matches < 0) {
			return -1;
		}

		if (matches == 1) {
			return 1;
		}
	}

	return 0;
}

#if PY_VERSION_HEX >= 0x030B0000
enum result group_members_from_fields(
	StructType * const type,
	PyObject * const self,
	PyObject * const msg_fallback
) {
	PY_MOVABLE(msg, NULL);

	if (type->struct_message_index >= 0) {
		PyObject * const bound = *struct_slot(type, self, type->struct_message_index);

		msg = bound != NULL ? PyObject_Str(bound) : PyUnicode_FromString("");
	} else if (msg_fallback != NULL) {
		msg = msg_fallback;
	} else {
		msg = PyUnicode_FromString("");
	}

	if (msg == NULL) {
		return RESULT_ERROR;
	}

	PY_MOVABLE(excs, NULL);

	if (type->struct_exceptions_index >= 0) {
		PyObject * const bound = *struct_slot(type, self, type->struct_exceptions_index);

		if (bound != NULL) {
			excs = PyTuple_Check(bound) ? Py_NewRef(bound) : PySequence_Tuple(bound);

			if (excs == NULL) {
				return RESULT_ERROR;
			}

			for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(excs); ++i) {
				if (!PyExceptionInstance_Check(PyTuple_GET_ITEM(excs, i))) {
					PyErr_Format(
						PyExc_ValueError,
						"Item %zd of second argument (exceptions) is not an exception",
						i
					);

					return RESULT_ERROR;
				}
			}
		}
	}

	if (excs == NULL) {
		excs = PyTuple_New(0);

		if (excs == NULL) {
			return RESULT_ERROR;
		}
	}

	return (
		store_args_from_member_fields(type, self, msg) != RESULT_OK ? RESULT_ERROR :
		carry_group_members(type, self, msg, excs, NULL, NULL, NULL)
	);
}
#endif

enum result carry_group_members(
	StructType * const type,
	PyObject * self,
	PyObject * const msg,
	PyObject * const excs,
	PyObject * const source,
	PyObject * const deepcopier,
	PyObject * const memo
) {
#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;

		Py_XSETREF(group->msg, (
			msg != NULL ? (
				shared_or_deepcopied(msg, deepcopier, memo)
			) :
			PyUnicode_FromString("")
		));

		if (group->msg == NULL) {
			return RESULT_ERROR;
		}

		Py_XSETREF(group->excs, (
			excs != NULL ? (
				shared_or_deepcopied(excs, deepcopier, memo)
			) :
			PyTuple_New(0)
		));

		if (group->excs == NULL) {
			return RESULT_ERROR;
		}

		PyObject * * const excs_str_slot = group_excs_str_slot(self);

		if (excs_str_slot != NULL) {
			Py_XSETREF(*excs_str_slot, (
				source != NULL && group->excs == ((PyBaseExceptionGroupObject *) source)->excs ?
				Py_XNewRef(group_excs_str(source)) :
				NULL
			));
		}

		return derive_members_string(self);
	}
#endif

	return RESULT_OK;
}

static enum result store_exception_args(PyObject * const self, PyObject * const args) {
	PyObject * old;

	STRUCT_BEGIN_CRITICAL_SECTION(self);
	old = ((PyBaseExceptionObject *) self)->args;
	((PyBaseExceptionObject *) self)->args = args;
	STRUCT_END_CRITICAL_SECTION();

	Py_XDECREF(old);

	return derive_members_string(self);
}

PyObject * exception_args_of(PyObject * const self) {
	PyObject * args;

	STRUCT_BEGIN_CRITICAL_SECTION(self);
	args = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
	STRUCT_END_CRITICAL_SECTION();

	return args;
}

static enum result derive_members_string(PyObject * const self) {
#if PY_VERSION_HEX >= 0x030B0000
	StructType const * const type = struct_type_of(self);

	if (!type->struct_group_family) {
		return RESULT_OK;
	}

	PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;
	PyObject * * const excs_str_slot = group_excs_str_slot(self);

	if (excs_str_slot == NULL || *excs_str_slot != NULL || group->excs == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(args, exception_args_of(self));

	if (args == NULL) {
		return RESULT_OK;
	}

	PyObject * const given = (
		type->struct_exceptions_index >= 0 ? *struct_slot(
			type,
			self,
			type->struct_exceptions_index
		) :
		NULL
	);
	bool const given_is_custom = given != NULL && !PyList_Check(given) && !PyTuple_Check(given);

	if (!given_is_custom && PyTuple_GET_SIZE(args) >= 2) {
		return RESULT_OK;
	}

	PyObject * const members_string = PyObject_Repr(given != NULL ? given : group->excs);

	if (members_string == NULL) {
		return RESULT_ERROR;
	}

	Py_XSETREF(*excs_str_slot, members_string);
#endif

	return RESULT_OK;
}

enum result install_empty_exception_payload(StructType * const type, PyObject * const self) {
	if (!PyExceptionInstance_Check(self)) {
		return RESULT_OK;
	}

	PY_OWNED(no_args, PyTuple_New(0));

	return (
		(
			no_args == NULL ||
			carry_group_members(type, self, NULL, no_args, NULL, NULL, NULL) != RESULT_OK
		) ? RESULT_ERROR :
		store_exception_args(self, Py_NewRef(no_args))
	);
}

#if PY_VERSION_HEX >= 0x030B0000
enum result store_group_args(
	StructType * const type,
	PyObject * const self,
	bool const from_fields
) {
	if (from_fields) {
		return store_args_from_member_fields(
			type,
			self,
			((PyBaseExceptionGroupObject *) self)->msg
		);
	}

	bool const has_message = type->struct_message_index >= 0;
	bool const has_exceptions = type->struct_exceptions_index >= 0;
	PY_MOVABLE(packed, NULL);

	{
		PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;
		PyObject * const given = (
			has_exceptions ? *struct_slot(type, self, type->struct_exceptions_index) :
			NULL
		);
		PyObject * const members = given != NULL ? given : group->excs;

		if (has_exceptions && type->struct_field_count >= 2) {
			packed = PyTuple_Pack(2, group->msg, members);
		} else if (has_exceptions) {
			packed = PyTuple_Pack(1, members);
		} else if (has_message) {
			packed = PyTuple_Pack(1, group->msg);
		} else {
			return RESULT_OK;
		}
	}

	if (packed == NULL) {
		return RESULT_ERROR;
	}

	return store_exception_args(self, py_move(&packed));
}
#endif

#if PY_VERSION_HEX >= 0x030B0000
static enum result store_args_from_member_fields(
	StructType * const type,
	PyObject * const self,
	PyObject * const msg
) {
	PyObject * const message = (
		type->struct_message_index >= 0 ? *struct_slot(type, self, type->struct_message_index) :
		NULL
	);
	PyObject * const exceptions = (
		type->struct_exceptions_index >= 0 ? *struct_slot(
			type,
			self,
			type->struct_exceptions_index
		) :
		NULL
	);
	PY_MOVABLE(packed, NULL);

	if (message != NULL && exceptions != NULL) {
		packed = PyTuple_Pack(2, message, exceptions);
	} else if (message != NULL) {
		packed = PyTuple_Pack(1, message);
	} else if (exceptions != NULL && type->struct_field_count >= 2) {
		packed = PyTuple_Pack(2, msg, exceptions);
	} else if (exceptions != NULL) {
		packed = PyTuple_Pack(1, exceptions);
	} else {
		return RESULT_OK;
	}

	return packed == NULL ? RESULT_ERROR : store_exception_args(self, py_move(&packed));
}
#endif

enum result set_exception_args_from_fields(
	StructType * const type,
	PyObject * const self,
	Py_ssize_t const field_count
) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return RESULT_OK;
	}

	Py_ssize_t bound_count = 0;

	while (bound_count < field_count && *struct_slot(type, self, bound_count) != NULL) {
		bound_count += 1;
	}

	PY_MOVABLE(args, PyTuple_New(bound_count));

	if (args == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 0; i < bound_count; ++i) {
		PyTuple_SET_ITEM(args, i, Py_NewRef(*struct_slot(type, self, i)));
	}

	return store_exception_args(self, py_move(&args));
}

static PyObject * family_called_with(
	StructType * const type,
	PyObject * const args,
	PyObject * const deepcopier,
	PyObject * const memo
) {
	PY_OWNED(arguments, shared_or_deepcopied(args, deepcopier, memo));

	return (
		arguments != NULL ? PyObject_Call((PyObject *) &type->heap_type.ht_type, arguments, NULL) :
		NULL
	);
}

PyObject * family_rebuilt_copy(
	StructType * const type,
	PyObject * const self,
	PyObject * const deepcopier,
	PyObject * const memo
) {
	PY_OWNED(values_snapshot, PyTuple_New(type->struct_field_count));

	if (values_snapshot == NULL) {
		return NULL;
	}

	struct_slots_ref_into(type, self, values_snapshot, NULL);

	PY_OWNED(args, exception_args_of(self));
	PY_MOVABLE(copy, (
		args != NULL && PyTuple_GET_SIZE(args) > 0 ? family_called_with(type, args, deepcopier, memo) :
		allocated_struct_copy(type)
	));

	if (copy == NULL) {
		return NULL;
	}

	struct_slots_set_from(type, copy, values_snapshot);

	return py_move(&copy);
}

enum result set_exception_args_from_original(
	StructType * const type,
	PyObject * const copy,
	PyObject * const original,
	PyObject * const deepcopier,
	PyObject * const memo
) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return RESULT_OK;
	}

	PY_OWNED(args, exception_args_of(original));
	PY_MOVABLE(copied_args, (
		args != NULL ? shared_or_deepcopied(args, deepcopier, memo) :
		PyTuple_New(0)
	));

	if (copied_args == NULL) {
		return RESULT_ERROR;
	}

	return store_exception_args(copy, py_move(&copied_args));
}

enum result set_exception_args_from_positionals(
	PyTypeObject * const cls,
	PyObject * const self,
	PyObject * const positionals
) {
	return (
		is_exception_struct(cls) ? store_exception_args(self, Py_XNewRef(positionals)) :
		RESULT_OK
	);
}

Py_ssize_t explicit_field_prefix(
	StructType const * const type,
	Py_ssize_t const positional_count,
	PyObject * const keyword_names,
	Py_ssize_t const carried_count
) {
	Py_ssize_t const keyword_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		bool explicit = prefix < positional_count || prefix < carried_count;

		for (Py_ssize_t i = 0; !explicit && i < keyword_count; ++i) {
			struct field_lookup const found = find_field(type, PyTuple_GET_ITEM(keyword_names, i));

			if (found.tag == FIELD_LOOKUP_ERROR) {
				return -1;
			}

			explicit = found.tag == FIELD_LOOKUP_FOUND && found.index == prefix;
		}

		if (!explicit) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

Py_ssize_t carried_payload_count(StructType * const type, PyObject * const self) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return 0;
	}

	PY_OWNED(args, exception_args_of(self));

	return args != NULL ? PyTuple_GET_SIZE(args) : 0;
}

Py_ssize_t explicit_dict_prefix(StructType const * const type, PyObject * const values) {
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		int const present = PyDict_Contains(
			values,
			PyTuple_GET_ITEM(type->struct_field_names, prefix)
		);

		if (present < 0) {
			return -1;
		}

		if (present == 0) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

Py_ssize_t explicit_items_prefix(
	StructType const * const type,
	PyObject * const items,
	Py_ssize_t const entry_count
) {
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, prefix);
		bool explicit = false;

		for (Py_ssize_t i = 0; i < entry_count; ++i) {
			PyObject * const pair = PySequence_Fast_GET_ITEM(items, i);
			int const compared = PyObject_RichCompareBool(name, PyTuple_GET_ITEM(pair, 0), Py_EQ);

			if (compared < 0) {
				return -1;
			}

			if (compared == 1) {
				explicit = true;
				break;
			}
		}

		if (!explicit) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

#ifdef TESTING

#	include "../testing.h"

static char const exceptions_source[] = {
#	embed "../../tests/c/fixtures/construct/exceptions.py" suffix(, '\0')
};

#	if PY_VERSION_HEX >= 0x030B0000
static char const members_string_source[] = {
#		embed "../../tests/c/fixtures/construct/members_string.py" suffix(, '\0')
};

static char const group_members_source[] = {
#		embed "../../tests/c/fixtures/construct/group_members.py" suffix(, '\0')
};
#	endif

static PyObject * exception_instance(void) {
	return testing_fixture(exceptions_source, "exception");
}

static PyObject * beta_named_instance(void) {
	return testing_fixture(exceptions_source, "beta_named");
}

static void test_the_explicit_prefix_stops_at_the_first_unexplicit_field(void) {
	PyObject * const plain = testing_two_field_instance();
	StructType * const type = struct_type_of(plain);
	PyObject * const beta_name = PyTuple_Pack(1, PyTuple_GET_ITEM(type->struct_field_names, 1));

	TEST_ASSERT_EQUAL_INT(2, explicit_field_prefix(type, 2, NULL, 0));

	PyObject * const named = beta_named_instance();
	TEST_ASSERT_EQUAL_INT(2, explicit_field_prefix(type, 1, beta_name, 0));

	TEST_ASSERT_EQUAL_INT(0, explicit_field_prefix(type, 0, beta_name, 0));

	Py_DECREF(beta_name);
	Py_DECREF(named);
	Py_DECREF(plain);
}

static void test_the_explicit_prefix_counts_carried_positionals(void) {
	PyObject * const instance = testing_two_field_instance();

	TEST_ASSERT_EQUAL_INT(2, explicit_field_prefix(struct_type_of(instance), 0, NULL, 2));

	Py_DECREF(instance);
}

static void test_change_names_touch_answers_for_each_change_shape(void) {
	PyObject * const instance = testing_two_field_instance();
	StructType * const type = struct_type_of(instance);
	PyObject * const beta_name = PyTuple_Pack(1, PyTuple_GET_ITEM(type->struct_field_names, 1));

	TEST_ASSERT_EQUAL_INT(1, change_names_touch(type, beta_name, 1, 1));
	TEST_ASSERT_EQUAL_INT(0, change_names_touch(type, beta_name, 1, 0));
	TEST_ASSERT_EQUAL_INT(0, change_names_touch(type, NULL, 1, 1));
	TEST_ASSERT_EQUAL_INT(0, change_names_touch(type, beta_name, 1, -1));

	Py_DECREF(beta_name);
	Py_DECREF(instance);
}

static void test_the_carried_payload_count_reads_the_stored_args(void) {
	PyObject * const exception = exception_instance();
	PyObject * const plain = testing_two_field_instance();

	TEST_ASSERT_EQUAL_INT(1, carried_payload_count(struct_type_of(exception), exception));
	TEST_ASSERT_EQUAL_INT(0, carried_payload_count(struct_type_of(plain), plain));

	Py_DECREF(plain);
	Py_DECREF(exception);
}

#	if PY_VERSION_HEX >= 0x030B0000

static PyObject * group_instance(void) {
	return testing_fixture(group_members_source, "group");
}

static void test_the_group_members_carry_writes_the_bound_fields(void) {
	PyObject * const instance = group_instance();
	StructType * const type = struct_type_of(instance);
	PyObject * const shell = type->heap_type.ht_type.tp_alloc(&type->heap_type.ht_type, 0);
	PyObject * const message = PyUnicode_FromString("boom");
	PyObject * const exceptions = PyList_New(1);
	PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) shell;

	TEST_ASSERT_NOT_NULL(shell);
	TEST_ASSERT_NOT_NULL(message);
	TEST_ASSERT_NOT_NULL(exceptions);
	TEST_ASSERT_NULL(group->msg);
	TEST_ASSERT_NULL(group->excs);

	PyList_SET_ITEM(exceptions, 0, PyObject_CallNoArgs(PyExc_ValueError));

	TEST_ASSERT_EQUAL_INT(RESULT_OK, write_slot(type, shell, type->struct_message_index, message));
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		write_slot(type, shell, type->struct_exceptions_index, exceptions)
	);
	TEST_ASSERT_EQUAL_INT(RESULT_OK, group_members_from_fields(type, shell, NULL));

	TEST_ASSERT_NOT_NULL(group->msg);
	TEST_ASSERT_EQUAL_INT(0, PyUnicode_CompareWithASCIIString(group->msg, "boom"));
	TEST_ASSERT_NOT_NULL(group->excs);
	TEST_ASSERT_TRUE(PyTuple_Check(group->excs));
	TEST_ASSERT_EQUAL_INT(1, PyTuple_GET_SIZE(group->excs));
	TEST_ASSERT_TRUE(PyExceptionInstance_Check(PyTuple_GET_ITEM(group->excs, 0)));
	TEST_ASSERT_NULL(group_excs_str(shell));

	TEST_ASSERT_EQUAL_INT(RESULT_OK, store_group_args(type, shell, true));

	PY_OWNED(stored, exception_args_of(shell));

	TEST_ASSERT_EQUAL_INT(2, PyTuple_GET_SIZE(stored));
	TEST_ASSERT_EQUAL_PTR(exceptions, PyTuple_GET_ITEM(stored, 1));
	TEST_ASSERT_NULL(group_excs_str(shell));

	Py_DECREF(exceptions);
	Py_DECREF(message);
	Py_DECREF(shell);
	Py_DECREF(instance);
}

static void test_a_group_built_without_member_fields_caches_its_members_string(void) {
	PyObject * const fixtures = testing_evaluate(members_string_source);
	PyObject * const instance = testing_entry(fixtures, "fieldless");

	TEST_ASSERT_NOT_NULL(instance);
	PY_OWNED(stored, exception_args_of(instance));

	TEST_ASSERT_EQUAL_INT(1, PyTuple_GET_SIZE(stored));
	TEST_ASSERT_EQUAL(group_layout_has_excs_str(), group_excs_str(instance) != NULL);

	Py_DECREF(fixtures);
}

static void test_an_empty_group_payload_caches_its_members_string(void) {
	PyObject * const instance = group_instance();
	StructType * const type = struct_type_of(instance);
	PyObject * const shell = type->heap_type.ht_type.tp_alloc(&type->heap_type.ht_type, 0);

	TEST_ASSERT_NOT_NULL(shell);
	TEST_ASSERT_EQUAL_INT(RESULT_OK, install_empty_exception_payload(type, shell));
	PY_OWNED(stored, exception_args_of(shell));

	TEST_ASSERT_EQUAL_INT(0, PyTuple_GET_SIZE(stored));
	TEST_ASSERT_EQUAL(group_layout_has_excs_str(), group_excs_str(shell) != NULL);
	TEST_ASSERT_TRUE(
		!group_layout_has_excs_str() ||
		PyUnicode_CompareWithASCIIString(group_excs_str(shell), "()") == 0
	);

	Py_DECREF(shell);
	Py_DECREF(instance);
}

static void test_the_group_layout_probe_matches_the_interpreter(void) {
	TEST_ASSERT_EQUAL(
		(
			(PY_VERSION_HEX >= 0x030D0C00 && PY_VERSION_HEX < 0x030E0000) ||
			PY_VERSION_HEX >= 0x030E0400
		),
		group_layout_has_excs_str()
	);
}

#	endif

void exceptions_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_the_explicit_prefix_stops_at_the_first_unexplicit_field);
	RUN_TEST(test_the_explicit_prefix_counts_carried_positionals);
	RUN_TEST(test_change_names_touch_answers_for_each_change_shape);
	RUN_TEST(test_the_carried_payload_count_reads_the_stored_args);
#	if PY_VERSION_HEX >= 0x030B0000
	RUN_TEST(test_the_group_members_carry_writes_the_bound_fields);
	RUN_TEST(test_a_group_built_without_member_fields_caches_its_members_string);
	RUN_TEST(test_an_empty_group_payload_caches_its_members_string);
	RUN_TEST(test_the_group_layout_probe_matches_the_interpreter);
#	endif
}

#endif
