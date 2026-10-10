#include <Python.h>

#include "compare.h"
#include "meta/meta.h"
#include "construct.h"
#include "hash.h"
#include "mixin.h"
#include "mixin/mixin.h"
#include "owned.h"
#include "repr.h"
#include "result.h"
#include "types.h"

static int assign_attribute(PyObject * self, PyObject * name, PyObject * value);
static PyObject * Struct_set_attribute(
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t count
);
static PyObject * Struct_delete_attribute(PyObject * self, PyObject * name);
PyObject * Struct_get_signature(PyObject * self, void * closure);
static PyObject * Struct_get_field_names(PyObject * self, void * closure);
static PyObject * Struct_get_defaults(PyObject * self, void * closure);
static PyObject * Struct_get_fields_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_defaults_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_annotations(PyObject * self, void * closure);
static PyObject * Struct_get_annotations_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_metadata(PyObject * self, void * closure);
static PyObject * Struct_get_metadata_as_msgspec(PyObject * self, void * closure);
static PyObject * metadata_of(PyObject * self, enum struct_metadata which, char const * name);
static PyGetSetDef Struct_getset[10];
static PyMethodDef Struct_methods[];

PyTypeObject StructMixin_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._StructMixin",
	.tp_doc = "The mixin carrying struct behavior; use Struct to build one.",
	.tp_basicsize = sizeof(PyObject),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_repr = Struct_repr,
	.tp_hash = Struct_hash,
	.tp_richcompare = Struct_rich_compare,
	.tp_getset = Struct_getset,
	.tp_methods = Struct_methods,
};

