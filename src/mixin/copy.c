#include <Python.h>

#include "mixin.h"
#include "../construct/construct.h"
#include "../meta/meta.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static PyObject * copy_reconstruct(
	PyObject * self,
	PyObject * reduced,
	PyObject * copy_module,
	PyObject * memo
);

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
static PyObject * copy_dispatch_prologue(
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

static PyObject * copy_delegate(
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
static PyObject * copy_reconstruct(
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

static PyObject * interned_copy(StructType const * const type) {
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

static PyObject * memo_failure(PyObject * const memo, PyObject * const key) {
	PyObject * error_type = NULL, *error_value = NULL, *traceback = NULL;

	PyErr_Fetch(&error_type, &error_value, &traceback);

	if (PyDict_DelItem(memo, key) < 0) {
		PyErr_Clear();
	}

	PyErr_Restore(error_type, error_value, traceback);

	return NULL;
}

PyObject * Struct_deepcopy(PyObject * const self, PyObject * const memo) {
	if (!PyDict_Check(memo)) {
		PyErr_SetString(PyExc_TypeError, "__deepcopy__() argument must be a dict");

		return NULL;
	}

	/* copy.deepcopy's entry check, reproduced so a direct protocol call
	 * honors a seeded memo. */
	PY_OWNED(key, PyLong_FromVoidPtr(self));

	if (key == NULL) {
		return NULL;
	}

	PY_MOVABLE(seeded, dict_value_ref(memo, key));

	if (seeded == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (seeded != NULL) {
		return py_move(&seeded);
	}

	if (!is_struct(self)) {
		PY_MOVABLE(
			delegated,
			copy_delegate(self, memo, memo, "__deepcopy__", "un(deep)copyable", true)
		);

		if (delegated == NULL || (delegated != self && PyDict_SetItem(memo, key, delegated) < 0)) {
			return NULL;
		}

		return py_move(&delegated);
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
		copy_dispatch_prologue(self, "__deepcopy__", memo, true, &copy_module, &copier)
	);

	if (deferred != NULL) {
		if (deferred != self && PyDict_SetItem(memo, key, deferred) < 0) {
			return NULL;
		}

		return py_move(&deferred);
	}

	if (PyErr_Occurred()) {
		return NULL;
	}

	if (copier != NULL) {
		PY_MOVABLE(reduced, PyObject_CallOneArg(copier, self));

		if (reduced == NULL) {
			return NULL;
		}

		PY_MOVABLE(reconstructed, copy_reconstruct(self, reduced, copy_module, memo));

		if (
			reconstructed == NULL ||
			(reconstructed != self && PyDict_SetItem(memo, key, reconstructed) < 0)
		) {
			return NULL;
		}

		return py_move(&reconstructed);
	}

	PY_MOVABLE(short_circuit, interned_copy(type));

	if (short_circuit != NULL) {
		return py_move(&short_circuit);
	}

	PY_OWNED(deepcopy, PyObject_GetAttrString(copy_module, "deepcopy"));

	if (deepcopy == NULL) {
		return memo_failure(memo, key);
	}

	PY_MOVABLE(copy, NULL);

	if (type->struct_family_owned) {
		/* The family's construction is its C members' only writer --
		 * OSError's live in __new__ and its init no-ops without it -- so
		 * the copy is the construction itself, with the source's
		 * positional payload deep-copied first: the C members hold the
		 * detached elements, not the original's. The constructor
		 * pre-filled the defaults; the source's values -- mutations and
		 * prior replaces included -- overwrite them, releasing the
		 * pre-filled references. */
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
			/* The shell registers before the payload's deep copy, so a
			 * self-referential args tuple resolves to it instead of
			 * re-entering deepcopy on the source forever; the memo entry
			 * then moves to the rebuilt copy. The family's construction
			 * formats the payload, and the self-reference the memo resolves
			 * to the shell may be the element it formats: the shell carries
			 * an empty payload so the NULL-args read every version guards
			 * against never fires. */
			PY_MOVABLE(shell, cls->tp_alloc(cls, 0));

			if (shell == NULL) {
				Py_DECREF(args);

				return NULL;
			}

			PY_MOVABLE(empty_payload, PyTuple_New(0));

			if (empty_payload == NULL) {
				Py_DECREF(args);

				return NULL;
			}

			((PyBaseExceptionObject *) shell)->args = py_move(&empty_payload);

			if (PyDict_SetItem(memo, key, shell) < 0) {
				Py_DECREF(args);

				return NULL;
			}

			PY_MOVABLE(deep_args, PyObject_CallFunctionObjArgs(deepcopy, args, memo, NULL));
			Py_DECREF(args);

			if (deep_args == NULL) {
				return memo_failure(memo, key);
			}

			PY_MOVABLE(rebuilt, PyObject_Call((PyObject *) cls, deep_args, NULL));

			if (rebuilt == NULL) {
				return memo_failure(memo, key);
			}

			copy = py_move(&rebuilt);

			if (PyDict_SetItem(memo, key, copy) < 0) {
				Py_CLEAR(copy);

				return NULL;
			}
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

	/* The shell is registered in the memo before its state is copied, so a
	 * field that references the source resolves to the shell instead of
	 * re-entering deepcopy; the shell's own loop then fills that field with
	 * the copy itself. */
	if (PyDict_SetItem(memo, key, copy) < 0) {
		return NULL;
	}

	PY_MOVABLE(dict, NULL);
	struct_slots_copy_into(type, self, copy, &dict);

	/* Each shallow copy is replaced by its deep copy, made outside the
	 * section because copy.deepcopy runs arbitrary Python. */
	for (Py_ssize_t i = 0; i < type->struct_field_count; i += 1) {
		PyObject * const value = *struct_slot(type, copy, i);

		if (value != NULL) {
			PY_MOVABLE(deep, PyObject_CallFunctionObjArgs(deepcopy, value, memo, NULL));

			if (deep == NULL) {
				return memo_failure(memo, key);
			}

			Py_SETREF(*struct_slot(type, copy, i), py_move(&deep));
		}
	}

	for (Py_ssize_t i = 0; i < type->struct_member_count; i += 1) {
		Py_ssize_t const offset = type->struct_member_offsets[i];
		PyObject * const value = *(PyObject * *) ((char *) copy + offset);

		if (value != NULL) {
			PY_MOVABLE(deep, PyObject_CallFunctionObjArgs(deepcopy, value, memo, NULL));

			if (deep == NULL) {
				return memo_failure(memo, key);
			}

			Py_SETREF(*((PyObject * *) ((char *) copy + offset)), py_move(&deep));
		}
	}

	if (dict != NULL) {
		/* PyDict_Copy runs under the dict's own lock, so the deepcopy walks
		 * a snapshot no concurrent writer can change size under. */
		PY_OWNED(snapshot, PyDict_Copy(dict));

		if (snapshot == NULL) {
			return memo_failure(memo, key);
		}

		PY_MOVABLE(deep_dict, PyObject_CallFunctionObjArgs(deepcopy, snapshot, memo, NULL));

		if (deep_dict == NULL) {
			return memo_failure(memo, key);
		}

		if (PyObject_GenericSetDict(copy, deep_dict, NULL) < 0) {
			return memo_failure(memo, key);
		}
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyBaseExceptionGroupObject * const source_group = (PyBaseExceptionGroupObject *) self;

		if (
			carry_group_members(
				type,
				copy,
				source_group->msg,
				source_group->excs,
				/* The body is a fresh deep copy, so the source's cached repr
				 * would describe members the copy does not hold; NULL
				 * rebuilds it from the detached body. */
				NULL,
				deepcopy,
				memo
			) !=
			RESULT_OK
		) {
			return memo_failure(memo, key);
		}
	}
#endif

	if (set_exception_args_from_original(type, copy, self, deepcopy, memo) != RESULT_OK) {
		return memo_failure(memo, key);
	}

	return py_move(&copy);
}
PyObject * frozen_instance_error(StructType const * const type) {
	PyObject * const cached = type->struct_state->frozen_instance_error;

	if (cached != NULL) {
		return cached;
	}

	/* A failed resolution is not cached, so a later failure retries the
	 * import instead of latching the fallback process-wide. */
	PY_OWNED(module, PyImport_ImportModule("dataclasses"));

	if (module == NULL) {
		if (
			!PyErr_ExceptionMatches(PyExc_ImportError) &&
			!PyErr_ExceptionMatches(PyExc_AttributeError)
		) {
			return NULL;
		}

		PyErr_Clear();

		return NULL;
	}

	PY_OWNED(resolved, optional_attribute(module, "FrozenInstanceError"));

	if (resolved == NULL || !PyExceptionClass_Check(resolved)) {
		return NULL;
	}

	/* Both racers hold the same module, so the critical section is on it; the
	 * loser's reference drops with its scope. */
	STRUCT_BEGIN_CRITICAL_SECTION(module);
		if (type->struct_state->frozen_instance_error == NULL) {
			type->struct_state->frozen_instance_error = Py_NewRef(resolved);
		}
	STRUCT_END_CRITICAL_SECTION();

	return type->struct_state->frozen_instance_error;
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

static void test_a_deepcopy_is_equal_and_distinct(void) {
	PyObject * const source = testing_two_field_instance();
	PyObject * const memo = PyDict_New();
	PyObject * const copy = Struct_deepcopy(source, memo);

	TEST_ASSERT_NOT_NULL(memo);
	TEST_ASSERT_NOT_NULL(copy);
	TEST_ASSERT_NOT_EQUAL(source, copy);
	TEST_ASSERT_EQUAL_INT(1, PyObject_RichCompareBool(copy, source, Py_EQ));

	Py_DECREF(copy);
	Py_DECREF(memo);
	Py_DECREF(source);
}

static void test_a_seeded_memo_answers_for_deepcopy(void) {
	PyObject * const source = testing_two_field_instance();
	PyObject * const memo = PyDict_New();
	PyObject * const key = PyLong_FromVoidPtr(source);
	PyObject * const sentinel = PyList_New(0);

	TEST_ASSERT_NOT_NULL(memo);
	TEST_ASSERT_NOT_NULL(key);
	TEST_ASSERT_NOT_NULL(sentinel);
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItem(memo, key, sentinel));
	TEST_ASSERT_EQUAL_PTR(sentinel, Struct_deepcopy(source, memo));

	Py_DECREF(sentinel);
	Py_DECREF(key);
	Py_DECREF(memo);
	Py_DECREF(source);
}

static void test_a_failed_memo_restore_keeps_the_error_and_the_contract(void) {
	PyObject * const memo = PyDict_New();
	PyObject * const key = PyLong_FromVoidPtr(memo);
	PyObject * const value = PyList_New(0);

	TEST_ASSERT_NOT_NULL(memo);
	TEST_ASSERT_NOT_NULL(key);
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItem(memo, key, value));

	PyErr_SetString(PyExc_ValueError, "kept");

	TEST_ASSERT_NULL(memo_failure(memo, key));
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
	TEST_ASSERT_EQUAL_INT(0, PyDict_Contains(memo, key));
	PyErr_Clear();

	Py_DECREF(value);
	Py_DECREF(key);
	Py_DECREF(memo);
}

static void test_the_frozen_error_resolution_caches_the_stock_class(void) {
	PyObject * const instance = testing_frozen_empty_instance();
	StructType const * const type = struct_type_of(instance);
	PyObject * const first = frozen_instance_error(type);
	PyObject * const second = frozen_instance_error(type);

	TEST_ASSERT_NOT_NULL(first);
	TEST_ASSERT_TRUE(PyExceptionClass_Check(first));
	TEST_ASSERT_EQUAL_PTR(first, second);
	TEST_ASSERT_TRUE(
		PyType_IsSubtype((PyTypeObject *) first, (PyTypeObject *) PyExc_AttributeError)
	);

	Py_DECREF(instance);
}

void copy_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_interned_copy_answers_the_singleton);
	RUN_TEST(test_a_copy_of_a_frozen_empty_struct_is_the_singleton);
	RUN_TEST(test_a_copy_of_a_fielded_struct_is_equal_and_distinct);
	RUN_TEST(test_a_deepcopy_is_equal_and_distinct);
	RUN_TEST(test_a_seeded_memo_answers_for_deepcopy);
	RUN_TEST(test_a_failed_memo_restore_keeps_the_error_and_the_contract);
	RUN_TEST(test_the_frozen_error_resolution_caches_the_stock_class);
}

#endif
