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
static enum result fill_defaults(
	StructType const * type,
	PyObject * self,
	Py_ssize_t positional_count
);
static struct field_lookup named_field(StructType const * type, PyObject * name);
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

PyObject * Struct_replace(
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const nargs,
	PyObject * const keyword_names
) {
	/* METH_FASTCALL methods receive self as the first parameter, so the
	 * positional count here excludes it: anything past zero is a second
	 * positional argument. */
	if (nargs != 0) {
		PyErr_Format(
			PyExc_TypeError,
			"%s.__replace__() takes exactly one positional argument (%zd given)",
			Py_TYPE(self)->tp_name,
			nargs
		);

		return NULL;
	}

	if (!is_struct(self)) {
		PyErr_Format(PyExc_TypeError, "%s object is not replaceable", Py_TYPE(self)->tp_name);

		return NULL;
	}

	StructType * const type = struct_type_of(self);
	Py_ssize_t const change_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;

	if (change_count == 0 && type->struct_options.frozen) {
		return Py_NewRef(self);
	}

	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (defines_own_init(type)) {
		PY_OWNED(changed, PyDict_New());

		if (changed == NULL) {
			return NULL;
		}

		for (Py_ssize_t i = 0; i < change_count; ++i) {
			if (named_field(type, PyTuple_GET_ITEM(keyword_names, i)).tag != FIELD_LOOKUP_FOUND) {
				return NULL;
			}
		}

		PY_OWNED(values, PyTuple_New(type->struct_field_count));

		if (values == NULL) {
			return NULL;
		}

		struct_slots_ref_into(type, self, values, NULL);

		for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
			PyObject * const value = PyTuple_GET_ITEM(values, i);

			if (value == NULL) {
				continue;
			}

			PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, i);
			int const present = PyDict_Contains(changed, name);

			if (present < 0) {
				return NULL;
			}

			if (present == 0 && PyDict_SetItem(changed, name, value) < 0) {
				return NULL;
			}
		}

		for (Py_ssize_t i = 0; i < change_count; ++i) {
			if (
				PyDict_SetItem(
					changed,
					PyTuple_GET_ITEM(keyword_names, i),
					arguments[nargs + i]
				) <
				0
			) {
				return NULL;
			}
		}

		PY_OWNED(no_arguments, PyTuple_New(0));

		if (no_arguments == NULL) {
			return NULL;
		}

		PY_MOVABLE(replaced, PyObject_Call((PyObject *) cls, no_arguments, changed));

		if (replaced == NULL) {
			return NULL;
		}

		if (Py_TYPE(replaced) != cls) {
			PyErr_SetString(
				PyExc_SystemError,
				"salix internal error: the replace construction returned a different type"
			);

			return NULL;
		}

		for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
			PyObject * const value = PyTuple_GET_ITEM(values, i);
			PyObject * * const slot = struct_slot(type, replaced, i);

			if (value != NULL && *slot == NULL) {
				*slot = Py_NewRef(value);
			}
		}

		PY_MOVABLE(source_dict, NULL);
		struct_slots_copy_into(type, self, replaced, &source_dict);

		if (source_dict != NULL && struct_dict_copy_merged(source_dict, replaced) < 0) {
			return NULL;
		}

		return py_move(&replaced);
	}

	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (copy == NULL) {
		return NULL;
	}

	if (bind_keywords(type, copy, arguments, 0, keyword_names) != RESULT_OK) {
		return NULL;
	}

	PY_MOVABLE(source_dict, NULL);
	struct_slots_copy_into(type, self, copy, &source_dict);

	if (run_post_init(type, copy) != RESULT_OK) {
		return NULL;
	}

	if (source_dict != NULL && struct_dict_copy_merged(source_dict, copy) < 0) {
		return NULL;
	}

	return py_move(&copy);
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
		struct field_lookup const found = named_field(type, keyword);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
			case FIELD_LOOKUP_MISSING:
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

static struct field_lookup named_field(StructType const * const type, PyObject * const name) {
	struct field_lookup const found = find_field(type, name);

	if (found.tag == FIELD_LOOKUP_MISSING) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() got an unexpected keyword argument '%U'",
			struct_type_name(type),
			name
		);
	}

	return found;
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
