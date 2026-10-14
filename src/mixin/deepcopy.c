#include <Python.h>

#include "mixin.h"
#include "../construct/construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static PyObject * memo_failure(PyObject * const memo, PyObject * const key) {
	PyObject * error_type = NULL, *error_value = NULL, *traceback = NULL;

	PyErr_Fetch(&error_type, &error_value, &traceback);

	if (PyDict_DelItem(memo, key) < 0) {
		PyErr_Clear();
	}

	PyErr_Restore(error_type, error_value, traceback);

	return NULL;
}

static PyObject * optional_table(PyObject * const copy_module, char const * const name) {
	PyObject * const found = optional_attribute(copy_module, name);

	return found != NULL || PyErr_Occurred() ? found : Py_NewRef(Py_None);
}

static PyObject * deepcopy_tables_build(PyObject * const copy_module, PyObject * const deepcopy) {
	static char const * const names[] = {
		[DEEPCOPY_DISPATCH] = "_deepcopy_dispatch",
		[DEEPCOPY_ATOMIC_TYPES] = "_atomic_types",
		[DEEPCOPY_ATOMIC_COPIER] = "_deepcopy_atomic",
		[DEEPCOPY_LIST] = "_deepcopy_list",
	};
	PY_MOVABLE(tables, PyTuple_New(DEEPCOPY_TABLE_COUNT));

	if (tables == NULL) {
		return NULL;
	}

	for (enum deepcopy_table entry = DEEPCOPY_DISPATCH; entry < DEEPCOPY_TABLE_COUNT; entry += 1) {
		PyObject * const found = optional_table(copy_module, names[entry]);

		if (found == NULL) {
			return NULL;
		}

		PyTuple_SET_ITEM(tables, entry, found);
	}

	PyObject * const atomic_types = PyTuple_GET_ITEM(tables, DEEPCOPY_ATOMIC_TYPES);
	PyObject * const list_copier = PyTuple_GET_ITEM(tables, DEEPCOPY_LIST);
	PyObject * const list_copier_defaults = (
		PyFunction_Check(list_copier) ? PyFunction_GET_DEFAULTS(list_copier) :
		NULL
	);
	bool const mirrorable = (
		list_copier_defaults != NULL &&
		PyTuple_GET_SIZE(list_copier_defaults) == 1 &&
		PyTuple_GET_ITEM(list_copier_defaults, 0) == deepcopy &&
		PyDict_CheckExact(PyTuple_GET_ITEM(tables, DEEPCOPY_DISPATCH)) &&
		(atomic_types == Py_None || PyAnySet_CheckExact(atomic_types))
	);

	PyTuple_SET_ITEM(tables, DEEPCOPY_FUNCTION, Py_NewRef(mirrorable ? deepcopy : Py_None));

	return py_move(&tables);
}

static PyObject * deepcopy_tables(
	struct salix_state * const state,
	PyObject * const copy_module,
	PyObject * const deepcopy
) {
	PyObject * cached = NULL;

	STRUCT_BEGIN_CRITICAL_SECTION(state->pickle_cache[PICKLE_REDUCE_EX_NAME]);
	cached = Py_XNewRef(state->deepcopy_tables);
	STRUCT_END_CRITICAL_SECTION();

	if (cached != NULL) {
		return cached;
	}

	PY_OWNED(built, deepcopy_tables_build(copy_module, deepcopy));

	if (built == NULL) {
		return NULL;
	}

	PyObject * stored = NULL;

	STRUCT_BEGIN_CRITICAL_SECTION(state->pickle_cache[PICKLE_REDUCE_EX_NAME]);

	if (state->deepcopy_tables == NULL) {
		state->deepcopy_tables = Py_NewRef(built);
	}

	stored = Py_NewRef(state->deepcopy_tables);
	STRUCT_END_CRITICAL_SECTION();

	return stored;
}

static PyObject * remembered(PyObject * const memo, PyObject * const value) {
	PY_OWNED(key, PyLong_FromVoidPtr(value));
	PyObject * const found = key != NULL ? dict_value_ref(memo, key) : NULL;

#if PY_VERSION_HEX >= 0x030F0000
	if (found == Py_None) {
		Py_DECREF(found);

		return NULL;
	}
#endif

	return found;
}

