#include <Python.h>
#include <stdbool.h>

#include "../annotations.h"
#include "../construct.h"
#include "fields.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static struct field_plan plan_from_parameters(
	struct parameter_lists const * parameters,
	PyObject * new_names,
	PyObject * default_by_name
);
static enum result plan_defaults(struct field_plan * plan);

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

enum result refuse_reserved_name(PyObject * const field_name) {
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

struct default_table {
	PyObject * defaults;
	Py_ssize_t * indices;
};

static struct default_table build_defaults(
	PyObject * names,
	PyObject * default_by_name,
	PyObject * keyword_only_names,
	PyObject * missing
);
static enum result refuse_misordered_parameters(
	struct parameter_lists const * parameters,
	PyObject * default_by_name
);
static PyObject * checked_annotations(PyObject * namespace);

struct field_plan field_plan_build(
	StructType const * const base,
	PyObject * const bases,
	PyObject * const namespace,
	bool const class_keyword_only,
	PyObject * const missing
) {
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
	PY_OWNED(mro_default_names, PyList_New(0));
	PY_OWNED(empty_extras, PyTuple_New(0));
	PY_OWNED(keyword_only_names, PySet_New(NULL));
	struct parameter_lists const parameters = {
		.names = parameter_names,
		.init_var_flags = init_var_flags,
		.annotations = annotation_values,
		.metadata = metadata_values,
		.keyword_only_names = keyword_only_names,
	};

	if (
		keyword_only_names == NULL ||
		parameter_names == NULL ||
		init_var_flags == NULL ||
		annotation_values == NULL ||
		metadata_values == NULL ||
		new_names == NULL ||
		default_by_name == NULL ||
		mro_default_names == NULL ||
		empty_extras == NULL ||
		append_inherited(base, &parameters, default_by_name, empty_extras) != RESULT_OK ||
		(
			merge_secondary_class_vars(
				bases,
				base,
				&parameters,
				default_by_name,
				empty_extras
			) != RESULT_OK
		)
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
			mro_default_names,
			empty_extras,
			class_keyword_only
		) != RESULT_OK
	) {
		return plan;
	}

	struct field_plan plan_built = plan_from_parameters(&parameters, new_names, default_by_name);

	if (plan_built.all_names == NULL) {
		return plan_built;
	}

	PY_OWNED(declared_list, PyDict_Keys(annotations));
	plan_built.declared_names = declared_list != NULL ? PyList_AsTuple(declared_list) : NULL;
	plan_built.declared_annotations = Py_NewRef(annotations);
	plan_built.mro_default_names = PyList_AsTuple(mro_default_names);
	plan_built.default_by_name = Py_NewRef(default_by_name);
	plan_built.keyword_only_names = PyFrozenSet_New(keyword_only_names);
	plan_built.missing = Py_NewRef(missing);

	if (
		plan_built.keyword_only_names == NULL ||
		plan_built.declared_names == NULL ||
		plan_built.declared_annotations == NULL ||
		plan_built.mro_default_names == NULL ||
		(PyList_GET_SIZE(mro_default_names) == 0 && plan_defaults(&plan_built) != RESULT_OK)
	) {
		field_plan_clear(&plan_built);
	}

	return plan_built;
}

PyObject * positional_parameter_names(struct field_plan const * const plan) {
	if (PySet_GET_SIZE(plan->keyword_only_names) == 0) {
		return PyList_AsTuple(plan->parameter_names);
	}

	PY_MOVABLE(positional, PyList_New(0));

	if (positional == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(plan->parameter_names); i += 1) {
		PyObject * const name = PyList_GET_ITEM(plan->parameter_names, i);
		int const keyword_only = PySet_Contains(plan->keyword_only_names, name);

		if (keyword_only < 0 || (keyword_only == 0 && PyList_Append(positional, name) < 0)) {
			return NULL;
		}
	}

	return PyList_AsTuple(positional);
}

