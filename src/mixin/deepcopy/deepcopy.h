#pragma once

#include "../mixin.h"

enum deep_route {
	DEEP_ROUTE_ERROR,
	DEEP_ROUTE_ATOMIC,
	DEEP_ROUTE_ITSELF,
	DEEP_ROUTE_LIST,
	DEEP_ROUTE_COPY_MODULE,
};

PyObject * deepcopy_tables(struct salix_state * state, PyObject * deepcopy);
enum deep_route route_of(PyObject * tables, PyObject * value);
PyObject * deep_value(PyObject * tables, PyObject * memo, PyObject * value);
PyObject * memo_entry(PyObject * memo, PyObject * key);
