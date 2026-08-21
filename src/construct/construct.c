#include <Python.h>

#include "construct.h"
#include "../meta/meta.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static void bind_positional(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count
);
static enum result bind_keywords(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count,
	PyObject * keyword_names
);
static enum result bind_named(
	StructType const * type,
	PyObject * self,
	PyObject * name,
	PyObject * value,
	Py_ssize_t positional_count
);
static enum result fill_defaults(
	StructType const * type,
	PyObject * self,
	Py_ssize_t positional_count
);
static enum result run_post_init(StructType const * type, PyObject * self);

static PyObject * interned_value(StructType const * const type, bool const no_arguments) {
	PyObject * const singleton = type->struct_singleton;

	return (singleton != NULL && no_arguments) ? Py_NewRef(singleton) : NULL;
}

PyObject * Struct_vectorcall(
	PyObject * const struct_class,
	PyObject * const * const arguments,
	size_t const argument_count_and_flags,
	PyObject * const keyword_names
) {
	StructType * const type = (StructType *) struct_class;
	Py_ssize_t const positional_count = PyVectorcall_NARGS(argument_count_and_flags);

	if (positional_count > type->struct_field_count) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() takes at most %zd positional arguments but %zd were given",
			struct_type_name(type),
			type->struct_field_count,
			positional_count
		);

		return NULL;
	}

	PyObject * const interned = interned_value(
		type,
		positional_count == 0 &&
			(keyword_names == NULL || PyTuple_GET_SIZE(keyword_names) == 0)
	);

	if (interned != NULL) {
		return interned;
	}

	PyTypeObject * const python_class = &type->heap_type.ht_type;

	PY_MOVABLE(self, python_class->tp_alloc(python_class, 0));

	if (self == NULL) {
		return NULL;
	}

	bind_positional(type, self, arguments, positional_count);

	if (
		bind_keywords(type, self, arguments, positional_count, keyword_names) != RESULT_OK ||
		fill_defaults(type, self, positional_count) != RESULT_OK ||
		run_post_init(type, self) != RESULT_OK
	) {
		return NULL;
	}

	return py_move(&self);
}

PyObject * Struct_new(
	PyTypeObject * const struct_class,
	PyObject * const arguments,
	PyObject * const keywords
) {
	StructType const * const type = (StructType *) struct_class;

	PY_MOVABLE(self, struct_class->tp_alloc(struct_class, 0));

	if (self == NULL) {
		return NULL;
	}

	return (
		fill_defaults(type, self, struct_required_count(type)) == RESULT_OK ? py_move(&self) :
		NULL
	);
}

PyObject * Struct_from_mapping(PyObject * const module, PyObject * const arguments) {
	PyObject * struct_class = NULL;
	PyObject * values = NULL;

	if (!PyArg_UnpackTuple(arguments, "from_mapping", 2, 2, &struct_class, &values)) {
		return NULL;
	}

	if (!is_struct_class(struct_class)) {
		PyErr_Format(
			PyExc_TypeError,
			"from_mapping() expects a struct class, not %.200s",
			Py_TYPE(struct_class)->tp_name
		);

		return NULL;
	}

	/* A list is PyMapping_Check-true through its subscript slot, and an
	 * ABC-style mapping carries the sequence slots through __len__ and
	 * __getitem__, so neither slot check alone names a mapping; items is
	 * what the fallback path needs either way. The probe binds a method
	 * object, so dicts, the hot path, never take it. */
	if (!PyDict_Check(values)) {
		int const has_items = PyObject_HasAttrString(values, "items");

		if (has_items < 0) {
			return NULL;
		}

		if (!PyMapping_Check(values) || has_items == 0) {
			PyErr_Format(
				PyExc_TypeError,
				"from_mapping() values must be a mapping, not %.200s",
				Py_TYPE(values)->tp_name
			);

			return NULL;
		}
	}

	StructType * const type = (StructType *) struct_class;

	if (defines_own_init(type)) {
		PY_OWNED(
			keywords,
			PyDict_Check(values) ? Py_NewRef(values) :
			PyObject_CallOneArg((PyObject *) &PyDict_Type, values)
		);

		if (keywords == NULL) {
			return NULL;
		}

		PY_OWNED(no_arguments, PyTuple_New(0));

		return no_arguments != NULL ? PyObject_Call(struct_class, no_arguments, keywords) : NULL;
	}

	Py_ssize_t entry_count;
	PY_MOVABLE(items, NULL);

	if (PyDict_Check(values)) {
		entry_count = PyDict_GET_SIZE(values);
	} else {
		items = PyMapping_Items(values);

		if (items == NULL) {
			return NULL;
		}

		entry_count = PyList_GET_SIZE(items);
	}

	PyObject * const interned = interned_value(type, entry_count == 0);

	if (interned != NULL) {
		return interned;
	}

	PyTypeObject * const cls = &type->heap_type.ht_type;

	PY_MOVABLE(built, cls->tp_alloc(cls, 0));

	if (built == NULL) {
		return NULL;
	}

	if (items == NULL) {
		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(values, &position, &key, &value)) {
			if (bind_named(type, built, key, value, 0) != RESULT_OK) {
				return NULL;
			}
		}
	} else {
		for (Py_ssize_t i = 0; i < entry_count; ++i) {
			PyObject * const pair = PyList_GET_ITEM(items, i);

			if (
				bind_named(
					type,
					built,
					PyTuple_GET_ITEM(pair, 0),
					PyTuple_GET_ITEM(pair, 1),
					0
				) !=
				RESULT_OK
			) {
				return NULL;
			}
		}
	}

	return (
		fill_defaults(
			type,
			built,
			0
		) == RESULT_OK && run_post_init(type, built) == RESULT_OK ? py_move(&built) :
		NULL
	);
}

