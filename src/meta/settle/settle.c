#include <Python.h>
#include <stdbool.h>
#include <string.h>

#include "../../fields.h"
#include "../meta.h"
#include "../../mixin.h"
#include "../../options.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"
#include "settle.h"

#ifdef TESTING
static bool settle_sweep_refused = false;
#endif

char const * const rebind_comparison[] = {
	"__eq__",
	"__lt__",
	"__le__",
	"__gt__",
	"__ge__",
	NULL,
};
char const * const rebind_not_equal[] = {"__ne__", NULL};
char const * const rebind_representation[] = {"__repr__", NULL};
char const * const rebind_mutability[] = {"__setattr__", "__delattr__", NULL};
char const * const rebind_hash[] = {"__hash__", NULL};
static char const * const rebind_never[] = {"__init__", "__post_init__", "__new__", NULL};

static char const * const * const settle_tables[] = {
	rebind_comparison,
	rebind_not_equal,
	rebind_representation,
	rebind_mutability,
	rebind_hash,
	rebind_never,
	NULL,
};
static void for_each_settle_name(
	void (*visit)(char const * const name, void * const context),
	void * const context
) {
	for (char const * const * const * tables = settle_tables; *tables != NULL; tables += 1) {
		for (char const * const * name = *tables; *name != NULL; name += 1) {
			visit(*name, context);
		}
	}
}

struct sweep_state {
	PyObject * namespace;
	bool failed;
};

static void probe_settle_name(char const * const name, void * const context) {
	struct sweep_state * const state = context;

	if (!state->failed && dict_has_string(state->namespace, name) < 0) {
#ifdef TESTING
		settle_sweep_refused = true;
#endif
		state->failed = true;
	}
}

enum result verify_settle_names_readable(PyObject * const original_namespace) {
	Py_ssize_t position = 0;
	PyObject * key;
	PyObject * value;

	while (PyDict_Next(original_namespace, &position, &key, &value)) {
		if (!PyUnicode_CheckExact(key)) {
			struct sweep_state state = {.namespace = original_namespace, .failed = false};

			for_each_settle_name(probe_settle_name, &state);

			return state.failed ? RESULT_ERROR : RESULT_OK;
		}
	}

	return RESULT_OK;
}

static bool table_names(char const * const * const names, char const * const name) {
	for (char const * const * entry = names; *entry != NULL; entry += 1) {
		if (strcmp(*entry, name) == 0) {
			return true;
		}
	}

	return false;
}

static bool settled_by_the_plan(char const * const name, struct binding_plan const plan) {
	if (table_names(rebind_comparison, name)) {
		return plan.rebind_comparison;
	}

	if (table_names(rebind_not_equal, name)) {
		return plan.rebind_not_equal || plan.answered_by_body;
	}

	if (table_names(rebind_representation, name)) {
		return plan.rebind_representation;
	}

	if (table_names(rebind_mutability, name)) {
		return plan.rebind_mutability;
	}

	if (table_names(rebind_hash, name)) {
		return plan.hash == HASH_BIND || plan.hash == HASH_NONE;
	}

	if (table_names(rebind_never, name)) {
		return false;
	}

	return plan.hash == HASH_BIND || plan.hash == HASH_NONE;
}

static enum result refuse_unplanned(StructType const * const struct_class) {
	PyErr_Format(
		PyExc_TypeError,
		"%.200s.__new__ returned a struct class this call did not plan: its "
		"metaclass is re-entered without the keywords or the class body's "
		"defaults, so what it built is not what was asked for",
		Py_TYPE(struct_class)->tp_name
	);

	return RESULT_ERROR;
}

enum result settle_cache_fill(struct salix_state * const state) {
	static char const * const names[SETTLE_BINDING_COUNT] = {
		"__eq__",
		"__ne__",
		"__lt__",
		"__le__",
		"__gt__",
		"__ge__",
		"__repr__",
	};

