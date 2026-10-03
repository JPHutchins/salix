#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

enum result write_slot(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index,
	PyObject * const value
) {
	char const * const name =
		PyUnicode_AsUTF8(PyTuple_GET_ITEM(type->struct_field_names, index));

	if (name == NULL) {
		return RESULT_ERROR;
	}

	PyMemberDef slot = {
		.name = name,
		.type = SLOT_MEMBER_TYPE,
		.offset = type->struct_slot_offsets[index],
	};

	return PyMember_SetOne((char *) self, &slot, value) == 0 ? RESULT_OK : RESULT_ERROR;
}

static struct field_lookup find_name(
	PyObject * const names,
	Py_ssize_t const count,
	PyObject * const name
) {
	for (Py_ssize_t i = 0; i < count; ++i) {
		if (name == PyTuple_GET_ITEM(names, i)) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_FOUND, .index = i};
		}
	}

	for (Py_ssize_t i = 0; i < count; ++i) {
		int const compared = PyUnicode_Compare(name, PyTuple_GET_ITEM(names, i));

		if (compared == 0) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_FOUND, .index = i};
		}

		if (compared == -1 && PyErr_Occurred()) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_ERROR};
		}
	}

	return (struct field_lookup){.tag = FIELD_LOOKUP_MISSING};
}

struct field_lookup find_field(StructType const * const type, PyObject * const name) {
	return find_name(type->struct_field_names, type->struct_field_count, name);
}

struct field_lookup find_init_var(StructType const * const type, PyObject * const name) {
	return find_name(type->struct_init_var_names, type->struct_init_var_count, name);
}

