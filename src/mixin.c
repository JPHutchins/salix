#include <Python.h>

#include "compare.h"
#include "construct.h"
#include "hash.h"
#include "mixin.h"
#include "owned.h"
#include "repr.h"
#include "result.h"
#include "types.h"

static int Struct_set_attribute(PyObject * self, PyObject * name, PyObject * value);
static PyObject * Struct_get_field_names(PyObject * self, void * closure);
static PyObject * Struct_get_defaults(PyObject * self, void * closure);
static PyObject * Struct_get_fields_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_defaults_as_msgspec(PyObject * self, void * closure);
static PyObject * metadata_of(PyObject * self, enum struct_metadata which, char const * name);
static PyObject * Struct_deepcopy(PyObject * self, PyObject * memo);
static PyObject * Struct_reduce_ex(PyObject * self, PyObject * args);
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
	.tp_methods = Struct_methods,
	.tp_getset = Struct_getset,
};

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
	{.name = NULL},
};

static PyMethodDef Struct_methods[] = {
	{
		.ml_name = "__getstate__",
		.ml_meth = Struct_get_state,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name = "__setstate__",
		.ml_meth = Struct_set_state,
		.ml_flags = METH_O,
	},
	{
		.ml_name = "__deepcopy__",
		.ml_meth = Struct_deepcopy,
		.ml_flags = METH_O,
	},
	{
		.ml_name = "__reduce_ex__",
		.ml_meth = Struct_reduce_ex,
		.ml_flags = METH_VARARGS,
	},
	{.ml_name = NULL},
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

static PyObject * object_reduce_ex(PyObject * const self, PyObject * const args) {
	PY_OWNED(reduce_ex, PyObject_GetAttrString((PyObject *) &PyBaseObject_Type, "__reduce_ex__"));

	if (reduce_ex == NULL) {
		return NULL;
	}

	PY_OWNED(call_args, PyTuple_New(PyTuple_GET_SIZE(args) + 1));

	if (call_args == NULL) {
		return NULL;
	}

	PyTuple_SET_ITEM(call_args, 0, Py_NewRef(self));

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); ++i) {
		PyTuple_SET_ITEM(call_args, i + 1, Py_NewRef(PyTuple_GET_ITEM(args, i)));
	}

	return PyObject_Call(reduce_ex, call_args, NULL);
}

/* copyreg's __newobj__ equivalents, raised around the __new__ call so
 * Struct_new can tell a reconstruction from a normal construction (the type
 * being reconstructed is visible to it during the call). The reduce names these
 * instead of copyreg's, so pickle and copy both route a getnewargs
 * reconstruction through them. A user __reduce__ may name them directly, so the
 * arguments are validated before any tuple item is read. */
static PyObject * reconstruct_raised(
	PyTypeObject * const cls,
	PyObject * const call_args,
	PyObject * const kwargs
) {
	PY_MOVABLE(new_method, PyObject_GetAttrString((PyObject *) cls, "__new__"));

	if (new_method == NULL) {
		return NULL;
	}

	PyTypeObject * const saved = struct_reconstruction_type;
	struct_reconstruction_type = cls;
	PY_MOVABLE(result, PyObject_Call(new_method, call_args, kwargs));
	struct_reconstruction_type = saved;

	return py_move(&result);
}

PyObject * Struct_reduce_newobj(PyObject * const module, PyObject * const args) {
	if (PyTuple_GET_SIZE(args) < 1) {
		PyErr_SetString(
			PyExc_TypeError,
			"__reduce_newobj__() missing 1 required positional argument: 'cls'"
		);

		return NULL;
	}

	return reconstruct_raised((PyTypeObject *) PyTuple_GET_ITEM(args, 0), args, NULL);
}

PyObject * Struct_reduce_newobj_check(PyObject * const module, PyObject * const args) {
	if (PyTuple_GET_SIZE(args) < 1) {
		PyErr_SetString(
			PyExc_TypeError,
			"__reduce_newobj__() missing 1 required positional argument: 'cls'"
		);

		return NULL;
	}

	PY_MOVABLE(result, reconstruct_raised((PyTypeObject *) PyTuple_GET_ITEM(args, 0), args, NULL));

	if (result == NULL || struct_reconstruction_complete(result) < 0) {
		return NULL;
	}

	return py_move(&result);
}

