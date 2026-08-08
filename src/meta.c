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

/* Where type.__new__ placed the slot it created for a field name. */
struct member_lookup {
	enum { MEMBER_LOOKUP_FOUND, MEMBER_LOOKUP_MISSING, MEMBER_LOOKUP_ERROR } tag;
	Py_ssize_t offset;
};

/* Whether a name is defined, and whether the dicts could be read at all. */
struct definition {
	enum { DEFINITION_READ, DEFINITION_UNREADABLE } tag;
	bool found;
};

/* Whether the __eq__ a class resolves came from a class body -- always a
 * base's, never this class's own, since a body that writes __eq__ is never
 * asked. */
struct equality_source {
	enum { EQUALITY_RESOLVED, EQUALITY_FAILED } tag;
	bool from_a_body;
	/* Whether this class has to bind the derived __ne__ itself.
	 *
	 * A struct base that resolves a body's __eq__ bound object's __ne__ into
	 * its own dict when it was built, so a subclass inherits the pair. A
	 * co-base that supplies __eq__ and its own __ne__ has already paired them,
	 * and taking that pairing away is not salix's to do. It is only a co-base
	 * supplying __eq__ *alone* that leaves the mixin's structural __ne__ as the
	 * next thing the lookup finds. */
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
static enum result bind_not_equal(PyObject * namespace, bool answered_by_a_body);
static enum result bind_hash(
	PyObject * namespace,
	struct options options,
	bool body_defines_eq,
	bool inherits_body_eq
);
static enum result drop_class_variables(PyObject * namespace, PyObject * all_names);
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
static enum result reject_unless_planned(
	StructType const * struct_class,
	struct field_plan const * plan,
	struct options options
);
static enum result install_post_init(StructType * struct_class);
static bool defines_own_init(StructType const * struct_class);
static PyObject * StructMeta_call(PyObject * self, PyObject * args, PyObject * keywords);
static Py_ssize_t * resolve_slot_offsets(
	StructType * struct_class,
	StructType const * base,
	PyObject * new_names,
	Py_ssize_t field_count
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

/* The mixin answers these for an instance; the metaclass answers the same
 * questions of the class, which is where msgspec puts them and so where a
 * reader looks first. Both spellings, for the reason src/mixin.c gives: the
 * sunder is salix's, and the dunder is msgspec's name honoured rather than a
 * dunder of salix's own invention. Unlike the mixin's, these getters need no
 * name of their own -- there is no non-struct for them to refuse. */
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

/*
 * Everything a struct does comes from _StructMixin, and every struct class
 * carries it because Struct does. A class with no struct base has no way to
 * have got it: it would build, construct, report its fields, and be a struct
 * in every visible way except behaviour.
 *
 * The one class that legitimately has no struct base is Struct, and the module
 * builds it through struct_create_root rather than through here -- so this
 * refusal has no exception to carve out, and there is no shape of `bases` that
 * gets a caller past it.
 */
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

/* Struct itself, built once from module init. The only class with no struct
 * base, and the only caller that does not come through a metaclass call. */
PyObject * struct_create_root(
	PyObject * const name,
	PyObject * const bases,
	PyObject * const namespace
) {
	return build_struct_class(&StructMeta_Type, NULL, name, bases, namespace, NULL);
}

/*
 * Creating a struct class is four steps: work out the fields, build the
 * namespace type.__new__ wants, make the type, then hand it the field table
 * that makes it a struct.
 */
static PyObject * build_struct_class(
	PyTypeObject * const metatype,
	StructType const * const base,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const original_namespace,
	PyObject * const keywords
) {
	/* The behaviour base rather than the layout one: every dunder a struct base
	 * carries is resolved by the MRO, which takes the first of them, so reading
	 * the options off anything else makes the record and the behaviour two
	 * different answers. */
	StructType const * const behaviour = find_behaviour_base(bases);
	struct options const inherited = inherited_options(bases, behaviour);
	struct options_request const request =
		options_read(keywords, inherited, base != NULL && base->struct_field_count > 0);

	if (request.tag == OPTIONS_REJECTED) {
		return NULL;
	}

	struct field_plan plan = field_plan_build(base, original_namespace);

	if (field_plan_failed(&plan)) {
		return NULL;
	}

	if (
		refuse_displaced_slots(
			original_namespace,
			plan.all_names,
			request.options.weakref || any_base_has_weakref_slot(bases)
		) !=
		RESULT_OK
	) {
		field_plan_clear(&plan);

		return NULL;
	}

	/* Read before build_class_namespace rebinds anything: afterwards every
	 * comparison name is present whether the body wrote one or salix did. An
	 * inherited one survives only if this body leaves equality alone, because a
	 * class that changes the eq option has salix's binding written into its own
	 * namespace and that is what the lookup finds first. */
	bool const body_defines_eq = PyDict_GetItemString(original_namespace, "__eq__") != NULL;

	/* Only asked when the answer can be used, because asking walks the class
	 * dicts along every co-base's MRO -- and a dict it cannot read is answered
	 * as a failure, so an unnecessary walk can still refuse a class that would
	 * otherwise build.
	 *
	 * A class that changes the eq option has salix's binding written into its
	 * own namespace for all six comparison names, and a class whose body writes
	 * __eq__ has its own ahead of every base. Either way nothing a base
	 * resolves is what the class gets. */
	struct equality_source const inherited_equality = (
		request.options.eq == inherited.eq && !body_defines_eq ? resolves_body_equality(bases) :
		(struct equality_source){.tag = EQUALITY_RESOLVED, .from_a_body = false}
	);

	if (inherited_equality.tag == EQUALITY_FAILED) {
		field_plan_clear(&plan);

		return NULL;
	}

	bool const inherits_body_eq = inherited_equality.from_a_body;

	PY_OWNED(
		namespace,
		build_class_namespace(
			original_namespace,
			plan.all_names,
			plan.new_names,
			request.options,
			base,
			inherited,
			request.options.frozen && any_struct_base_is_mutable(bases),
			body_defines_eq,
			inherits_body_eq,
			inherited_equality.needs_derived_not_equal
		)
	);
	StructType * struct_class = (
		namespace != NULL ? create_class(metatype, name, bases, namespace) :
		NULL
	);

	/* create_class builds as the winning metatype, so the ordinary handoff no
	 * longer comes back here already installed. What still can is a metaclass
	 * __new__ that returned a struct class -- possibly one it did not just make,
	 * whose slot offsets belong to a layout this plan knows nothing about.
	 * Installing over that is a field table pointed at the wrong memory, so the
	 * class it did build is held to what this call planned instead. */
	if (struct_class != NULL) {
		enum result const settled = (
			struct_class->struct_field_names == NULL ? install_fields(
				struct_class,
				base,
				&plan,
				request.options,
				body_defines_eq || inherits_body_eq
			) :
			reject_unless_planned(struct_class, &plan, request.options)
		);

		if (settled != RESULT_OK) {
			Py_CLEAR(struct_class);
		}
	}

	field_plan_clear(&plan);

	return (PyObject *) struct_class;
}

/*
 * The struct base this class extends the layout of: the one with the most
 * fields, which is what CPython settles on as tp_base and so where the slots
 * and their offsets come from. Taking the first match instead let a fieldless
 * base stand in front of one with fields, and every field of the second went
 * missing.
 *
 * Fields rather than tp_basicsize, because CPython discounts __weakref__ and
 * __dict__ when it compares layouts and salix's fields are the only other slots
 * a struct base adds. Below 3.12 a weakref slot still widens the type, so a
 * `weakref=True` fieldless base measures exactly as wide as a one-field base
 * and would win a comparison on size while CPython gave tp_base to the other.
 *
 * A fieldless struct base still carries the options a subclass inherits, so it
 * counts when it is the only one. Two struct bases tie here when one derives
 * from the other and when both are fieldless; either way the first of them is
 * also the most derived, which is the one CPython settles on.
 */
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

/* What a subclass starts from: the base's options, or the defaults when there
 * is no struct base to inherit from. */
static struct options base_options(StructType const * const base) {
	return base != NULL ? base->struct_options : options_initial();
}

/*
 * The struct base whose dunders the MRO will resolve: the first of them.
 *
 * Not the layout base: the layout question is where the slots are, and this is
 * which class answers for the dunders. Answering both with the widest base put
 * the record and the behaviour into disagreement three separate ways -- a class
 * recorded frozen that every write succeeded on, an explicit repr=True that
 * silently no-opped, and equal objects hashing differently.
 */
static StructType * find_behaviour_base(PyObject * const bases) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base)) {
			return (StructType *) base;
		}
	}

	return NULL;
}

