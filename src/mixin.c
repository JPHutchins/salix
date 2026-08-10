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
static PyObject * Struct_copy_delegate(PyObject * self);
static PyObject * Struct_get_field_names(PyObject * self, void * closure);
static PyObject * Struct_get_defaults(PyObject * self, void * closure);
static PyObject * Struct_get_fields_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_defaults_as_msgspec(PyObject * self, void * closure);
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
	{.ml_name = NULL},
};

static PyObject * Struct_copy_delegate(PyObject * const self) {
	PY_OWNED(reduced, PyObject_CallMethod(self, "__reduce_ex__", "i", 4));

	if (reduced == NULL) {
		return NULL;
	}

	if (PyUnicode_Check(reduced)) {
		return Py_NewRef(self);
	}

	if (!PyTuple_Check(reduced)) {
		PyErr_Format(
			PyExc_TypeError,
			"__reduce_ex__ returned %.200s, not a tuple",
			Py_TYPE(reduced)->tp_name
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

	Py_ssize_t const parts = PyTuple_GET_SIZE(reduced);
	PY_OWNED(arguments, PyTuple_New(parts + 2));

	if (arguments == NULL) {
		return NULL;
	}

	PyTuple_SET_ITEM(arguments, 0, Py_NewRef(self));
	PyTuple_SET_ITEM(arguments, 1, Py_NewRef(Py_None));

	for (Py_ssize_t i = 0; i < parts; ++i) {
		PyTuple_SET_ITEM(arguments, i + 2, Py_NewRef(PyTuple_GET_ITEM(reduced, i)));
	}

	return PyObject_Call(reconstruct, arguments, NULL);
}

static PyObject * Struct_copy(PyObject * const self, PyObject * const noargs) {
	if (!is_struct(self)) {
		return Struct_copy_delegate(self);
	}

	StructType * const type = struct_type_of(self);
	PyTypeObject * const cls = &type->heap_type.ht_type;
	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (copy == NULL) {
		return NULL;
	}

	PY_MOVABLE(dict, NULL);

	STRUCT_BEGIN_CRITICAL_SECTION(self);

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * const value = *struct_slot(type, self, i);

		if (value != NULL) {
			*struct_slot(type, copy, i) = Py_NewRef(value);
		}
	}

	/* The one read that sees the managed-dict slot without materializing
	 * the source's dict on 3.13, where the slot is NULL until first use;
	 * PyObject_GenericGetDict would create it, mutating the source. */
	PyObject * * const dict_slot = _PyObject_GetDictPtr(self);

	if (dict_slot != NULL) {
		dict = Py_XNewRef(*dict_slot);
	}
	STRUCT_END_CRITICAL_SECTION();

	if (dict != NULL) {
		PY_OWNED(copied, PyDict_Copy(dict));

		if (copied == NULL || PyObject_GenericSetDict(copy, copied, NULL) < 0) {
			return NULL;
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