static enum result keep_alive(PyObject * const memo, PyObject * const original) {
	PY_OWNED(key, PyLong_FromVoidPtr(memo));
	PY_OWNED(kept, key != NULL ? dict_value_ref(memo, key) : NULL);

	if (kept == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	if (kept != NULL && PyList_CheckExact(kept)) {
		return PyList_Append(kept, original) == 0 ? RESULT_OK : RESULT_ERROR;
	}

	if (kept != NULL) {
		PY_OWNED(appended, PyObject_CallMethod(kept, "append", "O", original));

		return appended != NULL ? RESULT_OK : RESULT_ERROR;
	}

	PY_OWNED(fresh, PyList_New(1));

	if (fresh == NULL) {
		return RESULT_ERROR;
	}

	PyList_SET_ITEM(fresh, 0, Py_NewRef(original));

	return PyDict_SetItem(memo, key, fresh) == 0 ? RESULT_OK : RESULT_ERROR;
}

enum deep_route {
	DEEP_ROUTE_ERROR,
	DEEP_ROUTE_ATOMIC,
	DEEP_ROUTE_ITSELF,
	DEEP_ROUTE_LIST,
	DEEP_ROUTE_COPY_MODULE,
};

static enum deep_route route_of(PyObject * const tables, PyObject * const value) {
	PyObject * const atomic_types = PyTuple_GET_ITEM(tables, DEEPCOPY_ATOMIC_TYPES);
	int const atomic = (
		atomic_types != Py_None ? PySet_Contains(atomic_types, (PyObject *) Py_TYPE(value)) :
		0
	);

	if (atomic != 0) {
		return atomic > 0 ? DEEP_ROUTE_ATOMIC : DEEP_ROUTE_ERROR;
	}

	PY_OWNED(
		copier,
		dict_value_ref(PyTuple_GET_ITEM(tables, DEEPCOPY_DISPATCH), (PyObject *) Py_TYPE(value))
	);

