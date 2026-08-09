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
static PyObject * Struct_copy(PyObject * self, PyObject * noargs);
static PyObject * Struct_deepcopy(PyObject * self, PyObject * memo);
static PyObject * reconstruction_args_of(PyObject * self, PyObject * * keywords);
static PyObject * call_method_noargs(PyObject * self, char const * name);
static PyObject * call_method_onearg(PyObject * self, char const * name, PyObject * arg);
static PyObject * deepcopy_object(PyObject * object, PyObject * memo);
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
		.ml_name = "__copy__",
		.ml_meth = Struct_copy,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name = "__deepcopy__",
		.ml_meth = Struct_deepcopy,
		.ml_flags = METH_O,
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

static PyObject * call_method_noargs(PyObject * const self, char const * const name) {
	PY_OWNED(method, PyObject_GetAttrString(self, name));

	if (method == NULL) {
		return NULL;
	}

	return PyObject_CallNoArgs(method);
}

static PyObject * call_method_onearg(
	PyObject * const self,
	char const * const name,
	PyObject * const arg
) {
	PY_OWNED(method, PyObject_GetAttrString(self, name));

	if (method == NULL) {
		return NULL;
	}

	return PyObject_CallOneArg(method, arg);
}

static PyObject * deepcopy_object(PyObject * const object, PyObject * const memo) {
	PY_OWNED(copy_module, PyImport_ImportModule("copy"));

	if (copy_module == NULL) {
		return NULL;
	}

	PY_OWNED(deepcopy, PyObject_GetAttrString(copy_module, "deepcopy"));

	if (deepcopy == NULL) {
		return NULL;
	}

	return PyObject_CallFunctionObjArgs(deepcopy, object, memo, NULL);
}

static PyObject * reconstruction_args_of(PyObject * const self, PyObject * * const keywords) {
	*keywords = NULL;
	StructType const * const type = struct_type_of(self);

	if (!type->struct_declares_getnewargs) {
		return PyTuple_New(0);
	}

	PY_OWNED(ex, optional_attribute(self, "__getnewargs_ex__"));

	if (ex != NULL) {
		PY_OWNED(pair, PyObject_CallNoArgs(ex));

		if (pair == NULL) {
			return NULL;
		}

		if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
			PyErr_SetString(
				PyExc_TypeError,
				"__getnewargs_ex__() should return a tuple (args, kwargs)"
			);

			return NULL;
		}

		PyObject * const args = PyTuple_GET_ITEM(pair, 0);
		PyObject * const kwargs = PyTuple_GET_ITEM(pair, 1);

		if (!PyTuple_Check(args)) {
			PyErr_SetString(
				PyExc_TypeError,
				"__getnewargs_ex__() should return a tuple of positional arguments"
			);

			return NULL;
		}

		if (kwargs != Py_None && !PyDict_Check(kwargs)) {
			PyErr_SetString(
				PyExc_TypeError,
				"__getnewargs_ex__() should return a dict of keyword arguments"
			);

			return NULL;
		}

		*keywords = kwargs != Py_None ? Py_NewRef(kwargs) : NULL;

		return Py_NewRef(args);
	}

	if (PyErr_Occurred()) {
		return NULL;
	}

	PY_OWNED(getnewargs, optional_attribute(self, "__getnewargs__"));

	if (getnewargs != NULL) {
		PY_MOVABLE(args, PyObject_CallNoArgs(getnewargs));

		if (args == NULL) {
			return NULL;
		}

		if (!PyTuple_Check(args)) {
			PyErr_SetString(PyExc_TypeError, "__getnewargs__() should return a tuple");

			return NULL;
		}

		return py_move(&args);
	}

	if (PyErr_Occurred()) {
		return NULL;
	}

	return PyTuple_New(0);
}

static PyObject * Struct_copy(PyObject * const self, PyObject * const noargs) {
	if (!is_struct(self)) {
		PyErr_Format(
			PyExc_AttributeError,
			"__copy__ is defined on structs, and %.200s is not one",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	PyTypeObject * const cls = Py_TYPE(self);
	PY_MOVABLE(keywords, NULL);
	PY_OWNED(args, reconstruction_args_of(self, &keywords));

	if (args == NULL) {
		return NULL;
	}

	PY_MOVABLE(created, cls->tp_new(cls, args, keywords));

	if (created == NULL) {
		return NULL;
	}

	PY_OWNED(state, call_method_noargs(self, "__getstate__"));

	if (state == NULL) {
		return NULL;
	}

	PY_MOVABLE(applied, call_method_onearg(created, "__setstate__", state));

	if (applied == NULL) {
		return NULL;
	}

	return py_move(&created);
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

	PyTypeObject * const cls = Py_TYPE(self);
	PY_MOVABLE(keywords, NULL);
	PY_OWNED(args, reconstruction_args_of(self, &keywords));

	if (args == NULL) {
		return NULL;
	}

	/* Create the shell with the raw reconstruction arguments, then register it
	 * in the memo before deep-copying the state, so a __getnewargs__ output that
	 * references self resolves to the copy instead of re-entering deepcopy. */
	PY_MOVABLE(created, cls->tp_new(cls, args, keywords));

	if (created == NULL) {
		return NULL;
	}

	PY_OWNED(key, PyLong_FromVoidPtr(self));

	if (key == NULL || PyDict_SetItem(memo, key, created) < 0) {
		return NULL;
	}

	PY_OWNED(state, call_method_noargs(self, "__getstate__"));

	if (state == NULL) {
		return NULL;
	}

	PY_OWNED(deep_state, deepcopy_object(state, memo));

	if (deep_state == NULL) {
		return NULL;
	}

	PY_MOVABLE(applied, call_method_onearg(created, "__setstate__", deep_state));

	if (applied == NULL) {
		return NULL;
	}

	return py_move(&created);
}
