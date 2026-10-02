#include <Python.h>
#include <stdbool.h>

#include "install.h"
#include "../../owned.h"

enum init_owner { INIT_OWNER_NONE, INIT_OWNER_AUTHOR, INIT_OWNER_FIELD_CONSTRUCTOR };

static enum init_owner static_exception_owner(PyTypeObject * const base) {
	if (base->tp_init == PyBaseObject_Type.tp_init) {
		return INIT_OWNER_NONE;
	}

	PyTypeObject * const base_exception = (PyTypeObject *) PyExc_BaseException;
	bool const args_only_init = (base->tp_init == NULL || base->tp_init == base_exception->tp_init);
	bool const group_new =
#if PY_VERSION_HEX >= 0x030B0000
		PyExc_BaseExceptionGroup != NULL &&
		base->tp_new == ((PyTypeObject *) PyExc_BaseExceptionGroup)->tp_new
#else
		false
#endif
		;

	return (args_only_init || group_new) ? INIT_OWNER_FIELD_CONSTRUCTOR : INIT_OWNER_AUTHOR;
}

static enum init_owner base_init_owner(PyTypeObject * const base) {
	PyObject * const dict = base->tp_dict;

	if (dict == NULL) {
		if (PyType_FastSubclass(base, Py_TPFLAGS_BASE_EXC_SUBCLASS)) {
			return static_exception_owner(base);
		}

		if (base->tp_init != PyBaseObject_Type.tp_init) {
			return INIT_OWNER_AUTHOR;
		}

		return INIT_OWNER_NONE;
	}

	if (base->tp_init == PyBaseObject_Type.tp_init) {
		return INIT_OWNER_NONE;
	}

	PY_OWNED(init_value, Py_XNewRef(dict_get_string(dict, "__init__")));

	if (init_value == NULL) {
		if (PyErr_Occurred()) {
			PyErr_Clear();

			return INIT_OWNER_AUTHOR;
		}

		return (
			PyType_FastSubclass(base, Py_TPFLAGS_BASE_EXC_SUBCLASS) ? INIT_OWNER_NONE :
			INIT_OWNER_AUTHOR
		);
	}

	if (PyType_FastSubclass(base, Py_TPFLAGS_BASE_EXC_SUBCLASS)) {
		return (
			(base->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0 ? INIT_OWNER_AUTHOR :
			static_exception_owner(base)
		);
	}

	return INIT_OWNER_AUTHOR;
}

static enum init_owner namespace_init_owner(PyTypeObject * const type, PyObject * const namespace) {
	int const present = dict_has_string(namespace, "__init__");

	if (present < 0) {
		PyErr_Clear();

		return INIT_OWNER_AUTHOR;
	}

	return present == 1 ? INIT_OWNER_AUTHOR : INIT_OWNER_NONE;
}

bool group_family_in_mro(PyTypeObject * const cls) {
#if PY_VERSION_HEX >= 0x030B0000
	PyObject * const mro = cls->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		if (
			PyExc_BaseExceptionGroup != NULL &&
			(PyTypeObject *) PyTuple_GET_ITEM(mro, i) == (PyTypeObject *) PyExc_BaseExceptionGroup
		) {
			return true;
		}
	}
#endif

	return false;
}

bool family_owns_in_mro(PyTypeObject * const cls) {
	PyObject * const mro = cls->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		enum init_owner const owner = base_init_owner(entry);

		if (owner == INIT_OWNER_NONE) {
			continue;
		}

		return (
			(entry->tp_flags & Py_TPFLAGS_HEAPTYPE) == 0 &&
			PyType_FastSubclass(entry, Py_TPFLAGS_BASE_EXC_SUBCLASS) &&
			owner == INIT_OWNER_AUTHOR
		);
	}

	return false;
}

bool defines_own_init(StructType * const struct_class, PyObject * const namespace) {
	PyTypeObject * const type = &struct_class->heap_type.ht_type;

	if (type->tp_init == PyBaseObject_Type.tp_init) {
		return false;
	}

	PyObject * const mro = type->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		enum init_owner const owner = (
			i == 0 && namespace != NULL ? namespace_init_owner(entry, namespace) :
			base_init_owner(entry)
		);

		if (owner != INIT_OWNER_NONE) {
			return owner == INIT_OWNER_AUTHOR;
		}
	}

	return false;
}

#ifdef TESTING

#	include "../../testing.h"

static char const owners_source[] = {
#	embed "../../../tests/c/fixtures/install/owners.py" suffix(, '\0')
};

#	if PY_VERSION_HEX >= 0x030B0000
static char const groups_source[] = {
#		embed "../../../tests/c/fixtures/install/groups.py" suffix(, '\0')
};
#	endif

static void test_a_body_init_owns_the_construction(void) {
	PyObject * const owners = testing_evaluate(owners_source);

	TEST_ASSERT_FALSE(defines_own_init((StructType *) testing_entry(owners, "plain"), NULL));
	TEST_ASSERT_TRUE(defines_own_init((StructType *) testing_entry(owners, "authored"), NULL));

	Py_DECREF(owners);
}

static void test_the_class_body_answers_while_the_type_is_built(void) {
	PyObject * const owners = testing_evaluate(owners_source);

	TEST_ASSERT_FALSE(
		defines_own_init(
			(StructType *) testing_entry(owners, "raised"),
			testing_entry(owners, "empty_namespace")
		)
	);
	TEST_ASSERT_TRUE(
		defines_own_init(
			(StructType *) testing_entry(owners, "raised"),
			testing_entry(owners, "authored_namespace")
		)
	);

	Py_DECREF(owners);
}

static void test_an_exception_family_with_its_own_init_owns_the_construction(void) {
	PyObject * const owners = testing_evaluate(owners_source);

	TEST_ASSERT_TRUE(family_owns_in_mro((PyTypeObject *) testing_entry(owners, "system_failure")));
	TEST_ASSERT_FALSE(family_owns_in_mro((PyTypeObject *) testing_entry(owners, "raised")));
	TEST_ASSERT_FALSE(family_owns_in_mro((PyTypeObject *) testing_entry(owners, "plain")));

	Py_DECREF(owners);
}

#	if PY_VERSION_HEX >= 0x030B0000
static void test_a_group_family_is_found_in_the_mro(void) {
	PyObject * const owners = testing_evaluate(owners_source);
	PyObject * const grouped = testing_evaluate(groups_source);

	TEST_ASSERT_TRUE(group_family_in_mro((PyTypeObject *) grouped));
	TEST_ASSERT_FALSE(group_family_in_mro((PyTypeObject *) testing_entry(owners, "raised")));

	Py_DECREF(grouped);
	Py_DECREF(owners);
}
#	endif

void init_owner_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_body_init_owns_the_construction);
	RUN_TEST(test_the_class_body_answers_while_the_type_is_built);
	RUN_TEST(test_an_exception_family_with_its_own_init_owns_the_construction);
#	if PY_VERSION_HEX >= 0x030B0000
	RUN_TEST(test_a_group_family_is_found_in_the_mro);
#	endif
}

#endif