void field_plan_clear(struct field_plan * const plan) {
	Py_CLEAR(plan->all_names);
	Py_CLEAR(plan->new_names);
	Py_CLEAR(plan->defaults);
	PyMem_Free(plan->field_default_indices);
	plan->field_default_indices = NULL;
	Py_CLEAR(plan->annotations);
	Py_CLEAR(plan->metadata);
	Py_CLEAR(plan->parameter_names);
	Py_CLEAR(plan->init_var_flags);
	Py_CLEAR(plan->init_var_names);
	Py_CLEAR(plan->init_var_defaults);
	PyMem_Free(plan->init_var_default_indices);
	plan->init_var_default_indices = NULL;
	Py_CLEAR(plan->init_var_annotations);
	Py_CLEAR(plan->declared_names);
	Py_CLEAR(plan->declared_annotations);
	Py_CLEAR(plan->class_var_positions);
	Py_CLEAR(plan->mro_default_names);
	Py_CLEAR(plan->default_by_name);
	Py_CLEAR(plan->keyword_only_names);
	Py_CLEAR(plan->missing);
}

static PyObject * class_value_in_mro(PyTypeObject * const created, PyObject * const name) {
	for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(created->tp_mro); i += 1) {
		PY_OWNED(
			entry_dict,
			struct_type_dict((PyTypeObject *) PyTuple_GET_ITEM(created->tp_mro, i))
		);

		if (entry_dict == NULL) {
			return NULL;
		}

		PY_MOVABLE(value, dict_value_ref(entry_dict, name));

		if (value != NULL || PyErr_Occurred()) {
			return py_move(&value);
		}
	}

	return NULL;
}

static PyObject * class_default_of(PyTypeObject * const created, PyObject * const name) {
	PY_MOVABLE(found, class_value_in_mro(created, name));

	if (found == NULL) {
		return NULL;
	}

	descrgetfunc const bind = Py_TYPE(found)->tp_descr_get;
	PY_MOVABLE(value, bind != NULL ? bind(found, NULL, (PyObject *) created) : py_move(&found));

	if (value == NULL && !PyErr_Occurred()) {
		PyErr_Format(
			PyExc_SystemError,
			"%.200s.__get__ returned NULL without setting an exception",
			Py_TYPE(found)->tp_name
		);
	}

	return value != NULL && PyObject_TypeCheck(value, &PyMemberDescr_Type) ? NULL : py_move(&value);
}

enum result field_plan_resolve_mro_defaults(
	struct field_plan * const plan,
	PyTypeObject * const created
) {
	if (PyTuple_GET_SIZE(plan->mro_default_names) == 0) {
		return RESULT_OK;
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(plan->mro_default_names); i += 1) {
		PyObject * const name = PyTuple_GET_ITEM(plan->mro_default_names, i);
		PY_OWNED(value, class_default_of(created, name));

		if (
			value != NULL ? PyDict_SetItem(plan->default_by_name, name, value) < 0 :
			PyErr_Occurred() != NULL || PyDict_DelItem(plan->default_by_name, name) < 0
		) {
			return RESULT_ERROR;
		}
	}

	return plan_defaults(plan);
}

static enum result plan_defaults(struct field_plan * const plan) {
	if (
		PyTuple_GET_SIZE(plan->init_var_names) > 0 &&
		(
			refuse_misordered_parameters(
				&(struct parameter_lists){
					.names = plan->parameter_names,
					.init_var_flags = plan->init_var_flags,
					.keyword_only_names = plan->keyword_only_names,
				},
				plan->default_by_name
			) != RESULT_OK
		)
	) {
		return RESULT_ERROR;
	}

	struct default_table const field_table = build_defaults(
		plan->all_names,
		plan->default_by_name,
		plan->keyword_only_names,
		plan->missing
	);
	struct default_table const init_var_table = (
		field_table.defaults != NULL ? build_defaults(
			plan->init_var_names,
			plan->default_by_name,
			plan->keyword_only_names,
			plan->missing
		) :
		(struct default_table){0}
	);

	if (init_var_table.defaults == NULL) {
		Py_XDECREF(field_table.defaults);
		PyMem_Free(field_table.indices);

		return RESULT_ERROR;
	}

	plan->defaults = field_table.defaults;
	plan->field_default_indices = field_table.indices;
	plan->init_var_defaults = init_var_table.defaults;
	plan->init_var_default_indices = init_var_table.indices;

	return RESULT_OK;
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

