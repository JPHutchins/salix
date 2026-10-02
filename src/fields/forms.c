#include <Python.h>
#include <stdbool.h>

#include "fields.h"
#include "../owned.h"
#include "../types.h"

static struct special_form form_within(PyObject * annotation, struct form_probes const * probes);
static struct special_form named_special_form(PyObject * text, struct form_probes const * probes);
static struct special_form form_named_by(PyObject * text, struct form_probes const * probes);
static bool class_var_object_top_level(PyObject * annotation, struct form_probes const * probes);
static bool names_form(PyObject * text, PyObject * needle);
static bool names_form_matching(PyObject * text, PyObject * needle, bool top_only);
static bool names_form_at_top(PyObject * text, PyObject * needle);
static bool top_level_prefix(PyObject * text, Py_ssize_t until);
static bool top_level_suffix(PyObject * text, Py_ssize_t from);
static bool continues_identifier(Py_UCS4 character);
struct special_form const CLASS_VAR_FORM = {
	.kind = SPECIAL_FORM_CLASS_VAR,
	.name = "ClassVar",
	.instead = "a class variable is a constant, so assign the value in the class body",
};
struct special_form const INIT_VAR_FORM = {
	.kind = SPECIAL_FORM_INIT_VAR,
	.name = "InitVar",
	.instead = "take the value in a custom __init__ and write the fields with set_field",
};

enum : int {
	SPECIAL_FORM_NODES = 32,
};

struct form_frontier {
	PyObject * nodes[SPECIAL_FORM_NODES];
	int count;
};

static void form_frontier_clear(struct form_frontier * const frontier) {
	while (frontier->count > 0) {
		Py_DECREF(frontier->nodes[--frontier->count]);
	}
}

static void form_frontier_push(struct form_frontier * const frontier, PyObject * const node) {
	if (frontier->count < SPECIAL_FORM_NODES) {
		frontier->nodes[frontier->count++] = Py_NewRef(node);
	}
}

static struct special_form form_named_by(
	PyObject * const annotation,
	struct form_probes const * const probes
) {
	if (probes->class_var != NULL && annotation == probes->class_var) {
		return CLASS_VAR_FORM;
	}

	if (
		probes->init_var != NULL &&
		(annotation == probes->init_var || (PyObject *) Py_TYPE(annotation) == probes->init_var)
	) {
		return INIT_VAR_FORM;
	}

	return (struct special_form){0};
}

static bool class_var_object_top_level(
	PyObject * const annotation,
	struct form_probes const * const probes
) {
	if (probes->class_var == NULL || annotation == probes->class_var) {
		return annotation == probes->class_var;
	}

	if (PyType_Check(annotation)) {
		return false;
	}

	PY_OWNED(origin, optional_attribute(annotation, "__origin__"));

	if (origin == NULL || origin != probes->class_var) {
		return false;
	}

	PY_OWNED(metadata, optional_attribute(annotation, "__metadata__"));

	if (PyErr_Occurred()) {
		return false;
	}

	return metadata == NULL;
}

bool class_var_top_level(PyObject * const annotation, struct form_probes const * const probes) {
	if (PyUnicode_Check(annotation)) {
		return (
			probes->class_var_name != NULL &&
			names_form_at_top(annotation, probes->class_var_name)
		);
	}

	return class_var_object_top_level(annotation, probes);
}

struct special_form special_form_of(
	PyObject * const annotation,
	struct form_probes const * const probes
) {
	if (PyUnicode_Check(annotation)) {
		return named_special_form(annotation, probes);
	}

	if (probes->class_var == NULL && probes->init_var == NULL) {
		return (struct special_form){0};
	}

	return form_within(annotation, probes);
}

static struct special_form form_within(
	PyObject * const annotation,
	struct form_probes const * const probes
) {
	__attribute__((cleanup(form_frontier_clear))) struct form_frontier frontier = {0};

	form_frontier_push(&frontier, annotation);

	for (int at = 0; at < frontier.count; ++at) {
		PyObject * const current = frontier.nodes[at];
		struct special_form const named = form_named_by(current, probes);

		if (named.name != NULL) {
			return named;
		}

		if (PyType_Check(current)) {
			continue;
		}

		PY_OWNED(origin, optional_attribute(current, "__origin__"));

		if (origin != NULL) {
			form_frontier_push(&frontier, origin);
		}

		PY_OWNED(arguments, PyErr_Occurred() ? NULL : optional_attribute(current, "__args__"));

		if (PyErr_Occurred()) {
			return (struct special_form){0};
		}

		for (
			Py_ssize_t i = 0;
			arguments != NULL && PyTuple_Check(arguments) && i < PyTuple_GET_SIZE(arguments);
			++i
		) {
			form_frontier_push(&frontier, PyTuple_GET_ITEM(arguments, i));
		}

		if (origin != NULL || arguments != NULL) {
			continue;
		}

		PY_OWNED(aliased, optional_attribute(current, "__value__"));

		if (PyErr_Occurred()) {
			return (struct special_form){0};
		}

		if (aliased != NULL) {
			form_frontier_push(&frontier, aliased);
		}
	}

	return (struct special_form){0};
}

