#include <Python.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../construct.h"
#include "../fields.h"
#include "meta.h"
#include "../mixin.h"
#include "../options.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

#ifndef Py_TPFLAGS_HAVE_VECTORCALL
#	define Py_TPFLAGS_HAVE_VECTORCALL _Py_TPFLAGS_HAVE_VECTORCALL
#endif

static int StructMeta_traverse(PyObject * self, visitproc visit, void * arg);
static int StructMeta_clear(PyObject * self);
static void StructMeta_dealloc(PyObject * self);
static PyObject * StructMeta_get_field_names(PyObject * self, void * closure);
static PyObject * StructMeta_get_defaults(PyObject * self, void * closure);
static PyGetSetDef StructMeta_getset[];
static PyObject * StructMeta_call(PyObject * self, PyObject * args, PyObject * keywords);
PyTypeObject StructMeta_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._StructMeta",
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
	{.name = NULL},
};

static PyObject * StructMeta_get_field_names(PyObject * const self, void * const closure) {
	return struct_metadata((StructType *) self, STRUCT_FIELD_NAMES);
}

static PyObject * StructMeta_get_defaults(PyObject * const self, void * const closure) {
	return struct_metadata((StructType *) self, STRUCT_DEFAULTS);
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

	return build_struct_class(metatype, base, name, bases, original_namespace, keywords);
}

PyObject * struct_create_root(
	PyObject * const name,
	PyObject * const bases,
	PyObject * const namespace
) {
	return build_struct_class(&StructMeta_Type, NULL, name, bases, namespace, NULL);
}

static PyObject * StructMeta_call(
	PyObject * const self,
	PyObject * const args,
	PyObject * const keywords
) {
	return (
		((PyTypeObject *) self)->tp_vectorcall != NULL ? PyVectorcall_Call(self, args, keywords) :
		PyType_Type.tp_call(self, args, keywords)
	);
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

/* `visit` and `arg` are not free names: Py_VISIT expands to reference both by
 * those exact spellings, so renaming either one stops the macro compiling. */
static int StructMeta_traverse(PyObject * const self, visitproc const visit, void * const arg) {
	StructType * const struct_class = (StructType *) self;

	Py_VISIT(struct_class->struct_field_names);
	Py_VISIT(struct_class->struct_defaults);
	Py_VISIT(struct_class->struct_post_init);

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
	PyMem_Free(struct_class->struct_slot_offsets);
	struct_class->struct_slot_offsets = NULL;
	PyMem_Free(struct_class->struct_member_offsets);
	struct_class->struct_member_offsets = NULL;
	struct_class->struct_member_count = 0;

	return PyType_Type.tp_clear(self);
}

#ifdef TESTING

#	include "../testing.h"

/* find_member reads a PyMemberDef array, which is trivially fabricated -- and
 * the miss is the branch that turns into a RuntimeError nothing else exercises. */
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

void meta_tests(void) {
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_declared_member_yields_its_offset);
	RUN_TEST(test_a_non_ascii_member_yields_its_offset);
	RUN_TEST(test_an_undeclared_member_is_missing);
	RUN_TEST(test_the_search_respects_the_declared_count);
}

#endif

static void StructMeta_dealloc(PyObject * const self) {
	PyObject_GC_UnTrack(self);
	StructMeta_clear(self);
	PyObject_GC_Track(self);
	PyType_Type.tp_dealloc(self);
}
