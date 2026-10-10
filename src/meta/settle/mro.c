#include <Python.h>
#include <stdbool.h>

#include "../../compare.h"
#include "../../hash.h"
#include "../meta.h"
#include "../../mixin.h"
#include "../../options.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"
#include "settle.h"

#ifdef TESTING
static PyTypeObject * frozen_column_repair_owner = NULL;
#endif

static int struct_base_count(PyObject * const bases) {
	int count = 0;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		if (is_struct_class(PyTuple_GET_ITEM(bases, i))) {
			count += 1;
		}
	}

	return count;
}

enum result settle_mro_bindings(
	StructType * const struct_class,
	PyObject * const bases,
	PyObject * const original_namespace,
	struct binding_plan const bindings,
	struct options const options
) {
	PyTypeObject * const type = (PyTypeObject *) struct_class;

	if (options.frozen) {
		enum setter_source const assigns = setter_source_of(type, "__setattr__");
		enum setter_source const deletes = setter_source_of(type, "__delattr__");

		if (assigns == SETTER_SOURCE_ERROR || deletes == SETTER_SOURCE_ERROR) {
			return RESULT_ERROR;
		}

		if (assigns != SETTER_SOURCE_STRUCT || deletes != SETTER_SOURCE_STRUCT) {
#ifdef TESTING
			frozen_column_repair_owner = type;
#endif
			if (
				settle_rebind(
					struct_class,
					original_namespace,
					rebind_mutability,
					true
				) != RESULT_OK
			) {
				return RESULT_ERROR;
			}
		}
	}

	if (struct_base_count(bases) <= 1) {
		return RESULT_OK;
	}

	PyTypeObject * const first_struct = (PyTypeObject *) find_behaviour_base(bases);
	struct salix_state * const state = struct_class->struct_state;

	PY_MOVABLE(resolved_eq, NULL);
	PY_MOVABLE(resolved_ne, NULL);
	PY_MOVABLE(resolved_repr, NULL);
	PY_MOVABLE(resolved_lt, NULL);
	PY_MOVABLE(resolved_le, NULL);
	PY_MOVABLE(resolved_gt, NULL);
	PY_MOVABLE(resolved_ge, NULL);
	PyTypeObject * eq_owner = NULL;
	PyTypeObject * ne_owner = NULL;
	PyTypeObject * repr_owner = NULL;
	PyTypeObject * lt_owner = NULL;
	PyTypeObject * le_owner = NULL;
	PyTypeObject * gt_owner = NULL;
	PyTypeObject * ge_owner = NULL;

	if (
		mro_dunders_of(
			type,
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
		) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	int const body_answers = honours_a_body_comparison(
		type,
		first_struct,
		original_namespace,
		state
	);

	if (body_answers < 0) {
		return RESULT_ERROR;
	}

	if (!body_answers && options.eq && type->tp_richcompare != Struct_rich_compare) {
		type->tp_richcompare = Struct_rich_compare;
	}

	PyObject * const target_eq = options.eq ? state->mixin_bindings[0] : state->object_bindings[0];
	bool const eq_is_salix_owned = (
		resolved_eq == state->mixin_bindings[0] ||
		resolved_eq == state->object_bindings[0]
	);

	if (
		resolved_eq != target_eq &&
		(eq_is_salix_owned || !honoured_owner(eq_owner, first_struct))
	) {
		if (
			settle_rebind_one(struct_class, original_namespace, "__eq__", options.eq) != RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	struct ordering_binding {
		char const * name;
		Py_ssize_t slot;
		PyObject * resolved;
		PyTypeObject * owner;
	};
	struct ordering_binding const orderings[4] = {
		{"__lt__", 2, resolved_lt, lt_owner},
		{"__le__", 3, resolved_le, le_owner},
		{"__gt__", 4, resolved_gt, gt_owner},
		{"__ge__", 5, resolved_ge, ge_owner},
	};

	for (Py_ssize_t i = 0; i < 4; ++i) {
		Py_ssize_t const slot = orderings[i].slot;
		PyObject * const target = (
			options.eq ? state->mixin_bindings[slot] :
			state->object_bindings[slot]
		);
		PyObject * const resolved_i = orderings[i].resolved;
		PyTypeObject * const owner_i = orderings[i].owner;
		bool const salix_owned = (
			resolved_i == state->mixin_bindings[slot] ||
			resolved_i == state->object_bindings[slot]
		);

		if (resolved_i != target && (salix_owned || !honoured_owner(owner_i, first_struct))) {
			if (
				settle_rebind_one(
					struct_class,
					original_namespace,
					orderings[i].name,
					options.eq
				) != RESULT_OK
			) {
				return RESULT_ERROR;
			}
		}
	}

	bool const body_eq_answers = !eq_is_salix_owned && honoured_owner(eq_owner, first_struct);

	PyObject * const target_ne = (
		body_eq_answers ? state->object_bindings[1] :
		options.eq ? state->mixin_bindings[1] :
		state->object_bindings[1]
	);
	bool const ne_is_salix_owned = (
		resolved_ne == state->mixin_bindings[1] ||
		resolved_ne == state->object_bindings[1]
	);

	if (
		resolved_ne != target_ne &&
		(ne_is_salix_owned || !honoured_owner(ne_owner, first_struct))
	) {
		if (
			settle_rebind(
				struct_class,
				original_namespace,
				rebind_not_equal,
				!body_eq_answers && options.eq
			) != RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	PyObject * const target_repr = (
		options.repr ? state->mixin_bindings[6] :
		state->object_bindings[6]
	);
	bool const repr_is_salix_owned = (
		resolved_repr == state->mixin_bindings[6] ||
		resolved_repr == state->object_bindings[6]
	);

	if (
		resolved_repr != target_repr &&
		(repr_is_salix_owned || !honoured_owner(repr_owner, first_struct))
	) {
		if (
			settle_rebind(
				struct_class,
				original_namespace,
				rebind_representation,
				options.repr
			) != RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	if (body_eq_answers && bindings.hash == HASH_BIND) {
		PY_OWNED(class_dict, struct_type_dict(type));

		if (class_dict == NULL || PyDict_SetItemString(class_dict, "__hash__", Py_None) < 0) {
			return RESULT_ERROR;
		}

		type->tp_hash = PyObject_HashNotImplemented;
	} else if (options.eq && bindings.hash == HASH_BIND && type->tp_hash != Struct_hash) {
		type->tp_hash = Struct_hash;
	}

	return RESULT_OK;
}

#ifdef TESTING

#	include "../../testing.h"

static char const bases_source[] = {
#	embed "../../../tests/c/fixtures/settle/bases.py" suffix(, '\0')
};

static Py_ssize_t swallow_calls = 0;

static int swallowing_setattro(PyObject * self, PyObject * name, PyObject * value) {
	swallow_calls += 1;

	return 0;
}

static PyTypeObject SwallowingType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "tests.Swallowing",
	.tp_basicsize = sizeof(PyObject),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_setattro = swallowing_setattro,
};

static PyObject * struct_class_with_field(
	PyObject * bases,
	PyObject * keywords,
	bool const body_setattr
) {
	PY_OWNED(name, PyUnicode_FromString("Built"));
	PY_OWNED(namespace, PyDict_New());
	PY_OWNED(annotations, PyDict_New());

	if (
		PyDict_SetItemString(annotations, "x", (PyObject *) &PyLong_Type) < 0 ||
		PyDict_SetItemString(namespace, "__annotations__", annotations) < 0 ||
		(body_setattr && PyDict_SetItemString(namespace, "__setattr__", Py_None) < 0)
	) {
		return NULL;
	}

	PY_OWNED(args, PyTuple_Pack(3, name, bases, namespace));

	return PyObject_Call((PyObject *) &StructMeta_Type, args, keywords);
}

static void test_a_raw_tp_setattro_co_base_does_not_divert_the_struct_slot(void) {
	TEST_ASSERT_EQUAL_INT(0, PyType_Ready(&SwallowingType));

	PY_OWNED(salix, PyImport_ImportModule("salix"));
	TEST_ASSERT_NOT_NULL(salix);

	PY_OWNED(struct_base, PyObject_GetAttrString(salix, "Struct"));
	TEST_ASSERT_NOT_NULL(struct_base);

	PY_OWNED(struct_bases, PyTuple_Pack(1, struct_base));
	PY_OWNED(mutable_keywords, PyDict_New());
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItemString(mutable_keywords, "frozen", Py_False));

	PY_OWNED(mutable_base, struct_class_with_field(struct_bases, mutable_keywords, false));
	TEST_ASSERT_NOT_NULL(mutable_base);

	PY_OWNED(raw_bases, PyTuple_Pack(2, (PyObject *) &SwallowingType, mutable_base));
	PY_OWNED(mutable_child, struct_class_with_field(raw_bases, NULL, false));
	TEST_ASSERT_NOT_NULL(mutable_child);
	TEST_ASSERT_EQUAL_INT(
		0,
		dict_has_string(((PyTypeObject *) mutable_child)->tp_dict, "__setattr__")
	);

	PY_OWNED(instance, PyObject_CallFunction(mutable_child, "i", 1));
	TEST_ASSERT_NOT_NULL(instance);
	PY_OWNED(nine, PyLong_FromLong(9));
	PY_OWNED(field_name, PyUnicode_FromString("x"));
	TEST_ASSERT_EQUAL_INT(0, PyObject_SetAttr(instance, field_name, nine));
	TEST_ASSERT_EQUAL_INT(1, swallow_calls);
	TEST_ASSERT_EQUAL_INT(0, PyObject_DelAttr(instance, field_name));
	TEST_ASSERT_EQUAL_INT(2, swallow_calls);

	PY_OWNED(frozen_base, struct_class_with_field(struct_bases, NULL, false));
	TEST_ASSERT_NOT_NULL(frozen_base);

	frozen_column_repair_owner = NULL;
	PY_OWNED(frozen_bases, PyTuple_Pack(2, (PyObject *) &SwallowingType, frozen_base));
	PY_OWNED(frozen_child, struct_class_with_field(frozen_bases, NULL, false));
	TEST_ASSERT_NOT_NULL(frozen_child);
	TEST_ASSERT_EQUAL_PTR(NULL, frozen_column_repair_owner);
	TEST_ASSERT_EQUAL_INT(
		SETTER_SOURCE_STRUCT,
		setter_source_of((PyTypeObject *) frozen_child, "__setattr__")
	);
	TEST_ASSERT_EQUAL_INT(
		1,
		dict_has_string(((PyTypeObject *) frozen_child)->tp_dict, "__setattr__")
	);

	frozen_column_repair_owner = NULL;
	PY_OWNED(escaped_child, struct_class_with_field(frozen_bases, NULL, true));
	TEST_ASSERT_NOT_NULL(escaped_child);
	TEST_ASSERT_EQUAL_PTR((PyObject *) escaped_child, (PyObject *) frozen_column_repair_owner);

	PY_OWNED(escaped_instance, PyObject_CallFunction(escaped_child, "i", 1));
	TEST_ASSERT_NOT_NULL(escaped_instance);
	TEST_ASSERT_EQUAL_INT(-1, PyObject_DelAttr(escaped_instance, field_name));
	PyErr_Clear();

	PY_OWNED(frozen_instance, PyObject_CallFunction(frozen_child, "i", 1));
	TEST_ASSERT_NOT_NULL(frozen_instance);
	TEST_ASSERT_EQUAL_INT(-1, PyObject_SetAttr(frozen_instance, field_name, nine));
	PyErr_Clear();
	TEST_ASSERT_EQUAL_INT(-1, PyObject_DelAttr(frozen_instance, field_name));
	PyErr_Clear();
	TEST_ASSERT_EQUAL_INT(2, swallow_calls);
}

static PyObject * struct_class_empty(PyObject * bases, PyObject * keywords) {
	PY_OWNED(name, PyUnicode_FromString("Built"));
	PY_OWNED(namespace, PyDict_New());

	if (name == NULL || namespace == NULL) {
		return NULL;
	}

	PY_OWNED(args, PyTuple_Pack(3, name, bases, namespace));

	if (args == NULL) {
		return NULL;
	}

	return PyObject_Call((PyObject *) &StructMeta_Type, args, keywords);
}

static void test_a_later_bases_slot_forces_the_record(void) {
	PY_OWNED(salix, PyImport_ImportModule("salix"));
	TEST_ASSERT_NOT_NULL(salix);

	PY_OWNED(struct_base, PyObject_GetAttrString(salix, "Struct"));
	TEST_ASSERT_NOT_NULL(struct_base);

	PY_OWNED(struct_bases, PyTuple_Pack(1, struct_base));
	TEST_ASSERT_NOT_NULL(struct_bases);

	PY_OWNED(weak_keywords, PyDict_New());
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItemString(weak_keywords, "weakref", Py_True));

	PY_OWNED(weak_base, struct_class_empty(struct_bases, weak_keywords));
	TEST_ASSERT_NOT_NULL(weak_base);

	PY_OWNED(plain_base, struct_class_with_field(struct_bases, NULL, false));
	TEST_ASSERT_NOT_NULL(plain_base);

	PY_OWNED(mixed_bases, PyTuple_Pack(2, plain_base, weak_base));
	TEST_ASSERT_NOT_NULL(mixed_bases);

	PY_OWNED(mixed_child, struct_class_empty(mixed_bases, NULL));
	TEST_ASSERT_NOT_NULL(mixed_child);

	TEST_ASSERT_TRUE(((StructType *) mixed_child)->struct_options.weakref);
	TEST_ASSERT_TRUE(carries_weakref_slot((PyTypeObject *) mixed_child));
}

static void test_only_struct_bases_are_counted(void) {
	PY_OWNED(bases, testing_evaluate(bases_source));

	TEST_ASSERT_EQUAL_INT(2, struct_base_count(testing_entry(bases, "two_structs")));
	TEST_ASSERT_EQUAL_INT(1, struct_base_count(testing_entry(bases, "one_struct")));
	TEST_ASSERT_EQUAL_INT(0, struct_base_count(testing_entry(bases, "no_struct")));
}

static void test_the_repair_runs_only_past_one_struct_base(void) {
	PY_OWNED(bases, testing_evaluate(bases_source));

	TEST_ASSERT_EQUAL_INT(
		1,
		dict_has_string(((PyTypeObject *) testing_entry(bases, "both"))->tp_dict, "__eq__")
	);
	TEST_ASSERT_EQUAL_INT(
		0,
		PyObject_RichCompareBool(
			PyTuple_GET_ITEM(testing_entry(bases, "both_pair"), 0),
			PyTuple_GET_ITEM(testing_entry(bases, "both_pair"), 1),
			Py_EQ
		)
	);
	TEST_ASSERT_EQUAL_INT(
		0,
		dict_has_string(((PyTypeObject *) testing_entry(bases, "only"))->tp_dict, "__eq__")
	);
	TEST_ASSERT_EQUAL_INT(
		1,
		PyObject_RichCompareBool(
			PyTuple_GET_ITEM(testing_entry(bases, "only_pair"), 0),
			PyTuple_GET_ITEM(testing_entry(bases, "only_pair"), 1),
			Py_EQ
		)
	);
}

void mro_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_raw_tp_setattro_co_base_does_not_divert_the_struct_slot);
	RUN_TEST(test_a_later_bases_slot_forces_the_record);
	RUN_TEST(test_only_struct_bases_are_counted);
	RUN_TEST(test_the_repair_runs_only_past_one_struct_base);
}

#endif
