#pragma once

#include "../meta.h"
#include "../../types.h"

struct chain_verdict {
	int accepts_all;
	int accepts_weakref;
	bool readable;
};

PyObject * metaclass_chain(PyTypeObject * winner);
struct chain_verdict chain_probe(
	PyObject * chain,
	PyObject * keywords,
	bool weakref_column,
	PyObject * * declined
);
StructType * create_class(
	PyTypeObject * metatype,
	PyTypeObject * handoff,
	PyObject * name,
	PyObject * bases,
	PyObject * namespace,
	PyObject * const * keyword_rungs,
	Py_ssize_t keyword_rung_count,
	PyObject * forwarded_keywords,
	PyObject * forwarded_options,
	bool laddered,
	PyObject * handoff_attempt,
	PyObject * handoff_declined,
	PyObject * handoff_new
);
PyTypeObject * winning_metatype(PyTypeObject * requested, PyObject * bases);
