#include <Python.h>
#include <stddef.h>
#include <string.h>

#include "meta.h"
#include "../mixin.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static int StructMeta_traverse(PyObject * self, visitproc visit, void * arg);
static int StructMeta_clear(PyObject * self);
static void StructMeta_dealloc(PyObject * self);
static PyObject * StructMeta_get_field_names(PyObject * self, void * closure);
static PyObject * StructMeta_get_defaults(PyObject * self, void * closure);
static PyObject * StructMeta_get_annotations(PyObject * self, void * closure);
static PyObject * StructMeta_get_metadata(PyObject * self, void * closure);
static PyGetSetDef StructMeta_getset[10];
static PyObject * StructMeta_call(PyObject * self, PyObject * args, PyObject * keywords);
static int StructMeta_setattro(PyObject * self, PyObject * name, PyObject * value);
PyTypeObject StructMeta_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._StructMeta",
	.tp_doc = "The metaclass of struct classes; use the Struct base to build one.",
	.tp_basicsize = sizeof(StructType),
	.tp_itemsize = sizeof(struct PyMemberDef),
	.tp_flags = (
		Py_TPFLAGS_DEFAULT |
		Py_TPFLAGS_TYPE_SUBCLASS |
		Py_TPFLAGS_HAVE_GC |
		Py_TPFLAGS_HAVE_VECTORCALL |
		Py_TPFLAGS_BASETYPE
	),
	.tp_new = StructMeta_new,
	.tp_dealloc = StructMeta_dealloc,
	.tp_traverse = StructMeta_traverse,
	.tp_clear = StructMeta_clear,
	.tp_call = StructMeta_call,
	.tp_setattro = StructMeta_setattro,
	.tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),
	.tp_getset = StructMeta_getset,
};

static PyGetSetDef StructMeta_getset[] = {
	{
		.name = "_struct_fields_",
		.get = StructMeta_get_field_names,
		.doc = "tuple of field names",
	},
	{
		.name = "_struct_defaults_",
		.get = StructMeta_get_defaults,
		.doc = "tuple of trailing defaults",
	},
	{
		.name = "__struct_fields__",
		.get = StructMeta_get_field_names,
		.doc = "tuple of field names, under msgspec's name for it",
	},
	{
		.name = "__struct_defaults__",
		.get = StructMeta_get_defaults,
		.doc = "tuple of trailing defaults, under msgspec's name for it",
	},
	{
		.name = "_struct_annotations_",
		.get = StructMeta_get_annotations,
		.doc = "the field annotations, aligned with the fields",
	},
	{
		.name = "__struct_annotations__",
		.get = StructMeta_get_annotations,
		.doc = "the field annotations under the public name for it",
	},
	{
		.name = "_struct_metadata_",
		.get = StructMeta_get_metadata,
		.doc = "the Annotated extras per field, aligned with the fields",
	},
	{
		.name = "__struct_metadata__",
		.get = StructMeta_get_metadata,
		.doc = "the Annotated extras under the public name for it",
	},
	{
		.name = "__signature__",
		.get = Struct_get_signature,
		.set = Struct_set_signature,
		.doc = "the constructor signature inspect.signature reads",
	},
	{.name = NULL},
};

static PyObject * StructMeta_get_field_names(PyObject * const self, void * const closure) {
	return struct_metadata((StructType *) self, STRUCT_FIELD_NAMES);
}

static PyObject * StructMeta_get_defaults(PyObject * const self, void * const closure) {
	return struct_metadata((StructType *) self, STRUCT_DEFAULTS);
}

static PyObject * StructMeta_get_annotations(PyObject * const self, void * const closure) {
	return struct_metadata((StructType *) self, STRUCT_ANNOTATIONS);
}

static PyObject * StructMeta_get_metadata(PyObject * const self, void * const closure) {
	return struct_metadata((StructType *) self, STRUCT_METADATA);
}

PyObject * StructMeta_new(
	PyTypeObject * const metatype,
	PyObject * const args,
	PyObject * const keywords
) {
	PyObject * name;
	PyObject * bases;
	PyObject * original_namespace;

	if (
		!PyArg_ParseTuple(
			args,
			"UO!O!:_StructMeta.__new__",
			&name,
			&PyTuple_Type,
			&bases,
			&PyDict_Type,
			&original_namespace
		)
	) {
		return NULL;
	}

	StructType const * const base = find_struct_base(bases);

	if (base == NULL) {
		PyErr_SetString(
			PyExc_TypeError,
			"a struct class inherits salix.Struct; the metaclass of one is not a "
			"way to make one"
		);

		return NULL;
	}

	return build_struct_class(
		metatype,
		base,
		name,
		bases,
		original_namespace,
		keywords,
		base->struct_state
	);
}

