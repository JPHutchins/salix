#include <Python.h>
#include <stdbool.h>

#include "fields.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static void form_probes_clear(struct form_probes * const probes) {
	Py_CLEAR(probes->class_var);
	Py_CLEAR(probes->init_var);
	Py_CLEAR(probes->keyword_only_marker);
	Py_CLEAR(probes->class_var_name);
	Py_CLEAR(probes->init_var_name);
	Py_CLEAR(probes->keyword_only_marker_name);
}

static enum result forget_default(PyObject * const default_by_name, PyObject * const name) {
	int const present = PyDict_Contains(default_by_name, name);

	return (
		present < 0 || (present == 1 && PyDict_DelItem(default_by_name, name) < 0) ? RESULT_ERROR :
		RESULT_OK
	);
}

static bool chooses_keyword_only(struct field_specifier const specifier, bool const by_context) {
	switch (specifier.keyword_only) {
		case KEYWORD_ONLY_CHOSEN:
			return true;
		case KEYWORD_ONLY_DECLINED:
			return false;
		case KEYWORD_ONLY_UNSPECIFIED:
			break;
	}

	return by_context;
}

static enum result record_keyword_only(
	PyObject * const keyword_only_names,
	PyObject * const name,
	bool const keyword_only
) {
	return (
		(
			keyword_only ? PySet_Add(keyword_only_names, name) :
			PySet_GET_SIZE(keyword_only_names) == 0 ? 0 :
			PySet_Discard(keyword_only_names, name)
		) < 0 ? RESULT_ERROR :
		RESULT_OK
	);
}

struct annotation_entry {
	PyObject * annotation;
	PyObject * metadata;
};

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
	PyObject * const kind_flag,
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
			(PyList_SetItem(parameters->init_var_flags, position, Py_NewRef(kind_flag)) == 0)
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

static enum result redeclare_as_init_var(
	struct parameter_lists const * const parameters,
	Py_ssize_t const position,
	PyObject * const annotation,
	PyObject * const empty_extras
) {
	if (PyList_GET_ITEM(parameters->init_var_flags, position) == Py_True) {
		return RESULT_OK;
	}

	return redeclare_parameter(
		parameters,
		position,
		Py_True,
		(struct annotation_entry){
			.annotation = Py_NewRef(annotation),
			.metadata = Py_NewRef(empty_extras),
		}
	);
}