static PyObject * validate_ex_args(
	PyObject * const args,
	PyTypeObject * * const cls,
	PyObject * * const newargs,
	PyObject * * const kwargs
) {
	if (PyTuple_GET_SIZE(args) != 3) {
		PyErr_SetString(
			PyExc_TypeError,
			"__reduce_newobj_ex__() expected 3 positional arguments: 'cls', 'args' and 'kwargs'"
		);

		return NULL;
	}

	PyObject * const class_object = PyTuple_GET_ITEM(args, 0);
	PyObject * const args_object = PyTuple_GET_ITEM(args, 1);
	PyObject * const kwargs_object = PyTuple_GET_ITEM(args, 2);

	if (!PyTuple_Check(args_object)) {
		PyErr_Format(
			PyExc_TypeError,
			"__reduce_newobj_ex__() 'args' must be a tuple, not %.200s",
			Py_TYPE(args_object)->tp_name
		);

		return NULL;
	}

	if (!PyDict_Check(kwargs_object)) {
		PyErr_Format(
			PyExc_TypeError,
			"__reduce_newobj_ex__() 'kwargs' must be a dict, not %.200s",
			Py_TYPE(kwargs_object)->tp_name
		);

		return NULL;
	}

	*cls = (PyTypeObject *) class_object;
	*newargs = args_object;
	*kwargs = kwargs_object;

	return args;
}

static PyObject * reconstruct_ex_raised(PyObject * const args, bool const check_complete) {
	PyTypeObject * cls = NULL;
	PyObject * newargs = NULL;
	PyObject * kwargs = NULL;

	if (validate_ex_args(args, &cls, &newargs, &kwargs) == NULL) {
		return NULL;
	}

	PY_MOVABLE(prefixed, PyTuple_New(PyTuple_GET_SIZE(newargs) + 1));

	if (prefixed == NULL) {
		return NULL;
	}

	PyTuple_SET_ITEM(prefixed, 0, Py_NewRef((PyObject *) cls));

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(newargs); ++i) {
		PyTuple_SET_ITEM(prefixed, i + 1, Py_NewRef(PyTuple_GET_ITEM(newargs, i)));
	}

	PY_MOVABLE(result, reconstruct_raised(cls, prefixed, kwargs));

	if (result == NULL || (check_complete && struct_reconstruction_complete(result) < 0)) {
		return NULL;
	}

	return py_move(&result);
}

PyObject * Struct_reduce_newobj_ex(PyObject * const module, PyObject * const args) {
	return reconstruct_ex_raised(args, false);
}

PyObject * Struct_reduce_newobj_ex_check(PyObject * const module, PyObject * const args) {
	return reconstruct_ex_raised(args, true);
}

static PyObject * reduce_with_newargs(
	PyObject * const self,
	PyObject * const newargs,
	PyObject * const newkwargs
) {
	bool const has_kwargs = newkwargs != NULL && PyDict_GET_SIZE(newkwargs) > 0;
	PY_OWNED(salix, PyImport_ImportModule("salix"));

	if (salix == NULL) {
		return NULL;
	}

	PY_MOVABLE(newobj, NULL);
	PY_MOVABLE(args, NULL);
	PY_OWNED(cls, Py_NewRef((PyObject *) Py_TYPE(self)));
	PY_MOVABLE(state, PyObject_CallMethod(self, "__getstate__", NULL));

	if (state == NULL) {
		return NULL;
	}

	/* A None state skips __setstate__ (stdlib's contract), so the completeness
	 * check cannot run there -- the reconstruction callable itself must verify
	 * the getnewargs arguments covered the required fields. A carried state
	 * uses the plain callable and leaves the check to __setstate__, which sees
	 * the state's explicitly-unset names. */
	bool const check_complete = state == Py_None;

	if (has_kwargs) {
		newobj = PyObject_GetAttrString(
			salix,
			check_complete ? "__reduce_newobj_ex_check__" : "__reduce_newobj_ex__"
		);
		args = PyTuple_Pack(3, cls, newargs, newkwargs);
	} else {
		newobj = PyObject_GetAttrString(
			salix,
			check_complete ? "__reduce_newobj_check__" : "__reduce_newobj__"
		);
		Py_ssize_t const count = PyTuple_GET_SIZE(newargs);
		args = PyTuple_New(count + 1);

		if (args != NULL) {
			PyTuple_SET_ITEM(args, 0, Py_NewRef(cls));

			for (Py_ssize_t i = 0; i < count; ++i) {
				PyTuple_SET_ITEM(args, i + 1, Py_NewRef(PyTuple_GET_ITEM(newargs, i)));
			}
		}
	}

	if (newobj == NULL || args == NULL) {
		return NULL;
	}

	return PyTuple_Pack(5, newobj, args, state, Py_None, Py_None);
}