static enum result run_post_init(StructType const * const type, PyObject * const self) {
	if (type->struct_post_init == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(returned, PyObject_CallOneArg(type->struct_post_init, self));

	return returned != NULL ? RESULT_OK : RESULT_ERROR;
}

static void bind_positional(
	StructType const * const type,
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const positional_count
) {
	for (Py_ssize_t i = 0; i < positional_count; ++i) {
		*struct_slot(type, self, i) = Py_NewRef(arguments[i]);
	}
}

static enum result bind_keywords(
	StructType const * const type,
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const positional_count,
	PyObject * const keyword_names
) {
	Py_ssize_t const keyword_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;

	for (Py_ssize_t i = 0; i < keyword_count; ++i) {
		if (
			bind_named(
				type,
				self,
				PyTuple_GET_ITEM(keyword_names, i),
				arguments[positional_count + i],
				positional_count
			) !=
			RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result bind_named(
	StructType const * const type,
	PyObject * const self,
	PyObject * const name,
	PyObject * const value,
	Py_ssize_t const positional_count
) {
	struct field_lookup const found = find_field(type, name);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
			return RESULT_ERROR;
		case FIELD_LOOKUP_MISSING:
			PyErr_Format(
				PyExc_TypeError,
				"%.200s() got an unexpected keyword argument '%U'",
				struct_type_name(type),
				name
			);

			return RESULT_ERROR;
		case FIELD_LOOKUP_FOUND:
			break;
	}

	PyObject * * const slot = struct_slot(type, self, found.index);

	if (*slot != NULL || found.index < positional_count) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() got multiple values for argument '%U'",
			struct_type_name(type),
			name
		);

		return RESULT_ERROR;
	}

	*slot = Py_NewRef(value);

	return RESULT_OK;
}

static enum result fill_defaults(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const positional_count
) {
	Py_ssize_t const required_count = struct_required_count(type);

	for (Py_ssize_t i = positional_count; i < type->struct_field_count; ++i) {
		PyObject * * const slot = struct_slot(type, self, i);

		if (*slot != NULL) {
			continue;
		}

		if (i < required_count) {
			PyErr_Format(
				PyExc_TypeError,
				"%.200s() missing required argument '%U'",
				struct_type_name(type),
				PyTuple_GET_ITEM(type->struct_field_names, i)
			);

			return RESULT_ERROR;
		}

		PyObject * const value = struct_default_copy(
			PyTuple_GET_ITEM(type->struct_defaults, i - required_count)
		);

		if (value == NULL) {
			return RESULT_ERROR;
		}

		*slot = value;
	}

	return RESULT_OK;
}
