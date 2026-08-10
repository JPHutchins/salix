#include <Python.h>
#include <stdbool.h>

#include "annotations.h"
#include "construct.h"
#include "fields.h"
#include "owned.h"
#include "result.h"
#include "types.h"

struct special_form {
	char const * name;
	char const * instead;
};

struct form_probes {
	PyObject * class_var;
	PyObject * init_var;
	PyObject * class_var_name;
	PyObject * init_var_name;
};

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
	PyObject * default_by_name
);
static enum result append_declared(
	StructType const * base,
	PyObject * annotations,
	PyObject * namespace,
	PyObject * all_names,
	PyObject * new_names,
	PyObject * default_by_name
);

static struct special_form const CLASS_VAR_FORM = {
	.name = "ClassVar",
	.instead = "write it below the fields, without an annotation",
};
static struct special_form const INIT_VAR_FORM = {
	.name = "InitVar",
	.instead = "take the value in a custom __init__ and write the fields with set_field",
};
static PyObject * build_defaults(PyObject * all_names, PyObject * default_by_name);
static enum result reject_unsafe_default(PyObject * field_name, PyObject * value);
static PyObject * checked_annotations(PyObject * namespace);
static enum inheritance inherits_field(StructType const * base, PyObject * field_name);
static struct special_form special_form_of(
	PyObject * annotation,
	struct form_probes const * probes
);
static struct special_form form_within(PyObject * annotation, struct form_probes const * probes);
static struct special_form named_special_form(PyObject * text, struct form_probes const * probes);
static bool names_form(PyObject * text, PyObject * needle);
static bool continues_identifier(Py_UCS4 character);
static PyObject * module_attribute(char const * module_name, char const * attribute);
static enum result refuse_shared_mutable_contents(PyObject * field_name, PyObject * value);

struct field_plan field_plan_build(StructType const * const base, PyObject * const namespace) {
	struct field_plan plan = {0};

	PY_OWNED(annotations, checked_annotations(namespace));

	if (annotations == NULL) {
		return plan;
	}

	PY_MOVABLE(all_names, PyList_New(0));
	PY_MOVABLE(new_names, PyList_New(0));
	PY_OWNED(default_by_name, PyDict_New());

	if (
		all_names != NULL &&
		new_names != NULL &&
		default_by_name != NULL &&
		append_inherited(base, all_names, default_by_name) == RESULT_OK &&
		append_declared(base, annotations, namespace, all_names, new_names, default_by_name) ==
			RESULT_OK
	) {
		plan.defaults = build_defaults(all_names, default_by_name);

		if (plan.defaults != NULL) {
			plan.all_names = py_move(&all_names);
			plan.new_names = py_move(&new_names);
		}
	}

	return plan;
}

