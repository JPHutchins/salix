#include <Python.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "construct.h"
#include "fields.h"
#include "meta.h"
#include "mixin.h"
#include "options.h"
#include "owned.h"
#include "result.h"
#include "types.h"

#ifndef Py_TPFLAGS_HAVE_VECTORCALL
#	define Py_TPFLAGS_HAVE_VECTORCALL _Py_TPFLAGS_HAVE_VECTORCALL
#endif

struct member_lookup {
	enum { MEMBER_LOOKUP_FOUND, MEMBER_LOOKUP_MISSING, MEMBER_LOOKUP_ERROR } tag;
	Py_ssize_t slot_offset;
};

struct definition {
	enum { DEFINITION_READ, DEFINITION_UNREADABLE } tag;
	bool found;
};

struct equality_source {
	enum { EQUALITY_RESOLVED, EQUALITY_FAILED } tag;
	bool from_a_body;
	bool needs_derived_not_equal;
};

static int StructMeta_traverse(PyObject * self, visitproc visit, void * arg);
static int StructMeta_clear(PyObject * self);
static void StructMeta_dealloc(PyObject * self);

static PyObject * build_struct_class(
	PyTypeObject * metatype,
	StructType const * base,
	PyObject * name,
	PyObject * bases,
	PyObject * original_namespace,
	PyObject * keywords
);
static StructType * find_struct_base(PyObject * bases);
static StructType * find_behaviour_base(PyObject * bases);
static struct equality_source resolves_body_equality(PyObject * bases);
static struct equality_source equality_from_the_co_bases(PyObject * bases, Py_ssize_t first);
static struct definition base_defines(PyObject * base, PyObject * name);
static struct definition any_base_defines(PyObject * bases, Py_ssize_t first, PyObject * name);
static struct options inherited_options(PyObject * bases, StructType const * behaviour);
static bool any_struct_base_is_mutable(PyObject * bases);
static bool has_weakref_slot(StructType const * base);
static bool any_base_has_weakref_slot(PyObject * bases);
static bool weakref_expected(struct options options, PyObject * bases);
static bool weakref_slot_is_new(struct options options, PyObject * bases);
static struct options base_options(StructType const * base);
static PyObject * build_class_namespace(
	PyObject * original_namespace,
	PyObject * all_names,
	PyObject * new_names,
	struct options options,
	StructType const * base,
	struct options inherited,
	bool frozen_across_bases,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal
);
static enum result refuse_displaced_slots(
	PyObject * original_namespace,
	PyObject * all_names,
	bool carries_a_weakref_slot
);
static PyObject * build_slots(PyObject * new_names, bool weakref);
static enum result set_match_args(PyObject * namespace, PyObject * all_names, bool wanted);
static enum result apply_options(
	PyObject * namespace,
	struct options options,
	struct options inherited,
	bool frozen_across_bases,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal
);
static enum result rebind(PyObject * namespace, char const * const * names, bool from_mixin);
static enum result drop_class_variables(PyObject * namespace, PyObject * all_names);
static enum result refuse_colliding_methods(
	PyObject * original_namespace,
	PyObject * all_names,
	PyObject * class_name
);
static enum result refuse_mixin_method_fields(PyObject * all_names);
static int defines_a_method(
	PyObject * bound,
	PyObject * field_name,
	PyObject * class_name,
	PyObject * * spelling
);
static int defined_in_this_body(
	PyObject * qualname,
	PyObject * field_name,
	PyObject * class_name,
	PyObject * * spelling
);
static int names_this_body(PyObject * qualname, PyObject * class_name, PyObject * name);
static PyObject * unmangled(PyObject * class_name, PyObject * field_name);
static StructType * create_class(
	PyTypeObject * metatype,
	PyObject * name,
	PyObject * bases,
	PyObject * namespace
);
static PyTypeObject * winning_metatype(PyTypeObject * requested, PyObject * bases);
static enum result install_fields(
	StructType * struct_class,
	StructType const * base,
	struct field_plan const * plan,
	struct options options,
	bool resolves_body_eq
);
static enum result settle_planned(
	StructType * struct_class,
	StructType const * base,
	PyObject * bases,
	PyObject * name,
	struct field_plan const * plan,
	PyObject * original_namespace,
	struct options options,
	struct options inherited,
	bool frozen_across_bases,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal
);
static enum result settle_rebind(
	StructType * struct_class,
	PyObject * original_namespace,
	char const * const * names,
	bool from_mixin
);
static enum result restore_stripped(
	StructType * struct_class,
	PyObject * original_namespace,
	PyObject * class_dict,
	char const * const * names
);
static enum result refuse_unplanned(StructType const * struct_class);
static enum result install_post_init(StructType * struct_class);
static bool defines_own_init(StructType const * struct_class);
static PyObject * StructMeta_call(PyObject * self, PyObject * args, PyObject * keywords);
static Py_ssize_t * resolve_slot_offsets(
	StructType * struct_class,
	StructType const * base,
	PyObject * new_names,
	Py_ssize_t field_count
);
static Py_ssize_t * resolve_member_offsets(
	StructType * struct_class,
	Py_ssize_t const * slot_offsets,
	Py_ssize_t field_count,
	Py_ssize_t * member_count
);
static PyObject * StructMeta_get_field_names(PyObject * self, void * closure);
static PyObject * StructMeta_get_defaults(PyObject * self, void * closure);
static PyGetSetDef StructMeta_getset[];

