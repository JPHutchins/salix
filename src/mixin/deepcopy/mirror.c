#include <Python.h>

#include "deepcopy.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"

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
		[DEEPCOPY_DICT] = "_deepcopy_dict",
		[DEEPCOPY_TUPLE] = "_deepcopy_tuple",
		[DEEPCOPY_REDUCTORS] = "dispatch_table",
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

PyObject * deepcopy_tables(struct salix_state * const state, PyObject * const deepcopy) {
	PyObject * cached = NULL;

	STRUCT_BEGIN_CRITICAL_SECTION(state->pickle_cache[PICKLE_REDUCE_EX_NAME]);
	cached = Py_XNewRef(state->deepcopy_tables);
	STRUCT_END_CRITICAL_SECTION();

	if (cached != NULL) {
		return cached;
	}

	PY_OWNED(copy_module, PyImport_ImportModule("copy"));
	PY_OWNED(built, copy_module != NULL ? deepcopy_tables_build(copy_module, deepcopy) : NULL);

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

static PyObject * cached_none_entry_absent = NULL;

static PyObject * none_entry_absent_probe(PyObject * const copy_module) {
	PY_OWNED(deepcopy, PyObject_GetAttrString(copy_module, "deepcopy"));
	PY_OWNED(probe, deepcopy != NULL ? PyList_New(0) : NULL);
	PY_OWNED(key, probe != NULL ? PyLong_FromVoidPtr(probe) : NULL);
	PY_OWNED(memo, key != NULL ? PyDict_New() : NULL);

	if (memo == NULL || PyDict_SetItem(memo, key, Py_None) < 0) {
		return NULL;
	}

	PY_OWNED(copied, PyObject_CallFunctionObjArgs(deepcopy, probe, memo, NULL));

	return copied != NULL ? PyBool_FromLong(copied != Py_None) : NULL;
}

enum memo_none_entry copy_reads_memo_none(void) {
	if (cached_none_entry_absent == NULL) {
		PY_OWNED(copy_module, PyImport_ImportModule("copy"));
		PY_OWNED(probed, copy_module != NULL ? none_entry_absent_probe(copy_module) : NULL);

		if (probed == NULL) {
			return MEMO_NONE_ENTRY_ERROR;
		}

		STRUCT_BEGIN_CRITICAL_SECTION(copy_module);

		if (cached_none_entry_absent == NULL) {
			cached_none_entry_absent = Py_NewRef(probed);
		}

		STRUCT_END_CRITICAL_SECTION();
	}

	return cached_none_entry_absent == Py_True ? MEMO_NONE_ENTRY_ABSENT : MEMO_NONE_ENTRY_PRESENT;
}

void memo_none_entry_free(void) {
	Py_CLEAR(cached_none_entry_absent);
}

static PyObject * remembered(PyObject * const memo, PyObject * const value) {
	enum memo_none_entry const none_entry = copy_reads_memo_none();
	PY_OWNED(key, none_entry != MEMO_NONE_ENTRY_ERROR ? PyLong_FromVoidPtr(value) : NULL);

	return key != NULL ? memo_entry(memo, key, none_entry) : NULL;
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

enum deep_route route_of(PyObject * const tables, PyObject * const value) {
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

PyObject * deep_value(PyObject * const tables, PyObject * const memo, PyObject * const value) {
	enum deep_route const route = route_of(tables, value);

	if (route != DEEP_ROUTE_LIST) {
		return deep_element(tables, memo, value, route);
	}

	PY_MOVABLE(found, remembered(memo, value));

	return found != NULL || PyErr_Occurred() ? py_move(&found) : deep_list(tables, memo, value);
}

#ifdef TESTING

#	include "../../testing.h"

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

void mirror_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_function_the_list_copier_does_not_recurse_through_is_not_mirrored);
}

#endif
