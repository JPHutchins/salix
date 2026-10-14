#include <Python.h>

#include "deepcopy.h"
#include "../../owned.h"
#include "../../types.h"

enum keeps_identity { KEEPS_IDENTITY_ERROR, KEEPS_IDENTITY_PROVEN, KEEPS_IDENTITY_UNPROVEN };

static enum keeps_identity item_keeps_identity(PyObject * const tables, PyObject * const item) {
	switch (route_of(tables, item)) {
		case DEEP_ROUTE_ERROR:
			return KEEPS_IDENTITY_ERROR;
		case DEEP_ROUTE_ATOMIC:
		case DEEP_ROUTE_ITSELF:
			return KEEPS_IDENTITY_PROVEN;
		case DEEP_ROUTE_LIST:
		case DEEP_ROUTE_COPY_MODULE:
			break;
	}

	return KEEPS_IDENTITY_UNPROVEN;
}

static enum keeps_identity every_item_keeps_identity(
	PyObject * const tables,
	PyObject * const items
) {
	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items); i += 1) {
		enum keeps_identity const kept = item_keeps_identity(tables, PyList_GET_ITEM(items, i));

		if (kept != KEEPS_IDENTITY_PROVEN) {
			return kept;
		}
	}

	return KEEPS_IDENTITY_PROVEN;
}

static enum keeps_identity every_entry_keeps_identity(
	PyObject * const tables,
	PyObject * const mapping
) {
	Py_ssize_t position = 0;
	PyObject * key = NULL;
	PyObject * entry = NULL;

	while (PyDict_Next(mapping, &position, &key, &entry)) {
		enum keeps_identity const key_kept = item_keeps_identity(tables, key);
		enum keeps_identity const kept = (
			key_kept == KEEPS_IDENTITY_PROVEN ? item_keeps_identity(tables, entry) :
			key_kept
		);

		if (kept != KEEPS_IDENTITY_PROVEN) {
			return kept;
		}
	}

	return KEEPS_IDENTITY_PROVEN;
}

static PyObject * proven_copy(PyObject * * const copied, enum keeps_identity const kept) {
	switch (kept) {
		case KEEPS_IDENTITY_ERROR:
		case KEEPS_IDENTITY_UNPROVEN:
			return NULL;
		case KEEPS_IDENTITY_PROVEN:
			break;
	}

	return py_move(copied);
}

static PyObject * list_of_identities(PyObject * const tables, PyObject * const value) {
	if (route_of(tables, value) != DEEP_ROUTE_LIST) {
		return NULL;
	}

	PY_MOVABLE(copied, PyList_GetSlice(value, 0, PY_SSIZE_T_MAX));

	return copied != NULL ? proven_copy(&copied, every_item_keeps_identity(tables, copied)) : NULL;
}

static int table_entry_is(
	PyObject * const table,
	PyTypeObject * const kind,
	PyObject * const expected
) {
	PY_OWNED(entry, dict_value_ref(table, (PyObject *) kind));

	return entry == NULL && PyErr_Occurred() ? -1 : (entry == NULL ? Py_None : entry) == expected;
}

static PyObject * dict_of_identities(PyObject * const tables, PyObject * const value) {
	PyObject * const dict_copier = PyTuple_GET_ITEM(tables, DEEPCOPY_DICT);
	PyObject * const dispatch = PyTuple_GET_ITEM(tables, DEEPCOPY_DISPATCH);
	int const stdlib_copier = (
		dict_copier != Py_None ? table_entry_is(dispatch, &PyDict_Type, dict_copier) :
		0
	);

	if (stdlib_copier <= 0) {
		return NULL;
	}

	PY_MOVABLE(copied, PyDict_Copy(value));

	return copied != NULL ? proven_copy(&copied, every_entry_keeps_identity(tables, copied)) : NULL;
}

static int set_reduces_through_the_stdlib(PyObject * const tables) {
	PyObject * const dispatch = PyTuple_GET_ITEM(tables, DEEPCOPY_DISPATCH);
	PyObject * const reductors = PyTuple_GET_ITEM(tables, DEEPCOPY_REDUCTORS);
	PyObject * const tuple_copier = PyTuple_GET_ITEM(tables, DEEPCOPY_TUPLE);
	PyObject * const list_copier = PyTuple_GET_ITEM(tables, DEEPCOPY_LIST);

	if (!PyDict_CheckExact(reductors) || tuple_copier == Py_None || list_copier == Py_None) {
		return 0;
	}

	int const no_set_copier = table_entry_is(dispatch, &PySet_Type, Py_None);
	int const no_set_reductor = (
		no_set_copier > 0 ? table_entry_is(reductors, &PySet_Type, Py_None) :
		no_set_copier
	);
	int const stdlib_tuple_copier = (
		no_set_reductor > 0 ? table_entry_is(dispatch, &PyTuple_Type, tuple_copier) :
		no_set_reductor
	);

	return (
		stdlib_tuple_copier > 0 ? table_entry_is(dispatch, &PyList_Type, list_copier) :
		stdlib_tuple_copier
	);
}

