#include <Python.h>

#include "settle.h"
#include "../../mixin.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"

enum result settle_rebind_one(
	StructType * const struct_class,
	PyObject * const original_namespace,
	char const * const name,
	bool const from_mixin
) {
	int const present = dict_has_string(original_namespace, name);

	if (present < 0) {
		return RESULT_ERROR;
	}

	if (present == 1) {
		return RESULT_OK;
	}

	PyObject * const source = (
		from_mixin ? (PyObject *) &StructMixin_Type :
		(PyObject *) &PyBaseObject_Type
	);
	PY_OWNED(bound, PyObject_GetAttrString(source, name));
	PY_OWNED(unicode_name, PyUnicode_FromString(name));

	if (
		bound == NULL ||
		unicode_name == NULL ||
		PyType_Type.tp_setattro((PyObject *) struct_class, unicode_name, bound) < 0
	) {
		return RESULT_ERROR;
	}

	return RESULT_OK;
}

enum result settle_rebind(
	StructType * const struct_class,
	PyObject * const original_namespace,
	char const * const * const names,
	bool const from_mixin
) {
	for (char const * const * name = names; *name != NULL; name += 1) {
		if (settle_rebind_one(struct_class, original_namespace, * name, from_mixin) != RESULT_OK) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

enum result restore_stripped(
	StructType * const struct_class,
	PyObject * const original_namespace,
	PyObject * const class_dict,
	char const * const * const * const tables
) {
	for (char const * const * const * table = tables; *table != NULL; table += 1) {
		for (char const * const * name = *table; *name != NULL; name += 1) {
			PyObject * const body_value = dict_get_string(original_namespace, *name);

			if (body_value == NULL) {
				if (PyErr_Occurred()) {
					return RESULT_ERROR;
				}

				continue;
			}

			PyObject * const class_value = dict_get_string(class_dict, *name);

			if (class_value == NULL && PyErr_Occurred()) {
				return RESULT_ERROR;
			}

			if (class_value == body_value) {
				continue;
			}

			PY_OWNED(unicode_name, PyUnicode_FromString(*name));

			if (
				unicode_name == NULL ||
				PyType_Type.tp_setattro((PyObject *) struct_class, unicode_name, body_value) < 0
			) {
				return RESULT_ERROR;
			}
		}
	}

	return RESULT_OK;
}

#ifdef TESTING

#	include "../../testing.h"

static PyObject * rebind_class(void) {
	return testing_evaluate("class R(Struct):\n    x: int = 1\nresult = R\n");
}

static void test_a_rebind_writes_the_mixins_binding(void) {
	PyObject * const cls = rebind_class();
	StructType * const type = (StructType *) cls;
	PyObject * const namespace = PyDict_New();
	PyObject * const class_dict = type->heap_type.ht_type.tp_dict;

	TEST_ASSERT_NOT_NULL(namespace);
	TEST_ASSERT_EQUAL_INT(0, dict_has_string(class_dict, "__eq__"));
	TEST_ASSERT_EQUAL_INT(RESULT_OK, settle_rebind_one(type, namespace, "__eq__", true));
	TEST_ASSERT_EQUAL_INT(1, dict_has_string(class_dict, "__eq__"));

	PyObject * const bound = PyObject_GetAttrString(cls, "__eq__");

	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[0], bound);

	Py_DECREF(bound);
	Py_DECREF(namespace);
	Py_DECREF(cls);
}

static void test_a_rebind_leaves_a_body_binding_alone(void) {
	PyObject * const cls = rebind_class();
	StructType * const type = (StructType *) cls;
	PyObject * const namespace = PyDict_New();
	PyObject * const class_dict = type->heap_type.ht_type.tp_dict;
	PyObject * const body_value = PyObject_GetAttrString((PyObject *) &PyBaseObject_Type, "__eq__");

	TEST_ASSERT_NOT_NULL(namespace);
	TEST_ASSERT_NOT_NULL(body_value);
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItemString(namespace, "__eq__", body_value));
	TEST_ASSERT_EQUAL_INT(0, dict_has_string(class_dict, "__eq__"));
	TEST_ASSERT_EQUAL_INT(RESULT_OK, settle_rebind_one(type, namespace, "__eq__", true));
	TEST_ASSERT_EQUAL_INT(0, dict_has_string(class_dict, "__eq__"));

	PyObject * const bound = PyObject_GetAttrString(cls, "__eq__");

	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[0], bound);

	Py_DECREF(bound);
	Py_DECREF(body_value);
	Py_DECREF(namespace);
	Py_DECREF(cls);
}

static void test_a_rebind_from_the_object_source_writes_objects_binding(void) {
	PyObject * const cls = rebind_class();
	StructType * const type = (StructType *) cls;
	PyObject * const namespace = PyDict_New();
	PyObject * const class_dict = type->heap_type.ht_type.tp_dict;

	TEST_ASSERT_NOT_NULL(namespace);
	TEST_ASSERT_EQUAL_INT(0, dict_has_string(class_dict, "__eq__"));
	TEST_ASSERT_EQUAL_INT(RESULT_OK, settle_rebind_one(type, namespace, "__eq__", false));
	TEST_ASSERT_EQUAL_INT(1, dict_has_string(class_dict, "__eq__"));

	PyObject * const bound = PyObject_GetAttrString(cls, "__eq__");

	TEST_ASSERT_EQUAL_PTR(type->struct_state->object_bindings[0], bound);

	Py_DECREF(bound);
	Py_DECREF(namespace);
	Py_DECREF(cls);
}

static void test_a_restore_writes_the_body_value_back(void) {
	PyObject * const cls = rebind_class();
	StructType * const type = (StructType *) cls;
	PyObject * const namespace = PyDict_New();
	PyObject * const body_value = PyObject_GetAttrString((PyObject *) &PyBaseObject_Type, "__eq__");
	char const * const eq_table[] = {"__eq__", NULL};
	char const * const * const tables[] = {eq_table, NULL};

	TEST_ASSERT_NOT_NULL(namespace);
	TEST_ASSERT_NOT_NULL(body_value);
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItemString(namespace, "__eq__", body_value));
	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		restore_stripped(type, namespace, type->heap_type.ht_type.tp_dict, tables)
	);

	PyObject * const bound = PyObject_GetAttrString(cls, "__eq__");

	TEST_ASSERT_EQUAL_PTR(body_value, bound);

	Py_DECREF(bound);
	Py_DECREF(body_value);
	Py_DECREF(namespace);
	Py_DECREF(cls);
}

void restore_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_rebind_writes_the_mixins_binding);
	RUN_TEST(test_a_rebind_leaves_a_body_binding_alone);
	RUN_TEST(test_a_rebind_from_the_object_source_writes_objects_binding);
	RUN_TEST(test_a_restore_writes_the_body_value_back);
}

#endif
