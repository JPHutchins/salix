#include <Python.h>

#include "compare.h"
#include "hash.h"
#include "mixin.h"
#include "owned.h"
#include "repr.h"
#include "result.h"
#include "types.h"

static int Struct_set_attribute(PyObject * self, PyObject * name, PyObject * value);
static PyObject * Struct_copy(PyObject * self, PyObject * noargs);
static PyObject * Struct_deepcopy(PyObject * self, PyObject * memo);
static PyObject * copy_delegate(
	PyObject * self,
	PyObject * argument,
	PyObject * memo,
	char const * name,
	char const * uncopyable,
	bool dispatch_truthy
);
static PyObject * copy_reconstruct(
	PyObject * self,
	PyObject * reduced,
	PyObject * copy_module,
	PyObject * memo
);
static PyObject * copy_dispatch_prologue(
	PyObject * self,
	char const * name,
	PyObject * argument,
	bool dispatch_truthy,
	PyObject * * copy_module,
	PyObject * * copier
);
static PyObject * deferred_co_base_copy(PyObject * self, PyObject * name);
static PyObject * Struct_get_field_names(PyObject * self, void * closure);
static PyObject * Struct_get_defaults(PyObject * self, void * closure);
static PyObject * Struct_get_fields_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_defaults_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_annotations(PyObject * self, void * closure);
static PyObject * Struct_get_annotations_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_metadata(PyObject * self, void * closure);
static PyObject * Struct_get_metadata_as_msgspec(PyObject * self, void * closure);
static PyObject * metadata_of(PyObject * self, enum struct_metadata which, char const * name);
static PyGetSetDef Struct_getset[];
static PyMethodDef Struct_methods[];

PyTypeObject StructMixin_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._StructMixin",
	.tp_basicsize = sizeof(PyObject),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_setattro = Struct_set_attribute,
	.tp_repr = Struct_repr,
	.tp_hash = Struct_hash,
	.tp_richcompare = Struct_rich_compare,
	.tp_getset = Struct_getset,
	.tp_methods = Struct_methods,
};

static PyMethodDef Struct_methods[] = {
	{"__copy__", Struct_copy, METH_NOARGS, NULL},
	{"__deepcopy__", Struct_deepcopy, METH_O, NULL},
	{.ml_name = NULL},
};

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

static PyObject * Struct_copy(PyObject * const self, PyObject * const noargs) {
	if (!is_struct(self)) {
		return copy_delegate(self, self, Py_None, "__copy__", "un(shallow)copyable", false);
	}

	StructType * const type = struct_type_of(self);
	PyTypeObject * const cls = &type->heap_type.ht_type;
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

	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (copy == NULL) {
		return NULL;
	}

	PY_MOVABLE(dict, NULL);
	struct_slots_copy_into(type, self, copy, &dict);

	if (dict != NULL) {
		PY_OWNED(copied, PyDict_Copy(dict));

		if (copied == NULL || PyObject_GenericSetDict(copy, copied, NULL) < 0) {
			return NULL;
		}
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

static PyObject * Struct_deepcopy(PyObject * const self, PyObject * const memo) {
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

	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

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

	PY_OWNED(deepcopy, PyObject_GetAttrString(copy_module, "deepcopy"));

	if (deepcopy == NULL) {
		return memo_failure(memo, key);
	}

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

	return py_move(&copy);
}

static PyGetSetDef Struct_getset[] = {
	{
		.name = "_struct_fields_",
		.get = Struct_get_field_names,
		.doc = "tuple of field names",
	},
	{
		.name = "_struct_defaults_",
		.get = Struct_get_defaults,
		.doc = "tuple of trailing defaults",
	},
	{
		.name = "__struct_fields__",
		.get = Struct_get_fields_as_msgspec,
		.doc = "tuple of field names, under msgspec's name for it",
	},
	{
		.name = "__struct_defaults__",
		.get = Struct_get_defaults_as_msgspec,
		.doc = "tuple of trailing defaults, under msgspec's name for it",
	},
	{
		.name = "_struct_annotations_",
		.get = Struct_get_annotations,
		.doc = "the field annotations as evaluated, aligned with the fields",
	},
	{
		.name = "__struct_annotations__",
		.get = Struct_get_annotations_as_msgspec,
		.doc = "the field annotations under the public name for it",
	},
	{
		.name = "_struct_metadata_",
		.get = Struct_get_metadata,
		.doc = "the Annotated extras per field, aligned with the fields",
	},
	{
		.name = "__struct_metadata__",
		.get = Struct_get_metadata_as_msgspec,
		.doc = "the Annotated extras under the public name for it",
	},
	{.name = NULL},
};

static PyObject * metadata_of(
	PyObject * const self,
	enum struct_metadata const which,
	char const * const name
) {
	if (is_struct(self)) {
		return struct_metadata(struct_type_of(self), which);
	}

	PyErr_Format(
		PyExc_AttributeError,
		"%s is defined on structs, and %.200s is not one",
		name,
		Py_TYPE(self)->tp_name
	);

	return NULL;
}

static int Struct_set_attribute(
	PyObject * const self,
	PyObject * const name,
	PyObject * const value
) {
	if (!is_struct(self)) {
		return PyObject_GenericSetAttr(self, name, value);
	}

	PyErr_Format(
		PyExc_TypeError,
		"%.200s object does not support attribute %s",
		Py_TYPE(self)->tp_name,
		value == NULL ? "deletion" : "assignment"
	);

	return RESULT_ERROR;
}

static PyObject * Struct_get_field_names(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_FIELD_NAMES, "_struct_fields_");
}

static PyObject * Struct_get_defaults(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_DEFAULTS, "_struct_defaults_");
}

static PyObject * Struct_get_fields_as_msgspec(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_FIELD_NAMES, "__struct_fields__");
}

static PyObject * Struct_get_defaults_as_msgspec(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_DEFAULTS, "__struct_defaults__");
}

static PyObject * Struct_get_annotations(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_ANNOTATIONS, "_struct_annotations_");
}

static PyObject * Struct_get_annotations_as_msgspec(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_ANNOTATIONS, "__struct_annotations__");
}

static PyObject * Struct_get_metadata(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_METADATA, "_struct_metadata_");
}

static PyObject * Struct_get_metadata_as_msgspec(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_METADATA, "__struct_metadata__");
}