	return (
		copier == NULL && PyErr_Occurred() ? DEEP_ROUTE_ERROR :
		copier == NULL || copier == Py_None ? (
			PyType_Check(value) ? DEEP_ROUTE_ITSELF :
			DEEP_ROUTE_COPY_MODULE
		) :
		copier == PyTuple_GET_ITEM(tables, DEEPCOPY_ATOMIC_COPIER) ? DEEP_ROUTE_ITSELF :
		copier != PyTuple_GET_ITEM(tables, DEEPCOPY_LIST) ? DEEP_ROUTE_COPY_MODULE :
		PyList_CheckExact(value) ? DEEP_ROUTE_LIST :
		DEEP_ROUTE_COPY_MODULE
	);
}

static PyObject * deep_element(
	PyObject * const tables,
	PyObject * const memo,
	PyObject * const value,
	enum deep_route const route
) {
	PY_MOVABLE(found, route == DEEP_ROUTE_ITSELF ? remembered(memo, value) : NULL);

	if (found != NULL || PyErr_Occurred()) {
		return py_move(&found);
	}

	switch (route) {
		case DEEP_ROUTE_ERROR:
			return NULL;
		case DEEP_ROUTE_ATOMIC:
		case DEEP_ROUTE_ITSELF:
			return Py_NewRef(value);
		case DEEP_ROUTE_LIST:
		case DEEP_ROUTE_COPY_MODULE:
			break;
	}

	return PyObject_CallFunctionObjArgs(
		PyTuple_GET_ITEM(tables, DEEPCOPY_FUNCTION),
		value,
		memo,
		NULL
	);
}

static enum result deep_elements(
	PyObject * const tables,
	PyObject * const memo,
	PyObject * const elements,
	PyObject * const copied
) {
	for (;;) {
		PY_OWNED(element, PyIter_Next(elements));

		if (element == NULL) {
			return PyErr_Occurred() ? RESULT_ERROR : RESULT_OK;
		}

		PY_OWNED(deep, deep_element(tables, memo, element, route_of(tables, element)));

		if (deep == NULL || PyList_Append(copied, deep) < 0) {
			return RESULT_ERROR;
		}
	}
}

static PyObject * deep_list(
	PyObject * const tables,
	PyObject * const memo,
	PyObject * const value
) {
	PY_OWNED(key, PyLong_FromVoidPtr(value));
	PY_MOVABLE(copied, PyList_New(0));

	if (key == NULL || copied == NULL || PyDict_SetItem(memo, key, copied) < 0) {
		return NULL;
	}

	PY_OWNED(elements, PyObject_GetIter(value));

	return (
		(
			elements != NULL &&
			deep_elements(tables, memo, elements, copied) == RESULT_OK &&
			PyDict_SetItem(memo, key, copied) == 0 &&
			keep_alive(memo, value) == RESULT_OK
		) ? py_move(&copied) :
		NULL
	);
}

static PyObject * deep_value(
	PyObject * const tables,
	PyObject * const memo,
	PyObject * const value
) {
	enum deep_route const route = route_of(tables, value);

	if (route != DEEP_ROUTE_LIST) {
		return deep_element(tables, memo, value, route);
	}

	PY_MOVABLE(found, remembered(memo, value));

	return found != NULL || PyErr_Occurred() ? py_move(&found) : deep_list(tables, memo, value);
}

PyObject * Struct_deepcopy(PyObject * const self, PyObject * const memo) {
	if (!PyDict_Check(memo)) {
		PyErr_SetString(PyExc_TypeError, "__deepcopy__() argument must be a dict");

		return NULL;
	}

	PY_OWNED(key, PyLong_FromVoidPtr(self));

	if (key == NULL) {
		return NULL;
	}

	PY_MOVABLE(seeded, dict_value_ref(memo, key));

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

	PY_OWNED(tables, deepcopy_tables(type->struct_state, copy_module, deepcopy));

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
			PY_MOVABLE(shell, cls->tp_alloc(cls, 0));

			if (shell == NULL) {
				Py_DECREF(args);

				return NULL;
			}

			PY_MOVABLE(empty_payload, PyTuple_New(0));

			if (empty_payload == NULL) {
				Py_DECREF(args);

				return NULL;
			}

			((PyBaseExceptionObject *) shell)->args = py_move(&empty_payload);

			if (PyDict_SetItem(memo, key, shell) < 0) {
				Py_DECREF(args);

				return NULL;
			}

			PY_MOVABLE(deep_args, PyObject_CallFunctionObjArgs(deepcopy, args, memo, NULL));
			Py_DECREF(args);

			if (deep_args == NULL) {
				return memo_failure(memo, key);
			}

			PY_MOVABLE(rebuilt, PyObject_Call((PyObject *) cls, deep_args, NULL));

			if (rebuilt == NULL) {
				return memo_failure(memo, key);
			}

			copy = py_move(&rebuilt);

			if (PyDict_SetItem(memo, key, copy) < 0) {
				Py_CLEAR(copy);

				return NULL;
			}
		} else {
			Py_DECREF(args);
			copy = cls->tp_alloc(cls, 0);

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
		copy = cls->tp_alloc(cls, 0);
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
				NULL,
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

#	include "../testing.h"

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

static void test_a_function_the_list_copier_does_not_recurse_through_is_not_mirrored(void) {
	PyObject * const copy_module = PyImport_ImportModule("copy");
	PyObject * const shallow = PyObject_GetAttrString(copy_module, "copy");
	PyObject * const tables = deepcopy_tables_build(copy_module, shallow);

	TEST_ASSERT_NOT_NULL(tables);
	TEST_ASSERT_EQUAL_PTR(Py_None, PyTuple_GET_ITEM(tables, DEEPCOPY_FUNCTION));

	Py_DECREF(tables);
	Py_DECREF(shallow);
	Py_DECREF(copy_module);
}

void deepcopy_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_deepcopy_is_equal_and_distinct);
	RUN_TEST(test_a_seeded_memo_answers_for_deepcopy);
	RUN_TEST(test_a_failed_memo_restore_keeps_the_error_and_the_contract);
	RUN_TEST(test_the_stdlib_deepcopy_is_mirrored);
	RUN_TEST(test_a_function_the_list_copier_does_not_recurse_through_is_not_mirrored);
}

#endif
