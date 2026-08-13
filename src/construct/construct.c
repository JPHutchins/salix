#include <Python.h>

#include "construct.h"
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
static enum result fill_defaults(
	StructType const * type,
	PyObject * self,
	Py_ssize_t positional_count
);
static enum result run_post_init(StructType const * type, PyObject * self);

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
	PY_MOVABLE(self, struct_class->tp_alloc(struct_class, 0));

	if (self == NULL) {
		return NULL;
	}

	StructType const * const type = (StructType *) struct_class;

	return (
		fill_defaults(type, self, struct_required_count(type)) == RESULT_OK ? py_move(&self) :
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
		PyObject * const keyword = PyTuple_GET_ITEM(keyword_names, i);
		struct field_lookup const found = find_field(type, keyword);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return RESULT_ERROR;
			case FIELD_LOOKUP_MISSING:
				PyErr_Format(
					PyExc_TypeError,
					"%.200s() got an unexpected keyword argument '%U'",
					struct_type_name(type),
					keyword
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
				keyword
			);

			return RESULT_ERROR;
		}

		*slot = Py_NewRef(arguments[positional_count + i]);
	}

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