static struct member_lookup find_member(
	PyMemberDef const * members,
	Py_ssize_t member_count,
	PyObject * name
);

PyTypeObject StructMeta_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._StructMeta",
	.tp_basicsize = sizeof(StructType),
	.tp_itemsize = sizeof(PyMemberDef),
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

static PyObject * build_struct_class(
	PyTypeObject * const metatype,
	StructType const * const base,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const original_namespace,
	PyObject * const keywords
) {
	StructType const * const behaviour = find_behaviour_base(bases);
	struct options const inherited = inherited_options(bases, behaviour);
	struct options_request const request =
		options_read(keywords, inherited, base != NULL && base->struct_field_count > 0);

	if (request.tag == OPTIONS_REJECTED) {
		return NULL;
	}

	if (
		metatype == &StructMeta_Type &&
		weakref_slot_is_new(request.options, bases) &&
		winning_metatype(metatype, bases)->tp_new != StructMeta_new
	) {
		PyErr_SetString(
			PyExc_TypeError,
			"weakref=True cannot cross a metaclass __new__ that hands the build "
			"off: the re-entered call cannot add the weakref slot"
		);

		return NULL;
	}

	struct field_plan plan = field_plan_build(base, original_namespace);

	if (field_plan_failed(&plan)) {
		return NULL;
	}

	if (
		refuse_colliding_methods(original_namespace, plan.all_names, name) != RESULT_OK ||
		refuse_mixin_method_fields(plan.all_names) != RESULT_OK ||
		refuse_displaced_slots(
				original_namespace,
				plan.all_names,
				weakref_expected(request.options, bases)
			) !=
			RESULT_OK
	) {
		field_plan_clear(&plan);

		return NULL;
	}

	bool const body_defines_eq = PyDict_GetItemString(original_namespace, "__eq__") != NULL;

	struct equality_source const inherited_equality = (
		request.options.eq == inherited.eq && !body_defines_eq ? resolves_body_equality(bases) :
		(struct equality_source){.tag = EQUALITY_RESOLVED, .from_a_body = false}
	);

	if (inherited_equality.tag == EQUALITY_FAILED) {
		field_plan_clear(&plan);

		return NULL;
	}

	bool const inherits_body_eq = inherited_equality.from_a_body;
	bool const frozen_across_bases = request.options.frozen && any_struct_base_is_mutable(bases);

	PY_OWNED(
		namespace,
		build_class_namespace(
			original_namespace,
			plan.all_names,
			plan.new_names,
			request.options,
			base,
			inherited,
			frozen_across_bases,
			body_defines_eq,
			inherits_body_eq,
			inherited_equality.needs_derived_not_equal
		)
	);
	StructType * struct_class = (
		namespace != NULL ? create_class(metatype, name, bases, namespace) :
		NULL
	);

	if (struct_class != NULL) {
		enum result const settled = (
			struct_class->struct_field_names == NULL ? install_fields(
				struct_class,
				base,
				&plan,
				request.options,
				body_defines_eq || inherits_body_eq
			) :
			settle_planned(
				struct_class,
				base,
				bases,
				name,
				&plan,
				original_namespace,
				request.options,
				inherited,
				frozen_across_bases,
				body_defines_eq,
				inherits_body_eq,
				inherited_equality.needs_derived_not_equal
			)
		);

		if (settled != RESULT_OK) {
			Py_CLEAR(struct_class);
		}
	}

	field_plan_clear(&plan);

	return (PyObject *) struct_class;
}

static StructType * find_struct_base(PyObject * const bases) {
	StructType * widest = NULL;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (!is_struct_class(base)) {
			continue;
		}

		StructType * const candidate = (StructType *) base;

		if (widest == NULL || candidate->struct_field_count > widest->struct_field_count) {
			widest = candidate;
		}
	}

	return widest;
}

static struct options base_options(StructType const * const base) {
	return base != NULL ? base->struct_options : options_initial();
}

static StructType * find_behaviour_base(PyObject * const bases) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base)) {
			return (StructType *) base;
		}
	}

	return NULL;
}

static struct equality_source resolves_body_equality(PyObject * const bases) {
	/* Only the first base can end the walk without reading anything: if it is a
	 * struct its branch answers, and every other case is the co-base walk,
	 * which is where the cost of looking lives. */
	if (PyTuple_GET_SIZE(bases) == 0) {
		return (struct equality_source){.tag = EQUALITY_RESOLVED, .from_a_body = false};
	}

	PyObject * const first = PyTuple_GET_ITEM(bases, 0);

	return (
		is_struct_class(
			first
		) ? (struct equality_source){
			.tag = EQUALITY_RESOLVED,
			.from_a_body = ((StructType *) first)->struct_resolves_body_eq,
			.needs_derived_not_equal = false,
		} :
		equality_from_the_co_bases(bases, 0)
	);
}

/* Split out so that the two names are interned once for the walk and not at
 * all for a class whose first base is a struct, which is nearly all of them. */