enum result run_post_init(StructType const * const type, PyObject * const self) {
	if (type->struct_post_init == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(returned, PyObject_CallOneArg(type->struct_post_init, self));

	return returned != NULL ? RESULT_OK : RESULT_ERROR;
}

PyObject * post_init_arguments_for(StructType const * const type, PyObject * const self) {
	PyObject * const arguments = PyTuple_New(1 + type->struct_init_var_count);

	if (arguments != NULL) {
		PyTuple_SET_ITEM(arguments, 0, Py_NewRef(self));
	}

	return arguments;
}

enum result run_post_init_with(
	StructType const * const type,
	PyObject * const post_init_arguments
) {
	if (type->struct_post_init == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(returned, PyObject_Call(type->struct_post_init, post_init_arguments, NULL));

	return returned != NULL ? RESULT_OK : RESULT_ERROR;
}

static enum result refuse_multiple_values(StructType const * const type, PyObject * const name) {
	PyErr_Format(
		PyExc_TypeError,
		"%.200s() got multiple values for argument '%U'",
		struct_type_name(type),
		name
	);

	return RESULT_ERROR;
}

static enum result bind_init_var(
	StructType const * const type,
	PyObject * const post_init_arguments,
	Py_ssize_t const index,
	PyObject * const name,
	PyObject * const value
) {
	if (PyTuple_GET_ITEM(post_init_arguments, 1 + index) != NULL) {
		return refuse_multiple_values(type, name);
	}

	PyTuple_SET_ITEM(post_init_arguments, 1 + index, Py_NewRef(value));

	return RESULT_OK;
}

enum result bind_parameter_named(
	StructType const * const type,
	PyObject * const self,
	PyObject * const post_init_arguments,
	PyObject * const name,
	PyObject * const value
) {
	struct field_lookup const field = find_field(type, name);

	switch (field.tag) {
		case FIELD_LOOKUP_ERROR:
			return RESULT_ERROR;
		case FIELD_LOOKUP_FOUND:
			return bind_named(type, self, name, value);
		case FIELD_LOOKUP_MISSING:
			break;
	}

	struct field_lookup const init_var = find_init_var(type, name);

	switch (init_var.tag) {
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

	return bind_init_var(type, post_init_arguments, init_var.index, name, value);
}

enum result bind_parameters(
	StructType const * const type,
	PyObject * const self,
	PyObject * const post_init_arguments,
	PyObject * const * const arguments,
	Py_ssize_t const positional_count,
	PyObject * const keyword_names
) {
	Py_ssize_t field_index = 0;
	Py_ssize_t init_var_index = 0;

	for (Py_ssize_t position = 0; position < positional_count; position += 1) {
		switch (struct_parameter_kind(type, position)) {
			case PARAMETER_FIELD:
				*struct_slot(type, self, field_index) = Py_NewRef(arguments[position]);
				field_index += 1;

				break;
			case PARAMETER_INIT_VAR:
				PyTuple_SET_ITEM(
					post_init_arguments,
					1 + init_var_index,
					Py_NewRef(arguments[position])
				);
				init_var_index += 1;

				break;
		}
	}

	Py_ssize_t const keyword_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;

	for (Py_ssize_t i = 0; i < keyword_count; i += 1) {
		if (
			bind_parameter_named(
				type,
				self,
				post_init_arguments,
				PyTuple_GET_ITEM(keyword_names, i),
				arguments[positional_count + i]
			) != RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

enum result fill_init_var_defaults(
	StructType const * const type,
	PyObject * const post_init_arguments
) {
	Py_ssize_t const required_count = struct_required_init_var_count(type);

	for (Py_ssize_t i = 0; i < type->struct_init_var_count; i += 1) {
		if (PyTuple_GET_ITEM(post_init_arguments, 1 + i) != NULL) {
			continue;
		}

		if (i < required_count) {
			PyErr_Format(
				PyExc_TypeError,
				"%.200s() missing required argument '%U'",
				struct_type_name(type),
				PyTuple_GET_ITEM(type->struct_init_var_names, i)
			);

			return RESULT_ERROR;
		}

		PyObject * const value = struct_default_copy(
			PyTuple_GET_ITEM(type->struct_init_var_defaults, i - required_count)
		);

		if (value == NULL) {
			return RESULT_ERROR;
		}

		PyTuple_SET_ITEM(post_init_arguments, 1 + i, value);
	}

	return RESULT_OK;
}

void bind_positional(
	StructType const * const type,
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const positional_count,
	bool const family_constructed
) {
#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		bool const member_shape = family_constructed || positional_count == 1;

		for (Py_ssize_t i = 0; i < positional_count; ++i) {
			Py_ssize_t const target = (
				member_shape ? (
					i == 0 ? (
						type->struct_message_index >= 0 ? type->struct_message_index :
						positional_count == 1 && type->struct_exceptions_index >= 0 ? type->struct_exceptions_index :
						0
					) :
					i == 1 && type->struct_exceptions_index >= 0 ? type->struct_exceptions_index :
					i
				) :
				i
			);

			if (*struct_slot(type, self, target) == NULL) {
				*struct_slot(type, self, target) = Py_NewRef(arguments[i]);
			} else if (family_constructed && i == 1 && target == type->struct_exceptions_index) {
				Py_SETREF(*struct_slot(type, self, target), Py_NewRef(arguments[i]));
			}
		}

		return;
	}
#endif

	for (Py_ssize_t i = 0; i < positional_count; ++i) {
		*struct_slot(type, self, i) = Py_NewRef(arguments[i]);
	}
}

enum result bind_keywords(
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
				arguments[positional_count + i]
			) != RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

enum result bind_named(
	StructType const * const type,
	PyObject * const self,
	PyObject * const name,
	PyObject * const value
) {
	struct field_lookup const found = named_field(type, name);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
		case FIELD_LOOKUP_MISSING:
			return RESULT_ERROR;
		case FIELD_LOOKUP_FOUND:
			break;
	}

	PyObject * * const slot = struct_slot(type, self, found.index);

	if (*slot != NULL) {
		return refuse_multiple_values(type, name);
	}

	*slot = Py_NewRef(value);

	return RESULT_OK;
}

struct field_lookup named_field(StructType const * const type, PyObject * const name) {
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
enum result fill_defaults(
	StructType const * const type,
	PyObject * const self,
	bool const require_all
) {
	Py_ssize_t const required_count = struct_required_count(type);

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * * const slot = struct_slot(type, self, i);

		if (*slot != NULL) {
			continue;
		}

		if (i < required_count) {
			if (!require_all) {
				continue;
			}

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

#ifdef TESTING

#	include "../testing.h"

static char const points_source[] = {
#	embed "../../tests/c/fixtures/construct/points.py" suffix(, '\0')
};

static char const seeds_source[] = {
#	embed "../../tests/c/fixtures/construct/seeds.py" suffix(, '\0')
};

#	if PY_VERSION_HEX >= 0x030B0000
static char const groups_source[] = {
#		embed "../../tests/c/fixtures/construct/groups.py" suffix(, '\0')
};
#	endif

static PyObject * unbound_instance(PyObject * const cls) {
	return ((PyTypeObject *) cls)->tp_alloc((PyTypeObject *) cls, 0);
}

static void test_an_interned_name_resolves_by_identity(void) {
	PyObject * const instance = testing_two_field_instance();
	PyObject * const name = PyUnicode_InternFromString("beta");
	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(1, found.index);

	Py_DECREF(name);
	Py_DECREF(instance);
}

static void test_a_name_assembled_at_runtime_resolves_by_comparison(void) {
	PyObject * const instance = testing_two_field_instance();
	PyObject * const fields = struct_type_of(instance)->struct_field_names;
	PyObject * const name = PyUnicode_FromFormat("%s%s", "al", "pha");

	TEST_ASSERT_NOT_EQUAL(PyTuple_GET_ITEM(fields, 0), name);

	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(0, found.index);

	Py_DECREF(name);
	Py_DECREF(instance);
}

static void test_a_name_that_is_not_a_field_is_missing(void) {
	PyObject * const instance = testing_two_field_instance();
	PyObject * const name = PyUnicode_FromString("gamma");
	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_MISSING, found.tag);

	Py_DECREF(name);
	Py_DECREF(instance);
}

static void test_a_named_value_binds_an_unbound_field_once(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const point = unbound_instance(testing_entry(fixtures, "point"));

	TEST_ASSERT_NOT_NULL(point);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_named(
			struct_type_of(point),
			point,
			testing_entry(fixtures, "x"),
			testing_entry(fixtures, "first")
		)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		*struct_slot(struct_type_of(point), point, 0)
	);
	TEST_ASSERT_EQUAL_INT(
		RESULT_ERROR,
		bind_named(
			struct_type_of(point),
			point,
			testing_entry(fixtures, "x"),
			testing_entry(fixtures, "second")
		)
	);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		*struct_slot(struct_type_of(point), point, 0)
	);

	Py_DECREF(point);
	Py_DECREF(fixtures);
}

static void test_a_named_value_for_a_name_that_is_not_a_field_is_refused(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const point = unbound_instance(testing_entry(fixtures, "point"));

	TEST_ASSERT_NOT_NULL(point);
	TEST_ASSERT_EQUAL_INT(
		RESULT_ERROR,
		bind_named(
			struct_type_of(point),
			point,
			testing_entry(fixtures, "z"),
			testing_entry(fixtures, "first")
		)
	);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();

	Py_DECREF(point);
	Py_DECREF(fixtures);
}

static void test_positionals_bind_in_declaration_order(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const point = unbound_instance(testing_entry(fixtures, "point"));

	TEST_ASSERT_NOT_NULL(point);

	bind_positional(
		struct_type_of(point),
		point,
		(PyObject * const []){testing_entry(fixtures, "first"), testing_entry(fixtures, "second")},
		2,
		false
	);

	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		*struct_slot(struct_type_of(point), point, 0)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "second"),
		*struct_slot(struct_type_of(point), point, 1)
	);

	Py_DECREF(point);
	Py_DECREF(fixtures);
}

static void test_keywords_bind_after_the_positionals_they_follow(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const point = unbound_instance(testing_entry(fixtures, "point"));

	TEST_ASSERT_NOT_NULL(point);

	bind_positional(
		struct_type_of(point),
		point,
		(PyObject * const []){testing_entry(fixtures, "first")},
		1,
		false
	);

	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_keywords(
			struct_type_of(point),
			point,
			(PyObject * const []){testing_entry(fixtures, "first"), testing_entry(fixtures, "second")},
			1,
			testing_entry(fixtures, "keyword_y")
		)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		*struct_slot(struct_type_of(point), point, 0)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "second"),
		*struct_slot(struct_type_of(point), point, 1)
	);