static PyObject * reduce_without_newargs(PyObject * const self) {
	PY_OWNED(empty, PyTuple_New(0));

	return empty != NULL ? reduce_with_newargs(self, empty, NULL) : NULL;
}

/* Whether the class's MRO binds its own __reduce__, which object.__reduce_ex__
 * honors before it touches getnewargs. Walking the class dicts, not getattr, so
 * a user __getattr__ never runs. Returns -1 with an exception set when a dict
 * cannot be read, so a real error is never swallowed into "no custom reduce". */
static int has_custom_reduce(PyObject * const self) {
	PY_OWNED(reduce_name, PyUnicode_InternFromString("__reduce__"));
	PY_OWNED(object_reduce, PyObject_GetAttrString((PyObject *) &PyBaseObject_Type, "__reduce__"));

	if (reduce_name == NULL || object_reduce == NULL) {
		return -1;
	}

	PyObject * const mro = Py_TYPE(self)->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyObject * const entry = PyTuple_GET_ITEM(mro, i);
		PY_OWNED(dict, struct_type_dict((PyTypeObject *) entry));

		if (dict == NULL) {
			return -1;
		}

		PY_MOVABLE(bound, dict_value_ref(dict, reduce_name));

		if (bound == NULL) {
			if (PyErr_Occurred()) {
				return -1;
			}

			continue;
		}

		return bound != object_reduce;
	}

	return 0;
}

static PyObject * getnewargs_plain(PyObject * const self) {
	PY_OWNED(method, PyObject_GetAttrString(self, "__getnewargs__"));

	if (method == NULL) {
		return NULL;
	}

	PY_MOVABLE(args, PyObject_CallNoArgs(method));

	if (args == NULL) {
		return NULL;
	}

	if (!PyTuple_Check(args)) {
		PyErr_Format(
			PyExc_TypeError,
			"__getnewargs__ should return a tuple, not '%.200s'",
			Py_TYPE(args)->tp_name
		);

		return NULL;
	}

	return py_move(&args);
}

static PyObject * getnewargs_ex(PyObject * const self, PyObject * * const kwargs) {
	PY_OWNED(method, PyObject_GetAttrString(self, "__getnewargs_ex__"));

	if (method == NULL) {
		return NULL;
	}

	PY_MOVABLE(result, PyObject_CallNoArgs(method));

	if (result == NULL) {
		return NULL;
	}

	if (!PyTuple_Check(result)) {
		PyErr_Format(
			PyExc_TypeError,
			"__getnewargs_ex__ should return a tuple, not '%.200s'",
			Py_TYPE(result)->tp_name
		);

		return NULL;
	}

	if (PyTuple_GET_SIZE(result) != 2) {
		PyErr_Format(
			PyExc_TypeError,
			"__getnewargs_ex__ should return a tuple of length 2, not %zd",
			PyTuple_GET_SIZE(result)
		);

		return NULL;
	}

	PyObject * const args = PyTuple_GET_ITEM(result, 0);
	PyObject * const kw = PyTuple_GET_ITEM(result, 1);

	if (!PyTuple_Check(args)) {
		PyErr_Format(
			PyExc_TypeError,
			"first item of the tuple returned by __getnewargs_ex__ must be a tuple, not '%.200s'",
			Py_TYPE(args)->tp_name
		);

		return NULL;
	}

	if (!PyDict_Check(kw)) {
		PyErr_Format(
			PyExc_TypeError,
			"second item of the tuple returned by __getnewargs_ex__ must be a dict, not '%.200s'",
			Py_TYPE(kw)->tp_name
		);

		return NULL;
	}

	*kwargs = Py_NewRef(kw);

	return Py_NewRef(args);
}

