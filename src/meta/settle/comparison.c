#include <Python.h>
#include <stdbool.h>

#include "settle.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"

enum result resolve_dunder(
	PyObject * const dict,
	PyObject * const name,
	PyObject * * const resolved,
	PyTypeObject * const entry,
	PyTypeObject * * const owner
) {
	if (*resolved != NULL) {
		return RESULT_OK;
	}

	PY_MOVABLE(found, dict_value_ref(dict, name));

	if (found == NULL) {
		return PyErr_Occurred() ? RESULT_ERROR : RESULT_OK;
	}

	*resolved = py_move(&found);
	*owner = entry;

	return RESULT_OK;
}

enum result mro_dunders_of(
	PyTypeObject * const type,
	PyObject * * const resolved_eq,
	PyObject * * const resolved_ne,
	PyObject * * const resolved_repr,
	PyTypeObject * * const eq_owner,
	PyTypeObject * * const ne_owner,
	PyTypeObject * * const repr_owner,
	PyObject * * const resolved_lt,
	PyObject * * const resolved_le,
	PyObject * * const resolved_gt,
	PyObject * * const resolved_ge,
	PyTypeObject * * const lt_owner,
	PyTypeObject * * const le_owner,
	PyTypeObject * * const gt_owner,
	PyTypeObject * * const ge_owner
) {
	*resolved_eq = NULL;
	*resolved_ne = NULL;
	*resolved_repr = NULL;
	*resolved_lt = NULL;
	*resolved_le = NULL;
	*resolved_gt = NULL;
	*resolved_ge = NULL;
	*eq_owner = NULL;
	*ne_owner = NULL;
	*repr_owner = NULL;
	*lt_owner = NULL;
	*le_owner = NULL;
	*gt_owner = NULL;
	*ge_owner = NULL;

	PY_OWNED(eq_name, PyUnicode_InternFromString("__eq__"));
	PY_OWNED(ne_name, PyUnicode_InternFromString("__ne__"));
	PY_OWNED(repr_name, PyUnicode_InternFromString("__repr__"));
	PY_OWNED(lt_name, PyUnicode_InternFromString("__lt__"));
	PY_OWNED(le_name, PyUnicode_InternFromString("__le__"));
	PY_OWNED(gt_name, PyUnicode_InternFromString("__gt__"));
	PY_OWNED(ge_name, PyUnicode_InternFromString("__ge__"));

	if (
		eq_name == NULL ||
		ne_name == NULL ||
		repr_name == NULL ||
		lt_name == NULL ||
		le_name == NULL ||
		gt_name == NULL ||
		ge_name == NULL
	) {
		return RESULT_ERROR;
	}

	PyObject * const mro = type->tp_mro;

	/* The ordering names read the class's own dict too: a pre-creation
	 * rebind injected the comparison family there on an eq-option change,
	 * and the honoured-body decision must see that injection rather than
	 * decide behind its back. */
	PyTypeObject * const own = (PyTypeObject *) PyTuple_GET_ITEM(mro, 0);
	PY_OWNED(own_dict, struct_type_dict(own));

	if (own_dict == NULL) {
		return RESULT_ERROR;
	}

	if (
		resolve_dunder(own_dict, lt_name, resolved_lt, own, lt_owner) != RESULT_OK ||
		resolve_dunder(own_dict, le_name, resolved_le, own, le_owner) != RESULT_OK ||
		resolve_dunder(own_dict, gt_name, resolved_gt, own, gt_owner) != RESULT_OK ||
		resolve_dunder(own_dict, ge_name, resolved_ge, own, ge_owner) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		PY_OWNED(dict, struct_type_dict(entry));

		if (dict == NULL) {
			return RESULT_ERROR;
		}

		if (
			resolve_dunder(dict, eq_name, resolved_eq, entry, eq_owner) != RESULT_OK ||
			resolve_dunder(dict, ne_name, resolved_ne, entry, ne_owner) != RESULT_OK ||
			resolve_dunder(dict, repr_name, resolved_repr, entry, repr_owner) != RESULT_OK ||
			resolve_dunder(dict, lt_name, resolved_lt, entry, lt_owner) != RESULT_OK ||
			resolve_dunder(dict, le_name, resolved_le, entry, le_owner) != RESULT_OK ||
			resolve_dunder(dict, gt_name, resolved_gt, entry, gt_owner) != RESULT_OK ||
			resolve_dunder(dict, ge_name, resolved_ge, entry, ge_owner) != RESULT_OK
		) {
			return RESULT_ERROR;
		}

		if (
			*resolved_eq != NULL &&
			*resolved_ne != NULL &&
			*resolved_repr != NULL &&
			*resolved_lt != NULL &&
			*resolved_le != NULL &&
			*resolved_gt != NULL &&
			*resolved_ge != NULL
		) {
			break;
		}
	}

	return RESULT_OK;
}

/* Whether a binding's owner is one the single-base path would have honoured:
 * the first struct base's own MRO chain, or a non-struct base. A later struct
 * base's binding is shadowed by the record. */
bool honoured_owner(PyTypeObject * const owner, PyTypeObject * const first_struct) {
	if (owner == NULL || !is_struct_class((PyObject *) owner)) {
		return true;
	}

	PyObject * const mro = first_struct->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		if ((PyTypeObject *) PyTuple_GET_ITEM(mro, i) == owner) {
			return true;
		}
	}

	return false;
}

