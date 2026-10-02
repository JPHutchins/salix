#include <Python.h>

#include "mixin.h"
#include "../construct/construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

/*
 * A __copy__ or __deepcopy__ defined by a co-base sits after _StructMixin in
 * the MRO, so the mixin's method would shadow it; copy.py resolves
 * __deepcopy__ on the instance and __copy__ on the class, and each branch
 * reproduces that lookup. The scan starts after the mixin: a method before it
 * was already found by getattr and called by copy.py, and a rebind of the
 * mixin's own method would otherwise re-enter it forever. A descriptor
 * __get__ AttributeError and None both mean "no method"; NULL means none was
 * found.
 */
static PyObject * deferred_co_base_copy(PyObject * const self, PyObject * const name) {
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

		if (PyUnicode_CompareWithASCIIString(name, "__deepcopy__") == 0) {
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
			/* copy.py's getattr(cls, '__copy__', None) binds a classmethod
			 * to the concrete class; the class-level access of the other
			 * descriptors is the lookup below. */
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

/*
 * The dispatch prologue shared by the struct and impostor paths. `argument`
 * is what the deferred method is called with, the instance for __copy__ and
 * the memo for __deepcopy__; `dispatch_truthy` selects the gate copy.py
 * applies to the dispatch_table branch, identity for copy and truthiness
 * for deepcopy. `copy_module` and `copier` are owned when returned non-NULL.
 */
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

	PY_OWNED(deferred, deferred_co_base_copy(self, copy_name));

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

	/* The one gate copy.py applies differently to the two operations: the
	 * copy branch tests identity, the deepcopy branch tests truthiness. */
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
		/* copy.py's reduce chain: __reduce_ex__ if present and not None
		 * (identity, per copy.py), else __reduce__ likewise but gated on
		 * truthiness (copy.py's own inconsistency), else the same
		 * uncopyable-object copy.Error. */
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

/*
 * The reduce branch of copy: a string result means "copy the identity",
 * otherwise the result goes to copy._reconstruct the way copy.py's `*rv`
 * does, with the memo copy.py would pass -- None for copy, the caller's
 * for deepcopy.
 */
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

	/* A body __new__ = None is the cannot-create marker; the cached flag
	 * answers at every construction entry point, the metatype's dispatch
	 * included. */
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

	if (copier != NULL) {
		PY_MOVABLE(reduced, PyObject_CallOneArg(copier, self));

		return reduced != NULL ? copy_reconstruct(self, reduced, copy_module, Py_None) : NULL;
	}

	PY_MOVABLE(short_circuit, interned_copy(type));

	if (short_circuit != NULL) {
		return py_move(&short_circuit);
	}

	PY_MOVABLE(copy, NULL);

	if (type->struct_family_owned) {
		/* The family's construction is its C members' only writer --
		 * OSError's live in __new__ and its init no-ops without it -- so
		 * the copy is the construction itself, with the source's
		 * positional payload. The constructor pre-filled the defaults;
		 * the source's values -- mutations and prior replaces included --
		 * overwrite them, releasing the pre-filled references. */
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
			/* An empty payload marks a from_mapping-built source: the
			 * family's parse has nothing to reconstruct, and the plain
			 * allocation keeps the members exactly as the source left
			 * them -- unset, not fabricated. */
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

void copy_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_interned_copy_answers_the_singleton);
	RUN_TEST(test_a_copy_of_a_frozen_empty_struct_is_the_singleton);
	RUN_TEST(test_a_copy_of_a_fielded_struct_is_equal_and_distinct);
}

#endif
