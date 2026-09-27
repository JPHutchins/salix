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

static enum result append_inherited(
	StructType const * base,
	PyObject * all_names,
	PyObject * default_by_name,
	PyObject * annotation_values,
	PyObject * metadata_values
);
static enum result append_declared(
	StructType const * base,
	PyObject * annotations,
	PyObject * namespace,
	PyObject * all_names,
	PyObject * new_names,
	PyObject * default_by_name,
	PyObject * annotation_values,
	PyObject * metadata_values,
	PyObject * empty_extras
);
static enum result append_annotation(
	PyObject * annotation,
	PyObject * annotation_values,
	PyObject * metadata_values,
	PyObject * empty_extras,
	bool typing_loaded
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

static bool class_var_machinery_name(PyObject * const name) {
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
static enum inheritance inherits_field(StructType const * base, PyObject * field_name);

struct field_plan field_plan_build(StructType const * const base, PyObject * const namespace) {
	struct field_plan plan = {0};

	PY_OWNED(annotations, checked_annotations(namespace));

	if (annotations == NULL) {
		return plan;
	}

	PY_MOVABLE(all_names, PyList_New(0));
	PY_MOVABLE(new_names, PyList_New(0));
	PY_MOVABLE(annotation_values, PyList_New(0));
	PY_MOVABLE(metadata_values, PyList_New(0));
	PY_OWNED(default_by_name, PyDict_New());
	PY_OWNED(empty_extras, PyTuple_New(0));

	if (
		all_names != NULL &&
		new_names != NULL &&
		annotation_values != NULL &&
		metadata_values != NULL &&
		default_by_name != NULL &&
		empty_extras != NULL &&
		(
			append_inherited(
				base,
				all_names,
				default_by_name,
				annotation_values,
				metadata_values
			) == RESULT_OK
		) &&
		(
			append_declared(
				base,
				annotations,
				namespace,
				all_names,
				new_names,
				default_by_name,
				annotation_values,
				metadata_values,
				empty_extras
			) == RESULT_OK
		)
	) {
		PyObject * built_defaults = build_defaults(all_names, default_by_name);
		PyObject * built_annotations = PyList_AsTuple(annotation_values);
		PyObject * built_metadata = PyList_AsTuple(metadata_values);

		if (built_defaults != NULL && built_annotations != NULL && built_metadata != NULL) {
			plan.defaults = built_defaults;
			plan.annotations = built_annotations;
			plan.metadata = built_metadata;
			plan.all_names = py_move(&all_names);
			plan.new_names = py_move(&new_names);
		} else {
			Py_XDECREF(built_defaults);
			Py_XDECREF(built_annotations);
			Py_XDECREF(built_metadata);
		}
	}

	return plan;
}

void field_plan_clear(struct field_plan * const plan) {
	Py_CLEAR(plan->all_names);
	Py_CLEAR(plan->new_names);
	Py_CLEAR(plan->defaults);
	Py_CLEAR(plan->annotations);
	Py_CLEAR(plan->metadata);
}

static PyObject * checked_annotations(PyObject * const namespace) {
	PY_MOVABLE(annotations, struct_annotations(namespace));

	if (annotations == NULL || PyDict_Check(annotations)) {
		return py_move(&annotations);
	}

	PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");

	return NULL;
}

static enum result append_inherited(
	StructType const * const base,
	PyObject * const all_names,
	PyObject * const default_by_name,
	PyObject * const annotation_values,
	PyObject * const metadata_values
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

	Py_ssize_t const required_count = struct_required_count(base);

	for (Py_ssize_t i = 0; i < base->struct_field_count; ++i) {
		PyObject * const field_name = PyTuple_GET_ITEM(base->struct_field_names, i);

		if (
			PyList_Append(all_names, field_name) < 0 ||
			PyList_Append(annotation_values, PyTuple_GET_ITEM(base->struct_annotations, i)) < 0 ||
			PyList_Append(metadata_values, PyTuple_GET_ITEM(base->struct_metadata, i)) < 0
		) {
			return RESULT_ERROR;
		}

		if (i < required_count) {
			continue;
		}

		PyObject * const inherited_default =
			PyTuple_GET_ITEM(base->struct_defaults, i - required_count);

		if (PyDict_SetItem(default_by_name, field_name, inherited_default) < 0) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result append_annotation(
	PyObject * const annotation,
	PyObject * const annotation_values,
	PyObject * const metadata_values,
	PyObject * const empty_extras,
	bool const typing_loaded
) {
	/* A type is the common case and cannot be the Annotated split; the probe
	 * costs a raise-and-clear cycle per field, so types skip it. */
	if (!typing_loaded || PyType_Check(annotation)) {
		return (
			(
				PyList_Append(annotation_values, annotation) >= 0 &&
				PyList_Append(metadata_values, empty_extras) >= 0
			) ? RESULT_OK :
			RESULT_ERROR
		);
	}

	PY_OWNED(extras, optional_attribute(annotation, "__metadata__"));

	if (extras == NULL) {
		if (PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		return (
			(
				PyList_Append(annotation_values, annotation) >= 0 &&
				PyList_Append(metadata_values, empty_extras) >= 0
			) ? RESULT_OK :
			RESULT_ERROR
		);
	}

	if (!PyTuple_Check(extras)) {
		PyErr_SetString(PyExc_TypeError, "Annotated metadata must be a tuple");

		return RESULT_ERROR;
	}

	PY_OWNED(origin, optional_attribute(annotation, "__origin__"));

	if (origin == NULL) {
		if (PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		return (
			(
				PyList_Append(annotation_values, annotation) >= 0 &&
				PyList_Append(metadata_values, extras) >= 0
			) ? RESULT_OK :
			RESULT_ERROR
		);
	}

	return (
		(
			PyList_Append(annotation_values, origin) >= 0 &&
			PyList_Append(metadata_values, extras) >= 0
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

static enum result append_declared(
	StructType const * const base,
	PyObject * const annotations,
	PyObject * const namespace,
	PyObject * const all_names,
	PyObject * const new_names,
	PyObject * const default_by_name,
	PyObject * const annotation_values,
	PyObject * const metadata_values,
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

		switch (inherits_field(base, field_name)) {
			case INHERITANCE_ERROR:
				return RESULT_ERROR;
			case INHERITANCE_INHERITED:
				continue;
			case INHERITANCE_NEW:
				break;
		}

		struct special_form const special = special_form_of(annotation, &probes);

		/* Before the answer is used at all, not only when it is "no form": the
		 * text path allocates on the way to either verdict, and a failure there
		 * must not be overwritten by a refusal that happens to agree. */
		if (PyErr_Occurred()) {
			return RESULT_ERROR;
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

			if (class_var_machinery_name(field_name)) {
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
				"'%U' is annotated %s, which salix does not support; %s",
				field_name,
				special.name,
				special.instead
			);

			return RESULT_ERROR;
		}

		if (
			PyList_Append(all_names, field_name) < 0 ||
			PyList_Append(new_names, field_name) < 0 ||
			(
				append_annotation(
					annotation,
					annotation_values,
					metadata_values,
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

static enum inheritance inherits_field(StructType const * const base, PyObject * const field_name) {
	Py_ssize_t const inherited_count = base != NULL ? base->struct_field_count : 0;

	for (Py_ssize_t i = 0; i < inherited_count; ++i) {
		enum inheritance const inherited = PyObject_RichCompareBool(
			field_name,
			PyTuple_GET_ITEM(base->struct_field_names, i),
			Py_EQ
		);

		if (inherited != INHERITANCE_NEW) {
			return inherited;
		}
	}

	return INHERITANCE_NEW;
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

		/* The stored default is what the class keeps, severed from the
		 * class-body object wherever the copy path can sever it; the share
		 * fallbacks deliberately keep the declared object.
		 *
		 * `_struct_defaults_` still hands the stored object out, so filling it
		 * through there defeats this. That route is out of contract. */
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
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_no_defaults_produces_an_empty_tuple);
	RUN_TEST(test_only_the_trailing_run_becomes_defaults);
	RUN_TEST(test_a_required_field_after_a_default_is_rejected);
}

#endif