static struct equality_source equality_from_the_co_bases(
	PyObject * const bases,
	Py_ssize_t const first
) {
	PY_OWNED(equal, PyUnicode_InternFromString("__eq__"));
	PY_OWNED(not_equal, PyUnicode_InternFromString("__ne__"));

	if (equal == NULL || not_equal == NULL) {
		return (struct equality_source){.tag = EQUALITY_FAILED};
	}

	for (Py_ssize_t i = first; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base)) {
			return (struct equality_source){
				.tag = EQUALITY_RESOLVED,
				.from_a_body = ((StructType *) base)->struct_resolves_body_eq,
			};
		}

		struct definition const supplies_equality = base_defines(base, equal);

		if (supplies_equality.tag == DEFINITION_UNREADABLE) {
			return (struct equality_source){.tag = EQUALITY_FAILED};
		}

		if (!supplies_equality.found) {
			continue;
		}

		struct definition const supplies_inequality = any_base_defines(bases, first, not_equal);

		return (
			supplies_inequality.tag == DEFINITION_UNREADABLE ? (struct equality_source){
				.tag = EQUALITY_FAILED,
			} :
			(struct equality_source){
				.tag = EQUALITY_RESOLVED,
				.from_a_body = true,
				.needs_derived_not_equal = !supplies_inequality.found,
			}
		);
	}

	return (struct equality_source){.tag = EQUALITY_RESOLVED, .from_a_body = false};
}

static struct definition any_base_defines(
	PyObject * const bases,
	Py_ssize_t const first,
	PyObject * const name
) {
	for (Py_ssize_t i = first; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base)) {
			break;
		}

		struct definition const found = base_defines(base, name);

		if (found.tag == DEFINITION_UNREADABLE || found.found) {
			return found;
		}
	}

	return (struct definition){.tag = DEFINITION_READ, .found = false};
}

static struct definition base_defines(PyObject * const base, PyObject * const name) {
	PyObject * const mro = ((PyTypeObject *) base)->tp_mro;

	if (mro == NULL) {
		return (struct definition){.tag = DEFINITION_READ, .found = false};
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyObject * const entry = PyTuple_GET_ITEM(mro, i);

		if (entry == (PyObject *) &PyBaseObject_Type || entry == (PyObject *) &StructMixin_Type) {
			continue;
		}

		PY_OWNED(dict, struct_type_dict((PyTypeObject *) entry));

		if (dict == NULL) {
			return (struct definition){.tag = DEFINITION_UNREADABLE};
		}

		int const present = PyDict_Contains(dict, name);

		if (present < 0) {
			return (struct definition){.tag = DEFINITION_UNREADABLE};
		}

		if (present == 1) {
			return (struct definition){.tag = DEFINITION_READ, .found = true};
		}
	}

	return (struct definition){.tag = DEFINITION_READ, .found = false};
}

static bool any_struct_base_is_mutable(PyObject * const bases) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base) && !((StructType *) base)->struct_options.frozen) {
			return true;
		}
	}

	return false;
}

static struct options inherited_options(
	PyObject * const bases,
	StructType const * const behaviour
) {
	struct options const from_behaviour = base_options(behaviour);
	bool promised_frozen = false;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);
		StructType const * const struct_base = (StructType *) base;

		promised_frozen |= (
			is_struct_class(base) &&
			struct_base->struct_field_count > 0 &&
			struct_base->struct_options.frozen
		);
	}

	return (struct options){
		.frozen = from_behaviour.frozen || promised_frozen,
		.eq = from_behaviour.eq,
		.order = from_behaviour.order,
		.repr = from_behaviour.repr,
		.match_args = from_behaviour.match_args,
		.weakref = from_behaviour.weakref,
	};
}

static bool has_weakref_slot(StructType const * const base) {
	return base != NULL && base->heap_type.ht_type.tp_weaklistoffset != 0;
}

static bool any_base_has_weakref_slot(PyObject * const bases) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (PyType_Check(base) && ((PyTypeObject *) base)->tp_weaklistoffset != 0) {
			return true;
		}
	}

	return false;
}

static bool weakref_expected(struct options const options, PyObject * const bases) {
	return options.weakref || any_base_has_weakref_slot(bases);
}

static bool weakref_slot_is_new(struct options const options, PyObject * const bases) {
	return options.weakref && !any_base_has_weakref_slot(bases);
}

static PyObject * build_class_namespace(
	PyObject * const original_namespace,
	PyObject * const all_names,
	PyObject * const new_names,
	struct options const options,
	StructType const * const base,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal
) {
	PY_OWNED(slots, build_slots(new_names, options.weakref && !has_weakref_slot(base)));
	PY_MOVABLE(namespace, PyDict_Copy(original_namespace));

	if (
		slots != NULL &&
		namespace != NULL &&
		drop_class_variables(namespace, all_names) == RESULT_OK &&
		PyDict_SetItemString(namespace, "__slots__", slots) == 0 &&
		set_match_args(namespace, all_names, options.match_args) == RESULT_OK &&
		apply_options(
				namespace,
				options,
				inherited,
				frozen_across_bases,
				body_defines_eq,
				inherits_body_eq,
				derive_not_equal
			) ==
			RESULT_OK
	) {
		return py_move(&namespace);
	}

	return NULL;
}