	for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(parameters->names); i += 1) {
		PyObject * const parameter_name = PySequence_Fast_GET_ITEM(parameters->names, i);
		PyObject * const init_var_flag = PySequence_Fast_GET_ITEM(parameters->init_var_flags, i);
		int const has_default = PyDict_Contains(default_by_name, parameter_name);
		int const keyword_only = (
			has_default < 0 ? -1 :
			PySet_Contains(parameters->keyword_only_names, parameter_name)
		);

		if (keyword_only < 0) {
			return RESULT_ERROR;
		}

		if (keyword_only == 1) {
			continue;
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

static PyObject * class_var_positions_of(
	struct parameter_lists const * const parameters,
	PyObject * const default_by_name
) {
	PY_OWNED(positions, PyList_New(0));

	if (positions == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(parameters->init_var_flags); i += 1) {
		if (PyList_GET_ITEM(parameters->init_var_flags, i) != Py_None) {
			continue;
		}

		PyObject * const class_var_name = PyList_GET_ITEM(parameters->names, i);
		PY_OWNED(position, PyLong_FromSsize_t(i));
		PY_OWNED(value, dict_value_ref(default_by_name, class_var_name));

		if (value == NULL && !PyErr_Occurred()) {
			PyErr_Format(
				PyExc_SystemError,
				"salix internal error: ClassVar '%U' has no recorded value",
				class_var_name
			);
		}

		PY_OWNED(
			entry,
			position != NULL && value != NULL ? PyTuple_Pack(3, position, class_var_name, value) :
			NULL
		);

		if (entry == NULL || PyList_Append(positions, entry) < 0) {
			return NULL;
		}
	}

	return PyList_AsTuple(positions);
}

static PyObject * without_removed(PyObject * const values, PyObject * const init_var_flags) {
	PY_MOVABLE(kept, PyList_New(0));

	if (kept == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(values); i += 1) {
		if (
			PyList_GET_ITEM(init_var_flags, i) != Py_None &&
			PyList_Append(kept, PyList_GET_ITEM(values, i)) < 0
		) {
			return NULL;
		}
	}

	return py_move(&kept);
}

static struct field_plan plan_from_kept_parameters(
	struct parameter_lists const * const parameters,
	PyObject * const new_names
) {
	int const has_init_vars = PySequence_Contains(parameters->init_var_flags, Py_True);

	if (has_init_vars < 0) {
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
		.init_var_annotations = kind_filtered_tuple(
			parameters->annotations,
			parameters->init_var_flags,
			true,
			has_init_vars
		),
	};

	if (
		all_names == NULL ||
		plan.annotations == NULL ||
		plan.metadata == NULL ||
		plan.init_var_flags == NULL ||
		plan.init_var_names == NULL ||
		plan.init_var_annotations == NULL
	) {
		field_plan_clear(&plan);

		return plan;
	}

	plan.all_names = py_move(&all_names);
	plan.new_names = Py_NewRef(new_names);

	return plan;
}

static struct field_plan plan_from_parameters(
	struct parameter_lists const * const parameters,
	PyObject * const new_names,
	PyObject * const default_by_name
) {
	int const has_removed = PySequence_Contains(parameters->init_var_flags, Py_None);

	if (has_removed < 0) {
		return (struct field_plan){0};
	}

	if (has_removed == 1) {
		PY_OWNED(kept_names, without_removed(parameters->names, parameters->init_var_flags));
		PY_OWNED(
			kept_annotations,
			without_removed(parameters->annotations, parameters->init_var_flags)
		);
		PY_OWNED(kept_metadata, without_removed(parameters->metadata, parameters->init_var_flags));
		PY_OWNED(
			kept_flags,
			without_removed(parameters->init_var_flags, parameters->init_var_flags)
		);

		if (
			kept_names == NULL ||
			kept_annotations == NULL ||
			kept_metadata == NULL ||
			kept_flags == NULL
		) {
			return (struct field_plan){0};
		}

		struct field_plan plan = plan_from_kept_parameters(
			&(struct parameter_lists){
				.names = kept_names,
				.init_var_flags = kept_flags,
				.annotations = kept_annotations,
				.metadata = kept_metadata,
			},
			new_names
		);

		if (plan.all_names == NULL) {
			return plan;
		}

		plan.class_var_positions = class_var_positions_of(parameters, default_by_name);

		if (plan.class_var_positions == NULL) {
			field_plan_clear(&plan);
		}

		return plan;
	}

	return plan_from_kept_parameters(parameters, new_names);
}

static PyObject * checked_annotations(PyObject * const namespace) {
	PY_OWNED(annotations, struct_annotations(namespace));

	if (annotations == NULL) {
		return NULL;
	}

	if (PyDict_Check(annotations)) {
		return PyDict_Copy(annotations);
	}

	PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");

	return NULL;
}

enum result insert_parameter(
	struct parameter_lists const * const parameters,
	Py_ssize_t const at,
	PyObject * const parameter_name,
	PyObject * const kind_flag,
	PyObject * const annotation,
	PyObject * const metadata
) {
	return (
		(
			PyList_Insert(parameters->names, at, parameter_name) >= 0 &&
			PyList_Insert(parameters->init_var_flags, at, kind_flag) >= 0 &&
			PyList_Insert(parameters->annotations, at, annotation) >= 0 &&
			PyList_Insert(parameters->metadata, at, metadata) >= 0
		) ? RESULT_OK :
		RESULT_ERROR
	);
}

enum result append_parameter(
	struct parameter_lists const * const parameters,
	PyObject * const parameter_name,
	PyObject * const kind_flag,
	PyObject * const annotation,
	PyObject * const metadata
) {
	return insert_parameter(
		parameters,
		PyList_GET_SIZE(parameters->names),
		parameter_name,
		kind_flag,
		annotation,
		metadata
	);
}

struct inheritance_lookup inherited_position(
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

static struct default_table build_defaults(
	PyObject * const names,
	PyObject * const default_by_name,
	PyObject * const keyword_only_names,
	PyObject * const missing
) {
	Py_ssize_t const count = PySequence_Fast_GET_SIZE(names);
	Py_ssize_t * const indices = PyMem_New(Py_ssize_t, count > 0 ? count : 1);

	if (indices == NULL) {
		PyErr_NoMemory();

		return (struct default_table){0};
	}

	Py_ssize_t first_default = count;
	bool positional_default_seen = false;

	for (Py_ssize_t i = 0; i < count; ++i) {
		PyObject * const name = PySequence_Fast_GET_ITEM(names, i);
		int const has_default = PyDict_Contains(default_by_name, name);
		int const keyword_only = (
			has_default < 0 ? -1 :
			PySet_GET_SIZE(keyword_only_names) == 0 ? 0 :
			PySet_Contains(keyword_only_names, name)
		);

		if (keyword_only < 0) {
			PyMem_Free(indices);

			return (struct default_table){0};
		}

		if (has_default == 0 && keyword_only == 0 && positional_default_seen) {
			PyMem_Free(indices);

			if (refuse_reserved_name(name) != RESULT_OK) {
				return (struct default_table){0};
			}

			PyErr_Format(
				PyExc_TypeError,
				"non-default field '%U' follows a field with a default",
				name
			);

			return (struct default_table){0};
		}

		positional_default_seen = (
			positional_default_seen ||
			(has_default == 1 && keyword_only == 0)
		);
		first_default = has_default == 1 && first_default == count ? i : first_default;
		indices[i] = has_default == 1 ? i - first_default : NO_DEFAULT;
	}

	PyObject * const defaults = PyTuple_New(count - first_default);

	if (defaults == NULL) {
		PyMem_Free(indices);

		return (struct default_table){0};
	}

	for (Py_ssize_t i = first_default; i < count; ++i) {
		PY_OWNED(
			value,
			(
				indices[i] != NO_DEFAULT ?
					dict_value_ref(default_by_name, PySequence_Fast_GET_ITEM(names, i)) :
				Py_NewRef(missing)
			)
		);
		PyObject * const stored = (
			value == NULL ? NULL :
			indices[i] != NO_DEFAULT ? struct_default_copy(NULL, value) :
			Py_NewRef(value)
		);

		if (stored == NULL) {
			Py_DECREF(defaults);
			PyMem_Free(indices);

			return (struct default_table){0};
		}

		PyTuple_SET_ITEM(defaults, i - first_default, stored);
	}

	return (struct default_table){.defaults = defaults, .indices = indices};
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

static PyObject * name_set_of(char const * const * const names, Py_ssize_t const count) {
	PyObject * const list = names_of(names, count);
	PyObject * const set = PySet_New(list);

	Py_DECREF(list);

	return set;
}

static void test_no_defaults_produces_an_empty_tuple(void) {
	char const * const names[] = {"a", "b"};
	PyObject * const all_names = names_of(names, 2);
	PyObject * const by_name = defaults_for(names, 0);
	PyObject * const keyword_only = name_set_of(names, 0);
	struct default_table const table = build_defaults(all_names, by_name, keyword_only, Py_None);

	TEST_ASSERT_NOT_NULL(table.defaults);
	TEST_ASSERT_EQUAL_INT(0, PyTuple_GET_SIZE(table.defaults));
	TEST_ASSERT_EQUAL_INT(NO_DEFAULT, table.indices[0]);
	TEST_ASSERT_EQUAL_INT(NO_DEFAULT, table.indices[1]);

	Py_DECREF(table.defaults);
	PyMem_Free(table.indices);
	Py_DECREF(keyword_only);
	Py_DECREF(by_name);
	Py_DECREF(all_names);
}

static void test_only_the_trailing_run_becomes_defaults(void) {
	char const * const names[] = {"a", "b", "c"};
	char const * const defaulted[] = {"b", "c"};
	PyObject * const all_names = names_of(names, 3);
	PyObject * const by_name = defaults_for(defaulted, 2);
	PyObject * const keyword_only = name_set_of(names, 0);
	struct default_table const table = build_defaults(all_names, by_name, keyword_only, Py_None);

	TEST_ASSERT_NOT_NULL(table.defaults);
	TEST_ASSERT_EQUAL_INT(2, PyTuple_GET_SIZE(table.defaults));
	TEST_ASSERT_EQUAL_INT(NO_DEFAULT, table.indices[0]);
	TEST_ASSERT_EQUAL_INT(0, table.indices[1]);
	TEST_ASSERT_EQUAL_INT(1, table.indices[2]);
	TEST_ASSERT_EQUAL_INT(1, PyLong_AsLong(PyTuple_GET_ITEM(table.defaults, table.indices[2])));

	Py_DECREF(table.defaults);
	PyMem_Free(table.indices);
	Py_DECREF(keyword_only);
	Py_DECREF(by_name);
	Py_DECREF(all_names);
}

static void test_a_required_keyword_only_field_inside_the_trailing_defaults_holds_the_sentinel(
	void
) {
	char const * const names[] = {"a", "b", "c"};
	char const * const defaulted[] = {"a", "c"};
	char const * const keyword_only_names[] = {"b"};
	PyObject * const all_names = names_of(names, 3);
	PyObject * const by_name = defaults_for(defaulted, 2);
	PyObject * const keyword_only = name_set_of(keyword_only_names, 1);
	struct default_table const table = build_defaults(all_names, by_name, keyword_only, Py_None);

	TEST_ASSERT_NOT_NULL(table.defaults);
	TEST_ASSERT_EQUAL_INT(3, PyTuple_GET_SIZE(table.defaults));
	TEST_ASSERT_EQUAL_INT(0, PyLong_AsLong(PyTuple_GET_ITEM(table.defaults, 0)));
	TEST_ASSERT_EQUAL_PTR(Py_None, PyTuple_GET_ITEM(table.defaults, 1));
	TEST_ASSERT_EQUAL_INT(1, PyLong_AsLong(PyTuple_GET_ITEM(table.defaults, 2)));
	TEST_ASSERT_EQUAL_INT(0, table.indices[0]);
	TEST_ASSERT_EQUAL_INT(NO_DEFAULT, table.indices[1]);
	TEST_ASSERT_EQUAL_INT(2, table.indices[2]);

	Py_DECREF(table.defaults);
	PyMem_Free(table.indices);
	Py_DECREF(keyword_only);
	Py_DECREF(by_name);
	Py_DECREF(all_names);
}

static void test_a_required_field_after_a_default_is_rejected(void) {
	char const * const names[] = {"a", "b"};
	char const * const defaulted[] = {"a"};
	PyObject * const all_names = names_of(names, 2);
	PyObject * const by_name = defaults_for(defaulted, 1);
	PyObject * const keyword_only = name_set_of(names, 0);
	struct default_table const table = build_defaults(all_names, by_name, keyword_only, Py_None);

	TEST_ASSERT_NULL(table.defaults);
	TEST_ASSERT_NULL(table.indices);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));

	PyErr_Clear();
	Py_DECREF(keyword_only);
	Py_DECREF(by_name);
	Py_DECREF(all_names);
}

static PyObject * silent_descriptor_get(PyObject * self, PyObject * instance, PyObject * owner) {
	return NULL;
}

static PyTypeObject SilentDescriptorType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "tests.SilentDescriptor",
	.tp_basicsize = sizeof(PyObject),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_new = PyType_GenericNew,
	.tp_descr_get = silent_descriptor_get,
};

static PyObject * struct_class_of(
	PyObject * const struct_base,
	char const * const name,
	PyObject * const bases,
	PyObject * const namespace
) {
	PY_OWNED(class_name, PyUnicode_FromString(name));
	PY_OWNED(arguments, class_name != NULL ? PyTuple_Pack(3, class_name, bases, namespace) : NULL);

	return (
		arguments != NULL ? PyObject_Call((PyObject *) Py_TYPE(struct_base), arguments, NULL) :
		NULL
	);
}

static void test_a_class_var_whose_get_returns_null_without_an_error_fails_the_redeclaration(void) {
	TEST_ASSERT_EQUAL_INT(0, PyType_Ready(&SilentDescriptorType));

	PY_OWNED(salix, PyImport_ImportModule("salix"));
	PY_OWNED(typing, PyImport_ImportModule("typing"));
	TEST_ASSERT_NOT_NULL(salix);
	TEST_ASSERT_NOT_NULL(typing);

	PY_OWNED(struct_base, PyObject_GetAttrString(salix, "Struct"));
	PY_OWNED(class_var, PyObject_GetAttrString(typing, "ClassVar"));
	TEST_ASSERT_NOT_NULL(struct_base);
	TEST_ASSERT_NOT_NULL(class_var);

	PY_OWNED(class_var_of_object, PyObject_GetItem(class_var, (PyObject *) &PyBaseObject_Type));
	PY_OWNED(silent, PyObject_CallNoArgs((PyObject *) &SilentDescriptorType));
	TEST_ASSERT_NOT_NULL(class_var_of_object);
	TEST_ASSERT_NOT_NULL(silent);

	PY_OWNED(base_annotations, Py_BuildValue("{sO}", "x", class_var_of_object));
	TEST_ASSERT_NOT_NULL(base_annotations);
	PY_OWNED(
		base_namespace,
		Py_BuildValue("{sOsO}", "__annotations__", base_annotations, "x", silent)
	);
	PY_OWNED(struct_bases, PyTuple_Pack(1, struct_base));
	TEST_ASSERT_NOT_NULL(base_namespace);
	TEST_ASSERT_NOT_NULL(struct_bases);

	PY_OWNED(base, struct_class_of(struct_base, "Base", struct_bases, base_namespace));
	TEST_ASSERT_NOT_NULL(base);

	PY_OWNED(sub_annotations, Py_BuildValue("{sO}", "x", (PyObject *) &PyLong_Type));
	TEST_ASSERT_NOT_NULL(sub_annotations);
	PY_OWNED(sub_namespace, Py_BuildValue("{sO}", "__annotations__", sub_annotations));
	PY_OWNED(sub_bases, PyTuple_Pack(1, base));
	TEST_ASSERT_NOT_NULL(sub_namespace);
	TEST_ASSERT_NOT_NULL(sub_bases);

	PY_OWNED(sub, struct_class_of(struct_base, "Sub", sub_bases, sub_namespace));
	TEST_ASSERT_NULL(sub);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_SystemError));
	PyErr_Clear();
}

void fields_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_no_defaults_produces_an_empty_tuple);
	RUN_TEST(test_only_the_trailing_run_becomes_defaults);
	RUN_TEST(test_a_required_keyword_only_field_inside_the_trailing_defaults_holds_the_sentinel);
	RUN_TEST(test_a_required_field_after_a_default_is_rejected);
	RUN_TEST(test_a_class_var_whose_get_returns_null_without_an_error_fails_the_redeclaration);
}

#endif