/*
 * Whether the __eq__ this class will resolve came from a class body, rather
 * than from the mixin or from object.
 *
 * Bases are asked in order, and the first whose branch supplies an __eq__ is
 * the answer. A non-struct base answers only when it resolves something other
 * than object's or the mixin's; otherwise it adds nothing to the lookup and
 * the next base decides. The first struct base ends the walk, because its
 * branch reaches the mixin and so always supplies one.
 *
 * Asking the struct base alone let a non-struct base ahead of it supply the
 * equality while salix bound a structural hash beside it, so two instances
 * compared equal and still took two slots in a set.
 *
 * Ending at the first struct base is exact for one struct base and an
 * approximation for two or more. With more than one, C3 can put a later struct
 * base's own __eq__ ahead of the mixin -- the mixin is a shared ancestor and is
 * deferred to the tail -- so the class resolves an __eq__ this walk never saw,
 * however many plain struct bases stand in front of it. That is the same
 * "recorded from one base, answered by the MRO" shape the rest of the option
 * handling has, and it is not settled here.
 */
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

		/* The equality is settled. Inequality is a separate question over the
		 * same bases, because the two can come from different ones:
		 * `class B(Equal, WeirdNotEqual, Base)` takes equality from the first
		 * and inequality from the second, which is what plain Python resolves. */
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

