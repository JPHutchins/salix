#include "introspection.h"

#include <stddef.h>

#include "owned.h"
#include "types.h"

enum field_attribute : unsigned char {
	FIELD_NAME,
	FIELD_TYPE,
	FIELD_DEFAULT,
	FIELD_DEFAULT_FACTORY,
	FIELD_INIT,
	FIELD_REPR,
	FIELD_HASH,
	FIELD_COMPARE,
	FIELD_METADATA,
	FIELD_KW_ONLY,
	FIELD_ANNOTATED_METADATA,
	FIELD_ATTRIBUTE_COUNT,
};

typedef struct {
	PyObject_HEAD
	PyObject * attributes[FIELD_ATTRIBUTE_COUNT];
} FieldObject;

static PyMemberDef Field_members[] = {
	{
		.name = "name",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_NAME]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "type",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_TYPE]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "default",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_DEFAULT]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "default_factory",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_DEFAULT_FACTORY]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "init",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_INIT]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "repr",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_REPR]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "hash",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_HASH]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "compare",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_COMPARE]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "metadata",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_METADATA]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "kw_only",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_KW_ONLY]),
		.flags = READ_ONLY_MEMBER,
	},
	{
		.name = "annotated_metadata",
		.type = SLOT_MEMBER_TYPE,
		.offset = offsetof(FieldObject, attributes[FIELD_ANNOTATED_METADATA]),
		.flags = READ_ONLY_MEMBER,
	},
	{.name = NULL},
};

static_assert(sizeof Field_members / sizeof Field_members[0] == FIELD_ATTRIBUTE_COUNT + 1);

static PyObject * * field_slot(PyObject * const field, Py_ssize_t const offset) {
	return (PyObject * *) ((char *) field + offset);
}

static PyObject * field_new(PyObject * const attributes[static FIELD_ATTRIBUTE_COUNT]) {
	FieldObject * const field = PyObject_GC_New(FieldObject, &Field_Type);

	if (field == NULL) {
		return NULL;
	}

	for (Py_ssize_t index = 0; index < FIELD_ATTRIBUTE_COUNT; index += 1) {
		field->attributes[index] = Py_NewRef(attributes[index]);
	}

	PyObject_GC_Track(field);

	return (PyObject *) field;
}

static int Field_traverse(PyObject * const self, visitproc const visit, void * const arg) {
	FieldObject * const field = (FieldObject *) self;

	for (Py_ssize_t index = 0; index < FIELD_ATTRIBUTE_COUNT; index += 1) {
		Py_VISIT(field->attributes[index]);
	}

	return 0;
}

static int Field_clear(PyObject * const self) {
	FieldObject * const field = (FieldObject *) self;

	for (Py_ssize_t index = 0; index < FIELD_ATTRIBUTE_COUNT; index += 1) {
		Py_CLEAR(field->attributes[index]);
	}

	return 0;
}

static void Field_dealloc(PyObject * const self) {
	PyObject_GC_UnTrack(self);
	Field_clear(self);
	Py_TYPE(self)->tp_free(self);
}

static PyObject * Field_repr(PyObject * const self) {
	PY_OWNED(parts, PyList_New(FIELD_ATTRIBUTE_COUNT));

	if (parts == NULL) {
		return NULL;
	}

	for (Py_ssize_t index = 0; index < FIELD_ATTRIBUTE_COUNT; index += 1) {
		PyMemberDef const * const member = &Field_members[index];
		PyObject * const part = PyUnicode_FromFormat(
			"%s=%R",
			member->name,
			*field_slot(self, member->offset)
		);

		if (part == NULL) {
			return NULL;
		}

		PyList_SET_ITEM(parts, index, part);
	}

	PY_OWNED(separator, PyUnicode_FromString(", "));
	PY_OWNED(joined, separator != NULL ? PyUnicode_Join(separator, parts) : NULL);

	return joined != NULL ? PyUnicode_FromFormat("Field(%U)", joined) : NULL;
}

static PyObject * Field_replace(
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const nargs,
	PyObject * const keyword_names
) {
	if (nargs != 0) {
		PyErr_Format(
			PyExc_TypeError,
			"Field.__replace__() takes no positional arguments (%zd given)",
			nargs
		);

		return NULL;
	}

	PY_MOVABLE(replaced, field_new(((FieldObject *) self)->attributes));

	if (replaced == NULL) {
		return NULL;
	}

	Py_ssize_t const change_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;

	for (Py_ssize_t change = 0; change < change_count; change += 1) {
		PyObject * const name = PyTuple_GET_ITEM(keyword_names, change);
		struct member_lookup const lookup = find_member(Field_members, FIELD_ATTRIBUTE_COUNT, name);

		switch (lookup.tag) {
			case MEMBER_LOOKUP_ERROR:
				return NULL;
			case MEMBER_LOOKUP_MISSING:
				PyErr_Format(
					PyExc_TypeError,
					"Field.__replace__() got an unexpected keyword argument '%U'",
					name
				);

				return NULL;
			case MEMBER_LOOKUP_FOUND:
				Py_SETREF(
					*field_slot(replaced, lookup.slot_offset),
					Py_NewRef(arguments[nargs + change])
				);
				break;
		}
	}

	return py_move(&replaced);
}