	Py_DECREF(point);
	Py_DECREF(fixtures);
}

static void test_a_default_fills_only_an_unbound_slot(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const point = unbound_instance(testing_entry(fixtures, "point"));

	TEST_ASSERT_NOT_NULL(point);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_named(
			struct_type_of(point),
			point,
			testing_entry(fixtures, "x"),
			testing_entry(fixtures, "first")
		)
	);
	TEST_ASSERT_EQUAL_INT(RESULT_OK, fill_defaults(struct_type_of(point), point, true));
	TEST_ASSERT_EQUAL_INT(7, PyLong_AsLong(*struct_slot(struct_type_of(point), point, 1)));

	Py_DECREF(point);
	Py_DECREF(fixtures);
}

static void test_a_bound_default_keeps_its_value(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const point = unbound_instance(testing_entry(fixtures, "point"));

	TEST_ASSERT_NOT_NULL(point);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_keywords(
			struct_type_of(point),
			point,
			(PyObject * const []){testing_entry(fixtures, "second")},
			0,
			testing_entry(fixtures, "keyword_y")
		)
	);
	TEST_ASSERT_EQUAL_INT(RESULT_OK, fill_defaults(struct_type_of(point), point, false));
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "second"),
		*struct_slot(struct_type_of(point), point, 1)
	);

	Py_DECREF(point);
	Py_DECREF(fixtures);
}