void field_plan_clear(struct field_plan * const plan) {
	Py_CLEAR(plan->all_names);
	Py_CLEAR(plan->new_names);
	Py_CLEAR(plan->defaults);
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
	PyObject * const default_by_name
) {
	if (base == NULL) {
		return RESULT_OK;
	}

	Py_ssize_t const required_count = struct_required_count(base);

	for (Py_ssize_t i = 0; i < base->struct_field_count; ++i) {
		PyObject * const field_name = PyTuple_GET_ITEM(base->struct_field_names, i);

		if (PyList_Append(all_names, field_name) < 0) {
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

static enum result append_declared(
	StructType const * const base,
	PyObject * const annotations,
	PyObject * const namespace,
	PyObject * const all_names,
	PyObject * const new_names,
	PyObject * const default_by_name
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

		PyObject * const declared_default = PyDict_GetItem(namespace, field_name);

		if (
			declared_default != NULL &&
			(
				PyDict_SetItem(default_by_name, field_name, declared_default) < 0 ||
				refuse_shared_mutable_contents(field_name, declared_default) != RESULT_OK
			)
		) {
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

		if (PyList_Append(all_names, field_name) < 0 || PyList_Append(new_names, field_name) < 0) {
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

enum : int {
	/* A budget on work rather than on depth, because bounding the shape stopped
	 * bounding the effort when the walk became a tree. Four hops was enough for
	 * a chain -- Optional[Annotated[ClassVar[int], 'm']] is four -- and this is
	 * enough for the arguments beside them. It is also what stops a cycle:
	 * whichever edge points back at an ancestor, the frontier runs out. */
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

/* Full is not a failure: the budget is the point, and a walk that runs out
 * answers "no form", which is what an unrecognised annotation answers. */
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

static struct special_form special_form_of(
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

	/* A scan can fail rather than answer -- PyUnicode_Find's own -2, or an
	 * allocation in the boundary check -- and both spell that as false. The
	 * second scan must not run on top of the first one's exception, and the
	 * caller reads the exception before it reads the verdict, so leaving it set
	 * is what reports the failure. */
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

static bool names_form(PyObject * const text, PyObject * const needle) {
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

		if (opens && closes) {
			return true;
		}

		at = found;
	}

	return false;
}

/*
 * The attribute if its module is already loaded, and NULL if it is not --
 * without importing, which is the whole point. A new reference, so the caller
 * owns it: returning a borrowed one out of a PY_OWNED scope is the shape
 * owned.h warns about, even where the module would have kept it alive.
 *
 * NULL with no exception set is the absent module, which is the ordinary answer
 * and turns the guard off for this class. NULL *with* an exception set is a
 * failure, and the caller has to tell
 * them apart: swallowing the second one turns a MemoryError into a silently
 * unguarded class, where a ClassVar becomes a field again with no way to
 * notice.
 */
static PyObject * module_attribute(char const * const module_name, char const * const attribute) {
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

static enum result reject_unsafe_default(PyObject * const field_name, PyObject * const value) {
	PyTypeObject * const kind = Py_TYPE(value);
	Py_ssize_t const filled = struct_copies_default(kind) ? PyObject_Size(value) : 0;

	if (filled <= 0) {
		return filled < 0 ? RESULT_ERROR : RESULT_OK;
	}

	PyErr_Format(
		PyExc_TypeError,
		"field '%U' defaults to a non-empty %.100s, whose copy could only be "
		"shallow and would leave the contents shared; default it to an empty "
		"one and fill it with set_field -- from __post_init__, or from your own "
		"__init__ if the body writes one, which displaces the constructor "
		"__post_init__ runs from",
		field_name,
		kind->tp_name
	);

	return RESULT_ERROR;
}

static enum result refuse_shared_mutable_contents(
	PyObject * const field_name,
	PyObject * const value
) {
	PyTypeObject * const kind = Py_TYPE(value);

	if (kind->tp_hash == NULL || kind->tp_hash == PyObject_HashNotImplemented) {
		return RESULT_OK;
	}

	if (PyObject_Hash(value) != -1 || !PyErr_Occurred()) {
		return RESULT_OK;
	}

	if (PyErr_ExceptionMatches(PyExc_RecursionError)) {
		PyErr_Clear();

		return RESULT_OK;
	}

	if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_ValueError)) {
		return RESULT_ERROR;
	}

	PyErr_Clear();

	PyErr_Format(
		PyExc_TypeError,
		"field '%U' defaults to a %.100s whose type hashes and whose value will "
		"not, which is how a container of something mutable answers; salix "
		"shares such a default across every instance, so give the field one "
		"that hashes and build the rest with set_field -- from __post_init__, "
		"or from your own __init__ if the body writes one",
		field_name,
		kind->tp_name
	);

	return RESULT_ERROR;
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
		PyObject * const value = PyDict_GetItem(default_by_name, field_name);

		/* Twice, on purpose. The first read is what raises in the ordinary case,
		 * but its verdict is not final: it reads an object the module still
		 * holds and can still write to between the two checks. It earns its
		 * place by being O(1) and keeping a default that is going to be refused
		 * from being built into a copy that is then thrown away -- that copy is
		 * the whole of what it saves, since copying a set hashes nothing
		 * (PySet_New copies the table).
		 *
		 * The second read is the one that counts, because it reads the copy --
		 * what the class keeps, and what no module-level alias still points at.
		 * A racing write is captured by the copy (the copy is made from the
		 * declaration, so of course it is) and refused there, so the race costs
		 * the work the first check exists to skip and not the invariant.
		 *
		 * `_struct_defaults_` still hands the stored object out, so filling it
		 * through there defeats this. That route is out of contract. */
		if (reject_unsafe_default(field_name, value) != RESULT_OK) {
			Py_DECREF(defaults);

			return NULL;
		}

		PyObject * const stored = struct_default_copy(value);

		if (stored == NULL || reject_unsafe_default(field_name, stored) != RESULT_OK) {
			Py_XDECREF(stored);
			Py_DECREF(defaults);

			return NULL;
		}

		PyTuple_SET_ITEM(defaults, i - first_default, stored);
	}

	return defaults;
}

#ifdef TESTING

#	include "testing.h"

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
