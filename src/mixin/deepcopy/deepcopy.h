#pragma once

#include "../mixin.h"
#include "../../types.h"

enum deep_route {
	DEEP_ROUTE_ERROR,
	DEEP_ROUTE_ATOMIC,
	DEEP_ROUTE_ITSELF,
	DEEP_ROUTE_LIST,
	DEEP_ROUTE_COPY_MODULE,
};

enum keeps_identity {
	KEEPS_IDENTITY_ERROR,
	KEEPS_IDENTITY_PROVEN,
	KEEPS_IDENTITY_UNPROVEN,
};

enum memo_none_entry {
	MEMO_NONE_ENTRY_PRESENT,
	MEMO_NONE_ENTRY_ABSENT,
};

PyObject * deepcopy_tables(struct salix_state * state, PyObject * deepcopy);
enum deep_route route_of(PyObject * tables, PyObject * value);
PyObject * deep_value(PyObject * tables, PyObject * memo, PyObject * value);

static inline enum keeps_identity route_keeps_identity(enum deep_route const route) {
	switch (route) {
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

static inline enum memo_none_entry copy_reads_memo_none(void) {
#if PY_VERSION_HEX >= 0x030F0000 && PY_VERSION_HEX < 0x03100000
	return Py_Version < 0x030F0100 ? MEMO_NONE_ENTRY_ABSENT : MEMO_NONE_ENTRY_PRESENT;
#else
	return MEMO_NONE_ENTRY_PRESENT;
#endif
}

static inline PyObject * memo_entry(PyObject * const memo, PyObject * const key) {
	PyObject * const found = dict_value_ref(memo, key);

	if (found == Py_None && copy_reads_memo_none() == MEMO_NONE_ENTRY_ABSENT) {
		Py_DECREF(found);

		return NULL;
	}

	return found;
}