/* The same question of every co-base, for the name the class will resolve
 * rather than for one base's answer. */
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

/*
 * Whether this base's own ancestry defines `name`, read from the class dicts
 * rather than by fetching the attribute.
 *
 * Fetching was the earlier shape and it was wrong twice over. It runs whatever
 * the base put under the name, so a descriptor that raises refused a class
 * that had built before -- four rounds of this review found a new instance of
 * that each time the set of names being read grew. And it can only classify
 * the result by pointer-comparing against object's and the mixin's cached
 * method-wrappers, which is an implementation detail of how CPython caches
 * type attributes rather than a fact the language guarantees.
 *
 * Asking the dicts answers exactly the question that matters -- did a class
 * body write this name -- and answers it for `__eq__ = object.__eq__` too,
 * which fetching could never tell from not defining one at all. object and the
 * mixin are skipped because those are the two answers that mean "nobody did".
 */
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

/*
 * What the class starts from: the behaviour base's options, with the two that
 * are facts about the other bases rather than preferences of that one.
 *
 * frozen is a promise every fielded base made separately -- a mutable subclass
 * hands a writable object to everything holding a reference of any of their
 * types -- so it is the strongest of them, not the first one's. weakref is a
 * slot the class has if any base carries one, and recording otherwise would be
 * the option disagreeing with tp_weaklistoffset.
 */
/*
 * Whether some struct base is mutable, which is the only reason a frozen class
 * has to bind __setattr__ rather than let the MRO find the mixin's. A mutable
 * base bound object's on its own transition, and the MRO reaches that binding
 * before the shared mixin.
 */
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

/* CPython refuses a second __weakref__ in a subclass, so an inherited one is
 * what `weakref=True` already got. */
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

/* The namespace handed to type.__new__: a copy of the original with every
 * class-body binding of a field name removed (so none of them clashes with the
 * __slots__ descriptor that reads the value) plus __slots__ / __match_args__
 * and whatever the options replace. */
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

/* __weakref__ is a slot like any other; a class that wants to be the target of
 * a weak reference asks for one. */
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

/* Left unset rather than emptied, so a subclass that opts out still matches
 * positionally on whatever its base declared -- as a dataclass does. */
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