/* object.__reduce_ex__ calls whatever __getnewargs__ resolves to, and a class
 * that declares a present-but-None value hits "'NoneType' object is not
 * callable". A struct treats that declaration as absence and builds the reduce
 * itself for every getnewargs-declaring class, so a None __getnewargs__ is
 * never called and the reconstruction always carries a restorable state. A
 * None __getstate__ is passed through: stdlib's contract is that it skips
 * __setstate__, and the reconstruction stands on the getnewargs arguments.
 * A class with no declaration, or one with a custom __reduce__, defers to
 * object.__reduce_ex__, which honors the protocol-below-2 refusal. */
static PyObject * Struct_reduce_ex(PyObject * const self, PyObject * const args) {
	if (!is_struct(self)) {
		return object_reduce_ex(self, args);
	}

	if (PyTuple_GET_SIZE(args) != 1) {
		/* object.__reduce_ex__ takes exactly one protocol argument; defer the
		 * argument-count validation so a missing or extra argument raises the
		 * same TypeError stdlib does. */
		return object_reduce_ex(self, args);
	}

	PyObject * const protocol_object = PyTuple_GET_ITEM(args, 0);
	int const protocol = (int) PyLong_AsLong(protocol_object);

	if (protocol == -1 && PyErr_Occurred()) {
		return NULL;
	}

	if (protocol < 2) {
		return object_reduce_ex(self, args);
	}

	int const custom_reduce = has_custom_reduce(self);

	if (custom_reduce < 0) {
		return NULL;
	}

	if (custom_reduce > 0) {
		return object_reduce_ex(self, args);
	}

	PY_OWNED(ex_name, PyUnicode_InternFromString("__getnewargs_ex__"));
	PY_OWNED(plain_name, PyUnicode_InternFromString("__getnewargs__"));

	if (ex_name == NULL || plain_name == NULL) {
		return NULL;
	}

	PY_MOVABLE(first_ex, NULL);
	PY_MOVABLE(first_plain, NULL);
	PyObject * const mro = Py_TYPE(self)->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyObject * const entry = PyTuple_GET_ITEM(mro, i);
		PY_OWNED(dict, struct_type_dict((PyTypeObject *) entry));

		if (dict == NULL) {
			return NULL;
		}

		if (first_ex == NULL) {
			first_ex = dict_value_ref(dict, ex_name);

			if (first_ex == NULL && PyErr_Occurred()) {
				return NULL;
			}
		}

		if (first_plain == NULL) {
			first_plain = dict_value_ref(dict, plain_name);

			if (first_plain == NULL && PyErr_Occurred()) {
				return NULL;
			}
		}

		if (first_ex != NULL && first_plain != NULL) {
			break;
		}
	}

	bool const ex_real = first_ex != NULL && first_ex != Py_None;
	bool const plain_real = first_plain != NULL && first_plain != Py_None;
	bool const ex_present_none = first_ex != NULL && first_ex == Py_None;
	bool const plain_present_none = first_plain != NULL && first_plain == Py_None;

	if (ex_real) {
		PY_MOVABLE(kwargs, NULL);
		PY_MOVABLE(newargs, getnewargs_ex(self, &kwargs));

		if (newargs == NULL) {
			return NULL;
		}

		return reduce_with_newargs(self, newargs, kwargs);
	}

	if (plain_real) {
		PY_MOVABLE(newargs, getnewargs_plain(self));

		if (newargs == NULL) {
			return NULL;
		}

		return reduce_with_newargs(self, newargs, NULL);
	}

	if (ex_present_none || plain_present_none) {
		return reduce_without_newargs(self);
	}

	return object_reduce_ex(self, args);
}

