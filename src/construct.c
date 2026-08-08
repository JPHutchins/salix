#include <Python.h>

#include "construct.h"
#include "owned.h"
#include "result.h"
#include "types.h"

/* Which field a keyword argument names, if any. */
struct field_lookup {
	enum { FIELD_LOOKUP_FOUND, FIELD_LOOKUP_MISSING, FIELD_LOOKUP_ERROR } tag;
	Py_ssize_t index;
};

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
static struct field_lookup find_field(StructType const * type, PyObject * name);
static enum result write_slot(
	StructType const * type,
	PyObject * self,
	Py_ssize_t index,
	PyObject * value
);
static enum result run_post_init(StructType const * type, PyObject * self);

/*
 * Instances are built straight into slot memory: allocate, then let each of
 * the three argument sources write the slots it owns. A half-written struct is
 * a valid object with NULL slots, so unwinding is just Py_DECREF.
 */
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

/*
 * What a class whose body writes __init__ allocates with. That class declined
 * the generated constructor, and fill_defaults went with it: a declared default
 * was never written, for the class and for every subclass, so
 * _struct_defaults_ advertised a value no instance would ever carry.
 * Writing them here means the __init__ runs over a struct that already holds
 * them and overwrites whatever it means to -- which is where a dataclass leaves
 * them too, on the class, readable.
 *
 * Fields with no default are left NULL. Supplying those is what the body's
 * __init__ is for, and reading one it did not write raises AttributeError as
 * it did before. __post_init__ is not run here for the same reason: it is the
 * generated constructor's last step, and this class does not have one.
 *
 * The arguments go unread, exactly as PyType_GenericNew left them: they are the
 * __init__'s to interpret. Which is also what this costs an __init__ that
 * overwrites every default from its own arguments: the copy is written and
 * thrown away, because nothing here can know that. fill_defaults can, since the
 * vectorcall sees which slots the caller filled. Measured on 3.14, a two-field
 * class whose __init__ assigns both: 115.1ns against 99.9 for
 * PyType_GenericNew, and a class declaring no defaults pays nothing.
 */
PyObject * Struct_new(
	PyTypeObject * const struct_class,
	PyObject * const arguments,
	PyObject * const keywords
) {
	PY_MOVABLE(self, struct_class->tp_alloc(struct_class, 0));

	if (self == NULL) {
		return NULL;
	}

	StructType const * const type = (StructType *) struct_class;

	/* fill_defaults with nothing supplied, which is what this is: every slot of
	 * a fresh instance is NULL so its skip never fires, and starting at
	 * required_count makes its missing-argument branch unreachable. Calling it
	 * rather than repeating it keeps one copy of what a default costs. */
	return (
		fill_defaults(type, self, struct_required_count(type)) == RESULT_OK ? py_move(&self) :
		NULL
	);
}

/*
 * The last thing the constructor does, so what it validates is a struct with
 * every field already written. Frozen means it cannot assign one back --
 * set_field below is the deliberate way through, and the only one, since a
 * frozen class's fields are read-only to every other path.
 */
