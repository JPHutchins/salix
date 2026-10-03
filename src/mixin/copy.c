#include <Python.h>

#include "mixin.h"
#include "../construct/construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

PyObject * co_base_override(PyObject * const self, PyObject * const name) {
	PyTypeObject * const cls = Py_TYPE(self);
	PyObject * const mro = cls->tp_mro;
	Py_ssize_t mixin = 0;

	while (
		mixin < PyTuple_GET_SIZE(mro) &&
		PyTuple_GET_ITEM(mro, mixin) != (PyObject *) &StructMixin_Type
	) {
		mixin += 1;
	}

	if (mixin == PyTuple_GET_SIZE(mro)) {
		return NULL;
	}

	for (Py_ssize_t i = mixin + 1; i < PyTuple_GET_SIZE(mro); i += 1) {
		PyObject * const entry = PyTuple_GET_ITEM(mro, i);

		if (entry == (PyObject *) &PyBaseObject_Type) {
			continue;
		}

		PY_OWNED(entry_dict, struct_type_dict((PyTypeObject *) entry));

		if (entry_dict == NULL) {
			return NULL;
		}

		int const present = PyDict_Contains(entry_dict, name);

		if (present < 0) {
			return NULL;
		}

		if (present == 0) {
			continue;
		}

		PY_OWNED(raw, dict_value_ref(entry_dict, name));

		if (raw == NULL) {
			return NULL;
		}

		PyObject * resolved;

		if (PyUnicode_CompareWithASCIIString(name, "__copy__") != 0) {
			PyTypeObject * const raw_type = Py_TYPE(raw);

			if (raw_type->tp_descr_get == NULL) {
				resolved = Py_NewRef(raw);
			} else {
				PY_MOVABLE(value, raw_type->tp_descr_get(raw, self, (PyObject *) cls));

				if (value == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
					PyErr_Clear();

					return NULL;
				}

				if (value == NULL) {
					return NULL;
				}

				resolved = py_move(&value);
			}
		} else if (PyObject_TypeCheck(raw, &PyClassMethod_Type)) {
			PY_MOVABLE(
				bound,
				PyObject_CallMethod(raw, "__get__", "OO", Py_None, (PyObject *) Py_TYPE(self))
			);

			if (bound == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
				PyErr_Clear();

				return NULL;
			}

			if (bound == NULL) {
				return NULL;
			}

			resolved = py_move(&bound);
		} else {
			PyObject * const value = PyObject_GetAttr(entry, name);

			if (value == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
				PyErr_Clear();

				return NULL;
			}

			if (value == NULL) {
				return NULL;
			}

			resolved = value;
		}

		if (resolved == Py_None) {
			Py_DECREF(resolved);

			return NULL;
		}

		return resolved;
	}

	return NULL;
}