static struct special_form named_special_form(
	PyObject * const text,
	struct form_probes const * const probes
) {
	if (names_form(text, probes->class_var_name)) {
		return CLASS_VAR_FORM;
	}

	if (PyErr_Occurred()) {
		return (struct special_form){0};
	}

	return names_form(text, probes->init_var_name) ? INIT_VAR_FORM : (struct special_form){0};
}

static bool continues_identifier(Py_UCS4 const character) {
	PY_OWNED(probe, PyUnicode_New(2, character > 'a' ? character : 'a'));

	if (
		probe == NULL ||
		PyUnicode_WriteChar(probe, 0, 'a') < 0 ||
		PyUnicode_WriteChar(probe, 1, character) < 0
	) {
		return false;
	}

	return PyUnicode_IsIdentifier(probe) == 1;
}

static bool top_level_prefix(PyObject * const text, Py_ssize_t const until) {
	Py_ssize_t last_non_space = -1;

	for (Py_ssize_t at = 0; at < until; ++at) {
		Py_UCS4 const character = PyUnicode_ReadChar(text, at);

		if (
			character != '.' &&
			!Py_UNICODE_ISSPACE(character) &&
			!continues_identifier(character)
		) {
			return false;
		}

		if (!Py_UNICODE_ISSPACE(character)) {
			last_non_space = at;
		}
	}

	return last_non_space == -1 || PyUnicode_ReadChar(text, last_non_space) == '.';
}

static bool top_level_suffix(PyObject * const text, Py_ssize_t const from) {
	Py_ssize_t const length = PyUnicode_GET_LENGTH(text);
	Py_ssize_t at = from;

	while (at < length && Py_UNICODE_ISSPACE(PyUnicode_ReadChar(text, at))) {
		++at;
	}

	if (at < length && PyUnicode_ReadChar(text, at) == '[') {
		int depth = 0;
		bool single_quoted = false;
		bool double_quoted = false;

		for (; at < length; ++at) {
			Py_UCS4 const character = PyUnicode_ReadChar(text, at);

			if (single_quoted) {
				if (character == '\\') {
					++at;
					continue;
				}

				single_quoted = character != '\'';
				continue;
			}

			if (double_quoted) {
				if (character == '\\') {
					++at;
					continue;
				}

				double_quoted = character != '"';
				continue;
			}

			if (character == '\'') {
				single_quoted = true;
				continue;
			}

			if (character == '"') {
				double_quoted = true;
				continue;
			}

			if (character == '[') {
				++depth;
				continue;
			}

			if (character == ']') {
				--depth;

				if (depth == 0) {
					++at;

					break;
				}
			}

			if (character == ',' && depth == 1) {
				return false;
			}
		}

		if (depth != 0) {
			return false;
		}
	}

	while (at < length && Py_UNICODE_ISSPACE(PyUnicode_ReadChar(text, at))) {
		++at;
	}

	return at == length;
}

static bool names_form_matching(
	PyObject * const text,
	PyObject * const needle,
	bool const top_only
) {
	Py_ssize_t const length = PyUnicode_GET_LENGTH(text);
	Py_ssize_t const form_length = PyUnicode_GET_LENGTH(needle);

	for (Py_ssize_t at = 0; at + form_length <= length; ++at) {
		Py_ssize_t const found = PyUnicode_Find(text, needle, at, length, 1);

		if (found < 0) {
			return false;
		}

		bool const opens = found == 0 || !continues_identifier(PyUnicode_ReadChar(text, found - 1));
		bool const closes = (
			found + form_length == length ||
			!continues_identifier(PyUnicode_ReadChar(text, found + form_length))
		);

		if (
			opens &&
			closes &&
			(
				!top_only ||
				(top_level_prefix(text, found) && top_level_suffix(text, found + form_length))
			)
		) {
			return true;
		}

		at = found;
	}

	return false;
}

static bool names_form(PyObject * const text, PyObject * const needle) {
	return names_form_matching(text, needle, false);
}