/*
 * An option is off when object answers the name and on when the mixin does, so
 * turning one on is as much work as turning it off: a subclass of a class that
 * opted out inherits that class's bindings, not the mixin's, and only an
 * explicit rebind gets the behaviour back.
 *
 * Bound in the namespace rather than written to the slots afterwards: the type
 * machinery derives tp_setattro, tp_richcompare, tp_hash and tp_repr from what
 * the class body defines, so assigning a slot directly would leave the dunder
 * still resolving one way while the operator took the other.
 */
static enum result apply_options(
	PyObject * const namespace,
	struct options const options,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal
) {
	/* All six, not just __eq__: they share tp_richcompare, and a class that
	 * rebinds only some of them gets the dispatching slot with the other source
	 * still answering the rest. */
	static char const * const comparison[] = {
		"__eq__",
		"__ne__",
		"__lt__",
		"__le__",
		"__gt__",
		"__ge__",
		NULL,
	};
	static char const * const representation[] = {"__repr__", NULL};
	static char const * const mutability[] = {"__setattr__", "__delattr__", NULL};

	/* Before the rebind below, which would otherwise put the mixin's __ne__ in
	 * beside the body's __eq__ for a class that also changed the eq option. */
	if (bind_not_equal(namespace, body_defines_eq || derive_not_equal) != RESULT_OK) {
		return RESULT_ERROR;
	}

	/* An unchanged option needs no rebinding, because `inherited` is read off
	 * the first struct base -- the one whose branch of the MRO is searched
	 * first. That is an approximation: a later struct base binds a dunder its
	 * own creation transitioned on, and the MRO reaches it before the shared
	 * mixin, so a second struct base can still answer for a name this class
	 * never rebound. */
	if (options.eq != inherited.eq && rebind(namespace, comparison, options.eq) != RESULT_OK) {
		return RESULT_ERROR;
	}

	if (
		options.repr != inherited.repr &&
		rebind(namespace, representation, options.repr) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	/* The exception, and the reason frozen is the only forced one: with more
	 * than one struct base it can be promised by a base that is not the one
	 * binding __setattr__, and then the class is recorded frozen while the MRO
	 * answers with object's and every write succeeds. */
	if (
		(options.frozen != inherited.frozen || frozen_across_bases) &&
		rebind(namespace, mutability, options.frozen) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	return bind_hash(namespace, options, body_defines_eq, inherits_body_eq);
}

/* A name the class body defined is neither source's to take. */
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

/*
 * An __eq__ that came from a class body gets Python's derived __ne__ with it,
 * the way every other construction does -- object.__ne__ calls __eq__ and
 * inverts, unless the body wrote a __ne__ of its own, which rebind leaves
 * alone.
 *
 * It has to be bound rather than left to the MRO, because the mixin is a base:
 * its structural __ne__ is what the lookup finds, and it answers a different
 * question from the body's __eq__, so `a == b` and `a != b` were both true.
 *
 * "From a class body" includes one this class *inherits* rather than writes.
 * A co-base ahead of the struct base supplies the __eq__ the class resolves,
 * and until that counted here the same pair of answers came back for it:
 * equality from the co-base and inequality from the mixin, both true at once.
 *
 * Binding it into the dict also puts it ahead of a __ne__ a *co-base* supplies,
 * which plain Python would let win -- the derived one is the end of the MRO
 * there, not the front. That is the same shape as a non-struct base's __eq__
 * shadowing the struct base's, and it is left alone for the same reason: the
 * mixin already discarded a co-base's __ne__ before this, so what changes here
 * is which answer wins rather than whether the co-base's is heard.
 */
static enum result bind_not_equal(PyObject * const namespace, bool const answered_by_a_body) {
	static char const * const not_equal[] = {"__ne__", NULL};

	return answered_by_a_body ? rebind(namespace, not_equal, false) : RESULT_OK;
}

/*
 * The hash follows from the other two answers rather than being an option of
 * its own: the tuple of the fields for a frozen value, object's identity hash
 * where equality is identity, and None for a value that compares by value and
 * can still move -- a key whose hash moves is not a key. Settled outright
 * rather than on a transition, because it is the one name two options answer.
 */
static enum result bind_hash(
	PyObject * const namespace,
	struct options const options,
	bool const body_defines_eq,
	bool const inherits_body_eq
) {
	if (PyDict_GetItemString(namespace, "__hash__") != NULL) {
		return RESULT_OK;
	}

	/* An __eq__ that came from a class body is not salix's to answer for, and
	 * neither is the hash beside it: whatever the MRO carries -- None from
	 * Python's own rule, or a __hash__ that same body wrote -- is already
	 * right, and binding one here would replace it. */
	if (inherits_body_eq) {
		return RESULT_OK;
	}

	/* Python's rule: a body that defines __eq__ and not __hash__ is unhashable. */
	if (body_defines_eq || (options.eq && !options.frozen)) {
		return PyDict_SetItemString(namespace, "__hash__", Py_None) == 0 ? RESULT_OK : RESULT_ERROR;
	}

	static char const * const hash_name[] = {"__hash__", NULL};

	return rebind(namespace, hash_name, options.eq);
}

/* Any class-body binding of a field name -- an annotated default, or a bare
 * assignment over a name the base already declared -- would sit in this class's
 * dict ahead of the __slots__ descriptor that reads the value. */
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

	/* type_new hands off to the winning metatype's tp_new when that is not the
	 * one it was given. Where the winner would only land back in StructMeta_new,
	 * building as the winner in the first place reaches the same class without
	 * the round trip -- which re-planned from the transformed namespace and with
	 * no keywords, so the requested options and the body's defaults were gone by
	 * the time it returned. */
	PyTypeObject * const winner = winning_metatype(metatype, bases);
	PyTypeObject * const builder = winner->tp_new == StructMeta_new ? winner : metatype;
	PY_MOVABLE(created, PyType_Type.tp_new(builder, type_args, NULL));

	if (created == NULL) {
		return NULL;
	}

	/* A StructMeta subclass overriding __new__ still decides what comes back
	 * here -- and install_fields writes StructType storage into it. */
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

/*
 * type_new's own rule: the most derived of the requested metatype and the
 * bases' metatypes builds the class. CPython keeps that rule in
 * _PyType_CalculateMetaclass, which is not in the public API, so it is written
 * out here rather than called.
 *
 * Only the winner is wanted, and only to decide who builds, so a conflict needs
 * no answer here -- whatever this returns, type_new recomputes the winner and
 * raises the metaclass-conflict error it has always raised. For unrelated
 * metatypes that is the first one this locked onto rather than the requested
 * one, which is why the loop can stay this simple.
 *
 * It is the third walk of `bases` in a class creation, after find_struct_base
 * and _PyType_CalculateMetaclass. Measured, and smaller than the measurement:
 * replacing the body with `return requested` leaves class creation at 9.77-9.87
 * us for a 16-field class either way, so the walk is bounded by the width of
 * that band rather than shown to be free. It is one Py_TYPE and one
 * PyType_IsSubtype per base, and a class has one.
 */
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

/*
 * The class came back already a struct, which means something other than this
 * call built it. A metaclass __new__ written in Python is one such thing: it is
 * re-entered through type_new with no keywords and a namespace the transform
 * has already taken the body defaults out of, so the class it installs is
 * planned from less than this call was handed. The options and the defaults are
 * where that shows.
 *
 * Refused rather than returned, and rather than installed over: the field table
 * this call planned describes a layout that class may not have. Until the
 * hand-off forwards what it was given, a caller gets an error instead of a
 * class that quietly is not the one asked for.
 */
static enum result reject_unless_planned(
	StructType const * const struct_class,
	struct field_plan const * const plan,
	struct options const options
) {
	PY_OWNED(planned, PyList_AsTuple(plan->all_names));

	if (planned == NULL) {
		return RESULT_ERROR;
	}

	int const same_fields =
		PyObject_RichCompareBool(struct_class->struct_field_names, planned, Py_EQ);

	if (same_fields < 0) {
		return RESULT_ERROR;
	}

	if (
		same_fields == 1 &&
		struct_class->struct_default_count == PyTuple_GET_SIZE(plan->defaults) &&
		options_agree(struct_class->struct_options, options)
	) {
		return RESULT_OK;
	}

	PyErr_Format(
		PyExc_TypeError,
		"%.200s.__new__ returned a struct class this call did not plan: its "
		"metaclass is re-entered without the keywords or the class body's "
		"defaults, so what it built is not what was asked for",
		Py_TYPE(struct_class)->tp_name
	);

	return RESULT_ERROR;
}

/* The type exists but is not yet a struct; this is what makes it one. */
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

	struct_class->struct_field_names = py_move(&field_names);
	struct_class->struct_defaults = Py_NewRef(plan->defaults);
	struct_class->struct_slot_offsets = offsets;
	struct_class->struct_field_count = field_count;
	struct_class->struct_default_count = PyTuple_GET_SIZE(plan->defaults);
	struct_class->struct_options = options;
	struct_class->struct_resolves_body_eq = resolves_body_eq;

	/* The mixin has no tp_new, because nothing ever needed one: the vectorcall
	 * allocates. A class that declined it needs one to get as far as its own
	 * __init__ -- and only such a class, so the mixin itself stays
	 * uninstantiable and nothing can hold a struct's dunders over an object
	 * that has no field table. Struct_new rather than PyType_GenericNew,
	 * because the generic one leaves every slot NULL and the declared defaults
	 * were then never written by anything. */
	if (defines_own_init(struct_class)) {
		struct_class->heap_type.ht_type.tp_new = Struct_new;
	} else {
		struct_class->heap_type.ht_type.tp_vectorcall = Struct_vectorcall;
	}

	return install_post_init(struct_class);
}

/*
 * A body that writes its own __init__ means it. The generated constructor is
 * what a struct gets, not what it is stuck with, and leaving the vectorcall
 * installed would discard the definition in silence -- tp_call never reaches
 * tp_init once tp_vectorcall answers.
 *
 * tp_init rather than a lookup: it is object's until something in the MRO
 * defines __init__, at which point the type machinery has already replaced it
 * with the dispatching slot. That covers an inherited one for free.
 */
static bool defines_own_init(StructType const * const struct_class) {
	return struct_class->heap_type.ht_type.tp_init != PyBaseObject_Type.tp_init;
}

/* PyVectorcall_Call cannot answer for the classes that just declined the
 * vectorcall, and type.__call__ is what they want anyway. */
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

/*
 * Resolved once, here, rather than looked up per construction: the constructor
 * writes slots and returns, and an MRO walk on every instance would be the
 * largest thing in it. The cost is that a __post_init__ bound to the class
 * after it exists is not seen.
 */
static enum result install_post_init(StructType * const struct_class) {
	PyObject * const hook = optional_attribute((PyObject *) struct_class, "__post_init__");

	if (hook == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	struct_class->struct_post_init = hook;

	return RESULT_OK;
}

/* Inherited fields keep the base's offsets; new ones are wherever type.__new__
 * just placed the slots it created from __slots__. */
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
				offsets[inherited_count + i] = found.offset;
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
			return (struct member_lookup){.tag = MEMBER_LOOKUP_FOUND, .offset = members[i].offset};
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

	/* Ahead of the guard: a class whose creation failed after the hook was
	 * resolved has one to drop and no fields. */
	Py_CLEAR(struct_class->struct_post_init);

	if (struct_class->struct_field_names == NULL) {  /* already cleared */
		return RESULT_OK;
	}

	Py_CLEAR(struct_class->struct_field_names);
	Py_CLEAR(struct_class->struct_defaults);
	PyMem_Free(struct_class->struct_slot_offsets);
	struct_class->struct_slot_offsets = NULL;

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
	TEST_ASSERT_EQUAL_INT(24, found.offset);

	Py_DECREF(name);
}

static void test_a_non_ascii_member_yields_its_offset(void) {
	PyObject * const name = PyUnicode_FromString("café");
	struct member_lookup const found = find_member(example_members, 3, name);

	TEST_ASSERT_EQUAL_INT(MEMBER_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(32, found.offset);

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
	/* GC invariants require dealloc to untrack immediately, but
	 * PyType_Type.tp_dealloc assumes the type is currently tracked — hence the
	 * untrack / clear / re-track dance (mirrors msgspec's StructMeta_dealloc). */
	PyObject_GC_UnTrack(self);
	StructMeta_clear(self);
	PyObject_GC_Track(self);
	PyType_Type.tp_dealloc(self);
}