PyObject * copy_dispatch_prologue(
	PyObject * const self,
	char const * const name,
	PyObject * const argument,
	bool const dispatch_truthy,
	PyObject * * const copy_module,
	PyObject * * const copier
) {
	PY_OWNED(copy_name, PyUnicode_InternFromString(name));

	if (copy_name == NULL) {
		return NULL;
	}

	PY_OWNED(deferred, co_base_override(self, copy_name));

	if (deferred == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (deferred != NULL) {
		return PyObject_CallOneArg(deferred, argument);
	}

	PY_MOVABLE(module, PyImport_ImportModule("copy"));

	if (module == NULL) {
		return NULL;
	}

	PY_OWNED(dispatch_table, PyObject_GetAttrString(module, "dispatch_table"));

	if (dispatch_table == NULL) {
		return NULL;
	}

	PY_MOVABLE(registered, dict_value_ref(dispatch_table, (PyObject *) Py_TYPE(self)));

	if (registered == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (registered == Py_None) {
		Py_CLEAR(registered);
	} else if (registered != NULL && dispatch_truthy) {
		int const truthy = PyObject_IsTrue(registered);

		if (truthy < 0) {
			return NULL;
		}

		if (truthy == 0) {
			Py_CLEAR(registered);
		}
	}

	*copy_module = py_move(&module);
	*copier = py_move(&registered);

	return NULL;
}

PyObject * copy_delegate(
	PyObject * const self,
	PyObject * const argument,
	PyObject * const memo,
	char const * const name,
	char const * const uncopyable,
	bool const dispatch_truthy
) {
	PY_MOVABLE(copy_module, NULL);
	PY_MOVABLE(copier, NULL);
	PY_MOVABLE(
		deferred,
		copy_dispatch_prologue(self, name, argument, dispatch_truthy, &copy_module, &copier)
	);

	if (deferred != NULL) {
		return py_move(&deferred);
	}

	if (PyErr_Occurred()) {
		return NULL;
	}

	PY_MOVABLE(reduced, NULL);

	if (copier != NULL) {
		reduced = PyObject_CallOneArg(copier, self);
	} else {
		PY_OWNED(reduce_ex, PyObject_GetAttrString(self, "__reduce_ex__"));

		if (reduce_ex == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
			PyErr_Clear();
		}

		if (reduce_ex == NULL && PyErr_Occurred()) {
			return NULL;
		}

		if (reduce_ex != NULL && reduce_ex != Py_None) {
			reduced = PyObject_CallFunction(reduce_ex, "i", 4);
		} else {
			PY_OWNED(reduce, PyObject_GetAttrString(self, "__reduce__"));

			if (reduce == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
				PyErr_Clear();
			}

			if (reduce == NULL && PyErr_Occurred()) {
				return NULL;
			}

			int const reduce_truthy = reduce != NULL ? PyObject_IsTrue(reduce) : 0;

			if (reduce_truthy < 0) {
				return NULL;
			}

			if (reduce_truthy) {
				reduced = PyObject_CallNoArgs(reduce);
			} else {
				PyObject * const error = PyObject_GetAttrString(copy_module, "Error");

				if (error == NULL) {
					return NULL;
				}

				PY_OWNED(type_name, PyObject_Str((PyObject *) Py_TYPE(self)));

				if (type_name == NULL) {
					Py_DECREF(error);

					return NULL;
				}

				PyErr_Format((PyObject *) error, "%s object of type %U", uncopyable, type_name);
				Py_DECREF(error);
			}
		}
	}

	return reduced != NULL ? copy_reconstruct(self, reduced, copy_module, memo) : NULL;
}

PyObject * copy_reconstruct(
	PyObject * const self,
	PyObject * const reduced,
	PyObject * const copy_module,
	PyObject * const memo
) {
	if (PyUnicode_Check(reduced)) {
		return Py_NewRef(self);
	}

	PY_OWNED(tuple, PySequence_Tuple(reduced));

	if (tuple == NULL) {
		return NULL;
	}

	PY_OWNED(reconstruct, PyObject_GetAttrString(copy_module, "_reconstruct"));

	if (reconstruct == NULL) {
		return NULL;
	}

	Py_ssize_t const parts = PyTuple_GET_SIZE(tuple);
	PY_OWNED(arguments, PyTuple_New(parts + 2));

	if (arguments == NULL) {
		return NULL;
	}

	PyTuple_SET_ITEM(arguments, 0, Py_NewRef(self));
	PyTuple_SET_ITEM(arguments, 1, Py_NewRef(memo));

	for (Py_ssize_t i = 0; i < parts; ++i) {
		PyTuple_SET_ITEM(arguments, i + 2, Py_NewRef(PyTuple_GET_ITEM(tuple, i)));
	}

	return PyObject_Call(reconstruct, arguments, NULL);
}

PyObject * interned_copy(StructType const * const type) {
	return type->struct_singleton != NULL ? Py_NewRef(type->struct_singleton) : NULL;
}

PyObject * Struct_copy(PyObject * const self, PyObject * const noargs) {
	if (!is_struct(self)) {
		return copy_delegate(self, self, Py_None, "__copy__", "un(shallow)copyable", false);
	}

	StructType * const type = struct_type_of(self);
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", cls->tp_name);

		return NULL;
	}

	PY_MOVABLE(copy_module, NULL);
	PY_MOVABLE(copier, NULL);
	PY_MOVABLE(
		deferred,
		copy_dispatch_prologue(self, "__copy__", self, false, &copy_module, &copier)
	);

	if (deferred != NULL) {
		return py_move(&deferred);
	}

	if (PyErr_Occurred()) {
		return NULL;
	}

	PY_MOVABLE(
		reduced,
		copier != NULL || type->struct_copies_through_reduce ? copy_reduction(self, copier) : NULL
	);

	if (reduced == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (reduced != NULL) {
		return copy_reconstruct(self, reduced, copy_module, Py_None);
	}

	PY_MOVABLE(short_circuit, interned_copy(type));

	if (short_circuit != NULL) {
		return py_move(&short_circuit);
	}

	PY_MOVABLE(copy, NULL);

	if (type->struct_family_owned) {
		PY_OWNED(values_snapshot, PyTuple_New(type->struct_field_count));

		if (values_snapshot == NULL) {
			return NULL;
		}

		struct_slots_ref_into(type, self, values_snapshot, NULL);

		PyObject * args;

		STRUCT_BEGIN_CRITICAL_SECTION(self);
		args = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
		STRUCT_END_CRITICAL_SECTION();

		if (args == NULL) {
			args = PyTuple_New(0);
		}

		if (args == NULL) {
			return NULL;
		}

		if (PyTuple_GET_SIZE(args) > 0) {
			PY_MOVABLE(rebuilt, PyObject_Call((PyObject *) cls, args, NULL));
			Py_DECREF(args);

			if (rebuilt == NULL) {
				return NULL;
			}

			copy = py_move(&rebuilt);
		} else {
			Py_DECREF(args);
			copy = cls->tp_alloc(cls, 0);

			if (copy == NULL) {
				return NULL;
			}
		}

		for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
			PyObject * const value = PyTuple_GET_ITEM(values_snapshot, i);

			if (value != NULL) {
				Py_XSETREF(*struct_slot(type, copy, i), Py_NewRef(value));
			}
		}
	} else {
		copy = cls->tp_alloc(cls, 0);
	}

	if (copy == NULL) {
		return NULL;
	}

	PY_MOVABLE(dict, NULL);
	struct_slots_copy_into(type, self, copy, &dict);

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyBaseExceptionGroupObject * const source_group = (PyBaseExceptionGroupObject *) self;

		if (
			carry_group_members(
				type,
				copy,
				source_group->msg,
				source_group->excs,
				group_excs_str(self),
				NULL,
				NULL
			) != RESULT_OK
		) {
			return NULL;
		}
	}
#endif

	if (
		(dict != NULL && struct_dict_copy_merged(dict, copy) < 0) ||
		set_exception_args_from_original(type, copy, self, NULL, NULL) != RESULT_OK
	) {
		return NULL;
	}

	return py_move(&copy);
}