static enum result refuse_displaced_slots(
	PyObject * const original_namespace,
	PyObject * const all_names,
	bool const carries_a_weakref_slot
) {
	PyObject * const declared = PyDict_GetItemString(original_namespace, "__slots__");

	if (declared == NULL) {
		return PyErr_Occurred() ? RESULT_ERROR : RESULT_OK;
	}

	PY_OWNED(
		entries,
		PyUnicode_Check(declared) ? PyTuple_Pack(1, declared) : PySequence_Tuple(declared)
	);

	if (entries == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(entries); ++i) {
		PyObject * const entry = PyTuple_GET_ITEM(entries, i);

		if (!PyUnicode_Check(entry)) {
			PyErr_Format(
				PyExc_TypeError,
				"__slots__ items must be strings, not '%.200s'",
				Py_TYPE(entry)->tp_name
			);

			return RESULT_ERROR;
		}

		if (PyUnicode_CompareWithASCIIString(entry, "__weakref__") == 0) {
			if (carries_a_weakref_slot) {
				continue;
			}

			PyErr_SetString(
				PyExc_TypeError,
				"__slots__ names __weakref__ and this class carries no weakref "
				"slot to name; a struct gets one from weakref=True or from a base "
				"that has one"
			);

			return RESULT_ERROR;
		}

		int const named_by_a_field = PySequence_Contains(all_names, entry);

		if (named_by_a_field < 0) {
			return RESULT_ERROR;
		}

		if (named_by_a_field == 1) {
			continue;
		}

		PyErr_Format(
			PyExc_TypeError,
			"__slots__ names %R, which is not a field of this class; a struct's "
			"fields are its slots, so salix would drop it -- declare it as a field, "
			"or drop __slots__",
			entry
		);

		return RESULT_ERROR;
	}

	return RESULT_OK;
}

static PyObject * build_slots(PyObject * const new_names, bool const weakref) {
	PY_OWNED(names, PySequence_List(new_names));

	if (names == NULL) {
		return NULL;
	}

	if (weakref) {
		PY_OWNED(weakref_name, PyUnicode_FromString("__weakref__"));

		if (weakref_name == NULL || PyList_Append(names, weakref_name) < 0) {
			return NULL;
		}
	}

	return PyList_AsTuple(names);
}

static enum result set_match_args(
	PyObject * const namespace,
	PyObject * const all_names,
	bool const wanted
) {
	if (!wanted) {
		return RESULT_OK;
	}

	PY_OWNED(match_args, PyList_AsTuple(all_names));

	return (
		match_args != NULL && PyDict_SetItemString(
			namespace,
			"__match_args__",
			match_args
		) == 0 ? RESULT_OK :
		RESULT_ERROR
	);
}

enum hash_binding {
	HASH_BODY_DEFINED,
	HASH_INHERITED_EQ,
	HASH_NONE,
	HASH_BIND,
};

struct binding_plan {
	bool answered_by_body;
	bool rebind_comparison;
	bool rebind_not_equal;
	bool rebind_representation;
	bool rebind_mutability;
	enum hash_binding hash;
	bool match_args_wanted;
};

static struct binding_plan binding_plan(
	struct options const options,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal,
	bool const body_defines_hash
) {
	struct binding_plan plan = {
		.answered_by_body = body_defines_eq || derive_not_equal,
		.rebind_comparison = options.eq != inherited.eq,
		.rebind_not_equal = options.eq != inherited.eq && !(body_defines_eq || derive_not_equal),
		.rebind_representation = options.repr != inherited.repr,
		.rebind_mutability = options.frozen != inherited.frozen || frozen_across_bases,
		.match_args_wanted = options.match_args,
	};

	if (body_defines_hash) {
		plan.hash = HASH_BODY_DEFINED;
	} else if (inherits_body_eq) {
		plan.hash = HASH_INHERITED_EQ;
	} else if (body_defines_eq || (options.eq && !options.frozen)) {
		plan.hash = HASH_NONE;
	} else {
		plan.hash = HASH_BIND;
	}

	return plan;
}

static bool settled_by_the_plan(char const * const name, struct binding_plan const plan) {
	if (
		strcmp(name, "__eq__") == 0 ||
		strcmp(name, "__lt__") == 0 ||
		strcmp(name, "__le__") == 0 ||
		strcmp(name, "__gt__") == 0 ||
		strcmp(name, "__ge__") == 0
	) {
		return plan.rebind_comparison;
	}

	if (strcmp(name, "__ne__") == 0) {
		return plan.rebind_not_equal || plan.answered_by_body;
	}

	if (strcmp(name, "__init__") == 0 || strcmp(name, "__post_init__") == 0) {
		return false;
	}

	if (strcmp(name, "__repr__") == 0) {
		return plan.rebind_representation;
	}

	if (strcmp(name, "__setattr__") == 0 || strcmp(name, "__delattr__") == 0) {
		return plan.rebind_mutability;
	}

	return plan.hash == HASH_BIND || plan.hash == HASH_NONE;
}

