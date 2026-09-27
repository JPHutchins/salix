#include <Python.h>

#include "construct.h"
#include "../meta/meta.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

int change_names_touch(
	StructType * const type,
	PyObject * const keyword_names,
	Py_ssize_t const change_count,
	Py_ssize_t const field_index
) {
	if (field_index < 0 || keyword_names == NULL) {
		return 0;
	}

	PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, field_index);

	for (Py_ssize_t i = 0; i < change_count; ++i) {
		int const matches = PyObject_RichCompareBool(
			PyTuple_GET_ITEM(keyword_names, i),
			name,
			Py_EQ
		);

		if (matches < 0) {
			return -1;
		}

		if (matches == 1) {
			return 1;
		}
	}

	return 0;
}

#if PY_VERSION_HEX >= 0x030B0000
enum result group_members_from_fields(
	StructType * const type,
	PyObject * const self,
	PyObject * const msg_fallback
) {
	/* The members' one writer for field-built instances: the message and
	 * the body come from the bound fields, and every body item is an
	 * exception -- the invariant the family's own __new__ enforces on its
	 * shapes, which the allocation fallbacks never reached. The fallback
	 * answers for a struct without a message field; it is consumed. */
	PY_MOVABLE(msg, NULL);

	if (type->struct_message_index >= 0) {
		PyObject * const bound = *struct_slot(type, self, type->struct_message_index);

		msg = bound != NULL ? PyObject_Str(bound) : PyUnicode_FromString("");
	} else if (msg_fallback != NULL) {
		msg = msg_fallback;
	} else {
		msg = PyUnicode_FromString("");
	}

	if (msg == NULL) {
		return RESULT_ERROR;
	}

	PY_MOVABLE(excs, NULL);

	if (type->struct_exceptions_index >= 0) {
		PyObject * const bound = *struct_slot(type, self, type->struct_exceptions_index);

		if (bound != NULL) {
			excs = PyTuple_Check(bound) ? Py_NewRef(bound) : PySequence_Tuple(bound);

			if (excs == NULL) {
				return RESULT_ERROR;
			}

			for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(excs); ++i) {
				if (!PyExceptionInstance_Check(PyTuple_GET_ITEM(excs, i))) {
					PyErr_Format(
						PyExc_ValueError,
						"Item %zd of second argument (exceptions) is not an exception",
						i
					);

					return RESULT_ERROR;
				}
			}
		}
	}

	if (excs == NULL) {
		excs = PyTuple_New(0);

		if (excs == NULL) {
			return RESULT_ERROR;
		}
	}

	/* The carry borrows both members and takes its own references; the
	 * locals here own them until this return releases them. */
	return carry_group_members(type, self, msg, excs, NULL, NULL, NULL);
}
#endif

enum result carry_group_members(
	StructType * const type,
	PyObject * self,
	PyObject * const msg,
	PyObject * const excs,
	PyObject * const excs_str,
	PyObject * const deepcopier,
	PyObject * const memo
) {
#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		/* The family's __new__ is the group members' only writer; every
		 * tp_alloc arm carries them itself so str()/repr()/raise never read
		 * NULL. The deep copy detaches the body through the memo. */
		PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;

		/* The overwrites release what the family's own __new__ wrote on a
		 * rebuild; a field-built shell's members are NULL, where a set is
		 * the same store. */
		Py_XSETREF(group->msg, (
			msg != NULL ? (
				deepcopier != NULL ? PyObject_CallFunctionObjArgs(deepcopier, msg, memo, NULL) :
				Py_XNewRef(msg)
			) :
			PyUnicode_FromString("")
		));
		Py_XSETREF(group->excs, (
			excs != NULL ? (
				deepcopier != NULL ? PyObject_CallFunctionObjArgs(deepcopier, excs, memo, NULL) :
				Py_XNewRef(excs)
			) :
			PyTuple_New(0)
		));
		bool complete = group->msg != NULL && group->excs != NULL;

#	if PY_VERSION_HEX >= 0x030D0C00
		if (group_layout_has_excs_str()) {
			/* 3.13.12+ backported the cached excs string: its repr
			 * dereferences it on 3.14 and, without it, reads args[1] past a
			 * one-item payload on 3.13.12+. The layout probe keeps a wheel
			 * built on newer headers off an older patch's shorter object.
			 * The source's cache answers when one exists; the repr is the
			 * rebuild. */
			Py_XSETREF(
				group->excs_str,
				(
					excs_str != NULL ? Py_NewRef(excs_str) :
					group->excs != NULL ? PyObject_Repr(group->excs) :
					NULL
				)
			);
			complete = complete && group->excs_str != NULL;
		}
#	endif

		if (!complete) {
			/* The partially carried members belong to the instance; the
			 * caller's return frees them with it. Clearing here would hand
			 * the caller a freed pointer its cleanup attribute then
			 * touches again. */
			return RESULT_ERROR;
		}
	}
