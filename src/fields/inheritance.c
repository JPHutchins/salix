#include <Python.h>
#include <stdbool.h>

#include "fields.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static enum result append_inherited_default(
	PyObject * const default_by_name,
	PyObject * const parameter_name,
	PyObject * const defaults,
	Py_ssize_t const index,
	Py_ssize_t const required_count
) {
	return (
		(
			index < required_count ||
			(
				PyDict_SetItem(
					default_by_name,
					parameter_name,
					PyTuple_GET_ITEM(defaults, index - required_count)
				) >= 0
			)
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

enum result append_inherited(
	StructType const * const base,
	struct parameter_lists const * const parameters,
	PyObject * const default_by_name,
	PyObject * const empty_extras
) {
	if (base == NULL) {
		return RESULT_OK;
	}

	if (
		base->struct_annotations == NULL ||
		base->struct_metadata == NULL ||
		PyTuple_GET_SIZE(base->struct_annotations) != base->struct_field_count ||
		PyTuple_GET_SIZE(base->struct_metadata) != base->struct_field_count
	) {
		PyErr_SetString(
			PyExc_SystemError,
			"salix internal error: a base's annotation tuples do not align with its fields"
		);

		return RESULT_ERROR;
	}

	Py_ssize_t const placeholder_count = (
		base->struct_class_var_positions != NULL ? PyTuple_GET_SIZE(
			base->struct_class_var_positions
		) :
		0
	);
	Py_ssize_t placeholder_index = 0;
	Py_ssize_t field_index = 0;
	Py_ssize_t init_var_index = 0;

	for (
		Py_ssize_t entry = 0;
		entry < struct_parameter_count(base) + placeholder_count;
		entry += 1
	) {
		PyObject * const placeholder = (
			placeholder_index < placeholder_count ? PyTuple_GET_ITEM(
				base->struct_class_var_positions,
				placeholder_index
			) :
			NULL
		);

		Py_ssize_t const placeholder_position = (
			placeholder != NULL ? PyLong_AsSsize_t(PyTuple_GET_ITEM(placeholder, 0)) :
			-1
		);

		if (placeholder_position == -1 && PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		if (placeholder_position == entry) {
			if (
				(
					append_parameter(
						parameters,
						PyTuple_GET_ITEM(placeholder, 1),
						Py_None,
						Py_None,
						empty_extras
					) != RESULT_OK
				) ||
				(
					PyDict_SetItem(
						default_by_name,
						PyTuple_GET_ITEM(placeholder, 1),
						PyTuple_GET_ITEM(placeholder, 2)
					) < 0
				)
			) {
				return RESULT_ERROR;
			}

			placeholder_index += 1;

			continue;
		}

		switch (struct_parameter_kind(base, entry - placeholder_index)) {
			case PARAMETER_FIELD: {
				PyObject * const field_name = PyTuple_GET_ITEM(
					base->struct_field_names,
					field_index
				);

				if (
					(
						append_parameter(
							parameters,
							field_name,
							Py_False,
							PyTuple_GET_ITEM(base->struct_annotations, field_index),
							PyTuple_GET_ITEM(base->struct_metadata, field_index)
						) != RESULT_OK
					) ||
					(
						append_inherited_default(
							default_by_name,
							field_name,
							base->struct_defaults,
							field_index,
							struct_required_count(base)
						) != RESULT_OK
					)
				) {
					return RESULT_ERROR;
				}

				field_index += 1;

				break;
			}
			case PARAMETER_INIT_VAR: {
				PyObject * const init_var_name =
					PyTuple_GET_ITEM(base->struct_init_var_names, init_var_index);

				if (
					(
						append_parameter(
							parameters,
							init_var_name,
							Py_True,
							PyTuple_GET_ITEM(base->struct_init_var_annotations, init_var_index),
							empty_extras
						) != RESULT_OK
					) ||
					(
						append_inherited_default(
							default_by_name,
							init_var_name,
							base->struct_init_var_defaults,
							init_var_index,
							struct_required_init_var_count(base)
						) != RESULT_OK
					)
				) {
					return RESULT_ERROR;
				}

				init_var_index += 1;

				break;
			}
		}
	}

	return RESULT_OK;
}

static bool carries_secondary_class_vars(PyObject * const bases, StructType const * const base) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i += 1) {
		PyObject * const entry = PyTuple_GET_ITEM(bases, i);

		if (
			is_struct_class(entry) &&
			(StructType const *) entry != base &&
			((StructType const *) entry)->struct_class_var_positions != NULL
		) {
			return true;
		}
	}

	return false;
}

static enum result record_merged_order(
	StructType const * const struct_base,
	PyObject * const merged_order,
	PyObject * const class_var_values,
	PyObject * const empty_extras
) {
	PY_OWNED(names, PyList_New(0));
	PY_OWNED(init_var_flags, PyList_New(0));
	PY_OWNED(annotations, PyList_New(0));
	PY_OWNED(metadata, PyList_New(0));
	PY_OWNED(default_by_name, PyDict_New());

	if (
		names == NULL ||
		init_var_flags == NULL ||
		annotations == NULL ||
		metadata == NULL ||
		default_by_name == NULL ||
		(
			append_inherited(
				struct_base,
				&(struct parameter_lists){
					.names = names,
					.init_var_flags = init_var_flags,
					.annotations = annotations,
					.metadata = metadata,
				},
				default_by_name,
				empty_extras
			) != RESULT_OK
		)
	) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(names); i += 1) {
		PyObject * const name = PyList_GET_ITEM(names, i);

		if (PyDict_SetDefault(merged_order, name, Py_None) == NULL) {
			return RESULT_ERROR;
		}

		if (PyList_GET_ITEM(init_var_flags, i) != Py_None) {
			continue;
		}

		PY_OWNED(value, dict_value_ref(default_by_name, name));

		if (
			value != NULL ? PyDict_SetItem(class_var_values, name, value) < 0 :
			PyErr_Occurred() != NULL
		) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

enum result merge_secondary_class_vars(
	PyObject * const bases,
	StructType const * const base,
	struct parameter_lists const * const parameters,
	PyObject * const default_by_name,
	PyObject * const empty_extras
) {
	if (!carries_secondary_class_vars(bases, base)) {
		return RESULT_OK;
	}

	PY_OWNED(merged_order, PyDict_New());
	PY_OWNED(class_var_values, PyDict_New());

	if (merged_order == NULL || class_var_values == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = PyTuple_GET_SIZE(bases) - 1; i >= 0; i -= 1) {
		PyObject * const entry = PyTuple_GET_ITEM(bases, i);

		if (
			is_struct_class(entry) &&
			(
				record_merged_order(
					(StructType const *) entry,
					merged_order,
					class_var_values,
					empty_extras
				) != RESULT_OK
			)
		) {
			return RESULT_ERROR;
		}
	}

	Py_ssize_t at = 0;
	Py_ssize_t cursor = 0;
	PyObject * name = NULL;

	while (PyDict_Next(merged_order, &cursor, &name, NULL)) {
		struct inheritance_lookup const inherited =
			inherited_position(parameters->names, PyList_GET_SIZE(parameters->names), name);

		if (inherited.tag == INHERITANCE_ERROR) {
			return RESULT_ERROR;
		}

		if (inherited.tag == INHERITANCE_INHERITED) {
			at = inherited.position + 1;

			continue;
		}

		PY_OWNED(value, dict_value_ref(class_var_values, name));

		if (value == NULL) {
			if (PyErr_Occurred()) {
				return RESULT_ERROR;
			}

			continue;
		}

		if (
			insert_parameter(parameters, at, name, Py_None, Py_None, empty_extras) != RESULT_OK ||
			PyDict_SetItem(default_by_name, name, value) < 0
		) {
			return RESULT_ERROR;
		}

		at += 1;
	}

	return RESULT_OK;
}