static PyObject * call_reduce_or_error(PyObject * const self) {
	PY_MOVABLE(reduce, PyObject_GetAttrString(self, "__reduce__"));

	if (reduce == NULL) {
		if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
			PyErr_Clear();
		} else {
			return NULL;
		}
	} else if (reduce == Py_None) {
		Py_DECREF(reduce);
		reduce = NULL;
	} else {
		return PyObject_CallNoArgs(reduce);
	}

	PY_OWNED(copy_module, PyImport_ImportModule("copy"));

	if (copy_module == NULL) {
		return NULL;
	}

	PY_OWNED(copy_error, PyObject_GetAttrString(copy_module, "Error"));

	if (copy_error == NULL) {
		return NULL;
	}

	PyErr_Format(copy_error, "un(deep)copyable object of type %R", (PyObject *) Py_TYPE(self));

	return NULL;
}

static PyObject * Struct_deepcopy(PyObject * const self, PyObject * const memo) {
	if (!is_struct(self)) {
		PyErr_Format(
			PyExc_AttributeError,
			"__deepcopy__ is defined on structs, and %.200s is not one",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	if (!PyDict_Check(memo)) {
		PyErr_SetString(PyExc_TypeError, "__deepcopy__() argument must be a dict");

		return NULL;
	}

	/* A struct deep-copies by reconstructing through the same reduce tuple
	 * copy.deepcopy uses for a class without __deepcopy__: copyreg's
	 * dispatch_table, then __reduce_ex__, then __reduce__. copy._reconstruct
	 * does the deep copy of the args and state, memoizing the new object
	 * before the state so a self-reference resolves to the copy. The point of
	 * defining __deepcopy__ at all is that copy.deepcopy probes the instance
	 * for it first -- a struct whose __getattr__ returns a value for unknown
	 * names would otherwise have that value called. */
	PY_MOVABLE(rv, NULL);
	PY_OWNED(copyreg, PyImport_ImportModule("copyreg"));

	if (copyreg == NULL) {
		return NULL;
	}

	PY_OWNED(dispatch_table, PyObject_GetAttrString(copyreg, "dispatch_table"));

	if (dispatch_table == NULL) {
		return NULL;
	}

	PY_MOVABLE(copier, dict_value_ref(dispatch_table, (PyObject *) Py_TYPE(self)));

	if (copier == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (copier != NULL) {
		rv = PyObject_CallOneArg(copier, self);
	} else {
		PyObject * const reduce_ex = PyObject_GetAttrString(self, "__reduce_ex__");

		if (reduce_ex == NULL) {
			if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
				PyErr_Clear();
				rv = call_reduce_or_error(self);
			} else {
				return NULL;
			}
		} else if (reduce_ex == Py_None) {
			Py_DECREF(reduce_ex);
			rv = call_reduce_or_error(self);
		} else {
			rv = PyObject_CallFunction(reduce_ex, "i", 4);
			Py_DECREF(reduce_ex);
		}
	}

	if (rv == NULL) {
		return NULL;
	}

	/* A string reduce result means "the object is its own copy", the same way
	 * stdlib copy.deepcopy and copy.copy handle it. */
	if (PyUnicode_Check(rv)) {
		return Py_NewRef(self);
	}

	if (!PyTuple_Check(rv)) {
		PyErr_Format(
			PyExc_TypeError,
			"__deepcopy__() reduce must return a tuple, not %.200s",
			Py_TYPE(rv)->tp_name
		);

		return NULL;
	}

	PY_OWNED(copy_module, PyImport_ImportModule("copy"));

	if (copy_module == NULL) {
		return NULL;
	}

	PY_OWNED(reconstruct, PyObject_GetAttrString(copy_module, "_reconstruct"));

	if (reconstruct == NULL) {
		return NULL;
	}

	Py_ssize_t const rv_size = PyTuple_GET_SIZE(rv);
	PY_OWNED(call_args, PyTuple_New(rv_size + 2));

	if (call_args == NULL) {
		return NULL;
	}

	PyTuple_SET_ITEM(call_args, 0, Py_NewRef(self));
	PyTuple_SET_ITEM(call_args, 1, Py_NewRef(memo));

	for (Py_ssize_t i = 0; i < rv_size; ++i) {
		PyTuple_SET_ITEM(call_args, i + 2, Py_NewRef(PyTuple_GET_ITEM(rv, i)));
	}

	return PyObject_Call(reconstruct, call_args, NULL);
}