static enum result redeclare_as_field(
	struct parameter_lists const * const parameters,
	Py_ssize_t const position,
	PyObject * const field_name,
	PyObject * const annotation,
	PyObject * const new_names,
	PyObject * const empty_extras,
	bool const typing_loaded
) {
	if (PyList_GET_ITEM(parameters->init_var_flags, position) == Py_False) {
		return RESULT_OK;
	}

	return (
		(
			PyList_Append(new_names, field_name) >= 0 &&
			(
				redeclare_parameter(
					parameters,
					position,
					Py_False,
					annotation_entry_of(annotation, empty_extras, typing_loaded)
				) == RESULT_OK
			)
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

static enum result remove_parameter(
	struct parameter_lists const * const parameters,
	Py_ssize_t const position
) {
	return (
		(
			(
				PySet_Discard(
					parameters->keyword_only_names,
					PyList_GET_ITEM(parameters->names, position)
				) >= 0
			) &&
			PyList_SetItem(parameters->init_var_flags, position, Py_NewRef(Py_None)) == 0
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

enum result append_declared(
	PyObject * const annotations,
	PyObject * const namespace,
	Py_ssize_t const inherited_count,
	struct parameter_lists const * const parameters,
	PyObject * const new_names,
	PyObject * const default_by_name,
	PyObject * const mro_default_names,
	PyObject * const empty_extras,
	bool const class_keyword_only
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

	bool after_keyword_only_marker = false;
	bool keyword_only_marker_probed = false;

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
			probes.keyword_only_marker_name = PyUnicode_FromString("KW_ONLY");

			if (
				probes.class_var_name == NULL ||
				probes.init_var_name == NULL ||
				probes.keyword_only_marker_name == NULL
			) {
				return RESULT_ERROR;
			}
		}

		if (!PyUnicode_CheckExact(field_name)) {
			PyErr_SetString(PyExc_TypeError, "annotation keys must be strings");

			return RESULT_ERROR;
		}

		if (
			!keyword_only_marker_probed &&
			!PyType_Check(annotation) &&
			!PyUnicode_Check(annotation)
		) {
			probes.keyword_only_marker = module_attribute("dataclasses", "KW_ONLY");
			keyword_only_marker_probed = true;

			if (probes.keyword_only_marker == NULL && PyErr_Occurred()) {
				return RESULT_ERROR;
			}
		}

		bool const marker = marks_keyword_only(annotation, &probes);

		if (PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		if (marker) {
			if (after_keyword_only_marker) {
				PyErr_Format(
					PyExc_TypeError,
					"'%U' is KW_ONLY, but KW_ONLY has already been specified",
					field_name
				);

				return RESULT_ERROR;
			}

			after_keyword_only_marker = true;

			continue;
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

		PY_OWNED(bound, dict_value_ref(namespace, field_name));

		if (bound == NULL && PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		struct field_specifier const specifier = field_specifier_of(bound);
		PyObject * const declared_default = (
			specifier.tag == FIELD_SPECIFIER_PRESENT ? specifier.default_value :
			bound
		);

		if (
			declared_default != NULL ? PyDict_SetItem(
				default_by_name,
				field_name,
				declared_default
			) < 0 :
			specifier.tag == FIELD_SPECIFIER_PRESENT &&
				forget_default(default_by_name, field_name) != RESULT_OK
		) {
			return RESULT_ERROR;
		}

		bool const keyword_only = chooses_keyword_only(
			specifier,
			after_keyword_only_marker || class_keyword_only
		);

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

		bool const redeclared = inherited.tag == INHERITANCE_INHERITED;

		if (
			redeclared &&
			declared_default == NULL &&
			specifier.tag == FIELD_SPECIFIER_ABSENT &&
			PyList_GET_ITEM(parameters->init_var_flags, inherited.position) == Py_None &&
			PyList_Append(mro_default_names, field_name) < 0
		) {
			return RESULT_ERROR;
		}

		if (init_var) {
			if (
				refuse_machinery_init_var(field_name) != RESULT_OK ||
				(
					redeclared ? (
						redeclare_as_init_var(
							parameters,
							inherited.position,
							annotation,
							empty_extras
						) != RESULT_OK
					) :
					(
						append_parameter(
							parameters,
							field_name,
							Py_True,
							annotation,
							empty_extras
						) != RESULT_OK
					)
				) ||
				(
					record_keyword_only(
						parameters->keyword_only_names,
						field_name,
						keyword_only
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

			if (specifier.tag == FIELD_SPECIFIER_PRESENT) {
				PyErr_Format(
					PyExc_TypeError,
					"'%U' is annotated ClassVar, and field() describes a field; %s",
					field_name,
					CLASS_VAR_FORM.instead
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

			if (
				(
					redeclared ? remove_parameter(parameters, inherited.position) :
					append_parameter(parameters, field_name, Py_None, Py_None, empty_extras)
				) !=
				RESULT_OK
			) {
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
			record_keyword_only(
				parameters->keyword_only_names,
				field_name,
				keyword_only
			) != RESULT_OK
		) {
			return RESULT_ERROR;
		}

		if (redeclared) {
			if (
				redeclare_as_field(
					parameters,
					inherited.position,
					field_name,
					annotation,
					new_names,
					empty_extras,
					probes.class_var != NULL
				) != RESULT_OK
			) {
				return RESULT_ERROR;
			}

			continue;
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