	for (Py_ssize_t i = 0; i < SETTLE_BINDING_COUNT; ++i) {
		Py_XSETREF(
			state->mixin_bindings[i],
			PyObject_GetAttrString((PyObject *) &StructMixin_Type, names[i])
		);
		Py_XSETREF(
			state->object_bindings[i],
			PyObject_GetAttrString((PyObject *) &PyBaseObject_Type, names[i])
		);

		if (state->mixin_bindings[i] == NULL || state->object_bindings[i] == NULL) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static int same_parameters(
	StructType const * const struct_class,
	struct field_plan const * const plan
) {
	int const same_init_vars = (
		struct_class->struct_init_var_names != NULL ? PyObject_RichCompareBool(
			struct_class->struct_init_var_names,
			plan->init_var_names,
			Py_EQ
		) :
		0
	);

	if (same_init_vars != 1) {
		return same_init_vars;
	}

	int const same_declared = (
		struct_class->struct_declared_names != NULL ? PyObject_RichCompareBool(
			struct_class->struct_declared_names,
			plan->declared_names,
			Py_EQ
		) :
		0
	);

	if (same_declared != 1) {
		return same_declared;
	}

	if (
		PyTuple_GET_SIZE(plan->init_var_flags) > 0 &&
		PyTuple_GET_SIZE(plan->init_var_flags) != struct_parameter_count(struct_class)
	) {
		return 0;
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(plan->init_var_flags); i += 1) {
		if (
			(PyTuple_GET_ITEM(plan->init_var_flags, i) == Py_True) !=
			(struct_parameter_kind(struct_class, i) == PARAMETER_INIT_VAR)
		) {
			return 0;
		}
	}

	return 1;
}

enum result settle_planned(
	StructType * const struct_class,
	StructType const * const base,
	PyObject * const bases,
	PyObject * const name,
	struct field_plan const * const plan,
	PyObject * const original_namespace,
	struct options const options,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal
) {
	PY_OWNED(planned, PyList_AsTuple(plan->all_names));
	PY_OWNED(planned_parameters, PyList_AsTuple(plan->parameter_names));

	if (planned == NULL || planned_parameters == NULL) {
		return RESULT_ERROR;
	}

	int const same_fields =
		PyObject_RichCompareBool(struct_class->struct_field_names, planned, Py_EQ);

	if (same_fields < 0) {
		return RESULT_ERROR;
	}

	int const same_init_vars = same_parameters(struct_class, plan);

	if (same_init_vars < 0) {
		return RESULT_ERROR;
	}

	PY_OWNED(class_dict, struct_type_dict(&struct_class->heap_type.ht_type));

	if (class_dict == NULL) {
		return RESULT_ERROR;
	}

	PyObject * const built_name = (PyObject *) struct_class->heap_type.ht_name;
	int const same_name = (
		built_name != NULL ? PyObject_RichCompareBool(built_name, name, Py_EQ) :
		0
	);

	if (same_name < 0) {
		return RESULT_ERROR;
	}

	int const same_bases = PyObject_RichCompareBool(
		((PyTypeObject *) struct_class)->tp_bases,
		bases,
		Py_EQ
	);

	if (same_bases < 0) {
		return RESULT_ERROR;
	}

	if (
		same_fields == 0 ||
		same_init_vars == 0 ||
		same_name == 0 ||
		same_bases == 0 ||
		find_struct_base(((PyTypeObject *) struct_class)->tp_bases) != base
	) {
		return refuse_unplanned(struct_class);
	}

	bool const carries_slot = carries_weakref_slot((PyTypeObject *) struct_class);

	if (carries_slot != options.weakref) {
		return refuse_unplanned(struct_class);
	}

	int const defines_hash = dict_has_string(original_namespace, rebind_hash[0]);
	int const defines_setattr = dict_has_string(original_namespace, "__setattr__");

	if (defines_hash < 0 || defines_setattr < 0) {
		return RESULT_ERROR;
	}

	struct binding_plan const bindings = binding_plan(
		options,
		inherited,
		frozen_across_bases,
		any_base_diverts_setattro(bases),
		body_defines_eq,
		inherits_body_eq,
		derive_not_equal,
		defines_hash == 1,
		defines_setattr == 1
	);

	for (char const * const * const * tables = settle_tables; *tables != NULL; tables += 1) {
		for (char const * const * name = *tables; *name != NULL; name += 1) {
			int const in_class = dict_has_string(class_dict, *name);

			if (in_class < 0) {
				return RESULT_ERROR;
			}

			if (in_class == 0) {
				continue;
			}

			int const in_body = dict_has_string(original_namespace, *name);

			if (in_body < 0) {
				return RESULT_ERROR;
			}

			if (in_body == 0 && !settled_by_the_plan(*name, bindings)) {
				return refuse_unplanned(struct_class);
			}
		}
	}

	if (
		restore_stripped(struct_class, original_namespace, class_dict, settle_tables) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_comparison &&
		(
			settle_rebind(
				struct_class,
				original_namespace,
				rebind_comparison,
				options.eq
			) != RESULT_OK
		)
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_not_equal &&
		settle_rebind(struct_class, original_namespace, rebind_not_equal, options.eq) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (bindings.answered_by_body) {
		int const defines_ne = dict_has_string(original_namespace, rebind_not_equal[0]);

		if (defines_ne < 0) {
			return RESULT_ERROR;
		}

		if (
			defines_ne == 0 &&
			settle_rebind(struct_class, original_namespace, rebind_not_equal, false) != RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	if (
		bindings.rebind_representation &&
		(
			settle_rebind(
				struct_class,
				original_namespace,
				rebind_representation,
				options.repr
			) != RESULT_OK
		)
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_mutability &&
		(
			settle_rebind(
				struct_class,
				original_namespace,
				rebind_mutability,
				options.frozen
			) != RESULT_OK
		)
	) {
		return RESULT_ERROR;
	}

	switch (bindings.hash) {
		case HASH_BODY_DEFINED:
		case HASH_INHERITED_EQ:
			break;
		case HASH_NONE: {
			PY_OWNED(hash_name_obj, PyUnicode_FromString(rebind_hash[0]));

			if (
				hash_name_obj == NULL ||
				PyType_Type.tp_setattro((PyObject *) struct_class, hash_name_obj, Py_None) < 0
			) {
				return RESULT_ERROR;
			}

			break;
		}
		case HASH_BIND:
			if (
				settle_rebind(
					struct_class,
					original_namespace,
					rebind_hash,
					options.eq
				) != RESULT_OK
			) {
				return RESULT_ERROR;
			}

			break;
	}

	if (bindings.match_args_wanted) {
		if (PyDict_SetItemString(class_dict, "__match_args__", planned_parameters) < 0) {
			return RESULT_ERROR;
		}
	} else {
		PyObject * const body_match_args = dict_get_string(original_namespace, "__match_args__");

		if (body_match_args != NULL) {
			if (PyDict_SetItemString(class_dict, "__match_args__", body_match_args) < 0) {
				return RESULT_ERROR;
			}
		} else if (PyErr_Occurred()) {
			return RESULT_ERROR;
		} else if (PyDict_DelItemString(class_dict, "__match_args__") < 0) {
			if (PyErr_ExceptionMatches(PyExc_KeyError)) {
				PyErr_Clear();
			} else {
				return RESULT_ERROR;
			}
		}
	}

	Py_SETREF(struct_class->struct_defaults, Py_NewRef(plan->defaults));
	Py_SETREF(struct_class->struct_annotations, Py_NewRef(plan->annotations));
	Py_SETREF(struct_class->struct_metadata, Py_NewRef(plan->metadata));
	struct_class->struct_default_count = PyTuple_GET_SIZE(plan->defaults);

	if (install_init_vars(struct_class, plan) != RESULT_OK) {
		return RESULT_ERROR;
	}

	struct_class->struct_options = options;
	struct_class->struct_resolves_body_eq = body_defines_eq || inherits_body_eq;

	return RESULT_OK;
}

#ifdef TESTING

#	include "../../testing.h"

struct hostile_probe_state {
	PyObject * hostile_class;
	PyObject * bases;
	PyObject * class_name;
};

static void probe_a_hostile_settle_name(char const * const name, void * const context) {
	struct hostile_probe_state const * const state = context;

	PY_OWNED(namespace, PyDict_New());
	PY_OWNED(annotations, PyDict_New());
	PY_OWNED(hostile_key, PyObject_CallFunction(state->hostile_class, "s", name));
	TEST_ASSERT_NOT_NULL(namespace);
	TEST_ASSERT_NOT_NULL(annotations);
	TEST_ASSERT_NOT_NULL(hostile_key);

	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItemString(annotations, "x", (PyObject *) &PyLong_Type));
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItemString(namespace, "__annotations__", annotations));
	TEST_ASSERT_EQUAL_INT(0, PyDict_SetItem(namespace, hostile_key, Py_True));

	PY_OWNED(args, PyTuple_Pack(3, state->class_name, state->bases, namespace));
	TEST_ASSERT_NOT_NULL(args);

	settle_sweep_refused = false;
	PY_OWNED(built, PyObject_Call((PyObject *) &StructMeta_Type, args, NULL));
	TEST_ASSERT_NULL(built);

	PyObject * exception_type;
	PyObject * exception_value;
	PyObject * traceback;
	PyErr_Fetch(&exception_type, &exception_value, &traceback);
	TEST_ASSERT_TRUE(PyErr_GivenExceptionMatches(exception_value, PyExc_ValueError));
	TEST_ASSERT_TRUE(settle_sweep_refused);
	Py_XDECREF(exception_type);
	Py_XDECREF(exception_value);
	Py_XDECREF(traceback);
}

static void test_every_settle_name_refuses_a_hostile_key(void) {
	PY_OWNED(
		hostile_class,
		testing_evaluate(
			"class HostileKey(str):\n"
			"    __hash__ = str.__hash__\n"
			"    def __eq__(self, other):\n"
			"        raise ValueError('nope')\n"
			"result = HostileKey\n"
		)
	);
	TEST_ASSERT_NOT_NULL(hostile_class);

	PY_OWNED(salix, PyImport_ImportModule("salix"));
	TEST_ASSERT_NOT_NULL(salix);

	PY_OWNED(struct_base, PyObject_GetAttrString(salix, "Struct"));
	TEST_ASSERT_NOT_NULL(struct_base);

	PY_OWNED(bases, PyTuple_Pack(1, struct_base));
	TEST_ASSERT_NOT_NULL(bases);

	PY_OWNED(class_name, PyUnicode_FromString("Hostile"));
	TEST_ASSERT_NOT_NULL(class_name);

	struct hostile_probe_state const state = {
		.hostile_class = hostile_class,
		.bases = bases,
		.class_name = class_name,
	};

	for_each_settle_name(probe_a_hostile_settle_name, (void *) &state);
}

void class_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_every_settle_name_refuses_a_hostile_key);
}

#endif