static enum result apply_options(
	PyObject * const namespace,
	struct options const options,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal
) {
	static char const * const comparison[] = {
		"__eq__",
		"__lt__",
		"__le__",
		"__gt__",
		"__ge__",
		NULL,
	};
	static char const * const not_equal[] = {"__ne__", NULL};
	static char const * const representation[] = {"__repr__", NULL};
	static char const * const mutability[] = {"__setattr__", "__delattr__", NULL};
	static char const * const hash_name[] = {"__hash__", NULL};
	struct binding_plan const plan = binding_plan(
		options,
		inherited,
		frozen_across_bases,
		body_defines_eq,
		inherits_body_eq,
		derive_not_equal,
		PyDict_GetItemString(namespace, "__hash__") != NULL
	);

	if (
		plan.answered_by_body &&
		PyDict_GetItemString(namespace, "__ne__") == NULL &&
		rebind(namespace, not_equal, false) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (plan.rebind_comparison && rebind(namespace, comparison, options.eq) != RESULT_OK) {
		return RESULT_ERROR;
	}

	if (plan.rebind_not_equal && rebind(namespace, not_equal, options.eq) != RESULT_OK) {
		return RESULT_ERROR;
	}

	if (
		plan.rebind_representation &&
		rebind(namespace, representation, options.repr) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (plan.rebind_mutability && rebind(namespace, mutability, options.frozen) != RESULT_OK) {
		return RESULT_ERROR;
	}

	switch (plan.hash) {
		case HASH_BODY_DEFINED:
		case HASH_INHERITED_EQ:
			break;
		case HASH_NONE:
			if (PyDict_SetItemString(namespace, "__hash__", Py_None) != 0) {
				return RESULT_ERROR;
			}

			break;
		case HASH_BIND:
			if (rebind(namespace, hash_name, options.eq) != RESULT_OK) {
				return RESULT_ERROR;
			}

			break;
	}

	return RESULT_OK;
}

static enum result rebind(
	PyObject * const namespace,
	char const * const * const names,
	bool const from_mixin
) {
	PyObject * const source = (
		from_mixin ? (PyObject *) &StructMixin_Type :
		(PyObject *) &PyBaseObject_Type
	);

	for (char const * const * name = names; *name != NULL; ++name) {
		if (PyDict_GetItemString(namespace, *name) != NULL) {
			continue;
		}

		PY_OWNED(bound, PyObject_GetAttrString(source, *name));

		if (bound == NULL || PyDict_SetItemString(namespace, *name, bound) < 0) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result drop_class_variables(PyObject * const namespace, PyObject * const all_names) {
	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(all_names); ++i) {
		PyObject * const field_name = PyList_GET_ITEM(all_names, i);
		int const present = PyDict_Contains(namespace, field_name);

		if (present < 0 || (present == 1 && PyDict_DelItem(namespace, field_name) < 0)) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result refuse_mixin_method_fields(PyObject * const all_names) {
	PY_OWNED(mixin_dict, struct_type_dict(&StructMixin_Type));

	if (mixin_dict == NULL) {
		return RESULT_ERROR;
	}

	Py_ssize_t position = 0;
	PyObject * field_name;
	PyObject * method;

	while (PyDict_Next(mixin_dict, &position, &field_name, &method)) {
		if (!PyObject_TypeCheck(method, &PyMethodDescr_Type)) {
			continue;
		}

		int const present = PySequence_Contains(all_names, field_name);

		if (present < 0) {
			return RESULT_ERROR;
		}

		if (present == 1) {
			PyErr_Format(
				PyExc_TypeError,
				"%U is a field, and the mixin defines a method of the same "
				"name which the field's descriptor would shadow; rename the field",
				field_name
			);

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result refuse_colliding_methods(
	PyObject * const original_namespace,
	PyObject * const all_names,
	PyObject * const class_name
) {
	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(all_names); ++i) {
		PyObject * const field_name = PyList_GET_ITEM(all_names, i);
		PY_OWNED(bound, dict_value_ref(original_namespace, field_name));

		if (bound == NULL) {
			if (PyErr_Occurred()) {
				return RESULT_ERROR;
			}

			continue;
		}

		PY_MOVABLE(spelling, NULL);
		int const method = defines_a_method(bound, field_name, class_name, &spelling);

		if (method < 0) {
			return RESULT_ERROR;
		}

		if (method == 1) {
			PyErr_Format(
				PyExc_TypeError,
				"'%U' is a field, and the class body binds a %.100s to that name "
				"which salix would drop for the descriptor that reads the field; "
				"rename one of them",
				spelling != NULL ? spelling : field_name,
				Py_TYPE(bound)->tp_name
			);

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static int defines_a_method(
	PyObject * const bound,
	PyObject * const field_name,
	PyObject * const class_name,
	PyObject * * const spelling
) {
	if (
		PyObject_TypeCheck(bound, &PyProperty_Type) ||
		PyObject_TypeCheck(bound, &PyClassMethod_Type) ||
		PyObject_TypeCheck(bound, &PyStaticMethod_Type)
	) {
		return 1;
	}

	if (!PyFunction_Check(bound)) {
		return 0;
	}

	return defined_in_this_body(
		((PyFunctionObject *) bound)->func_qualname,
		field_name,
		class_name,
		spelling
	);
}

static int defined_in_this_body(
	PyObject * const qualname,
	PyObject * const field_name,
	PyObject * const class_name,
	PyObject * * const spelling
) {
	if (qualname == NULL || !PyUnicode_Check(qualname)) {
		return 0;
	}

	int const stored = names_this_body(qualname, class_name, field_name);

	if (stored != 0) {
		*spelling = stored == 1 ? Py_NewRef(field_name) : NULL;

		return stored;
	}

	PY_OWNED(source_name, unmangled(class_name, field_name));

	if (source_name == NULL) {
		return -1;
	}

	if (source_name == field_name) {
		return 0;
	}

	int const written = names_this_body(qualname, class_name, source_name);

	*spelling = written == 1 ? Py_NewRef(source_name) : NULL;

	return written;
}

static int names_this_body(
	PyObject * const qualname,
	PyObject * const class_name,
	PyObject * const name
) {
	PY_OWNED(own, PyUnicode_FromFormat("%U.%U", class_name, name));

	if (own == NULL) {
		return -1;
	}

	Py_ssize_t const length = PyUnicode_GET_LENGTH(qualname);
	Py_ssize_t const tail = PyUnicode_GET_LENGTH(own);
	Py_ssize_t const matched = PyUnicode_Tailmatch(qualname, own, 0, length, 1);

	if (matched <= 0) {
		return matched < 0 ? -1 : 0;
	}

	return length == tail || PyUnicode_ReadChar(qualname, length - tail - 1) == '.';
}

static PyObject * unmangled(PyObject * const class_name, PyObject * const field_name) {
	Py_ssize_t const class_length = PyUnicode_GET_LENGTH(class_name);
	Py_ssize_t leading = 0;

	while (leading < class_length && PyUnicode_ReadChar(class_name, leading) == '_') {
		++leading;
	}

	if (leading == class_length) {
		return Py_NewRef(field_name);
	}

	PY_OWNED(stripped, PyUnicode_Substring(class_name, leading, class_length));

	if (stripped == NULL) {
		return NULL;
	}

	PY_OWNED(prefix, PyUnicode_FromFormat("_%U__", stripped));

	if (prefix == NULL) {
		return NULL;
	}

	Py_ssize_t const mangled = PyUnicode_Tailmatch(field_name, prefix, 0, PY_SSIZE_T_MAX, -1);

	if (mangled < 0) {
		return NULL;
	}

	if (mangled == 0) {
		return Py_NewRef(field_name);
	}

	return PyUnicode_Substring(
		field_name,
		PyUnicode_GET_LENGTH(prefix) - 2,
		PyUnicode_GET_LENGTH(field_name)
	);
}

static StructType * create_class(
	PyTypeObject * const metatype,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const namespace
) {
	PY_OWNED(type_args, PyTuple_Pack(3, name, bases, namespace));

	if (type_args == NULL) {
		return NULL;
	}

	PyTypeObject * const winner = winning_metatype(metatype, bases);
	PyTypeObject * const builder = winner->tp_new == StructMeta_new ? winner : metatype;
	PY_MOVABLE(created, PyType_Type.tp_new(builder, type_args, NULL));

	if (created == NULL) {
		return NULL;
	}

	if (!is_struct_class(created)) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s.__new__ returned %.200s, which is not a struct class",
			winner->tp_name,
			Py_TYPE(created)->tp_name
		);

		return NULL;
	}

	return (StructType *) py_move(&created);
}

/* It is the third walk of `bases` in a class creation, after find_struct_base
 * and _PyType_CalculateMetaclass. Measured, and smaller than the measurement:
 * replacing the body with `return requested` leaves class creation at 9.77-9.87
 * us for a 16-field class either way, so the walk is bounded by the width of
 * that band rather than shown to be free. It is one Py_TYPE and one
 * PyType_IsSubtype per base, and a class has one. */
static PyTypeObject * winning_metatype(PyTypeObject * const requested, PyObject * const bases) {
	PyTypeObject * winner = requested;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyTypeObject * const candidate = Py_TYPE(PyTuple_GET_ITEM(bases, i));

		if (PyType_IsSubtype(candidate, winner)) {
			winner = candidate;
		}
	}

	return winner;
}

static enum result settle_rebind(
	StructType * const struct_class,
	PyObject * const original_namespace,
	char const * const * const names,
	bool const from_mixin
) {
	PyObject * const source = (
		from_mixin ? (PyObject *) &StructMixin_Type :
		(PyObject *) &PyBaseObject_Type
	);

	for (char const * const * name = names; *name != NULL; name += 1) {
		if (PyDict_GetItemString(original_namespace, *name) != NULL) {
			continue;
		}

		PY_OWNED(bound, PyObject_GetAttrString(source, *name));
		PY_OWNED(unicode_name, PyUnicode_FromString(*name));

		if (
			bound == NULL ||
			unicode_name == NULL ||
			PyType_Type.tp_setattro((PyObject *) struct_class, unicode_name, bound) < 0
		) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
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

static enum result settle_planned(
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
	static char const * const comparison[] = {
		"__eq__",
		"__lt__",
		"__le__",
		"__gt__",
		"__ge__",
		NULL,
	};
	static char const * const not_equal[] = {"__ne__", NULL};
	static char const * const representation[] = {"__repr__", NULL};
	static char const * const mutability[] = {"__setattr__", "__delattr__", NULL};
	static char const * const hash_name[] = {"__hash__", NULL};
	static char const * const body_dunders[] = {
		"__eq__",
		"__ne__",
		"__lt__",
		"__le__",
		"__gt__",
		"__ge__",
		"__repr__",
		"__setattr__",
		"__delattr__",
		"__hash__",
		"__init__",
		"__post_init__",
		NULL,
	};
	PY_OWNED(planned, PyList_AsTuple(plan->all_names));

	if (planned == NULL) {
		return RESULT_ERROR;
	}

	int const same_fields =
		PyObject_RichCompareBool(struct_class->struct_field_names, planned, Py_EQ);

	if (same_fields < 0) {
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

	if (
		same_fields == 0 ||
		same_name == 0 ||
		find_struct_base(((PyTypeObject *) struct_class)->tp_bases) != base
	) {
		return refuse_unplanned(struct_class);
	}

	bool const carries_slot = ((PyTypeObject *) struct_class)->tp_weaklistoffset != 0;

	if (carries_slot != weakref_expected(options, bases)) {
		return refuse_unplanned(struct_class);
	}

	struct binding_plan const bindings = binding_plan(
		options,
		inherited,
		frozen_across_bases,
		body_defines_eq,
		inherits_body_eq,
		derive_not_equal,
		PyDict_GetItemString(original_namespace, "__hash__") != NULL
	);

	for (char const * const * name = body_dunders; *name != NULL; name += 1) {
		if (
			PyDict_GetItemString(class_dict, *name) != NULL &&
			PyDict_GetItemString(original_namespace, *name) == NULL &&
			!settled_by_the_plan(*name, bindings)
		) {
			return refuse_unplanned(struct_class);
		}
	}

	if (restore_stripped(struct_class, original_namespace, class_dict, body_dunders) != RESULT_OK) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_comparison &&
		settle_rebind(struct_class, original_namespace, comparison, options.eq) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_not_equal &&
		settle_rebind(struct_class, original_namespace, not_equal, options.eq) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (bindings.answered_by_body && PyDict_GetItemString(original_namespace, "__ne__") == NULL) {
		/* bind_not_equal's answered case on a live class: the fresh build
		 * binds object's __ne__ when the body answered equality itself. */
		PY_OWNED(object_ne, PyObject_GetAttrString((PyObject *) &PyBaseObject_Type, "__ne__"));
		PY_OWNED(ne_name, PyUnicode_FromString("__ne__"));

		if (
			object_ne == NULL ||
			ne_name == NULL ||
			PyType_Type.tp_setattro((PyObject *) struct_class, ne_name, object_ne) < 0
		) {
			return RESULT_ERROR;
		}
	}

	if (
		bindings.rebind_representation &&
		settle_rebind(struct_class, original_namespace, representation, options.repr) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_mutability &&
		settle_rebind(struct_class, original_namespace, mutability, options.frozen) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	switch (bindings.hash) {
		case HASH_BODY_DEFINED:
		case HASH_INHERITED_EQ:
			break;
		case HASH_NONE: {
			PY_OWNED(hash_name_obj, PyUnicode_FromString("__hash__"));

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
				settle_rebind(struct_class, original_namespace, hash_name, options.eq) !=
				RESULT_OK
			) {
				return RESULT_ERROR;
			}

			break;
	}

	if (bindings.match_args_wanted) {
		if (PyDict_SetItemString(class_dict, "__match_args__", planned) < 0) {
			return RESULT_ERROR;
		}
	} else {
		PyObject * const body_match_args = PyDict_GetItemString(
			original_namespace,
			"__match_args__"
		);

		if (body_match_args != NULL) {
			if (PyDict_SetItemString(class_dict, "__match_args__", body_match_args) < 0) {
				return RESULT_ERROR;
			}
		} else if (PyDict_DelItemString(class_dict, "__match_args__") < 0) {
			if (PyErr_ExceptionMatches(PyExc_KeyError)) {
				PyErr_Clear();
			} else {
				return RESULT_ERROR;
			}
		}
	}

	Py_SETREF(struct_class->struct_defaults, Py_NewRef(plan->defaults));
	struct_class->struct_default_count = PyTuple_GET_SIZE(plan->defaults);
	struct_class->struct_options = options;
	struct_class->struct_resolves_body_eq = body_defines_eq || inherits_body_eq;

	if (defines_own_init(struct_class)) {
		struct_class->heap_type.ht_type.tp_new = Struct_new;
		struct_class->heap_type.ht_type.tp_vectorcall = NULL;
	} else {
		struct_class->heap_type.ht_type.tp_vectorcall = Struct_vectorcall;
	}

	return install_post_init(struct_class);
}

static enum result restore_stripped(
	StructType * const struct_class,
	PyObject * const original_namespace,
	PyObject * const class_dict,
	char const * const * const names
) {
	for (char const * const * name = names; *name != NULL; name += 1) {
		PyObject * const body_value = PyDict_GetItemString(original_namespace, *name);

		if (body_value == NULL || PyDict_GetItemString(class_dict, *name) == body_value) {
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

	return RESULT_OK;
}

static enum result install_fields(
	StructType * const struct_class,
	StructType const * const base,
	struct field_plan const * const plan,
	struct options const options,
	bool const resolves_body_eq
) {
	PY_MOVABLE(field_names, PyList_AsTuple(plan->all_names));

	if (field_names == NULL) {
		return RESULT_ERROR;
	}

	Py_ssize_t const field_count = PyTuple_GET_SIZE(field_names);
	Py_ssize_t * const offsets =
		resolve_slot_offsets(struct_class, base, plan->new_names, field_count);

	if (offsets == NULL) {
		return RESULT_ERROR;
	}

	Py_ssize_t member_count = 0;
	Py_ssize_t * const member_offsets =
		resolve_member_offsets(struct_class, offsets, field_count, &member_count);

	if (member_offsets == NULL) {
		PyMem_Free(offsets);

		return RESULT_ERROR;
	}

	struct_class->struct_field_names = py_move(&field_names);
	struct_class->struct_defaults = Py_NewRef(plan->defaults);
	struct_class->struct_slot_offsets = offsets;
	struct_class->struct_member_offsets = member_offsets;
	struct_class->struct_member_count = member_count;
	struct_class->struct_field_count = field_count;
	struct_class->struct_default_count = PyTuple_GET_SIZE(plan->defaults);
	struct_class->struct_options = options;
	struct_class->struct_resolves_body_eq = resolves_body_eq;

	if (defines_own_init(struct_class)) {
		struct_class->heap_type.ht_type.tp_new = Struct_new;
	} else {
		struct_class->heap_type.ht_type.tp_vectorcall = Struct_vectorcall;
	}

	return install_post_init(struct_class);
}

static bool defines_own_init(StructType const * const struct_class) {
	return struct_class->heap_type.ht_type.tp_init != PyBaseObject_Type.tp_init;
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

static enum result install_post_init(StructType * const struct_class) {
	PyObject * const hook = optional_attribute((PyObject *) struct_class, "__post_init__");

	if (hook == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	struct_class->struct_post_init = hook;

	return RESULT_OK;
}

static Py_ssize_t * resolve_slot_offsets(
	StructType * const struct_class,
	StructType const * const base,
	PyObject * const new_names,
	Py_ssize_t const field_count
) {
	Py_ssize_t * const offsets = PyMem_New(Py_ssize_t, field_count > 0 ? field_count : 1);

	if (offsets == NULL) {
		PyErr_NoMemory();

		return NULL;
	}

	Py_ssize_t const inherited_count = base != NULL ? base->struct_field_count : 0;

	for (Py_ssize_t i = 0; i < inherited_count; ++i) {
		offsets[i] = base->struct_slot_offsets[i];
	}

	PyMemberDef const * const members = struct_heap_type_members(struct_class);
	Py_ssize_t const member_count = Py_SIZE(struct_class);

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(new_names); ++i) {
		PyObject * const field_name = PyList_GET_ITEM(new_names, i);
		struct member_lookup const found = find_member(members, member_count, field_name);

		switch (found.tag) {
			case MEMBER_LOOKUP_ERROR:
				PyMem_Free(offsets);
				return NULL;
			case MEMBER_LOOKUP_MISSING:
				PyErr_Format(PyExc_RuntimeError, "could not find slot offset for %R", field_name);
				PyMem_Free(offsets);

				return NULL;
			case MEMBER_LOOKUP_FOUND:
				offsets[inherited_count + i] = found.slot_offset;
		}
	}

	return offsets;
}

static Py_ssize_t * resolve_member_offsets(
	StructType * const struct_class,
	Py_ssize_t const * const slot_offsets,
	Py_ssize_t const field_count,
	Py_ssize_t * const member_count
) {
	/* A non-struct base's own __slots__ members are not struct fields, so
	 * the copy would never touch them without this table: one walk here, at
	 * class creation, instead of rescanning every MRO dict on every copy.
	 * The struct's own field descriptors are skipped by offset, and a
	 * weakref slot is a getset descriptor, never a member one. */
	*member_count = 0;

	PyObject * const mro = struct_class->heap_type.ht_type.tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		PY_OWNED(entry_dict, struct_type_dict(entry));

		if (entry_dict == NULL) {
			return NULL;
		}

		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(entry_dict, &position, &key, &value)) {
			if (!PyObject_TypeCheck(value, &PyMemberDescr_Type)) {
				continue;
			}

			PyMemberDef const * const member = ((PyMemberDescrObject *) value)->d_member;

			if (member == NULL || member->type != SLOT_MEMBER_TYPE) {
				continue;
			}

			bool is_struct_field = false;

			for (Py_ssize_t f = 0; f < field_count; ++f) {
				if (member->offset == slot_offsets[f]) {
					is_struct_field = true;
					break;
				}
			}

			if (!is_struct_field) {
				++*member_count;
			}
		}
	}

	Py_ssize_t * const offsets = PyMem_New(Py_ssize_t, *member_count > 0 ? *member_count : 1);

	if (offsets == NULL) {
		PyErr_NoMemory();

		return NULL;
	}

	Py_ssize_t written = 0;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		PY_OWNED(entry_dict, struct_type_dict(entry));

		if (entry_dict == NULL) {
			PyMem_Free(offsets);

			return NULL;
		}

		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(entry_dict, &position, &key, &value)) {
			if (!PyObject_TypeCheck(value, &PyMemberDescr_Type)) {
				continue;
			}

			PyMemberDef const * const member = ((PyMemberDescrObject *) value)->d_member;

			if (member == NULL || member->type != SLOT_MEMBER_TYPE) {
				continue;
			}

			bool is_struct_field = false;

			for (Py_ssize_t f = 0; f < field_count; ++f) {
				if (member->offset == slot_offsets[f]) {
					is_struct_field = true;
					break;
				}
			}

			if (!is_struct_field) {
				offsets[written++] = member->offset;
			}
		}
	}

	return offsets;
}

static struct member_lookup find_member(
	PyMemberDef const * const members,
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

#	include "testing.h"

/* find_member reads a PyMemberDef array, which is trivially fabricated -- and
 * the miss is the branch that turns into a RuntimeError nothing else exercises. */
static PyMemberDef const example_members[] = {
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
