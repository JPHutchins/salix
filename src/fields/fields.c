#include <Python.h>
#include <stdbool.h>

#include "../annotations.h"
#include "../construct.h"
#include "fields.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static void form_probes_clear(struct form_probes * const probes) {
	Py_CLEAR(probes->class_var);
	Py_CLEAR(probes->init_var);
	Py_CLEAR(probes->class_var_name);
	Py_CLEAR(probes->init_var_name);
}

enum inheritance : int {
	INHERITANCE_ERROR = -1,
	INHERITANCE_NEW = 0,
	INHERITANCE_INHERITED = 1,
};

struct inheritance_lookup {
	enum inheritance tag;
	Py_ssize_t position;
};

struct annotation_entry {
	PyObject * annotation;
	PyObject * metadata;
};

struct parameter_lists {
	PyObject * names;
	PyObject * init_var_flags;
	PyObject * annotations;
	PyObject * metadata;
};

static enum result append_inherited(
	StructType const * base,
	struct parameter_lists const * parameters,
	PyObject * default_by_name,
	PyObject * empty_extras
);
static enum result append_declared(
	PyObject * annotations,
	PyObject * namespace,
	Py_ssize_t inherited_count,
	struct parameter_lists const * parameters,
	PyObject * new_names,
	PyObject * default_by_name,
	PyObject * empty_extras
);
static enum result append_annotation(
	PyObject * annotation,
	struct parameter_lists const * parameters,
	PyObject * empty_extras,
	bool typing_loaded
);
static struct field_plan plan_from_parameters(
	struct parameter_lists const * parameters,
	PyObject * new_names,
	PyObject * default_by_name
);

char const * const reserved_metadata_names[] = {
	"_struct_fields_",
	"_struct_defaults_",
	"__struct_fields__",
	"__struct_defaults__",
	"_struct_annotations_",
	"_struct_metadata_",
	"__struct_annotations__",
	"__struct_metadata__",
	NULL,
};

char const * reserved_metadata_name_of(PyObject * const name) {
	for (char const * const * reserved = reserved_metadata_names; *reserved != NULL; ++reserved) {
		if (PyUnicode_CompareWithASCIIString(name, *reserved) == 0) {
			return *reserved;
		}
	}

	return NULL;
}

static enum result refuse_reserved_name(PyObject * const field_name) {
	char const * const reserved = reserved_metadata_name_of(field_name);

	if (reserved == NULL) {
		return RESULT_OK;
	}

	PyErr_Format(
		PyExc_TypeError,
		"'%s' is reserved for salix's metadata and cannot be a field "
		"or a class-body binding",
		reserved
	);

	return RESULT_ERROR;
}

static bool machinery_name(PyObject * const name) {
	Py_ssize_t const length = PyUnicode_GET_LENGTH(name);

	return (
		length >= 4 &&
		PyUnicode_ReadChar(name, 0) == '_' &&
		PyUnicode_ReadChar(name, 1) == '_' &&
		PyUnicode_ReadChar(name, length - 1) == '_' &&
		PyUnicode_ReadChar(name, length - 2) == '_'
	);
}
static PyObject * build_defaults(PyObject * all_names, PyObject * default_by_name);
static PyObject * checked_annotations(PyObject * namespace);
static struct inheritance_lookup inherited_position(
	PyObject * parameter_names,
	Py_ssize_t inherited_count,
	PyObject * field_name
);

struct field_plan field_plan_build(StructType const * const base, PyObject * const namespace) {
	struct field_plan plan = {0};

	PY_OWNED(annotations, checked_annotations(namespace));

	if (annotations == NULL) {
		return plan;
	}

	PY_OWNED(parameter_names, PyList_New(0));
	PY_OWNED(init_var_flags, PyList_New(0));
	PY_OWNED(annotation_values, PyList_New(0));
	PY_OWNED(metadata_values, PyList_New(0));
	PY_OWNED(new_names, PyList_New(0));
	PY_OWNED(default_by_name, PyDict_New());
	PY_OWNED(empty_extras, PyTuple_New(0));
	struct parameter_lists const parameters = {
		.names = parameter_names,
		.init_var_flags = init_var_flags,
		.annotations = annotation_values,
		.metadata = metadata_values,
	};