/* Whether a comparison dunder the single-base path would honour is user code:
 * one in the class's own body, or one owned by the first struct base's chain
 * or a co-base, that is neither the mixin's nor object's. */
int honours_a_body_comparison(
	PyTypeObject * const type,
	PyTypeObject * const first_struct,
	PyObject * const original_namespace,
	struct salix_state const * const state
) {
	PyObject * const mro = type->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);

		if (i > 0 && !honoured_owner(entry, first_struct)) {
			continue;
		}

		PY_OWNED(dict, i == 0 ? Py_NewRef(original_namespace) : struct_type_dict(entry));

		if (dict == NULL) {
			return -1;
		}

		for (Py_ssize_t name = 0; name < 6; ++name) {
			PyObject * const bound = dict_get_string(
				dict,
				(char const * const[6]){
					"__eq__",
					"__ne__",
					"__lt__",
					"__le__",
					"__gt__",
					"__ge__",
				}[name]
			);

			if (bound == NULL) {
				if (PyErr_Occurred()) {
					return -1;
				}

				continue;
			}

			if (bound != state->mixin_bindings[name] && bound != state->object_bindings[name]) {
				return 1;
			}
		}
	}

	return 0;
}

#ifdef TESTING

#	include "../../testing.h"

static PyObject * comparison_class(void) {
	return testing_evaluate("class C(Struct):\n    x: int = 1\nresult = C\n");
}

static void test_resolve_dunder_answers_each_shape(void) {
	PyObject * const dict = PyDict_New();
	PyObject * const value = PyLong_FromLong(7);
	PyObject * const name = PyUnicode_FromString("__eq__");
	PyTypeObject * owner = NULL;
	PyObject * resolved = NULL;

	TEST_ASSERT_NOT_NULL(dict);
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_NOT_NULL(name);

	TEST_ASSERT_EQUAL_INT(RESULT_OK, resolve_dunder(dict, name, &resolved, Py_TYPE(value), &owner));
	TEST_ASSERT_NULL(resolved);
	TEST_ASSERT_NULL(owner);

	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItemString(dict, "__eq__", value));
	TEST_ASSERT_EQUAL_INT(RESULT_OK, resolve_dunder(dict, name, &resolved, Py_TYPE(value), &owner));
	TEST_ASSERT_EQUAL_PTR(value, resolved);
	TEST_ASSERT_EQUAL_PTR(Py_TYPE(value), owner);

	TEST_ASSERT_EQUAL_INT(RESULT_OK, resolve_dunder(dict, name, &resolved, NULL, &owner));
	TEST_ASSERT_EQUAL_PTR(value, resolved);
	TEST_ASSERT_EQUAL_PTR(Py_TYPE(value), owner);

	Py_DECREF(resolved);
	Py_DECREF(name);
	Py_DECREF(value);
	Py_DECREF(dict);
}

static void test_the_owner_rule_honours_only_the_first_structs_chain(void) {
	PyObject * const cls = comparison_class();
	PyObject * const other = testing_evaluate("class D(Struct):\n    y: int = 2\nresult = D\n");
	PyTypeObject * const type = (PyTypeObject *) cls;

	TEST_ASSERT_TRUE(honoured_owner(type, type));
	TEST_ASSERT_FALSE(honoured_owner((PyTypeObject *) other, type));
	TEST_ASSERT_TRUE(honoured_owner(NULL, type));
	TEST_ASSERT_TRUE(honoured_owner(&PyBaseObject_Type, type));

	Py_DECREF(other);
	Py_DECREF(cls);
}

static void test_the_mro_walk_resolves_the_salix_bindings(void) {
	PyObject * const cls = comparison_class();
	StructType * const type = (StructType *) cls;

	PyObject * resolved_eq = NULL, *resolved_ne = NULL, *resolved_repr = NULL;
	PyObject * resolved_lt = NULL, *resolved_le = NULL, *resolved_gt = NULL, *resolved_ge = NULL;
	PyTypeObject * eq_owner = NULL, *ne_owner = NULL, *repr_owner = NULL;
	PyTypeObject * lt_owner = NULL, *le_owner = NULL, *gt_owner = NULL, *ge_owner = NULL;

	TEST_ASSERT_EQUAL_INT(
		RESULT_OK,
		mro_dunders_of(
			(PyTypeObject *) type,
			&resolved_eq,
			&resolved_ne,
			&resolved_repr,
			&eq_owner,
			&ne_owner,
			&repr_owner,
			&resolved_lt,
			&resolved_le,
			&resolved_gt,
			&resolved_ge,
			&lt_owner,
			&le_owner,
			&gt_owner,
			&ge_owner
		)
	);
	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[0], resolved_eq);
	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[1], resolved_ne);
	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[6], resolved_repr);
	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[2], resolved_lt);
	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[3], resolved_le);
	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[4], resolved_gt);
	TEST_ASSERT_EQUAL_PTR(type->struct_state->mixin_bindings[5], resolved_ge);

	Py_DECREF(resolved_eq);
	Py_DECREF(resolved_ne);
	Py_DECREF(resolved_repr);
	Py_DECREF(resolved_lt);
	Py_DECREF(resolved_le);
	Py_DECREF(resolved_gt);
	Py_DECREF(resolved_ge);
	Py_DECREF(cls);
}

void comparison_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_resolve_dunder_answers_each_shape);
	RUN_TEST(test_the_owner_rule_honours_only_the_first_structs_chain);
	RUN_TEST(test_the_mro_walk_resolves_the_salix_bindings);
}

#endif
