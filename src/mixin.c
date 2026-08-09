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
		rv = PyObject_CallMethod(self, "__reduce_ex__", "i", 4);

		/* Fall back to __reduce__ only when __reduce_ex__ is absent, matching
		 * stdlib copy.deepcopy: a present __reduce_ex__ that raises AttributeError
		 * propagates, it is not treated as absence. */
		if (rv == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
			PyObject * const reduce_ex = PyObject_GetAttrString(self, "__reduce_ex__");

			if (reduce_ex == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
				PyErr_Clear();
				rv = PyObject_CallMethod(self, "__reduce__", NULL);
			} else {
				Py_XDECREF(reduce_ex);
			}
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