#endif

	return RESULT_OK;
}

static void store_exception_args(PyObject * const self, PyObject * const args) {
	PyObject * old;

	/* The store holds the same lock every reader takes, and the old
	 * reference's release is deferred past the end of the section -- the
	 * PyMember_SetOne pattern the struct slots rely on. */
	STRUCT_BEGIN_CRITICAL_SECTION(self);
	old = ((PyBaseExceptionObject *) self)->args;
	((PyBaseExceptionObject *) self)->args = args;
	STRUCT_END_CRITICAL_SECTION();

	Py_XDECREF(old);
}

#if PY_VERSION_HEX >= 0x030B0000
enum result store_group_args(
	StructType * const type,
	PyObject * const self,
	bool const from_fields
) {
	/* The group payload is the members' shape -- (msg, excs) when both
	 * member fields exist, the single member otherwise -- so pickle's
	 * positional reconstruction binds them by index back onto the same
	 * fields. Construction packs the bound field values, the family's own
	 * args shape; replace packs the carried members, whose state the
	 * fields may diverge from by contract. A struct without either member
	 * field keeps the field-value payload the explicit-prefix writer
	 * produced. */
	bool const has_message = type->struct_message_index >= 0;
	bool const has_exceptions = type->struct_exceptions_index >= 0;
	PY_MOVABLE(packed, NULL);

	if (from_fields) {
		PyObject * const message = (
			has_message ? *struct_slot(type, self, type->struct_message_index) :
			NULL
		);
		PyObject * const exceptions = (
			has_exceptions ? *struct_slot(type, self, type->struct_exceptions_index) :
			NULL
		);

		if (message != NULL && exceptions != NULL) {
			packed = PyTuple_Pack(2, message, exceptions);
		} else if (message != NULL) {
			packed = PyTuple_Pack(1, message);
		} else if (exceptions != NULL && type->struct_field_count >= 2) {
			/* No message field: the carried or family-written member is the
			 * message, and the two-item payload reconstructs through the
			 * family shape, which round-trips it -- where the struct's own
			 * arity accepts two positionals. */
			PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;
			packed = PyTuple_Pack(2, group->msg, exceptions);
		} else if (exceptions != NULL) {
			packed = PyTuple_Pack(1, exceptions);
		} else {
			return RESULT_OK;
		}
	} else {
		PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;

		if (has_exceptions && type->struct_field_count >= 2) {
			packed = PyTuple_Pack(2, group->msg, group->excs);
		} else if (has_exceptions) {
			packed = PyTuple_Pack(1, group->excs);
		} else if (has_message) {
			packed = PyTuple_Pack(1, group->msg);
		} else {
			return RESULT_OK;
		}
	}

	if (packed == NULL) {
		return RESULT_ERROR;
	}

	store_exception_args(self, py_move(&packed));

	return RESULT_OK;
}
#endif

enum result set_exception_args_from_fields(
	StructType * const type,
	PyObject * const self,
	Py_ssize_t const field_count
) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return RESULT_OK;
	}

	/* The exception's C-level args member sits at offset zero when the first
	 * base is the exception; str()/repr()/__reduce__ read it without a NULL
	 * check. The payload is the leading run of explicitly-supplied fields:
	 * __reduce__'s (cls, args) reconstructs them positionally, and the
	 * auto-filled defaults -- trailing or behind a gap -- stay out. Every
	 * arm computes the same explicit prefix and this is its one writer. */
	Py_ssize_t bound_count = 0;

	while (bound_count < field_count && *struct_slot(type, self, bound_count) != NULL) {
		bound_count += 1;
	}

	PY_MOVABLE(args, PyTuple_New(bound_count));

	if (args == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 0; i < bound_count; ++i) {
		PyTuple_SET_ITEM(args, i, Py_NewRef(*struct_slot(type, self, i)));
	}

	store_exception_args(self, py_move(&args));

	return RESULT_OK;
}

