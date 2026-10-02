#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

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

struct field_lookup find_field(StructType const * const type, PyObject * const name) {
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

enum result run_post_init(StructType const * const type, PyObject * const self) {
	if (type->struct_post_init == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(returned, PyObject_CallOneArg(type->struct_post_init, self));

	return returned != NULL ? RESULT_OK : RESULT_ERROR;
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
		/* The family's accepted shape is (msg, excs), so the positionals
		 * bind by the resolved member indexes whatever the declaration
		 * order; a one-item payload -- the graded pack's single-member
		 * shape a pickle reconstructs through the fallback -- binds its
		 * member field the same way. Rejected shapes with more positionals
		 * are field values in declaration order, and a slot already
		 * written keeps its value. */
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
				/* The family accepted the shape, so positional 1 IS the
				 * body: it wins the exceptions slot over a positional 0
				 * that fell through to declaration order at the same
				 * index. */
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

	/* The slot itself answers whether a positional bound the field -- a
	 * member-index mapping can leave leading slots untouched, so a count
	 * cannot. */
	PyObject * * const slot = struct_slot(type, self, found.index);

	if (*slot != NULL) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() got multiple values for argument '%U'",
			struct_type_name(type),
			name
		);

		return RESULT_ERROR;
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
	/* Slot-state driven: whatever bound each slot -- a positional, a
	 * keyword, a member-index mapping -- a NULL slot is what needs a
	 * default or the missing-required error. The wrapper passes false: on
	 * the own-init path the author's init owns the required fields and an
	 * unbound one is its state, not a call error. */
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

/* The identity scan is the fast path; the equality scan exists only for a name
 * that was not interned, which Python-level tests reach only by accident. */
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

void construct_tests(void) {
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_interned_name_resolves_by_identity);
	RUN_TEST(test_a_name_assembled_at_runtime_resolves_by_comparison);
	RUN_TEST(test_a_name_that_is_not_a_field_is_missing);
}

#endif
