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
static PyObject * copy_reconstruct(PyObject * self, PyObject * reduced, PyObject * copy_module);
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
	PY_OWNED(copy_module, PyImport_ImportModule("copy"));

	if (copy_module == NULL) {
		return NULL;
	}

	PY_OWNED(dispatch_table, PyObject_GetAttrString(copy_module, "dispatch_table"));

	if (dispatch_table == NULL) {
		return NULL;
	}

	PY_MOVABLE(reduced, NULL);
	PY_OWNED(copier, dict_value_ref(dispatch_table, (PyObject *) Py_TYPE(self)));

	if (copier == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (copier != NULL) {
		reduced = PyObject_CallOneArg(copier, self);
	} else {
		reduced = PyObject_CallMethod(self, "__reduce_ex__", "i", 4);
	}

	return reduced != NULL ? copy_reconstruct(self, reduced, copy_module) : NULL;
}

/*
 * The reduce branch of copy.copy: a string result means "copy the identity",
 * otherwise the result is handed to copy._reconstruct the way copy.py's
 * `*rv` is, so a non-tuple iterable is accepted and a non-iterable fails
 * the same TypeError `*rv` would raise.
 */
static PyObject * copy_reconstruct(
	PyObject * const self,
	PyObject * const reduced,
	PyObject * const copy_module
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
	PyTuple_SET_ITEM(arguments, 1, Py_NewRef(Py_None));

	for (Py_ssize_t i = 0; i < parts; ++i) {
		PyTuple_SET_ITEM(arguments, i + 2, Py_NewRef(PyTuple_GET_ITEM(tuple, i)));
	}

	return PyObject_Call(reconstruct, arguments, NULL);
}

static PyObject * struct_copy_name(void) {
	static PyObject * name = NULL;

	if (name == NULL) {
		name = PyUnicode_InternFromString("__copy__");
	}

	return name;
}

static PyObject * Struct_copy(PyObject * const self, PyObject * const noargs) {
	if (!is_struct(self)) {
		return Struct_copy_delegate(self);
	}

	StructType * const type = struct_type_of(self);
	PyTypeObject * const cls = &type->heap_type.ht_type;

	/* A __copy__ defined by a co-base sits after _StructMixin in the MRO, so
	 * the mixin's method would shadow it for copy.copy's lookup. Defer to
	 * the first __copy__ found outside the mixin's own dict, exactly the one
	 * copy.copy would have found without the mixin's method in the way. The
	 * value is resolved through attribute lookup on the defining class, so a
	 * classmethod, property or member descriptor is bound the way copy.py's
	 * getattr would, and fails the same TypeError it would. */
	PyObject * const copy_name = struct_copy_name();

	if (copy_name == NULL) {
		return NULL;
	}

	PY_OWNED(name, Py_NewRef(copy_name));

	PyObject * const mro = cls->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyObject * const entry = PyTuple_GET_ITEM(mro, i);

		if (entry == (PyObject *) &StructMixin_Type) {
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

		PY_MOVABLE(method, PyObject_GetAttr(entry, name));

		if (method == NULL) {
			return NULL;
		}

		return PyObject_CallOneArg(method, self);
	}

	/* copy.py consults dispatch_table before __copy__, and the mixin's
	 * method would shadow a user registration for a real struct; honor it
	 * the same way the delegate does, with the same reduce-path handling. */
	PY_OWNED(copy_module, PyImport_ImportModule("copy"));

	if (copy_module == NULL) {
		return NULL;
	}

	PY_OWNED(dispatch_table, PyObject_GetAttrString(copy_module, "dispatch_table"));

	if (dispatch_table == NULL) {
		return NULL;
	}

	PY_OWNED(copier, dict_value_ref(dispatch_table, (PyObject *) cls));

	if (copier == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (copier != NULL) {
		PY_MOVABLE(reduced, PyObject_CallOneArg(copier, self));

		return reduced != NULL ? copy_reconstruct(self, reduced, copy_module) : NULL;
	}

	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (copy == NULL) {
		return NULL;
	}

	struct_slots_copy_into(type, self, copy);

	PY_MOVABLE(dict, struct_instance_dict_ref(self));

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