	if (
		parameter_names == NULL ||
		init_var_flags == NULL ||
		annotation_values == NULL ||
		metadata_values == NULL ||
		new_names == NULL ||
		default_by_name == NULL ||
		empty_extras == NULL ||
		append_inherited(base, &parameters, default_by_name, empty_extras) != RESULT_OK
	) {
		return plan;
	}

	Py_ssize_t const inherited_count = PyList_GET_SIZE(parameter_names);

	if (
		append_declared(
			annotations,
			namespace,
			inherited_count,
			&parameters,
			new_names,
			default_by_name,
			empty_extras
		) != RESULT_OK
	) {
		return plan;
	}

	return plan_from_parameters(&parameters, new_names, default_by_name);
}

void field_plan_clear(struct field_plan * const plan) {
	Py_CLEAR(plan->all_names);
	Py_CLEAR(plan->new_names);
	Py_CLEAR(plan->defaults);
	Py_CLEAR(plan->annotations);
	Py_CLEAR(plan->metadata);
	Py_CLEAR(plan->parameter_names);
	Py_CLEAR(plan->init_var_flags);
	Py_CLEAR(plan->init_var_names);
	Py_CLEAR(plan->init_var_defaults);
	Py_CLEAR(plan->init_var_annotations);
}

static PyObject * kind_filtered(
	PyObject * const values,
	PyObject * const init_var_flags,
	bool const init_vars,
	bool const has_init_vars
) {
	if (!has_init_vars) {
		return init_vars ? PyList_New(0) : Py_NewRef(values);
	}

	PY_MOVABLE(kept, PyList_New(0));

	if (kept == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(values); i += 1) {
		if (
			(PyList_GET_ITEM(init_var_flags, i) == Py_True) == init_vars &&
			PyList_Append(kept, PyList_GET_ITEM(values, i)) < 0
		) {
			return NULL;
		}
	}

	return py_move(&kept);
}

static PyObject * kind_filtered_tuple(
	PyObject * const values,
	PyObject * const init_var_flags,
	bool const init_vars,
	bool const has_init_vars
) {
	PY_OWNED(kept, kind_filtered(values, init_var_flags, init_vars, has_init_vars));

	return kept != NULL ? PyList_AsTuple(kept) : NULL;
}