static bool names_form_at_top(PyObject * const text, PyObject * const needle) {
	return names_form_matching(text, needle, true);
}

PyObject * module_attribute(char const * const module_name, char const * const attribute) {
	PY_OWNED(name, PyUnicode_FromString(module_name));

	if (name == NULL) {
		return NULL;
	}

	PY_OWNED(module, PyImport_GetModule(name));

	if (module == NULL) {
		return NULL;
	}

	return optional_attribute(module, attribute);
}

#ifdef TESTING

#	include "../testing.h"
#	include <stdlib.h>

static PyObject * form_probes_for(void) {
	struct form_probes * const probes = calloc(1, sizeof(struct form_probes));
	PyObject * const typing = PyImport_ImportModule("typing");
	PyObject * const dataclasses = PyImport_ImportModule("dataclasses");

	TEST_ASSERT_NOT_NULL(probes);
	TEST_ASSERT_NOT_NULL(typing);
	TEST_ASSERT_NOT_NULL(dataclasses);
	probes->class_var = module_attribute("typing", "ClassVar");
	probes->init_var = module_attribute("dataclasses", "InitVar");
	probes->class_var_name = PyUnicode_FromString("ClassVar");
	probes->init_var_name = PyUnicode_FromString("InitVar");
	TEST_ASSERT_NOT_NULL(probes->class_var);
	TEST_ASSERT_NOT_NULL(probes->init_var);
	TEST_ASSERT_NOT_NULL(probes->class_var_name);
	TEST_ASSERT_NOT_NULL(probes->init_var_name);

	Py_DECREF(dataclasses);
	Py_DECREF(typing);

	return (PyObject *) probes;
}

static void form_probes_free(PyObject * const boxed) {
	struct form_probes * const probes = (struct form_probes *) boxed;

	Py_DECREF(probes->class_var);
	Py_DECREF(probes->init_var);
	Py_DECREF(probes->class_var_name);
	Py_DECREF(probes->init_var_name);
	free(probes);
}

static void test_special_form_of_names_each_shape(void) {
	struct form_probes * const probes = (struct form_probes *) form_probes_for();

	PyObject * const class_var_text = PyUnicode_FromString("ClassVar[int]");
	PyObject * const init_var_text = PyUnicode_FromString("InitVar[int]");
	PyObject * const plain_text = PyUnicode_FromString("int");

	TEST_ASSERT_NOT_NULL(class_var_text);
	TEST_ASSERT_NOT_NULL(init_var_text);
	TEST_ASSERT_NOT_NULL(plain_text);
	TEST_ASSERT_EQUAL_INT(SPECIAL_FORM_CLASS_VAR, special_form_of(class_var_text, probes).kind);
	TEST_ASSERT_EQUAL_INT(SPECIAL_FORM_INIT_VAR, special_form_of(init_var_text, probes).kind);
	TEST_ASSERT_EQUAL_INT(SPECIAL_FORM_NONE, special_form_of(plain_text, probes).kind);

	Py_DECREF(plain_text);
	Py_DECREF(init_var_text);
	Py_DECREF(class_var_text);
	form_probes_free((PyObject *) probes);
}

static void test_the_top_level_walk_names_only_depth_zero_shapes(void) {
	struct form_probes * const probes = (struct form_probes *) form_probes_for();

	PyObject * const top_text = PyUnicode_FromString("ClassVar[int]");
	PyObject * const nested_text = PyUnicode_FromString("list[ClassVar[int]]");

	TEST_ASSERT_NOT_NULL(top_text);
	TEST_ASSERT_NOT_NULL(nested_text);
	TEST_ASSERT_TRUE(class_var_top_level(top_text, probes));
	TEST_ASSERT_FALSE(class_var_top_level(nested_text, probes));

	Py_DECREF(nested_text);
	Py_DECREF(top_text);
	form_probes_free((PyObject *) probes);
}

static void test_an_identifier_continues_only_on_identifier_bytes(void) {
	TEST_ASSERT_TRUE(continues_identifier('x'));
	TEST_ASSERT_TRUE(continues_identifier('_'));
	TEST_ASSERT_TRUE(continues_identifier('9'));
	TEST_ASSERT_FALSE(continues_identifier('['));
	TEST_ASSERT_FALSE(continues_identifier(' '));
}

void forms_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_special_form_of_names_each_shape);
	RUN_TEST(test_the_top_level_walk_names_only_depth_zero_shapes);
	RUN_TEST(test_an_identifier_continues_only_on_identifier_bytes);
}

#endif