PyObject * struct_create_root(
	PyObject * const name,
	PyObject * const bases,
	PyObject * const namespace,
	PyObject * const module
) {
	struct salix_state * const state = (struct salix_state *) PyModule_GetState(module);

	if (state == NULL) {
		return NULL;
	}

	PyObject * const root = build_struct_class(
		&StructMeta_Type,
		NULL,
		name,
		bases,
		namespace,
		NULL,
		state
	);

	if (root == NULL) {
		return NULL;
	}

	((StructType *) root)->struct_state = (struct salix_state *) PyModule_GetState(module);
	Py_XSETREF(((PyHeapTypeObject *) root)->ht_module, Py_NewRef(module));

	return root;
}

static PyObject * StructMeta_call(
	PyObject * const self,
	PyObject * const args,
	PyObject * const keywords
) {
	PyTypeObject * const type = (PyTypeObject *) self;

	if (((StructType *) self)->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", type->tp_name);

		return NULL;
	}

	PY_MOVABLE(result, type->tp_vectorcall != NULL ? PyVectorcall_Call(self, args, keywords) :
		PyType_Type.tp_call(self, args, keywords));

	if (result != NULL && !PyObject_TypeCheck(result, type)) {
		PyErr_Format(
			PyExc_TypeError,
			"%s.__new__(%s) is not safe, use %s.__new__()",
			Py_TYPE(result)->tp_name,
			type->tp_name,
			type->tp_name
		);
		Py_CLEAR(result);
	}

	return py_move(&result);
}

struct member_lookup find_member(
	struct PyMemberDef const * const members,
	Py_ssize_t const member_count,
	PyObject * const name
) {
	Py_ssize_t name_size = 0;
	char const * const encoded_name = PyUnicode_AsUTF8AndSize(name, &name_size);

	if (encoded_name == NULL) {
		return (struct member_lookup){.tag = MEMBER_LOOKUP_ERROR};
	}

	for (Py_ssize_t i = 0; i < member_count; ++i) {
		size_t const member_size = strlen(members[i].name);

		if (
			name_size == (Py_ssize_t) member_size &&
			memcmp(encoded_name, members[i].name, member_size) == 0
		) {
			return (struct member_lookup){
				.tag = MEMBER_LOOKUP_FOUND,
				.slot_offset = members[i].offset,
			};
		}
	}

	return (struct member_lookup){.tag = MEMBER_LOOKUP_MISSING};
}

static int StructMeta_traverse(PyObject * const self, visitproc const visit, void * const arg) {
	StructType * const struct_class = (StructType *) self;

	Py_VISIT(struct_class->struct_field_names);
	Py_VISIT(struct_class->struct_defaults);
	Py_VISIT(struct_class->struct_annotations);
	Py_VISIT(struct_class->struct_metadata);
	Py_VISIT(struct_class->struct_post_init);
	Py_VISIT(struct_class->struct_singleton);
	Py_VISIT(struct_class->struct_signature);
	Py_VISIT(struct_class->struct_init_var_names);
	Py_VISIT(struct_class->struct_init_var_defaults);
	Py_VISIT(struct_class->struct_init_var_annotations);
	Py_VISIT(struct_class->struct_declared_names);
	Py_VISIT(struct_class->struct_class_var_positions);

	return PyType_Type.tp_traverse(self, visit, arg);
}

static int StructMeta_clear(PyObject * const self) {
	StructType * const struct_class = (StructType *) self;

	Py_CLEAR(struct_class->struct_post_init);

	if (struct_class->struct_field_names == NULL) {
		return RESULT_OK;
	}

	Py_CLEAR(struct_class->struct_field_names);
	Py_CLEAR(struct_class->struct_defaults);
	Py_CLEAR(struct_class->struct_annotations);
	Py_CLEAR(struct_class->struct_metadata);
	Py_CLEAR(struct_class->struct_singleton);
	Py_CLEAR(struct_class->struct_signature);
	Py_CLEAR(struct_class->struct_init_var_names);
	Py_CLEAR(struct_class->struct_init_var_defaults);
	Py_CLEAR(struct_class->struct_init_var_annotations);
	Py_CLEAR(struct_class->struct_declared_names);
	Py_CLEAR(struct_class->struct_class_var_positions);
	PyMem_Free(struct_class->struct_parameter_kinds);
	struct_class->struct_parameter_kinds = NULL;
	struct_class->struct_init_var_count = 0;
	PyMem_Free(struct_class->struct_slot_offsets);
	struct_class->struct_slot_offsets = NULL;
	PyMem_Free(struct_class->struct_member_offsets);
	struct_class->struct_member_offsets = NULL;
	struct_class->struct_member_count = 0;

	return PyType_Type.tp_clear(self);
}

#ifdef TESTING

#	include "../testing.h"
#	include "../fields.h"
#	include "../mixin.h"

static struct PyMemberDef const example_members[] = {
	{.name = "alpha", .offset = 16},
	{.name = "beta", .offset = 24},
	{.name = "café", .offset = 32},
	{.name = NULL},
};