enum result set_exception_args_from_original(
	StructType * const type,
	PyObject * const copy,
	PyObject * const original,
	PyObject * const deepcopier,
	PyObject * const memo
) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return RESULT_OK;
	}

	/* The source's payload is the truth on every arm -- whatever produced
	 * it, the copy keeps it, so a copied exception formats like the
	 * original. The ref is taken under the source's own lock: args is a
	 * settable member whose store holds a critical section on the instance,
	 * and a load-then-incref outside it is the free-threaded use-after-free
	 * the struct slots fixed the same way. The deep copy detaches the items
	 * through the memo, so a deepcopy's payload is the deep field values,
	 * not the original's live objects. */
	PyObject * args;

	STRUCT_BEGIN_CRITICAL_SECTION(original);
	args = Py_XNewRef(((PyBaseExceptionObject *) original)->args);
	STRUCT_END_CRITICAL_SECTION();

	PY_MOVABLE(copied_args, NULL);

	if (args == NULL) {
		copied_args = PyTuple_New(0);
	} else if (deepcopier != NULL) {
		copied_args = PyObject_CallFunctionObjArgs(deepcopier, args, memo, NULL);
		Py_DECREF(args);
	} else {
		copied_args = args;
	}

	if (copied_args == NULL) {
		return RESULT_ERROR;
	}

	store_exception_args(copy, py_move(&copied_args));

	return RESULT_OK;
}

void set_exception_args_from_positionals(
	PyTypeObject * const cls,
	PyObject * const self,
	PyObject * const positionals
) {
	if (!is_exception_struct(cls)) {
		return;
	}

	/* The own-init path runs the author's __init__ after the allocation, so
	 * the fields are not bound here; BaseException_new's contract holds: the
	 * positional tuple is the payload. */
	store_exception_args(self, Py_XNewRef(positionals));
}

Py_ssize_t explicit_field_prefix(
	StructType const * const type,
	Py_ssize_t const positional_count,
	PyObject * const keyword_names,
	Py_ssize_t const carried_count
) {
	Py_ssize_t const keyword_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		bool explicit = prefix < positional_count || prefix < carried_count;

		for (Py_ssize_t i = 0; !explicit && i < keyword_count; ++i) {
			struct field_lookup const found = find_field(type, PyTuple_GET_ITEM(keyword_names, i));

			if (found.tag == FIELD_LOOKUP_ERROR) {
				return -1;
			}

			explicit = found.tag == FIELD_LOOKUP_FOUND && found.index == prefix;
		}

		if (!explicit) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

Py_ssize_t carried_payload_count(StructType * const type, PyObject * const self) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return 0;
	}

	PyObject * args;

	STRUCT_BEGIN_CRITICAL_SECTION(self);
	args = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
	STRUCT_END_CRITICAL_SECTION();

	Py_ssize_t const count = args != NULL ? PyTuple_GET_SIZE(args) : 0;
	Py_XDECREF(args);

	return count;
}