static void test_an_unbound_required_field_is_an_error_only_when_required(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const point = unbound_instance(testing_entry(fixtures, "point"));

	TEST_ASSERT_NOT_NULL(point);
	TEST_ASSERT_EQUAL_INT(RESULT_OK, fill_defaults(struct_type_of(point), point, false));
	TEST_ASSERT_EQUAL_INT(7, PyLong_AsLong(*struct_slot(struct_type_of(point), point, 1)));
	TEST_ASSERT_EQUAL_INT(RESULT_ERROR, fill_defaults(struct_type_of(point), point, true));
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();

	Py_DECREF(point);
	Py_DECREF(fixtures);
}

static void test_the_post_init_hook_runs_and_its_error_propagates(void) {
	PyObject * const fixtures = testing_evaluate(points_source);
	PyObject * const logged = unbound_instance(testing_entry(fixtures, "logged"));
	PyObject * const refused = unbound_instance(testing_entry(fixtures, "refused"));

	TEST_ASSERT_NOT_NULL(logged);
	TEST_ASSERT_NOT_NULL(refused);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_named(
			struct_type_of(logged),
			logged,
			testing_entry(fixtures, "x"),
			testing_entry(fixtures, "first")
		)
	);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_named(
			struct_type_of(refused),
			refused,
			testing_entry(fixtures, "x"),
			testing_entry(fixtures, "first")
		)
	);
	TEST_ASSERT_EQUAL_INT(RESULT_OK, run_post_init(struct_type_of(logged), logged));
	TEST_ASSERT_EQUAL_INT(1, PyList_GET_SIZE(testing_entry(fixtures, "calls")));
	TEST_ASSERT_EQUAL_INT(RESULT_ERROR, run_post_init(struct_type_of(refused), refused));
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
	PyErr_Clear();

	Py_DECREF(refused);
	Py_DECREF(logged);
	Py_DECREF(fixtures);
}