static void test_a_declared_member_yields_its_offset(void) {
	PyObject * const name = PyUnicode_FromString("beta");
	struct member_lookup const found = find_member(example_members, 2, name);

	TEST_ASSERT_EQUAL_INT(MEMBER_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(24, found.slot_offset);

	Py_DECREF(name);
}

static void test_a_non_ascii_member_yields_its_offset(void) {
	PyObject * const name = PyUnicode_FromString("café");
	struct member_lookup const found = find_member(example_members, 3, name);

	TEST_ASSERT_EQUAL_INT(MEMBER_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(32, found.slot_offset);

	Py_DECREF(name);
}

static void test_an_undeclared_member_is_missing(void) {
	PyObject * const name = PyUnicode_FromString("gamma");
	struct member_lookup const found = find_member(example_members, 3, name);

	TEST_ASSERT_EQUAL_INT(MEMBER_LOOKUP_MISSING, found.tag);

	Py_DECREF(name);
}

static void test_the_search_respects_the_declared_count(void) {
	PyObject * const name = PyUnicode_FromString("beta");
	struct member_lookup const found = find_member(example_members, 1, name);

	TEST_ASSERT_EQUAL_INT(MEMBER_LOOKUP_MISSING, found.tag);

	Py_DECREF(name);
}

static PyGetSetDef const * getset_named(
	PyGetSetDef const * const getsets,
	char const * const name
) {
	for (PyGetSetDef const * entry = getsets; entry->name != NULL; ++entry) {
		if (strcmp(entry->name, name) == 0) {
			return entry;
		}
	}

	return NULL;
}

static bool reserved_set_contains(char const * const name) {
	for (char const * const * reserved = reserved_metadata_names; *reserved != NULL; ++reserved) {
		if (strcmp(*reserved, name) == 0) {
			return true;
		}
	}

	return false;
}

static void test_the_reservation_and_the_getset_tables_agree(void) {
	for (PyGetSetDef const * entry = StructMeta_Type.tp_getset; entry->name != NULL; ++entry) {
		if (strcmp(entry->name, "__signature__") == 0) {
			continue;
		}

		TEST_ASSERT_TRUE(reserved_set_contains(entry->name));
	}

	for (PyGetSetDef const * entry = StructMixin_Type.tp_getset; entry->name != NULL; ++entry) {
		if (strcmp(entry->name, "__signature__") == 0) {
			continue;
		}

		TEST_ASSERT_TRUE(reserved_set_contains(entry->name));
	}

	for (char const * const * reserved = reserved_metadata_names; *reserved != NULL; ++reserved) {
		TEST_ASSERT_NOT_NULL(getset_named(StructMeta_Type.tp_getset, *reserved));
		TEST_ASSERT_NOT_NULL(getset_named(StructMixin_Type.tp_getset, *reserved));
	}
}

void meta_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_declared_member_yields_its_offset);
	RUN_TEST(test_a_non_ascii_member_yields_its_offset);
	RUN_TEST(test_an_undeclared_member_is_missing);
	RUN_TEST(test_the_search_respects_the_declared_count);
	RUN_TEST(test_the_reservation_and_the_getset_tables_agree);
}

#endif

enum class_name_kind { CLASS_NAME_FREE, CLASS_NAME_FIELD, CLASS_NAME_CLASS_VAR };

static enum class_name_kind class_name_kind_of(
	StructType const * const type,
	PyObject * const name
) {
	PyObject * const field_names = type->struct_field_names;
	PyObject * const class_var_positions = type->struct_class_var_positions;

	for (Py_ssize_t i = 0; field_names != NULL && i < PyTuple_GET_SIZE(field_names); i += 1) {
		if (PyUnicode_Compare(name, PyTuple_GET_ITEM(field_names, i)) == 0) {
			return CLASS_NAME_FIELD;
		}
	}

	for (
		Py_ssize_t i = 0;
		class_var_positions != NULL && i < PyTuple_GET_SIZE(class_var_positions);
		i += 1
	) {
		PyObject * const class_var_name = PyTuple_GET_ITEM(
			PyTuple_GET_ITEM(class_var_positions, i),
			1
		);

		if (PyUnicode_Compare(name, class_var_name) == 0) {
			return CLASS_NAME_CLASS_VAR;
		}
	}

	return CLASS_NAME_FREE;
}

static int StructMeta_setattro(
	PyObject * const self,
	PyObject * const name,
	PyObject * const value
) {
	enum class_name_kind const kind = (
		PyUnicode_Check(name) ? class_name_kind_of((StructType const *) self, name) :
		CLASS_NAME_FREE
	);

	switch (kind) {
		case CLASS_NAME_FREE:
			return PyType_Type.tp_setattro(self, name, value);
		case CLASS_NAME_FIELD:
		case CLASS_NAME_CLASS_VAR:
			break;
	}

	PyErr_Format(
		PyExc_TypeError,
		"cannot %s '%U' attribute of struct class '%.200s': %s",
		value == NULL ? "delete" : "set",
		name,
		((PyTypeObject *) self)->tp_name,
		kind == CLASS_NAME_FIELD ? "it is a field, and the class attribute is the field's slot" :
		"it is a ClassVar, and a class variable is a constant"
	);

	return -1;
}

static void StructMeta_dealloc(PyObject * const self) {
	PyObject_GC_UnTrack(self);
	StructMeta_clear(self);
	PyObject_GC_Track(self);
	PyType_Type.tp_dealloc(self);
}