static PyMethodDef Struct_methods[] = {
	{
		"__setattr__",
		(PyCFunction)(void (*)(void)) Struct_set_attribute,
		METH_FASTCALL,
		NULL,
	},
	{"__delattr__", Struct_delete_attribute, METH_O, NULL},
	{"__copy__", Struct_copy, METH_NOARGS, NULL},
	{"__deepcopy__", Struct_deepcopy, METH_O, NULL},
	{"__reduce_ex__", Struct_reduce_ex, METH_O, NULL},
	{"__setstate__", Struct_setstate, METH_O, NULL},
	{
		"__replace__",
		(PyCFunction)(void (*)(void)) Struct_replace,
		METH_FASTCALL | METH_KEYWORDS,
		NULL,
	},
	{.ml_name = NULL},
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
	{
		.name = "_struct_annotations_",
		.get = Struct_get_annotations,
		.doc = "the field annotations, aligned with the fields",
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
	{
		.name = "__signature__",
		.get = Struct_get_signature,
		.doc = "the constructor signature inspect.signature reads",
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

PyObject * Struct_get_signature(PyObject * const self, void * const closure) {
	StructType * type;

	if (is_struct(self)) {
		type = struct_type_of(self);
	} else if (is_struct_class(self)) {
		type = (StructType *) self;
	} else {
		PyErr_Format(
			PyExc_AttributeError,
			"__signature__ is defined on structs, and %.200s is not one",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	PyTypeObject * const cls = (PyTypeObject *) type;
	PyObject * const mro = cls->tp_mro;
	Py_ssize_t mixin = 0;

	while (
		mixin < PyTuple_GET_SIZE(mro) &&
		PyTuple_GET_ITEM(mro, mixin) != (PyObject *) &StructMixin_Type
	) {
		mixin += 1;
	}

	PY_OWNED(binding_name, PyUnicode_FromString("__signature__"));

	if (binding_name == NULL) {
		return NULL;
	}

	PY_OWNED(own_dict, struct_type_dict(cls));

	if (own_dict == NULL) {
		return NULL;
	}

	PY_MOVABLE(own_binding, dict_value_ref(own_dict, binding_name));

	if (own_binding != NULL) {
		if (own_binding != Py_None) {
			return py_move(&own_binding);
		}

		Py_DECREF(own_binding);
	}
	if (PyErr_Occurred()) {
		return NULL;
	}

	if (type->struct_own_init) {
		PyErr_SetString(
			PyExc_AttributeError,
			"the class defines its own __init__, whose signature answers instead"
		);

		return NULL;
	}

	for (Py_ssize_t i = 1; i < mixin; i += 1) {
		PyObject * const entry = PyTuple_GET_ITEM(mro, i);
		PyTypeObject * const entry_type = (PyTypeObject *) entry;

		if (PyType_FastSubclass(entry_type, Py_TPFLAGS_BASE_EXC_SUBCLASS)) {
			PY_OWNED(entry_dict, struct_type_dict(entry_type));

			if (entry_dict == NULL) {
				if (PyErr_Occurred()) {
					return NULL;
				}

				continue;
			}

			PY_MOVABLE(entry_binding, dict_value_ref(entry_dict, binding_name));

			if (entry_binding == NULL) {
				if (PyErr_Occurred()) {
					return NULL;
				}

				continue;
			}

			if (entry_binding == Py_None) {
				continue;
			}

			if ((entry_type->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0) {
				return py_move(&entry_binding);
			}

			PyTypeObject * const parent_type = (PyTypeObject *) PyTuple_GET_ITEM(mro, i + 1);
			PY_OWNED(parent_dict, struct_type_dict(parent_type));

			if (parent_dict == NULL) {
				if (PyErr_Occurred()) {
					return NULL;
				}

				continue;
			}

			PY_OWNED(parent_binding, dict_value_ref(parent_dict, binding_name));

			if (parent_binding == NULL && PyErr_Occurred()) {
				return NULL;
			}

			if (entry_binding != parent_binding) {
				return py_move(&entry_binding);
			}

			continue;
		}

		PY_OWNED(entry_dict, struct_type_dict(entry_type));

		if (entry_dict == NULL) {
			return NULL;
		}

		int const present = PyDict_Contains(entry_dict, binding_name);

		if (present < 0) {
			return NULL;
		}

		if (present == 0) {
			continue;
		}

		PY_MOVABLE(bound, dict_value_ref(entry_dict, binding_name));

		if (bound != NULL) {
			if (bound != Py_None) {
				return py_move(&bound);
			}

			Py_DECREF(bound);
		}

		if (PyErr_Occurred()) {
			return NULL;
		}
	}

	if (type->struct_signature != NULL) {
		return Py_NewRef(type->struct_signature);
	}

	PY_OWNED(inspect_module, PyImport_ImportModule("inspect"));

	if (inspect_module == NULL) {
		return NULL;
	}

	PY_OWNED(parameter_type, PyObject_GetAttrString(inspect_module, "Parameter"));
	PY_OWNED(signature_type, PyObject_GetAttrString(inspect_module, "Signature"));
	PY_OWNED(parameters, PyList_New(struct_parameter_count(type)));

	if (parameter_type == NULL || signature_type == NULL || parameters == NULL) {
		return NULL;
	}

	PY_OWNED(kind, PyObject_GetAttrString(parameter_type, "POSITIONAL_OR_KEYWORD"));
	PY_OWNED(empty, PyObject_GetAttrString(parameter_type, "empty"));

	if (kind == NULL || empty == NULL) {
		return NULL;
	}

	Py_ssize_t const required_count = struct_required_count(type);
	Py_ssize_t const required_init_var_count = struct_required_init_var_count(type);
	Py_ssize_t field_index = 0;
	Py_ssize_t init_var_index = 0;

	for (Py_ssize_t position = 0; position < struct_parameter_count(type); position += 1) {
		bool const init_var = struct_parameter_kind(type, position) == PARAMETER_INIT_VAR;
		Py_ssize_t const index = init_var ? init_var_index : field_index;
		Py_ssize_t const required = init_var ? required_init_var_count : required_count;
		PY_OWNED(
			arguments,
			PyTuple_Pack(
				2,
				PyTuple_GET_ITEM(
					init_var ? type->struct_init_var_names : type->struct_field_names,
					index
				),
				kind
			)
		);
		PY_OWNED(keywords, PyDict_New());

		if (
			arguments == NULL ||
			keywords == NULL ||
			(
				PyDict_SetItemString(
					keywords,
					"default",
					index < required ? empty : PyTuple_GET_ITEM(
						init_var ? type->struct_init_var_defaults : type->struct_defaults,
						index - required
					)
				) < 0
			) ||
			(
				PyDict_SetItemString(
					keywords,
					"annotation",
					PyTuple_GET_ITEM(
						init_var ? type->struct_init_var_annotations : type->struct_annotations,
						index
					)
				) < 0
			)
		) {
			return NULL;
		}

		PY_MOVABLE(parameter, PyObject_Call(parameter_type, arguments, keywords));

		if (parameter == NULL) {
			return NULL;
		}

		PyList_SET_ITEM(parameters, position, py_move(&parameter));
		init_var_index += init_var ? 1 : 0;
		field_index += init_var ? 0 : 1;
	}

	PY_MOVABLE(signature, PyObject_CallOneArg(signature_type, parameters));

	if (signature == NULL) {
		return NULL;
	}

	STRUCT_BEGIN_CRITICAL_SECTION(type);

	if (type->struct_signature == NULL) {
		type->struct_signature = Py_NewRef(signature);
	}

	STRUCT_END_CRITICAL_SECTION();

	return py_move(&signature);
}

int Struct_set_signature(PyObject * const self, PyObject * const value, void * const closure) {
	if (!is_struct_class(self)) {
		PyErr_Format(
			PyExc_AttributeError,
			"__signature__ is defined on structs, and %.200s is not one",
			Py_TYPE(self)->tp_name
		);

		return -1;
	}

	PY_OWNED(dict, struct_type_dict((PyTypeObject *) self));

	if (dict == NULL) {
		return -1;
	}

	if (value == Py_None) {
		if (
			PyDict_DelItemString(dict, "__signature__") < 0 &&
			!PyErr_ExceptionMatches(PyExc_KeyError)
		) {
			return -1;
		}

		PyErr_Clear();
	} else if (value == NULL) {
		if (PyDict_DelItemString(dict, "__signature__") < 0) {
			if (PyErr_ExceptionMatches(PyExc_KeyError)) {
				PyErr_Clear();
				PyErr_Format(PyExc_AttributeError, "__signature__");
			}

			return -1;
		}
	} else if (PyDict_SetItemString(dict, "__signature__", value) < 0) {
		return -1;
	}

	PyType_Modified((PyTypeObject *) self);

	return 0;
}

static PyObject * frozen_instance_error(StructType const * const type) {
	PyObject * const cached = type->struct_state->frozen_instance_error;

	if (cached != NULL) {
		return cached;
	}

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

	STRUCT_BEGIN_CRITICAL_SECTION(module);
		if (type->struct_state->frozen_instance_error == NULL) {
			type->struct_state->frozen_instance_error = Py_NewRef(resolved);
		}
	STRUCT_END_CRITICAL_SECTION();

	return type->struct_state->frozen_instance_error;
}

static PyObject * Struct_set_attribute(
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const count
) {
	if (count != 2) {
		PyErr_Format(PyExc_TypeError, "expected 2 arguments, got %zd", count);

		return NULL;
	}

	return assign_attribute(self, arguments[0], arguments[1]) == 0 ? Py_NewRef(Py_None) : NULL;
}

static PyObject * Struct_delete_attribute(PyObject * const self, PyObject * const name) {
	return assign_attribute(self, name, NULL) == 0 ? Py_NewRef(Py_None) : NULL;
}

enum setter_source setter_source_of(PyTypeObject const * const type, char const * const name) {
	PY_OWNED(key, PyUnicode_InternFromString(name));
	PY_OWNED(object_dict, key != NULL ? struct_type_dict(&PyBaseObject_Type) : NULL);
	PY_OWNED(struct_dict, key != NULL ? struct_type_dict(&StructMixin_Type) : NULL);
	PyObject * const object_setter = (
		object_dict != NULL ? PyDict_GetItemWithError(object_dict, key) :
		NULL
	);
	PyObject * const struct_setter = (
		struct_dict != NULL ? PyDict_GetItemWithError(struct_dict, key) :
		NULL
	);

	if (object_setter == NULL || struct_setter == NULL) {
		if (!PyErr_Occurred()) {
			PyErr_Format(PyExc_SystemError, "salix internal error: no %s to resolve against", name);
		}

		return SETTER_SOURCE_ERROR;
	}

	PyObject * const mro = type->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i += 1) {
		PY_OWNED(entry_dict, struct_type_dict((PyTypeObject *) PyTuple_GET_ITEM(mro, i)));
		PyObject * const found = (
			entry_dict != NULL ? PyDict_GetItemWithError(entry_dict, key) :
			NULL
		);

		if (PyErr_Occurred()) {
			return SETTER_SOURCE_ERROR;
		}

		if (found != NULL) {
			return (
				found == object_setter ? SETTER_SOURCE_OBJECT :
				found == struct_setter ? SETTER_SOURCE_STRUCT :
				SETTER_SOURCE_OTHER
			);
		}
	}

	return SETTER_SOURCE_OBJECT;
}

static int assign_attribute(PyObject * const self, PyObject * const name, PyObject * const value) {
	if (!is_struct(self) || !struct_type_of(self)->struct_options.frozen) {
		return PyObject_GenericSetAttr(self, name, value);
	}

	if (PyUnicode_Check(name) && PyUnicode_CompareWithASCIIString(name, "__orig_class__") == 0) {
		return 0;
	}

	if (PyUnicode_Check(name)) {
		PyObject * const frozen_error = frozen_instance_error(struct_type_of(self));

		if (frozen_error != NULL) {
			PyErr_Format(
				frozen_error,
				value == NULL ? "cannot delete field %R" : "cannot assign to field %R",
				name
			);

			return RESULT_ERROR;
		}

		if (PyErr_Occurred()) {
			return RESULT_ERROR;
		}
	}

	PyErr_Format(
		PyExc_AttributeError,
		"'%.200s' object does not support attribute %s",
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

#ifdef TESTING

#	include "testing.h"

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

void mixin_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_the_frozen_error_resolution_caches_the_stock_class);
}

#endif