static void test_positionals_bind_fields_and_init_vars_in_parameter_order(void) {
	PyObject * const fixtures = testing_evaluate(seeds_source);
	PyObject * const seeded = unbound_instance(testing_entry(fixtures, "seeded"));

	TEST_ASSERT_NOT_NULL(seeded);

	PyObject * const post_init_arguments = post_init_arguments_for(struct_type_of(seeded), seeded);

	TEST_ASSERT_NOT_NULL(post_init_arguments);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_parameters(
			struct_type_of(seeded),
			seeded,
			post_init_arguments,
			(PyObject * const []){
				testing_entry(fixtures, "first"),
				testing_entry(fixtures, "second"),
				testing_entry(fixtures, "third"),
			},
			3,
			NULL
		)
	);
	TEST_ASSERT_EQUAL_PTR(seeded, PyTuple_GET_ITEM(post_init_arguments, 0));
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		*struct_slot(struct_type_of(seeded), seeded, 0)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "second"),
		PyTuple_GET_ITEM(post_init_arguments, 1)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "third"),
		*struct_slot(struct_type_of(seeded), seeded, 1)
	);
	TEST_ASSERT_NULL(PyTuple_GET_ITEM(post_init_arguments, 2));

	Py_DECREF(post_init_arguments);
	Py_DECREF(seeded);
	Py_DECREF(fixtures);
}

static void test_a_named_init_var_binds_once_and_an_unknown_name_is_refused(void) {
	PyObject * const fixtures = testing_evaluate(seeds_source);
	PyObject * const seeded = unbound_instance(testing_entry(fixtures, "seeded"));

	TEST_ASSERT_NOT_NULL(seeded);

	PyObject * const post_init_arguments = post_init_arguments_for(struct_type_of(seeded), seeded);

	TEST_ASSERT_NOT_NULL(post_init_arguments);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_parameter_named(
			struct_type_of(seeded),
			seeded,
			post_init_arguments,
			testing_entry(fixtures, "scale"),
			testing_entry(fixtures, "first")
		)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		PyTuple_GET_ITEM(post_init_arguments, 2)
	);
	TEST_ASSERT_EQUAL_INT(
		RESULT_ERROR,
		bind_parameter_named(
			struct_type_of(seeded),
			seeded,
			post_init_arguments,
			testing_entry(fixtures, "scale"),
			testing_entry(fixtures, "second")
		)
	);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		PyTuple_GET_ITEM(post_init_arguments, 2)
	);
	TEST_ASSERT_EQUAL_INT(
		RESULT_ERROR,
		bind_parameter_named(
			struct_type_of(seeded),
			seeded,
			post_init_arguments,
			testing_entry(fixtures, "seed"),
			testing_entry(fixtures, "second")
		)
	);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();

	Py_DECREF(post_init_arguments);
	Py_DECREF(seeded);
	Py_DECREF(fixtures);
}

static void test_an_init_var_default_fills_only_an_unbound_init_var(void) {
	PyObject * const fixtures = testing_evaluate(seeds_source);
	PyObject * const seeded = unbound_instance(testing_entry(fixtures, "seeded"));

	TEST_ASSERT_NOT_NULL(seeded);

	PyObject * const post_init_arguments = post_init_arguments_for(struct_type_of(seeded), seeded);

	TEST_ASSERT_NOT_NULL(post_init_arguments);
	TEST_ASSERT_EQUAL_INT(
		RESULT_ERROR,
		fill_init_var_defaults(struct_type_of(seeded), post_init_arguments)
	);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		bind_parameter_named(
			struct_type_of(seeded),
			seeded,
			post_init_arguments,
			testing_entry(fixtures, "flag"),
			testing_entry(fixtures, "first")
		)
	);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		fill_init_var_defaults(struct_type_of(seeded), post_init_arguments)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		PyTuple_GET_ITEM(post_init_arguments, 1)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "default_scale"),
		PyTuple_GET_ITEM(post_init_arguments, 2)
	);

	Py_DECREF(post_init_arguments);
	Py_DECREF(seeded);
	Py_DECREF(fixtures);
}

