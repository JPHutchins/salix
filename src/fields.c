#include <Python.h>
#include <stdbool.h>

#include "annotations.h"
#include "construct.h"
#include "fields.h"
#include "owned.h"
#include "result.h"
#include "types.h"

/* An annotation naming something that is not a field, and what to do instead. */
struct special_form {
	char const * name;
	char const * instead;
};

/* What the two paths match an annotation against. Built once per class, because
 * the text path's needles are the same two words for every field in it. Owning
 * what it holds, so that a needle is written in one place rather than into a
 * local and then into here. */
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

/* Both paths answer the same question and owe the user the same sentence, so
 * the answers live here rather than once per path -- and the text path's
 * needles are built from these names, so each form is spelled once. */
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

/* The plan only takes references once every step has succeeded; the working
 * collections belong to this scope either way. */
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

/* Inherited fields keep their position and defaults. */
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

/* New fields come from this class's annotations, in declaration order. */
static enum result append_declared(
	StructType const * const base,
	PyObject * const annotations,
	PyObject * const namespace,
	PyObject * const all_names,
	PyObject * const new_names,
	PyObject * const default_by_name
) {
	/* A class with no annotations of its own declares no fields and so can name
	 * no forms; the four probes below are the whole cost of asking. */
	if (PyDict_GET_SIZE(annotations) == 0) {
		return RESULT_OK;
	}

	/* Once per class, not once per field. Absent means the module was never
	 * imported, so nothing in this body can be naming what it holds -- but
	 * absent and failed both come back NULL, and only the exception tells them
	 * apart. Failing here has to fail the class rather than quietly leave it
	 * unguarded. */
	__attribute__((cleanup(form_probes_clear))) struct form_probes probes = {0};

	probes.class_var = module_attribute("typing", "ClassVar");

	if (probes.class_var == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	probes.init_var = module_attribute("dataclasses", "InitVar");

	if (probes.init_var == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	/* The names are taken first, because the object path asks the annotation
	 * for `__origin__` and `__args__` and that runs the class author's code --
	 * which can add to or delete from this very dict. PyDict_Next's cursor and
	 * the pointers it hands back are only defined while the dict is unmodified,
	 * and one list per class is a cheap way not to depend on which mutations
	 * CPython happens to survive. */
	PY_OWNED(declared, PyDict_Keys(annotations));

	if (declared == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t at = 0; at < PyList_GET_SIZE(declared); ++at) {
		PyObject * const field_name = PyList_GET_ITEM(declared, at);

		/* Held, because a later annotation's `__getattr__` may have deleted this
		 * one between the snapshot and here -- and gone is not a field. */
		PY_OWNED(annotation, dict_value_ref(annotations, field_name));

		if (annotation == NULL) {
			if (PyErr_Occurred()) {
				return RESULT_ERROR;
			}

			continue;
		}

		/* From the same two strings the refusal quotes, so there is one spelling
		 * of each form in the file. Built at the first text annotation rather
		 * than in the matcher, which runs per field and per form, and not up
		 * front, which taxes every class whose annotations are all objects.
		 *
		 * At the first one and not from a question asked of the dict beforehand:
		 * the walk runs the class author's `__getattr__`, which can put a str
		 * into this dict after such a question has answered "no text here". That
		 * is exactly what happened -- the answer was cached, the needles stayed
		 * NULL, and the injected str reached PyUnicode_GET_LENGTH(NULL).
		 * Deciding where the value is used cannot go stale between the decision
		 * and the use. */
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

		/* A default is the class-body value bound to the field name. */
		PyObject * const declared_default = PyDict_GetItem(namespace, field_name);

		if (
			declared_default != NULL &&
			PyDict_SetItem(default_by_name, field_name, declared_default) < 0
		) {
			return RESULT_ERROR;
		}

		/* Skip if this name was already inherited (override of annotation, not
		 * a new slot). */
		switch (inherits_field(base, field_name)) {
			case INHERITANCE_ERROR:
				return RESULT_ERROR;
			case INHERITANCE_INHERITED:
				continue;
			case INHERITANCE_NEW:
				break;
		}

		/* After the inheritance check, so re-annotating an inherited field is
		 * the no-op it has always been rather than a new refusal. */
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

/* PyObject_RichCompareBool answers -1 for an error and nothing else, so the
 * three cases are the return value itself -- no PyErr_Occurred() to tell a
 * failure apart from a "less than", and so no invariant about the exception
 * state on entry. The same tri-state compare.c reads into `enum comparison`. */
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

/*
 * ClassVar and InitVar name something that is not a field, and a checker
 * reading the stub already knows it -- dataclass_transform excludes both. Left
 * alone they become fields, so `registry: ClassVar[int] = 0` swallows the first
 * positional argument and checked code and running code disagree about what it
 * means. Refused instead, until there is a way to ask for them.
 *
 * The annotation is walked rather than probed once, because
 * Annotated[ClassVar[int], ...] reaches ClassVar two hops down and is otherwise
 * the same bug wearing a wrapper. A tree and not a chain: __origin__, every
 * element of __args__, and a PEP 695 alias's __value__ are all edges.
 */
enum : int {
	/* A budget on work rather than on depth, because bounding the shape stopped
	 * bounding the effort when the walk became a tree. Four hops was enough for
	 * a chain -- Optional[Annotated[ClassVar[int], 'm']] is four -- and this is
	 * enough for the arguments beside them. It is also what stops a cycle:
	 * whichever edge points back at an ancestor, the frontier runs out. */
	SPECIAL_FORM_NODES = 32,
};

/*
 * Owned, every one of them. __origin__ hands back a new reference, and an
 * argument outlives the tuple it was read from only because this holds it.
 *
 * A fixed array rather than a list, because the frontier is bounded anyway and
 * an allocation here would be one per subscripted annotation per class -- the
 * cost the needles were just moved out of the matcher to avoid.
 */
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

	/* Neither module is loaded, so nothing reachable from here can be either
	 * form and the walk below can only ask attributes of things for nothing. */
	if (probes->class_var == NULL && probes->init_var == NULL) {
		return (struct special_form){0};
	}

	return form_within(annotation, probes);
}

/*
 * The form can be anywhere in a subscripted annotation, not only at the end of
 * the __origin__ chain. `Optional[Annotated[ClassVar[int], 'm']]` keeps it in
 * the arguments, where a chain walk never looks, so it became a field while
 * the text path refused the same source, on the path almost every class takes.
 * Both are walked now.
 *
 * It does not reach `Annotated[int, ClassVar]`, where the form is metadata
 * rather than the type, because Annotated keeps metadata in __metadata__ and
 * not in __args__ -- so that shape is still a field here while the text path
 * refuses it. The disagreement is left open here; when this comment claimed the
 * walk had closed it, the test one file over already said otherwise.
 *
 * A str reached by the walk is walked and not matched, which costs nothing
 * today: the only strings in an __args__ are the ones a caller put there, and
 * the text matcher is for an annotation that arrived as source rather than for
 * anything that resembles one.
 *
 * A queue rather than a recursion, because clang-tidy's misc-no-recursion is an
 * error here and the shape does not need one: the frontier is what bounds the
 * walk, so neither a self-referential __origin__ nor an __args__ holding its
 * own owner can spin.
 */
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

		/* A plain class is the common annotation and has neither attribute;
		 * asking anyway costs two AttributeErrors raised and cleared. */
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

		/* A PEP 695 alias is the thing it aliases: `type CV = ClassVar[int]`
		 * used as an annotation is a ClassVar, and walking to its __value__ is
		 * what says so. Asked only where the other two were absent, because a
		 * TypeAliasType has neither -- so every ordinary subscripted annotation
		 * answers before this lookup happens. */
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

/*
 * Under `from __future__ import annotations` the annotation is its own source
 * text, so the only thing left to match is the spelling. A module alias is
 * covered, since `t.ClassVar[int]` still ends in the form after a dot; a
 * *renamed* import is not -- `from typing import ClassVar as CV` gives
 * `CV[int]`, which resolves to nothing here and becomes a field, as it did
 * before any of this. dataclasses guesses at those against sys.modules; this
 * does not.
 */
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

/*
 * The form standing on its own somewhere in the text -- at the start, after a
 * dot for a module alias, inside a subscript for `Annotated[ClassVar[int], ...]`
 * -- rather than as part of a longer name. Not MyClassVar, and not ClassVarish.
 *
 * The boundary is "no identifier character adjacent" rather than a list of the
 * punctuation seen so far. Enumerating openers and closers separately is how
 * `ClassVar ` and `ClassVar [int]`, both legal and both stored verbatim under
 * future annotations, walked past an earlier version of this.
 *
 * Characters rather than UTF-8 bytes, and Python's own identifier rule rather
 * than a hand-written one. PEP 3131 lets a name hold any Unicode letter, so an
 * ASCII-only test read `théClassVar` as the form standing alone; calling every
 * byte at or above 0x80 an identifier character fixed that and broke the other
 * direction, since `€` is not one. Working on the str also means a lone
 * surrogate or an embedded NUL is nothing special -- neither has to survive an
 * encode that the guard would otherwise fail open on.
 *
 * Nothing here clears an error it did not expect. A failed allocation would
 * otherwise answer "not a form", which reads as "this is a field" -- the same
 * fail-open module_attribute had, in the path that decides the same question.
 *
 * A heuristic in both directions, and the only thing available once the
 * annotation is source text. It misses a renamed import -- `ClassVar as CV`
 * gives `CV[int]` -- and it refuses a user's own type that happens to be called
 * ClassVar, and `Annotated[int, ClassVar]` where the form is metadata rather
 * than the type -- including when it is quoted, since a quote is a boundary.
 * That last one is deliberate: `x: 'ClassVar[int]'` is a nested forward
 * reference and has to be refused, and nothing short of parsing tells the two
 * apart. The object path is exact, and it is what runs unless the module asked
 * for `from __future__ import annotations`.
 */
static bool continues_identifier(Py_UCS4 const character) {
	/* Python owns the answer and spells it one way in the C API: a name is an
	 * identifier when its first character starts one and the rest continue one,
	 * so "a" followed by this character asks about exactly this character. */
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

		/* -1 is "not in the text" and -2 is "could not look", and this answers
		 * both with false: the caller tells them apart by the exception, which
		 * it reads before it reads the verdict. */
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
 * and turns the guard off for this class because there is nothing it could be
 * naming. NULL *with* an exception set is a failure, and the caller has to tell
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

	/* A module that exists without the attribute is a stdlib salix does not
	 * recognise, not a failure -- which is what optional_attribute means, so it
	 * is what answers here rather than a second copy of it. */
	return optional_attribute(module, attribute);
}

/*
 * An empty mutable container is copied per instance, so it means what it looks
 * like it means. A non-empty one cannot be: copying it is necessarily shallow,
 * and `xs: list = [[1]]` would hand every instance its own outer list around
 * the *same* inner one -- an aliasing bug one level down from the one being
 * fixed. Refused instead, which is what msgspec does and for the same reason.
 *
 * Only the exact builtins, because the copy has to preserve the type and
 * PyDict_Copy of a defaultdict is a dict. A subclass is shared, as it is
 * there.
 *
 * It reaches a class whose body writes its own __init__ on the same terms as
 * every other: Struct_new writes that class's declared defaults before the
 * __init__ runs, so a non-empty one would be copied per instance and is
 * shallow there for the same reason. It used to over-fire on a declaration
 * nothing would ever hand out; the message states the rule rather than a
 * consequence because that reads the same either way, and names the remedy
 * that still works there: __post_init__ runs from the constructor a body
 * __init__ displaces, so only set_field from that __init__ is left.
 */
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

/* Build the defaults tuple as the trailing run of defaulted fields, and
 * enforce that no required field follows a defaulted one (same rule as
 * Python function signatures). */
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

/* build_defaults takes plain lists and dicts, so it is testable without a class
 * -- and the ordering rule is easier to state here than through a class body. */
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