#ifdef TESTING

#	include "../testing.h"

static char const reductions_source[] = {
#	embed "../../tests/c/fixtures/mixin/reductions.py" suffix(, '\0')
};

static void test_an_interned_copy_answers_the_singleton(void) {
	PyObject * const frozen = testing_frozen_empty_instance();
	PyObject * const fielded = testing_two_field_instance();

	TEST_ASSERT_EQUAL_PTR(frozen, interned_copy(struct_type_of(frozen)));
	TEST_ASSERT_NULL(interned_copy(struct_type_of(fielded)));

	Py_DECREF(fielded);
	Py_DECREF(frozen);
}

static void test_a_copy_of_a_frozen_empty_struct_is_the_singleton(void) {
	PyObject * const frozen = testing_frozen_empty_instance();

	TEST_ASSERT_EQUAL_PTR(frozen, Struct_copy(frozen, NULL));

	Py_DECREF(frozen);
}

static void test_a_copy_of_a_fielded_struct_is_equal_and_distinct(void) {
	PyObject * const source = testing_two_field_instance();
	PyObject * const copy = Struct_copy(source, NULL);

	TEST_ASSERT_NOT_NULL(copy);
	TEST_ASSERT_NOT_EQUAL(source, copy);
	TEST_ASSERT_EQUAL_INT(1, PyObject_RichCompareBool(copy, source, Py_EQ));

	Py_DECREF(copy);
	Py_DECREF(source);
}

static void test_a_string_reduction_copies_the_identity(void) {
	PyObject * const fixtures = testing_evaluate(reductions_source);
	PyObject * const copied = copy_reconstruct(
		testing_entry(fixtures, "pair"),
		testing_entry(fixtures, "string_reduction"),
		testing_entry(fixtures, "copy_module"),
		Py_None
	);

	TEST_ASSERT_NOT_NULL(copied);
	TEST_ASSERT_EQUAL_PTR(testing_entry(fixtures, "pair"), copied);

	Py_DECREF(copied);
	Py_DECREF(fixtures);
}

static void test_a_tuple_reduction_rebuilds_through_the_memo(void) {
	PyObject * const fixtures = testing_evaluate(reductions_source);
	PyObject * const rebuilt = copy_reconstruct(
		testing_entry(fixtures, "pair"),
		testing_entry(fixtures, "tuple_reduction"),
		testing_entry(fixtures, "copy_module"),
		testing_entry(fixtures, "memo")
	);

	TEST_ASSERT_NOT_NULL(rebuilt);
	TEST_ASSERT_EQUAL_INT(
		1,
		PyObject_RichCompareBool(rebuilt, testing_entry(fixtures, "rebuilt_pair"), Py_EQ)
	);
	TEST_ASSERT_EQUAL_INT(1, PyDict_GET_SIZE(testing_entry(fixtures, "memo")));

	Py_DECREF(rebuilt);
	Py_DECREF(fixtures);
}

void copy_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_interned_copy_answers_the_singleton);
	RUN_TEST(test_a_copy_of_a_frozen_empty_struct_is_the_singleton);
	RUN_TEST(test_a_copy_of_a_fielded_struct_is_equal_and_distinct);
	RUN_TEST(test_a_string_reduction_copies_the_identity);
	RUN_TEST(test_a_tuple_reduction_rebuilds_through_the_memo);
}

#endif
