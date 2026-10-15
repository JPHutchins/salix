#include <Python.h>

#include "deepcopy.h"
#include "../../construct/construct.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"

static PyObject * memo_failure(PyObject * const memo, PyObject * const key) {
	PyObject * error_type = NULL, *error_value = NULL, *traceback = NULL;

	PyErr_Fetch(&error_type, &error_value, &traceback);

	if (PyDict_DelItem(memo, key) < 0) {
		PyErr_Clear();
	}

	PyErr_Restore(error_type, error_value, traceback);

	return NULL;
}

static PyObject * allocated_copy(StructType * const type) {
	PyTypeObject * const cls = &type->heap_type.ht_type;
	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (copy != NULL && install_empty_exception_payload(type, copy) != RESULT_OK) {
		return NULL;
	}

	return py_move(&copy);
}

PyObject * Struct_deepcopy(PyObject * const self, PyObject * const memo) {
	if (!PyDict_Check(memo)) {
		PyErr_SetString(PyExc_TypeError, "__deepcopy__() argument must be a dict");

		return NULL;
	}

	PY_OWNED(key, PyLong_FromVoidPtr(self));
	enum memo_none_entry const none_entry = (
		key != NULL ? copy_reads_memo_none() :
		MEMO_NONE_ENTRY_ERROR
	);

	if (none_entry == MEMO_NONE_ENTRY_ERROR) {
		return NULL;
	}

	PY_MOVABLE(seeded, memo_entry(memo, key, none_entry));

	if (seeded == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (seeded != NULL) {
		return py_move(&seeded);
	}

	if (!is_struct(self)) {
		PY_MOVABLE(
			delegated,
			copy_delegate(self, memo, memo, COPY_DEEP_HOOK_NAME, "un(deep)copyable", true)
		);

		if (delegated == NULL || (delegated != self && PyDict_SetItem(memo, key, delegated) < 0)) {
			return NULL;
		}

		return py_move(&delegated);
	}

	StructType * const type = struct_type_of(self);
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", cls->tp_name);

		return NULL;
	}

	PY_MOVABLE(copy_module, NULL);
	PY_MOVABLE(copier, NULL);
	PY_MOVABLE(
		deferred,
		copy_dispatch_prologue(self, COPY_DEEP_HOOK_NAME, memo, true, &copy_module, &copier)
	);

	if (deferred != NULL) {
		if (deferred != self && PyDict_SetItem(memo, key, deferred) < 0) {
			return NULL;
		}

		return py_move(&deferred);
	}

	if (PyErr_Occurred()) {
		return NULL;
	}

	PY_MOVABLE(
		reduced,
		copier != NULL || type->struct_copies_through_reduce ? copy_reduction(self, copier) : NULL
	);

	if (reduced == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (reduced != NULL) {
		PY_MOVABLE(reconstructed, copy_reconstruct(self, reduced, copy_module, memo));

		if (
			reconstructed == NULL ||
			(reconstructed != self && PyDict_SetItem(memo, key, reconstructed) < 0)
		) {
			return NULL;
		}

		return py_move(&reconstructed);
	}

	if (refused_until_created(type, "deepcopy")) {
		return NULL;
	}

	PY_MOVABLE(short_circuit, interned_copy(type));

	if (short_circuit != NULL) {
		return py_move(&short_circuit);
	}

	PY_OWNED(
		deepcopy,
		PyObject_GetAttr(copy_module, type->struct_state->copy_cache[COPY_DEEPCOPY_NAME])
	);

	if (deepcopy == NULL) {
		return memo_failure(memo, key);
	}

	PY_OWNED(tables, deepcopy_tables(type->struct_state, deepcopy));

	if (tables == NULL) {
		return memo_failure(memo, key);
	}

	PyObject * const mirrored = (
		PyDict_CheckExact(
			memo
		) && PyTuple_GET_ITEM(tables, DEEPCOPY_FUNCTION) == deepcopy ? tables :
		NULL
	);

	PY_MOVABLE(copy, NULL);

	if (type->struct_family_owned) {
		PY_OWNED(values_snapshot, PyTuple_New(type->struct_field_count));

		if (values_snapshot == NULL) {
			return NULL;
		}

		struct_slots_ref_into(type, self, values_snapshot, NULL);

		PyObject * args;

		STRUCT_BEGIN_CRITICAL_SECTION(self);
		args = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
		STRUCT_END_CRITICAL_SECTION();

		if (args == NULL) {
			args = PyTuple_New(0);
		}

		if (args == NULL) {
			return NULL;
		}

		if (PyTuple_GET_SIZE(args) > 0) {
			PY_OWNED(deep_args, PyObject_CallFunctionObjArgs(deepcopy, args, memo, NULL));
			Py_DECREF(args);
			copy = deep_args != NULL ? PyObject_Call((PyObject *) cls, deep_args, NULL) : NULL;

			if (copy == NULL) {
				return NULL;
			}
		} else {
			Py_DECREF(args);
			copy = allocated_copy(type);

			if (copy == NULL) {
				return NULL;
			}
		}

		for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
			PyObject * const value = PyTuple_GET_ITEM(values_snapshot, i);

			if (value != NULL) {
				Py_XSETREF(*struct_slot(type, copy, i), Py_NewRef(value));
			}
		}
	} else {
		copy = allocated_copy(type);
	}

	if (copy == NULL) {
		return NULL;
	}

	if (PyDict_SetItem(memo, key, copy) < 0) {
		return NULL;
	}

	PY_MOVABLE(dict, NULL);
	struct_slots_copy_into(type, self, copy, &dict);

	for (Py_ssize_t i = 0; i < type->struct_field_count; i += 1) {
		PyObject * const value = *struct_slot(type, copy, i);

		if (value != NULL) {
			PY_MOVABLE(
				deep,
				mirrored != NULL ? deep_value(mirrored, memo, value) :
				PyObject_CallFunctionObjArgs(deepcopy, value, memo, NULL)
			);

			if (deep == NULL) {
				return memo_failure(memo, key);
			}

			Py_SETREF(*struct_slot(type, copy, i), py_move(&deep));
		}
	}

	for (Py_ssize_t i = 0; i < type->struct_member_count; i += 1) {
		Py_ssize_t const offset = type->struct_member_offsets[i];
		PyObject * const value = *(PyObject * *) ((char *) copy + offset);

		if (value != NULL) {
			PY_MOVABLE(
				deep,
				mirrored != NULL ? deep_value(mirrored, memo, value) :
				PyObject_CallFunctionObjArgs(deepcopy, value, memo, NULL)
			);

			if (deep == NULL) {
				return memo_failure(memo, key);
			}

			Py_SETREF(*((PyObject * *) ((char *) copy + offset)), py_move(&deep));
		}
	}

	if (dict != NULL) {
		PY_OWNED(snapshot, PyDict_Copy(dict));

		if (snapshot == NULL) {
			return memo_failure(memo, key);
		}

		PY_MOVABLE(deep_dict, PyObject_CallFunctionObjArgs(deepcopy, snapshot, memo, NULL));

		if (deep_dict == NULL) {
			return memo_failure(memo, key);
		}

		if (PyObject_GenericSetDict(copy, deep_dict, NULL) < 0) {
			return memo_failure(memo, key);
		}
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyBaseExceptionGroupObject * const source_group = (PyBaseExceptionGroupObject *) self;

		if (
			carry_group_members(
				type,
				copy,
				source_group->msg,
				source_group->excs,
				group_excs_str(self),
				deepcopy,
				memo
			) != RESULT_OK
		) {
			return memo_failure(memo, key);
		}
	}
#endif

	if (set_exception_args_from_original(type, copy, self, deepcopy, memo) != RESULT_OK) {
		return memo_failure(memo, key);
	}

	return py_move(&copy);
}