static void test_the_post_init_hook_receives_the_init_vars_after_the_instance(void) {
	PyObject * const fixtures = testing_evaluate(seeds_source);
	PyObject * const seeded = unbound_instance(testing_entry(fixtures, "seeded"));

	TEST_ASSERT_NOT_NULL(seeded);

	PyObject * const post_init_arguments = PyTuple_Pack(
		3,
		seeded,
		testing_entry(fixtures, "first"),
		testing_entry(fixtures, "second")
	);

	TEST_ASSERT_NOT_NULL(post_init_arguments);
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		run_post_init_with(struct_type_of(seeded), post_init_arguments)
	);
	TEST_ASSERT_EQUAL_INT(1, PyList_GET_SIZE(testing_entry(fixtures, "calls")));
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "first"),
		PyTuple_GET_ITEM(PyList_GET_ITEM(testing_entry(fixtures, "calls"), 0), 0)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "second"),
		PyTuple_GET_ITEM(PyList_GET_ITEM(testing_entry(fixtures, "calls"), 0), 1)
	);

	Py_DECREF(post_init_arguments);
	Py_DECREF(seeded);
	Py_DECREF(fixtures);
}

#	if PY_VERSION_HEX >= 0x030B0000
static void test_a_group_payload_binds_by_member_index(void) {
	PyObject * const fixtures = testing_evaluate(groups_source);
	PyObject * const accepted = unbound_instance(testing_entry(fixtures, "grouped"));
	PyObject * const single = unbound_instance(testing_entry(fixtures, "grouped"));

	TEST_ASSERT_NOT_NULL(accepted);
	TEST_ASSERT_NOT_NULL(single);

	bind_positional(
		struct_type_of(accepted),
		accepted,
		(PyObject * const []){
			testing_entry(fixtures, "message"),
			testing_entry(fixtures, "exceptions"),
		},
		2,
		true
	);
	bind_positional(
		struct_type_of(single),
		single,
		(PyObject * const []){testing_entry(fixtures, "message")},
		1,
		false
	);

	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "exceptions"),
		*struct_slot(struct_type_of(accepted), accepted, 0)
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "message"),
		*struct_slot(struct_type_of(accepted), accepted, 1)
	);
	TEST_ASSERT_NULL(*struct_slot(struct_type_of(single), single, 0));
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(fixtures, "message"),
		*struct_slot(struct_type_of(single), single, 1)
	);

	Py_DECREF(single);
	Py_DECREF(accepted);
	Py_DECREF(fixtures);
}
#	endif

void construct_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_interned_name_resolves_by_identity);
	RUN_TEST(test_a_name_assembled_at_runtime_resolves_by_comparison);
	RUN_TEST(test_a_name_that_is_not_a_field_is_missing);
	RUN_TEST(test_a_named_value_binds_an_unbound_field_once);
	RUN_TEST(test_a_named_value_for_a_name_that_is_not_a_field_is_refused);
	RUN_TEST(test_positionals_bind_in_declaration_order);
	RUN_TEST(test_keywords_bind_after_the_positionals_they_follow);
	RUN_TEST(test_a_default_fills_only_an_unbound_slot);
	RUN_TEST(test_a_bound_default_keeps_its_value);
	RUN_TEST(test_an_unbound_required_field_is_an_error_only_when_required);
	RUN_TEST(test_the_post_init_hook_runs_and_its_error_propagates);
	RUN_TEST(test_positionals_bind_fields_and_init_vars_in_parameter_order);
	RUN_TEST(test_a_named_init_var_binds_once_and_an_unknown_name_is_refused);
	RUN_TEST(test_an_init_var_default_fills_only_an_unbound_init_var);
	RUN_TEST(test_the_post_init_hook_receives_the_init_vars_after_the_instance);
#	if PY_VERSION_HEX >= 0x030B0000
	RUN_TEST(test_a_group_payload_binds_by_member_index);
#	endif
}

#endif