Py_ssize_t explicit_dict_prefix(StructType const * const type, PyObject * const values) {
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		int const present = PyDict_Contains(
			values,
			PyTuple_GET_ITEM(type->struct_field_names, prefix)
		);

		if (present < 0) {
			return -1;
		}

		if (present == 0) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

Py_ssize_t explicit_items_prefix(
	StructType const * const type,
	PyObject * const items,
	Py_ssize_t const entry_count
) {
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, prefix);
		bool explicit = false;

		for (Py_ssize_t i = 0; i < entry_count; ++i) {
			PyObject * const pair = PySequence_Fast_GET_ITEM(items, i);
			int const compared = PyObject_RichCompareBool(name, PyTuple_GET_ITEM(pair, 0), Py_EQ);

			if (compared < 0) {
				return -1;
			}

			if (compared == 1) {
				explicit = true;
				break;
			}
		}

		if (!explicit) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

#ifdef TESTING

#	include "../testing.h"

static PyObject * exception_instance(void) {
	return testing_evaluate(
		"class E(Exception, Struct, frozen=True):\n"
		"    message: str\n"
		"result = E(\"boom\")\n"
	);
}

static PyObject * beta_named_instance(void) {
	return testing_evaluate(
		"class P(Struct):\n    alpha: int\n    beta: int\nresult = P(1, beta=9)\n"
	);
}

static void test_the_explicit_prefix_stops_at_the_first_unexplicit_field(void) {
	PyObject * const plain = testing_two_field_instance();
	StructType * const type = struct_type_of(plain);
	PyObject * const beta_name = PyTuple_Pack(1, PyTuple_GET_ITEM(type->struct_field_names, 1));

	TEST_ASSERT_EQUAL_INT(2, explicit_field_prefix(type, 2, NULL, 0));

	PyObject * const named = beta_named_instance();
	TEST_ASSERT_EQUAL_INT(2, explicit_field_prefix(type, 1, beta_name, 0));

	TEST_ASSERT_EQUAL_INT(0, explicit_field_prefix(type, 0, beta_name, 0));

	Py_DECREF(beta_name);
	Py_DECREF(named);
	Py_DECREF(plain);
}

static void test_the_explicit_prefix_counts_carried_positionals(void) {
	PyObject * const instance = testing_two_field_instance();

	TEST_ASSERT_EQUAL_INT(2, explicit_field_prefix(struct_type_of(instance), 0, NULL, 2));

	Py_DECREF(instance);
}

static void test_change_names_touch_answers_for_each_change_shape(void) {
	PyObject * const instance = testing_two_field_instance();
	StructType * const type = struct_type_of(instance);
	PyObject * const beta_name = PyTuple_Pack(1, PyTuple_GET_ITEM(type->struct_field_names, 1));

	TEST_ASSERT_EQUAL_INT(1, change_names_touch(type, beta_name, 1, 1));
	TEST_ASSERT_EQUAL_INT(0, change_names_touch(type, beta_name, 1, 0));
	TEST_ASSERT_EQUAL_INT(0, change_names_touch(type, NULL, 1, 1));
	TEST_ASSERT_EQUAL_INT(0, change_names_touch(type, beta_name, 1, -1));

	Py_DECREF(beta_name);
	Py_DECREF(instance);
}

static void test_the_carried_payload_count_reads_the_stored_args(void) {
	PyObject * const exception = exception_instance();
	PyObject * const plain = testing_two_field_instance();

	TEST_ASSERT_EQUAL_INT(1, carried_payload_count(struct_type_of(exception), exception));
	TEST_ASSERT_EQUAL_INT(0, carried_payload_count(struct_type_of(plain), plain));

	Py_DECREF(plain);
	Py_DECREF(exception);
}

#	if PY_VERSION_HEX >= 0x030B0000

static PyObject * group_instance(void) {
	return testing_evaluate(
		"class G(ExceptionGroup, Struct, frozen=False):\n"
		"    message: str\n"
		"    exceptions: list\n"
		"result = G(\"boom\", [ValueError(\"x\")])\n"
	);
}

static void test_the_group_members_carry_writes_the_bound_fields(void) {
	PyObject * const instance = group_instance();
	StructType * const type = struct_type_of(instance);
	PyObject * const shell = type->heap_type.ht_type.tp_alloc(&type->heap_type.ht_type, 0);
	PyObject * const message = PyUnicode_FromString("boom");
	PyObject * const exceptions = PyList_New(1);
	PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) shell;

	TEST_ASSERT_NOT_NULL(shell);
	TEST_ASSERT_NOT_NULL(message);
	TEST_ASSERT_NOT_NULL(exceptions);
	TEST_ASSERT_NULL(group->msg);
	TEST_ASSERT_NULL(group->excs);

	PyList_SET_ITEM(exceptions, 0, PyObject_CallNoArgs(PyExc_ValueError));

	TEST_ASSERT_EQUAL_INT(RESULT_OK, write_slot(type, shell, type->struct_message_index, message));
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		write_slot(type, shell, type->struct_exceptions_index, exceptions)
	);
	TEST_ASSERT_EQUAL_INT(RESULT_OK, group_members_from_fields(type, shell, NULL));

	TEST_ASSERT_NOT_NULL(group->msg);
	TEST_ASSERT_EQUAL_INT(0, PyUnicode_CompareWithASCIIString(group->msg, "boom"));
	TEST_ASSERT_NOT_NULL(group->excs);
	TEST_ASSERT_TRUE(PyTuple_Check(group->excs));
	TEST_ASSERT_EQUAL_INT(1, PyTuple_GET_SIZE(group->excs));
	TEST_ASSERT_TRUE(PyExceptionInstance_Check(PyTuple_GET_ITEM(group->excs, 0)));

	Py_DECREF(exceptions);
	Py_DECREF(message);
	Py_DECREF(shell);
	Py_DECREF(instance);
}

#	endif

void exceptions_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_the_explicit_prefix_stops_at_the_first_unexplicit_field);
	RUN_TEST(test_the_explicit_prefix_counts_carried_positionals);
	RUN_TEST(test_change_names_touch_answers_for_each_change_shape);
	RUN_TEST(test_the_carried_payload_count_reads_the_stored_args);
#	if PY_VERSION_HEX >= 0x030B0000
	RUN_TEST(test_the_group_members_carry_writes_the_bound_fields);
#	endif
}

#endif