static enum result run_post_init(StructType const * const type, PyObject * const self) {
	if (type->struct_post_init == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(returned, PyObject_CallOneArg(type->struct_post_init, self));

	return returned != NULL ? RESULT_OK : RESULT_ERROR;
}

/* Positional arguments are in field order by definition, so this is a copy. */
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
		struct field_lookup const found = find_field(type, keyword);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return RESULT_ERROR;
			case FIELD_LOOKUP_MISSING:
				PyErr_Format(
					PyExc_TypeError,
					"%.200s() got an unexpected keyword argument '%U'",
					struct_type_name(type),
					keyword
				);

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

/* Whatever position and keyword left unwritten: a default if the field has
 * one, otherwise the call is short an argument. */
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

/*
 * A mutable default belongs to the instance, not to the class: `xs: list = []`
 * reads as an empty list per struct, and handing every instance the same one is
 * a bug people write by accident. The four builtins that spell "container I
 * will mutate" are copied, and only when empty -- a non-empty one is refused at
 * class creation, since copying it could only be shallow.
 *
 * Class creation takes a copy too, so what the class stores is not the object
 * the body named. `shared = []` kept at module level and appended to afterwards
 * would otherwise make the stored default non-empty behind the refusal's back,
 * and every instance would get a shallow copy of it. msgspec severs the same
 * alias by turning the default into a Factory.
 *
 * Everything else is shared, and "everything else" is wider than it sounds.
 * Sharing is right for an int, a string or a tuple of them, and for a frozen
 * struct of them: none can be rebound or mutated. It is wrong for two kinds of
 * default this does not reach -- a shallowly-immutable container of something
 * mutable (`([],)`, a frozen struct holding a list), and a mutable container
 * that simply is not one of the four (`array.array`, `deque`, `defaultdict`, a
 * writable `memoryview`, or a subclass of any of the four). Those are shared
 * outright, and a non-empty one is not refused either, because the refusal
 * whitelists the same four types.
 *
 * Every one of them is unhashable, which is the single test that would replace
 * this list. msgspec shares them as well.
 *
 * dataclasses refuses the shape outright and needs default_factory to express
 * it at all. This copies, so the common spelling means what it looks like it
 * means, and pays for it only on the fields that have one.
 *
 * The type and the constructor that copies it are named together, because no
 * predicate can supply the second. One list, so the refusal and the copy cannot
 * come to different answers about which types those are.
 *
 * A list of statements rather than a static array of {type, constructor}: on
 * Windows a `PyTypeObject` is imported from python3.dll, and the address of a
 * dllimport symbol is not a compile-time constant, so the array version
 * compiles everywhere except the platform half the wheels are cross-built for.
 */
typedef PyObject * (*default_copier)(PyObject * declared);

/* PyList_GetSlice takes bounds; the other three constructors take the object. */
static PyObject * copy_list(PyObject * const declared) {
	return PyList_GetSlice(declared, 0, PyList_GET_SIZE(declared));
}

static default_copier copies_default(PyTypeObject const * const kind) {
	if (kind == &PyList_Type) {
		return copy_list;
	}

	if (kind == &PyDict_Type) {
		return PyDict_Copy;
	}

	if (kind == &PySet_Type) {
		return PySet_New;
	}

	if (kind == &PyByteArray_Type) {
		return PyByteArray_FromObject;
	}

	return NULL;
}

bool struct_copies_default(PyTypeObject const * const kind) {
	return copies_default(kind) != NULL;
}

PyObject * struct_default_copy(PyObject * const declared) {
	default_copier const copy = copies_default(Py_TYPE(declared));

	/* Everything else is the object itself, a subclass of one of the four
	 * included: copying with a constructor for the wrong type would change the
	 * value. */
	return copy != NULL ? copy(declared) : Py_NewRef(declared);
}

/*
 * The one way to write a struct's field after it is built, and the reason a
 * frozen one can still be computed rather than only passed in.
 *
 * It resolves the name against the field table and writes that slot, which is
 * the path the constructor takes, so it cannot add an attribute -- a name the
 * class did not declare has no slot to write and is an error. That is the whole
 * safety argument: the escape hatch reaches exactly what the class already
 * spells out, and nothing else.
 *
 * Intended for __post_init__, before the instance has escaped. Which value a
 * racing write leaves behind is the caller's problem, as it is for any object
 * whose invariants outlive its constructor -- but only which value: the write
 * itself is as safe as `self.x = v`, and for the same reason.
 */
PyObject * Struct_set_field(PyObject * const module, PyObject * const arguments) {
	PyObject * self = NULL;
	PyObject * name = NULL;
	PyObject * value = NULL;

	if (!PyArg_UnpackTuple(arguments, "set_field", 3, 3, &self, &name, &value)) {
		return NULL;
	}

	if (!is_struct(self)) {
		PyErr_Format(
			PyExc_TypeError,
			"set_field() expects a struct, not %.200s",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	if (!PyUnicode_Check(name)) {
		PyErr_Format(
			PyExc_TypeError,
			"set_field() field name must be str, not %.200s",
			Py_TYPE(name)->tp_name
		);

		return NULL;
	}

	StructType * const type = struct_type_of(self);
	struct field_lookup const found = find_field(type, name);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
			return NULL;
		case FIELD_LOOKUP_MISSING:
			PyErr_Format(
				PyExc_AttributeError,
				"%.200s has no field '%U'",
				struct_type_name(type),
				name
			);

			return NULL;
		case FIELD_LOOKUP_FOUND:
			if (write_slot(type, self, found.index, value) != RESULT_OK) {
				return NULL;
			}
	}

	Py_RETURN_NONE;
}

/*
 * Through CPython's own member setter rather than a store of our own, so the
 * free-threading guarantee is inherited here exactly as it is for `self.x = v`.
 * A plain Py_XSETREF is a load, a store and a decref: two threads read the same
 * previous value, both store, and both release it -- one reference, two
 * releases, and 3.14t dies on it. PyMember_SetOne takes a critical section on
 * the instance and defers the release past the end of it.
 *
 * The offset is the one type.__new__ gave this field, so the descriptor here
 * describes a slot that already exists rather than looking one up.
 */
static enum result write_slot(
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

/*
 * Keyword names arrive interned in the overwhelmingly common case, so the
 * identity scan resolves them without touching PyUnicode_Compare; the equality
 * scan is the fallback for names assembled at runtime.
 */
static struct field_lookup find_field(StructType const * const type, PyObject * const name) {
	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		if (name == PyTuple_GET_ITEM(type->struct_field_names, i)) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_FOUND, .index = i};
		}
	}

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		int const compared = PyUnicode_Compare(name, PyTuple_GET_ITEM(type->struct_field_names, i));

		if (compared == 0) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_FOUND, .index = i};
		}

		if (compared == -1 && PyErr_Occurred()) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_ERROR};
		}
	}

	return (struct field_lookup){.tag = FIELD_LOOKUP_MISSING};
}

#ifdef TESTING

#	include "testing.h"

static PyObject * two_field_instance(void) {
	return testing_evaluate("class P(Struct):\n    alpha: int\n    beta: int\nresult = P(1, 2)\n");
}

/* The identity scan is the fast path; the equality scan exists only for a name
 * that was not interned, which Python-level tests reach only by accident. */
static void test_an_interned_name_resolves_by_identity(void) {
	PyObject * const instance = two_field_instance();
	PyObject * const name = PyUnicode_InternFromString("beta");
	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(1, found.index);

	Py_DECREF(name);
	Py_DECREF(instance);
}

static void test_a_name_assembled_at_runtime_resolves_by_comparison(void) {
	PyObject * const instance = two_field_instance();
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
	PyObject * const instance = two_field_instance();
	PyObject * const name = PyUnicode_FromString("gamma");
	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_MISSING, found.tag);

	Py_DECREF(name);
	Py_DECREF(instance);
}

void construct_tests(void) {
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_interned_name_resolves_by_identity);
	RUN_TEST(test_a_name_assembled_at_runtime_resolves_by_comparison);
	RUN_TEST(test_a_name_that_is_not_a_field_is_missing);
}

#endif