#ifdef TESTING

#	include "../../testing.h"

static void test_a_deepcopy_is_equal_and_distinct(void) {
	PyObject * const source = testing_two_field_instance();
	PyObject * const memo = PyDict_New();
	PyObject * const copy = Struct_deepcopy(source, memo);

	TEST_ASSERT_NOT_NULL(memo);
	TEST_ASSERT_NOT_NULL(copy);
	TEST_ASSERT_NOT_EQUAL(source, copy);
	TEST_ASSERT_EQUAL_INT(1, PyObject_RichCompareBool(copy, source, Py_EQ));

	Py_DECREF(copy);
	Py_DECREF(memo);
	Py_DECREF(source);
}

static void test_a_seeded_memo_answers_for_deepcopy(void) {
	PyObject * const source = testing_two_field_instance();
	PyObject * const memo = PyDict_New();
	PyObject * const key = PyLong_FromVoidPtr(source);
	PyObject * const sentinel = PyList_New(0);

	TEST_ASSERT_NOT_NULL(memo);
	TEST_ASSERT_NOT_NULL(key);
	TEST_ASSERT_NOT_NULL(sentinel);
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItem(memo, key, sentinel));
	TEST_ASSERT_EQUAL_PTR(sentinel, Struct_deepcopy(source, memo));

	Py_DECREF(sentinel);
	Py_DECREF(key);
	Py_DECREF(memo);
	Py_DECREF(source);
}

static void test_a_failed_memo_restore_keeps_the_error_and_the_contract(void) {
	PyObject * const memo = PyDict_New();
	PyObject * const key = PyLong_FromVoidPtr(memo);
	PyObject * const value = PyList_New(0);

	TEST_ASSERT_NOT_NULL(memo);
	TEST_ASSERT_NOT_NULL(key);
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItem(memo, key, value));

	PyErr_SetString(PyExc_ValueError, "kept");

	TEST_ASSERT_NULL(memo_failure(memo, key));
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
	TEST_ASSERT_EQUAL_INT(0, PyDict_Contains(memo, key));
	PyErr_Clear();

	Py_DECREF(value);
	Py_DECREF(key);
	Py_DECREF(memo);
}

static void test_the_stdlib_deepcopy_is_mirrored(void) {
	PyObject * const source = testing_two_field_instance();
	PyObject * const memo = PyDict_New();
	PyObject * const copy = Struct_deepcopy(source, memo);
	PyObject * const copy_module = PyImport_ImportModule("copy");
	PyObject * const deepcopy = PyObject_GetAttrString(copy_module, "deepcopy");
	PyObject * const tables = struct_type_of(source)->struct_state->deepcopy_tables;

	TEST_ASSERT_NOT_NULL(copy);
	TEST_ASSERT_NOT_NULL(deepcopy);
	TEST_ASSERT_NOT_NULL(tables);
	TEST_ASSERT_EQUAL_PTR(deepcopy, PyTuple_GET_ITEM(tables, DEEPCOPY_FUNCTION));

	Py_DECREF(deepcopy);
	Py_DECREF(copy_module);
	Py_DECREF(copy);
	Py_DECREF(memo);
	Py_DECREF(source);
}

void deepcopy_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_deepcopy_is_equal_and_distinct);
	RUN_TEST(test_a_seeded_memo_answers_for_deepcopy);
	RUN_TEST(test_a_failed_memo_restore_keeps_the_error_and_the_contract);
	RUN_TEST(test_the_stdlib_deepcopy_is_mirrored);
}

#endif
