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

enum memo_none_entry {
	MEMO_NONE_ENTRY_ERROR,
	MEMO_NONE_ENTRY_PRESENT,
	MEMO_NONE_ENTRY_ABSENT,
};

PyObject * deepcopy_tables(struct salix_state * state, PyObject * deepcopy);
enum deep_route route_of(PyObject * tables, PyObject * value);
PyObject * deep_value(PyObject * tables, PyObject * memo, PyObject * value);
enum memo_none_entry copy_reads_memo_none(void);

static inline PyObject * memo_entry(
	PyObject * const memo,
	PyObject * const key,
	enum memo_none_entry const none_entry
) {
	PyObject * const found = dict_value_ref(memo, key);

	if (found == Py_None && none_entry == MEMO_NONE_ENTRY_ABSENT) {
		Py_DECREF(found);

		return NULL;
	}

	return found;
}