static PyObject * set_of_identities(PyObject * const tables, PyObject * const value) {
	int const reduces = set_reduces_through_the_stdlib(tables);

	if (reduces <= 0) {
		return NULL;
	}

	PY_OWNED(elements, PySequence_List(value));

	if (elements == NULL) {
		return NULL;
	}

	switch (every_item_keeps_identity(tables, elements)) {
		case KEEPS_IDENTITY_ERROR:
		case KEEPS_IDENTITY_UNPROVEN:
			return NULL;
		case KEEPS_IDENTITY_PROVEN:
			break;
	}

	return PySet_New(elements);
}

static int hook_defined_outside_the_mixin(PyTypeObject * const cls, PyObject * const hook_name) {
	PyObject * const mro = cls->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i += 1) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);

		if (entry == &StructMixin_Type) {
			continue;
		}

		PY_OWNED(entry_dict, struct_type_dict(entry));
		int const present = entry_dict != NULL ? PyDict_Contains(entry_dict, hook_name) : -1;

		if (present != 0) {
			return present;
		}
	}

	return 0;
}

static bool is_copied_container(PyObject * const value) {
	return PyList_CheckExact(value) || PyDict_CheckExact(value) || PySet_CheckExact(value);
}

static bool containers_alias(StructType const * const type, PyObject * const instance) {
	for (Py_ssize_t i = 0; i < type->struct_field_count; i += 1) {
		PyObject * const value = *struct_slot(type, instance, i);

		for (Py_ssize_t j = 0; value != NULL && is_copied_container(value) && j < i; j += 1) {
			if (*struct_slot(type, instance, j) == value) {
				return true;
			}
		}
	}

	return false;
}

static PyObject * plain_field_copy(PyObject * const tables, PyObject * const field) {
	switch (item_keeps_identity(tables, field)) {
		case KEEPS_IDENTITY_ERROR:
			return NULL;
		case KEEPS_IDENTITY_PROVEN:
			return Py_NewRef(field);
		case KEEPS_IDENTITY_UNPROVEN:
			break;
	}

	return (
		PyList_CheckExact(field) ? list_of_identities(tables, field) :
		PyDict_CheckExact(field) ? dict_of_identities(tables, field) :
		PySet_CheckExact(field) ? set_of_identities(tables, field) :
		NULL
	);
}

static PyObject * plain_struct_copy(PyObject * const tables, PyObject * const value) {
	StructType * const type = struct_type_of(value);
	PyTypeObject * const cls = &type->heap_type.ht_type;
	PyObject * const reductors = PyTuple_GET_ITEM(tables, DEEPCOPY_REDUCTORS);
	int const unreduced = (
		PyDict_CheckExact(reductors) ? table_entry_is(reductors, cls, Py_None) :
		0
	);

	if (
		unreduced <= 0 ||
		type->struct_copies_through_reduce ||
		type->struct_cannot_create ||
		type->struct_singleton != NULL ||
		type->struct_member_count != 0 ||
		PyType_FastSubclass(cls, Py_TPFLAGS_BASE_EXC_SUBCLASS)
	) {
		return NULL;
	}

	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));
	PY_MOVABLE(dict, NULL);

	if (copy == NULL) {
		return NULL;
	}

	struct_slots_copy_into(type, value, copy, &dict);

	if (dict != NULL || containers_alias(type, copy)) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < type->struct_field_count; i += 1) {
		PyObject * const field = *struct_slot(type, copy, i);

		if (field == NULL) {
			continue;
		}

		PyObject * const copied = plain_field_copy(tables, field);

		if (copied == NULL) {
			return NULL;
		}

		Py_SETREF(*struct_slot(type, copy, i), copied);
	}

	return py_move(&copy);
}

static PyObject * struct_of_identities(
	struct salix_state const * const state,
	PyObject * const tables,
	PyObject * const value
) {
	PyTypeObject * const cls = Py_TYPE(value);
	PyObject * const dispatch = PyTuple_GET_ITEM(tables, DEEPCOPY_DISPATCH);
	bool const reaches_copy_module = route_of(tables, value) == DEEP_ROUTE_COPY_MODULE;
	int const unregistered = reaches_copy_module ? table_entry_is(dispatch, cls, Py_None) : 0;

	if (
		unregistered <= 0 ||
		cls->tp_getattro != PyObject_GenericGetAttr ||
		cls->tp_dictoffset != 0
	) {
		return NULL;
	}

	int const hooked = hook_defined_outside_the_mixin(cls, state->copy_cache[COPY_DEEP_HOOK_NAME]);

	if (hooked != 0) {
		return NULL;
	}

	PY_MOVABLE(plain, plain_struct_copy(tables, value));

	if (plain != NULL || PyErr_Occurred()) {
		return py_move(&plain);
	}

	PY_OWNED(memo, PyDict_New());

	return memo != NULL ? Struct_deepcopy(value, memo) : NULL;
}

PyObject * fresh_deepcopy(
	struct salix_state * const state,
	PyObject * const deepcopy,
	PyObject * const value
) {
	PY_OWNED(tables, deepcopy_tables(state, deepcopy));

	if (tables == NULL || PyTuple_GET_ITEM(tables, DEEPCOPY_FUNCTION) != deepcopy) {
		return NULL;
	}

	return (
		PyList_CheckExact(value) ? list_of_identities(tables, value) :
		PyDict_CheckExact(value) ? dict_of_identities(tables, value) :
		PySet_CheckExact(value) ? set_of_identities(tables, value) :
		is_struct(value) ? struct_of_identities(state, tables, value) :
		NULL
	);
}