static PyMethodDef Field_methods[] = {
	{
		"__replace__",
		(PyCFunction)(void (*)(void)) Field_replace,
		METH_FASTCALL | METH_KEYWORDS,
		NULL,
	},
	{.ml_name = NULL},
};

PyTypeObject Field_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix.Field",
	.tp_doc = "A struct field, under dataclasses.Field's attribute names; fields() builds them.",
	.tp_basicsize = sizeof(FieldObject),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
	.tp_dealloc = Field_dealloc,
	.tp_traverse = Field_traverse,
	.tp_clear = Field_clear,
	.tp_repr = Field_repr,
	.tp_members = Field_members,
	.tp_methods = Field_methods,
};

static PyObject * Missing_repr(PyObject * const self) {
	return PyUnicode_FromString("MISSING");
}

static PyObject * Missing_reduce(PyObject * const self, PyObject * const unused) {
	return PyUnicode_FromString("MISSING");
}

static PyMethodDef Missing_methods[] = {
	{"__reduce__", Missing_reduce, METH_NOARGS, NULL},
	{.ml_name = NULL},
};

PyTypeObject Missing_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._MissingType",
	.tp_basicsize = sizeof(PyObject),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
	.tp_repr = Missing_repr,
	.tp_methods = Missing_methods,
};

enum result introspection_install(PyObject * const module, struct salix_state * const state) {
	if (PyType_Ready(&Field_Type) < 0 || PyType_Ready(&Missing_Type) < 0) {
		return RESULT_ERROR;
	}

	PY_OWNED(empty, PyDict_New());

	if (empty == NULL) {
		return RESULT_ERROR;
	}

	Py_XSETREF(state->missing, PyObject_New(PyObject, &Missing_Type));
	Py_XSETREF(state->empty_metadata, PyDictProxy_New(empty));

	return (
		(
			state->missing == NULL ||
			state->empty_metadata == NULL ||
			PyModule_AddObjectRef(module, "Field", (PyObject *) &Field_Type) < 0 ||
			PyModule_AddObjectRef(module, "MISSING", state->missing) < 0
		) ? RESULT_ERROR :
		RESULT_OK
	);
}

static PyObject * field_records_of(StructType const * const type) {
	struct salix_state const * const state = type->struct_state;
	Py_ssize_t const count = PyTuple_GET_SIZE(type->struct_field_names);
	Py_ssize_t const required = count - PyTuple_GET_SIZE(type->struct_defaults);
	PY_MOVABLE(records, PyTuple_New(count));

	if (records == NULL) {
		return NULL;
	}

	for (Py_ssize_t position = 0; position < count; position += 1) {
		PyObject * const record = field_new(
			(PyObject * const[FIELD_ATTRIBUTE_COUNT]){
				[FIELD_NAME] = PyTuple_GET_ITEM(type->struct_field_names, position),
				[FIELD_TYPE] = PyTuple_GET_ITEM(type->struct_annotations, position),
				[FIELD_DEFAULT] = (
					position < required ? state->missing :
					PyTuple_GET_ITEM(type->struct_defaults, position - required)
				),
				[FIELD_DEFAULT_FACTORY] = state->missing,
				[FIELD_INIT] = Py_True,
				[FIELD_REPR] = Py_True,
				[FIELD_HASH] = Py_None,
				[FIELD_COMPARE] = Py_True,
				[FIELD_METADATA] = state->empty_metadata,
				[FIELD_KW_ONLY] = Py_False,
				[FIELD_ANNOTATED_METADATA] = PyTuple_GET_ITEM(type->struct_metadata, position),
			}
		);

		if (record == NULL) {
			return NULL;
		}

		PyTuple_SET_ITEM(records, position, record);
	}

	return py_move(&records);
}

PyObject * Struct_fields(PyObject * const module, PyObject * const class_or_instance) {
	PyObject * const struct_class = (
		is_struct_class(class_or_instance) ? class_or_instance :
		is_struct(class_or_instance) ? (PyObject *) Py_TYPE(class_or_instance) :
		NULL
	);

	if (struct_class == NULL) {
		PyErr_Format(
			PyExc_TypeError,
			PyType_Check(class_or_instance) ?
			"fields() expects a struct class or instance, not the class %.200s" :
			"fields() expects a struct class or instance, not an instance of %.200s",
			PyType_Check(class_or_instance) ? ((PyTypeObject *) class_or_instance)->tp_name :
			Py_TYPE(class_or_instance)->tp_name
		);

		return NULL;
	}

	StructType * const type = (StructType *) struct_class;

	if (!struct_class_created(type)) {
		PyErr_Format(
			PyExc_TypeError,
			"fields() cannot read '%.200s' until its class has finished being created",
			struct_type_name(type)
		);

		return NULL;
	}

	if (type->struct_field_names == NULL) {
		return PyTuple_New(0);
	}

	PyObject * records = NULL;

	STRUCT_BEGIN_CRITICAL_SECTION(type);
	records = (
		type->struct_field_records != NULL ? Py_NewRef(type->struct_field_records) :
		field_records_of(type)
	);

	if (records != NULL && type->struct_field_records == NULL) {
		type->struct_field_records = Py_NewRef(records);
	}

	STRUCT_END_CRITICAL_SECTION();

	return records;
}