static enum result refuse_misordered_parameters(
	struct parameter_lists const * const parameters,
	PyObject * const default_by_name
) {
	PyObject * first_defaulted_flag = NULL;

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(parameters->names); i += 1) {
		PyObject * const parameter_name = PyList_GET_ITEM(parameters->names, i);
		PyObject * const init_var_flag = PyList_GET_ITEM(parameters->init_var_flags, i);
		int const has_default = PyDict_Contains(default_by_name, parameter_name);

		if (has_default < 0) {
			return RESULT_ERROR;
		}

		if (has_default == 1) {
			first_defaulted_flag = (
				first_defaulted_flag != NULL ? first_defaulted_flag :
				init_var_flag
			);
		} else if (first_defaulted_flag != NULL) {
			PyErr_Format(
				PyExc_TypeError,
				"non-default %s '%U' follows %s with a default",
				init_var_flag == Py_True ? "InitVar" : "field",
				parameter_name,
				first_defaulted_flag == Py_True ? "an InitVar" : "a field"
			);

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static struct field_plan plan_from_parameters(
	struct parameter_lists const * const parameters,
	PyObject * const new_names,
	PyObject * const default_by_name
) {
	int const has_init_vars = PySequence_Contains(parameters->init_var_flags, Py_True);

	if (
		has_init_vars < 0 ||
		(
			has_init_vars == 1 &&
			refuse_misordered_parameters(parameters, default_by_name) != RESULT_OK
		)
	) {
		return (struct field_plan){0};
	}

	PY_MOVABLE(
		all_names,
		kind_filtered(parameters->names, parameters->init_var_flags, false, has_init_vars)
	);
	PY_OWNED(
		init_var_name_list,
		kind_filtered(parameters->names, parameters->init_var_flags, true, has_init_vars)
	);
	struct field_plan plan = {
		.defaults = all_names != NULL ? build_defaults(all_names, default_by_name) : NULL,
		.annotations = kind_filtered_tuple(
			parameters->annotations,
			parameters->init_var_flags,
			false,
			has_init_vars
		),
		.metadata = kind_filtered_tuple(
			parameters->metadata,
			parameters->init_var_flags,
			false,
			has_init_vars
		),
		.parameter_names = Py_NewRef(parameters->names),
		.init_var_flags = (
			has_init_vars ? PyList_AsTuple(parameters->init_var_flags) :
			PyTuple_New(0)
		),
		.init_var_names = init_var_name_list != NULL ? PyList_AsTuple(init_var_name_list) : NULL,
		.init_var_defaults = (
			init_var_name_list != NULL ? build_defaults(init_var_name_list, default_by_name) :
			NULL
		),
		.init_var_annotations = kind_filtered_tuple(
			parameters->annotations,
			parameters->init_var_flags,
			true,
			has_init_vars
		),
	};

	if (
		all_names == NULL ||
		plan.defaults == NULL ||
		plan.annotations == NULL ||
		plan.metadata == NULL ||
		plan.init_var_flags == NULL ||
		plan.init_var_names == NULL ||
		plan.init_var_defaults == NULL ||
		plan.init_var_annotations == NULL
	) {
		field_plan_clear(&plan);

		return plan;
	}

	plan.all_names = py_move(&all_names);
	plan.new_names = Py_NewRef(new_names);

	return plan;
}

static PyObject * checked_annotations(PyObject * const namespace) {
	PY_MOVABLE(annotations, struct_annotations(namespace));

	if (annotations == NULL || PyDict_Check(annotations)) {
		return py_move(&annotations);
	}

	PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");

	return NULL;
}

static enum result append_parameter(
	struct parameter_lists const * const parameters,
	PyObject * const parameter_name,
	bool const init_var,
	PyObject * const annotation,
	PyObject * const metadata
) {
	return (
		(
			PyList_Append(parameters->names, parameter_name) >= 0 &&
			PyList_Append(parameters->init_var_flags, init_var ? Py_True : Py_False) >= 0 &&
			PyList_Append(parameters->annotations, annotation) >= 0 &&
			PyList_Append(parameters->metadata, metadata) >= 0
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

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

static enum result append_inherited(
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

	Py_ssize_t field_index = 0;
	Py_ssize_t init_var_index = 0;

	for (Py_ssize_t position = 0; position < struct_parameter_count(base); position += 1) {
		switch (struct_parameter_kind(base, position)) {
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
							false,
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
							true,
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

static struct annotation_entry annotation_entry_of(
	PyObject * const annotation,
	PyObject * const empty_extras,
	bool const typing_loaded
) {
	if (!typing_loaded || PyType_Check(annotation)) {
		return (struct annotation_entry){
			.annotation = Py_NewRef(annotation),
			.metadata = Py_NewRef(empty_extras),
		};
	}

	PY_MOVABLE(extras, optional_attribute(annotation, "__metadata__"));

	if (extras == NULL) {
		return PyErr_Occurred() ? (struct annotation_entry){0} : (struct annotation_entry){
			.annotation = Py_NewRef(annotation),
			.metadata = Py_NewRef(empty_extras),
		};
	}

	if (!PyTuple_Check(extras)) {
		PyErr_SetString(PyExc_TypeError, "Annotated metadata must be a tuple");

		return (struct annotation_entry){0};
	}

	PY_MOVABLE(origin, optional_attribute(annotation, "__origin__"));

	if (origin == NULL) {
		return PyErr_Occurred() ? (struct annotation_entry){0} : (struct annotation_entry){
			.annotation = Py_NewRef(annotation),
			.metadata = py_move(&extras),
		};
	}

	return (struct annotation_entry){.annotation = py_move(&origin), .metadata = py_move(&extras)};
}

static enum result append_annotation(
	PyObject * const annotation,
	struct parameter_lists const * const parameters,
	PyObject * const empty_extras,
	bool const typing_loaded
) {
	struct annotation_entry const entry = annotation_entry_of(
		annotation,
		empty_extras,
		typing_loaded
	);
	PY_OWNED(entry_annotation, entry.annotation);
	PY_OWNED(entry_metadata, entry.metadata);

	return (
		(
			entry_annotation != NULL &&
			PyList_Append(parameters->annotations, entry_annotation) >= 0 &&
			PyList_Append(parameters->metadata, entry_metadata) >= 0
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

static enum result redeclare_parameter(
	struct parameter_lists const * const parameters,
	Py_ssize_t const position,
	bool const init_var,
	struct annotation_entry const entry
) {
	if (entry.annotation == NULL) {
		return RESULT_ERROR;
	}

	int const annotation_set = PyList_SetItem(parameters->annotations, position, entry.annotation);
	int const metadata_set = PyList_SetItem(parameters->metadata, position, entry.metadata);

	return (
		(
			annotation_set == 0 &&
			metadata_set == 0 &&
			(
				PyList_SetItem(
					parameters->init_var_flags,
					position,
					Py_NewRef(init_var ? Py_True : Py_False)
				) == 0
			)
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

static enum result refuse_machinery_init_var(PyObject * const field_name) {
	if (!machinery_name(field_name)) {
		return RESULT_OK;
	}

	PyErr_Format(
		PyExc_TypeError,
		"'%U' cannot be an InitVar: a double-underscore name shadows "
		"Python's slot and protocol lookups; rename it",
		field_name
	);

	return RESULT_ERROR;
}

static enum result redeclare_inherited(
	struct parameter_lists const * const parameters,
	Py_ssize_t const position,
	PyObject * const field_name,
	PyObject * const annotation,
	struct special_form const special,
	bool const init_var,
	PyObject * const new_names,
	PyObject * const empty_extras,
	bool const typing_loaded
) {
	bool const inherited_init_var = (
		PyList_GET_ITEM(parameters->init_var_flags, position) == Py_True
	);

	if (init_var == inherited_init_var || (!init_var && special.name != NULL)) {
		return RESULT_OK;
	}

	if (init_var) {
		return (
			(
				refuse_machinery_init_var(field_name) == RESULT_OK &&
				(
					redeclare_parameter(
						parameters,
						position,
						true,
						(struct annotation_entry){
							.annotation = Py_NewRef(annotation),
							.metadata = Py_NewRef(empty_extras),
						}
					) == RESULT_OK
				)
			) ? RESULT_OK :
			RESULT_ERROR
		);
	}

	return (
		(
			PyList_Append(new_names, field_name) >= 0 &&
			(
				redeclare_parameter(
					parameters,
					position,
					false,
					annotation_entry_of(annotation, empty_extras, typing_loaded)
				) == RESULT_OK
			)
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

static enum result append_declared(
	PyObject * const annotations,
	PyObject * const namespace,
	Py_ssize_t const inherited_count,
	struct parameter_lists const * const parameters,
	PyObject * const new_names,
	PyObject * const default_by_name,
	PyObject * const empty_extras
) {
	if (PyDict_GET_SIZE(annotations) == 0) {
		return RESULT_OK;
	}

	__attribute__((cleanup(form_probes_clear))) struct form_probes probes = {0};

	probes.class_var = module_attribute("typing", "ClassVar");

	if (probes.class_var == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	probes.init_var = module_attribute("dataclasses", "InitVar");

	if (probes.init_var == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	PY_OWNED(declared, PyDict_Keys(annotations));

	if (declared == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t at = 0; at < PyList_GET_SIZE(declared); ++at) {
		PyObject * const field_name = PyList_GET_ITEM(declared, at);

		PY_OWNED(annotation, dict_value_ref(annotations, field_name));

		if (annotation == NULL) {
			if (PyErr_Occurred()) {
				return RESULT_ERROR;
			}

			continue;
		}

		if (PyUnicode_Check(annotation) && probes.class_var_name == NULL) {
			probes.class_var_name = PyUnicode_FromString(CLASS_VAR_FORM.name);
			probes.init_var_name = PyUnicode_FromString(INIT_VAR_FORM.name);

			if (probes.class_var_name == NULL || probes.init_var_name == NULL) {
				return RESULT_ERROR;
			}
		}

		if (!PyUnicode_CheckExact(field_name)) {
			PyErr_SetString(PyExc_TypeError, "annotation keys must be strings");

			return RESULT_ERROR;
		}

		if (refuse_reserved_name(field_name) != RESULT_OK) {
			return RESULT_ERROR;
		}

		if (PyUnicode_CompareWithASCIIString(field_name, "__signature__") == 0) {
			PyErr_Format(
				PyExc_TypeError,
				"'__signature__' is salix's signature machinery and cannot be a "
				"field; bind a signature in the class body without an annotation"
			);

			return RESULT_ERROR;
		}

		PY_OWNED(declared_default, dict_value_ref(namespace, field_name));

		if (declared_default == NULL) {
			if (PyErr_Occurred()) {
				return RESULT_ERROR;
			}
		} else if (PyDict_SetItem(default_by_name, field_name, declared_default) < 0) {
			return RESULT_ERROR;
		}

		struct inheritance_lookup const inherited =
			inherited_position(parameters->names, inherited_count, field_name);

		if (inherited.tag == INHERITANCE_ERROR) {
			return RESULT_ERROR;
		}

		struct special_form const special = special_form_of(annotation, &probes);

		if (PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		bool const init_var = (
			special.kind == SPECIAL_FORM_INIT_VAR &&
			init_var_top_level(annotation, &probes)
		);

		switch (inherited.tag) {
			case INHERITANCE_ERROR:
				return RESULT_ERROR;
			case INHERITANCE_INHERITED:
				if (
					redeclare_inherited(
						parameters,
						inherited.position,
						field_name,
						annotation,
						special,
						init_var,
						new_names,
						empty_extras,
						probes.class_var != NULL
					) != RESULT_OK
				) {
					return RESULT_ERROR;
				}

				continue;
			case INHERITANCE_NEW:
				break;
		}

		if (init_var) {
			if (
				refuse_machinery_init_var(field_name) != RESULT_OK ||
				(
					append_parameter(
						parameters,
						field_name,
						true,
						annotation,
						empty_extras
					) != RESULT_OK
				)
			) {
				return RESULT_ERROR;
			}

			continue;
		}

		if (special.name != NULL && special.kind == SPECIAL_FORM_CLASS_VAR) {
			bool const top_level = class_var_top_level(annotation, &probes);

			if (PyErr_Occurred()) {
				return RESULT_ERROR;
			}

			if (!top_level) {
				PyErr_Format(
					PyExc_TypeError,
					"'%U' is annotated %s, which salix does not support; "
					"write it below the fields, without an annotation",
					field_name,
					special.name
				);

				return RESULT_ERROR;
			}

			if (machinery_name(field_name)) {
				PyErr_Format(
					PyExc_TypeError,
					"'%U' cannot be a ClassVar: a double-underscore name shadows "
					"Python's slot and protocol lookups; rename it",
					field_name
				);

				return RESULT_ERROR;
			}

			if (declared_default == NULL) {
				PyErr_Format(
					PyExc_TypeError,
					"'%U' is annotated ClassVar without an assigned value; %s",
					field_name,
					CLASS_VAR_FORM.instead
				);

				return RESULT_ERROR;
			}

			continue;
		}

		if (special.name != NULL) {
			PyErr_Format(
				PyExc_TypeError,
				"'%U' is annotated %s inside another annotation, which salix does not support; %s",
				field_name,
				special.name,
				special.instead
			);

			return RESULT_ERROR;
		}

		if (
			PyList_Append(parameters->names, field_name) < 0 ||
			PyList_Append(parameters->init_var_flags, Py_False) < 0 ||
			PyList_Append(new_names, field_name) < 0 ||
			(
				append_annotation(
					annotation,
					parameters,
					empty_extras,
					probes.class_var != NULL
				) != RESULT_OK
			)
		) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static struct inheritance_lookup inherited_position(
	PyObject * const parameter_names,
	Py_ssize_t const inherited_count,
	PyObject * const field_name
) {
	for (Py_ssize_t i = 0; i < inherited_count; i += 1) {
		enum inheritance const inherited = PyObject_RichCompareBool(
			field_name,
			PyList_GET_ITEM(parameter_names, i),
			Py_EQ
		);

		if (inherited != INHERITANCE_NEW) {
			return (struct inheritance_lookup){.tag = inherited, .position = i};
		}
	}

	return (struct inheritance_lookup){.tag = INHERITANCE_NEW};
}

static PyObject * build_defaults(PyObject * const all_names, PyObject * const default_by_name) {
	Py_ssize_t const field_count = PyList_GET_SIZE(all_names);
	Py_ssize_t first_default = field_count;

	for (Py_ssize_t i = 0; i < field_count; ++i) {
		PyObject * const field_name = PyList_GET_ITEM(all_names, i);
		int const has_default = PyDict_Contains(default_by_name, field_name);

		if (has_default < 0) {
			return NULL;
		}

		if (has_default) {
			first_default = first_default == field_count ? i : first_default;
		} else if (first_default != field_count) {
			if (refuse_reserved_name(field_name) != RESULT_OK) {
				return NULL;
			}

			PyErr_Format(
				PyExc_TypeError,
				"non-default field '%U' follows a field with a default",
				field_name
			);

			return NULL;
		}
	}

	PyObject * const defaults = PyTuple_New(field_count - first_default);

	if (defaults == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = first_default; i < field_count; ++i) {
		PyObject * const field_name = PyList_GET_ITEM(all_names, i);
		PY_OWNED(value, dict_value_ref(default_by_name, field_name));

		if (value == NULL) {
			Py_DECREF(defaults);

			return NULL;
		}

		PyObject * const stored = struct_default_copy(value);

		if (stored == NULL) {
			Py_DECREF(defaults);

			return NULL;
		}

		PyTuple_SET_ITEM(defaults, i - first_default, stored);
	}

	return defaults;
}

#ifdef TESTING

#	include "../testing.h"

static PyObject * names_of(char const * const * const names, Py_ssize_t const count) {
	PyObject * const list = PyList_New(0);

	for (Py_ssize_t i = 0; i < count; ++i) {
		PyObject * const name = PyUnicode_FromString(names[i]);

		PyList_Append(list, name);
		Py_DECREF(name);
	}

	return list;
}

static PyObject * defaults_for(char const * const * const names, Py_ssize_t const count) {
	PyObject * const mapping = PyDict_New();

	for (Py_ssize_t i = 0; i < count; ++i) {
		PyObject * const value = PyLong_FromSsize_t(i);

		PyDict_SetItemString(mapping, names[i], value);
		Py_DECREF(value);
	}

	return mapping;
}

static void test_no_defaults_produces_an_empty_tuple(void) {
	char const * const names[] = {"a", "b"};
	PyObject * const all_names = names_of(names, 2);
	PyObject * const by_name = defaults_for(names, 0);
	PyObject * const defaults = build_defaults(all_names, by_name);

	TEST_ASSERT_NOT_NULL(defaults);
	TEST_ASSERT_EQUAL_INT(0, PyTuple_GET_SIZE(defaults));

	Py_DECREF(defaults);
	Py_DECREF(by_name);
	Py_DECREF(all_names);
}

static void test_only_the_trailing_run_becomes_defaults(void) {
	char const * const names[] = {"a", "b", "c"};
	char const * const defaulted[] = {"b", "c"};
	PyObject * const all_names = names_of(names, 3);
	PyObject * const by_name = defaults_for(defaulted, 2);
	PyObject * const defaults = build_defaults(all_names, by_name);

	TEST_ASSERT_NOT_NULL(defaults);
	TEST_ASSERT_EQUAL_INT(2, PyTuple_GET_SIZE(defaults));

	Py_DECREF(defaults);
	Py_DECREF(by_name);
	Py_DECREF(all_names);
}

static void test_a_required_field_after_a_default_is_rejected(void) {
	char const * const names[] = {"a", "b"};
	char const * const defaulted[] = {"a"};
	PyObject * const all_names = names_of(names, 2);
	PyObject * const by_name = defaults_for(defaulted, 1);
	PyObject * const defaults = build_defaults(all_names, by_name);

	TEST_ASSERT_NULL(defaults);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));

	PyErr_Clear();
	Py_DECREF(by_name);
	Py_DECREF(all_names);
}

void fields_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_no_defaults_produces_an_empty_tuple);
	RUN_TEST(test_only_the_trailing_run_becomes_defaults);
	RUN_TEST(test_a_required_field_after_a_default_is_rejected);
}

#endif
