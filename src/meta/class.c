#include <Python.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if PY_VERSION_HEX < 0x030B0000
#	include <code.h>
#else
#	include <cpython/code.h>
#endif

#include "../compare.h"
#include "../construct.h"
#include "../fields.h"
#include "meta.h"
#include "../mixin.h"
#include "../options.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"
#include "../hash.h"

static StructType * create_class(
	PyTypeObject * metatype,
	PyTypeObject * handoff,
	PyObject * name,
	PyObject * bases,
	PyObject * namespace,
	PyObject * forwarded_keywords
);
static PyTypeObject * winning_metatype(PyTypeObject * requested, PyObject * bases);
struct chain_verdict {
	int accepts_all;
	int accepts_weakref;
	bool readable;
};
static int code_accepts_keyword(PyCodeObject * code, PyObject * varnames, char const * keyword);
static PyObject * metaclass_chain(PyTypeObject * winner);
static struct chain_verdict chain_probe(PyObject * chain, PyObject * keywords, bool weakref_column);
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
static enum result settle_mro_bindings(
	StructType * struct_class,
	PyObject * bases,
	PyObject * original_namespace,
	struct binding_plan bindings,
	struct options options
);
static enum result restore_stripped(
	StructType * struct_class,
	PyObject * original_namespace,
	PyObject * class_dict,
	char const * const * const * tables
);
static bool table_names(char const * const * const names, char const * const name);
static bool settled_by_the_plan(char const * const name, struct binding_plan const plan);
static enum result refuse_unplanned(StructType const * struct_class);
static enum result install_post_init(StructType * struct_class);
static bool defines_own_init(StructType const * struct_class);
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

PyObject * build_struct_class(
	PyTypeObject * const metatype,
	StructType const * const base,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const original_namespace,
	PyObject * const keywords
) {
	StructType const * const behaviour = find_behaviour_base(bases);
	bool promised_frozen = false;
	struct options const inherited = inherited_options(bases, behaviour, &promised_frozen);
	struct options_request const request =
		options_read(keywords, inherited, promised_frozen);

	if (request.tag == OPTIONS_REJECTED) {
		return NULL;
	}

	PyTypeObject * const handoff = winning_metatype(metatype, bases);
	PyObject * forwarded_keywords = NULL;
	PY_MOVABLE(weakref_only, NULL);
	PY_MOVABLE(chain, NULL);
	struct chain_verdict verdict = {.accepts_all = 1, .accepts_weakref = 1, .readable = true};

	if (
		handoff != metatype &&
		handoff->tp_new != StructMeta_new &&
		keywords != NULL &&
		PyDict_GET_SIZE(keywords) > 0
	) {
		chain = metaclass_chain(handoff);

		if (chain == NULL) {
			return NULL;
		}

		verdict = chain_probe(chain, keywords, request.options.weakref);

		if (verdict.accepts_all < 0) {
			return NULL;
		}

		if (verdict.accepts_all == 1) {
			forwarded_keywords = keywords;
		} else if (request.options.weakref && verdict.accepts_weakref == 1) {
			weakref_only = PyDict_New();

			if (
				weakref_only == NULL ||
				PyDict_SetItemString(weakref_only, option_keywords[OPTION_WEAKREF], Py_True) < 0
			) {
				return NULL;
			}

			forwarded_keywords = weakref_only;
		}
	}

	if (
		weakref_slot_is_new(request.options, bases) &&
		handoff->tp_new != StructMeta_new &&
		verdict.accepts_weakref == 0
	) {
		PyErr_SetString(
			PyExc_TypeError,
			verdict.readable ?
				"weakref=True cannot cross a metaclass __new__ that hands the build "
				"off: the re-entered call cannot add the weakref slot" :
				"weakref=True cannot cross a metaclass __new__ that hands the build "
				"off: the chain contains a __new__ whose signature cannot be read, so "
				"the re-entered call cannot be verified to add the weakref slot"
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
		refuse_slot_name_fields(plan.new_names) != RESULT_OK ||
		refuse_displaced_slots(original_namespace, plan.all_names, bases, request.options) !=
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
			bases,
			inherited,
			frozen_across_bases,
			body_defines_eq,
			inherits_body_eq,
			inherited_equality.needs_derived_not_equal
		)
	);
	StructType * struct_class = (
		namespace != NULL ? create_class(
			metatype,
			handoff,
			name,
			bases,
			namespace,
			forwarded_keywords
		) :
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
		} else {
			struct binding_plan const bindings = binding_plan(
				request.options,
				inherited,
				frozen_across_bases,
				body_defines_eq,
				inherits_body_eq,
				inherited_equality.needs_derived_not_equal,
				PyDict_GetItemString(original_namespace, rebind_hash[0]) != NULL,
				PyDict_GetItemString(original_namespace, "__setattr__") != NULL
			);

			if (
				settle_mro_bindings(
					struct_class,
					bases,
					original_namespace,
					bindings,
					request.options
				) !=
				RESULT_OK
			) {
				Py_CLEAR(struct_class);
			}
		}
	}

	field_plan_clear(&plan);

	return (PyObject *) struct_class;
}

static StructType * create_class(
	PyTypeObject * const metatype,
	PyTypeObject * const handoff,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const namespace,
	PyObject * const forwarded_keywords
) {
	PY_OWNED(type_args, PyTuple_Pack(3, name, bases, namespace));

	if (type_args == NULL) {
		return NULL;
	}

	PyTypeObject * const builder = handoff->tp_new == StructMeta_new ? handoff : metatype;

	PY_MOVABLE(
		created,
		PyType_Type.tp_new(builder, type_args, builder == handoff ? NULL : forwarded_keywords)
	);

	if (created == NULL) {
		return NULL;
	}

	if (!is_struct_class(created)) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s.__new__ returned %.200s, which is not a struct class",
			handoff->tp_name,
			Py_TYPE(created)->tp_name
		);

		return NULL;
	}

	return (StructType *) py_move(&created);
}

static int code_accepts_keyword(
	PyCodeObject * const code,
	PyObject * const varnames,
	char const * const keyword
) {
	Py_ssize_t const named = code->co_argcount + code->co_kwonlyargcount;

	for (Py_ssize_t i = code->co_posonlyargcount; i < named; ++i) {
		int const compared = PyUnicode_CompareWithASCIIString(
			PyTuple_GET_ITEM(varnames, i),
			keyword
		);

		if (compared == 0) {
			return 1;
		}

		if (compared < 0 && PyErr_Occurred()) {
			return -1;
		}
	}

	return 0;
}

static struct chain_verdict chain_probe(
	PyObject * const chain,
	PyObject * const keywords,
	bool const weakref_column
) {
	struct chain_verdict verdict = {.accepts_all = 1, .accepts_weakref = 1, .readable = true};

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(chain); ++i) {
		PyObject * const link = PyList_GET_ITEM(chain, i);

		if (!PyFunction_Check(link)) {
			verdict.readable = false;
			verdict.accepts_all = 0;
			verdict.accepts_weakref = 0;
			continue;
		}

		PyCodeObject * const code = (PyCodeObject *) ((PyFunctionObject *) link)->func_code;

		if ((code->co_flags & CO_VARKEYWORDS) != 0) {
			continue;
		}

#if PY_VERSION_HEX >= 0x030B0000
		PY_OWNED(varnames, PyCode_GetVarnames(code));
#else
		PyObject * const varnames = code->co_varnames;
#endif

		if (varnames == NULL) {
			verdict.accepts_all = -1;

			return verdict;
		}

		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(keywords, &position, &key, &value)) {
			char const * const name = PyUnicode_AsUTF8(key);

			if (name == NULL) {
				verdict.accepts_all = -1;

				return verdict;
			}

			int const accepts = code_accepts_keyword(code, varnames, name);

			if (accepts < 0) {
				verdict.accepts_all = -1;

				return verdict;
			}

			if (accepts == 0) {
				verdict.accepts_all = 0;
				break;
			}
		}

		if (weakref_column) {
			int const accepts_weakref = code_accepts_keyword(
				code,
				varnames,
				option_keywords[OPTION_WEAKREF]
			);

			if (accepts_weakref < 0) {
				verdict.accepts_all = -1;

				return verdict;
			}

			if (accepts_weakref == 0) {
				verdict.accepts_weakref = 0;
			}
		}
	}

	return verdict;
}

static PyObject * metaclass_chain(PyTypeObject * const winner) {
	PY_MOVABLE(chain, PyList_New(0));

	if (chain == NULL) {
		return NULL;
	}

	PyObject * const mro = winner->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const link = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);

		if (link == &StructMeta_Type || link == &PyType_Type) {
			break;
		}

		if (link->tp_new == StructMeta_new) {
			continue;
		}

		PY_OWNED(new, PyObject_GetAttrString((PyObject *) link, "__new__"));

		if (new == NULL || PyList_Append(chain, new) < 0) {
			return NULL;
		}
	}

	return py_move(&chain);
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

static int struct_base_count(PyObject * const bases) {
	int count = 0;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		if (is_struct_class(PyTuple_GET_ITEM(bases, i))) {
			count += 1;
		}
	}

	return count;
}

static PyObject * mixin_bindings[7];
static PyObject * object_bindings[7];
static int settle_candidates(
	PyObject * * const mixin_eq,
	PyObject * * const object_eq,
	PyObject * * const mixin_ne,
	PyObject * * const object_ne,
	PyObject * * const mixin_repr,
	PyObject * * const object_repr
) {
	char const * const names[7] = {
		"__eq__",
		"__ne__",
		"__lt__",
		"__le__",
		"__gt__",
		"__ge__",
		"__repr__",
	};

	for (Py_ssize_t i = 0; i < 7; ++i) {
		if (mixin_bindings[i] == NULL) {
			mixin_bindings[i] = PyObject_GetAttrString((PyObject *) &StructMixin_Type, names[i]);
		}

		if (object_bindings[i] == NULL) {
			object_bindings[i] = PyObject_GetAttrString((PyObject *) &PyBaseObject_Type, names[i]);
		}
	}

	for (Py_ssize_t i = 0; i < 7; ++i) {
		if (mixin_bindings[i] == NULL || object_bindings[i] == NULL) {
			return -1;
		}
	}

	*mixin_eq = Py_NewRef(mixin_bindings[0]);
	*object_eq = Py_NewRef(object_bindings[0]);
	*mixin_ne = Py_NewRef(mixin_bindings[1]);
	*object_ne = Py_NewRef(object_bindings[1]);
	*mixin_repr = Py_NewRef(mixin_bindings[6]);
	*object_repr = Py_NewRef(object_bindings[6]);

	return 0;
}

/* What the class's MRO hands out below the class's own dict for the three
 * names, asked after the class exists so the answer is the real resolution,
 * in one walk, with the defining entry tracked for each. */
static enum result mro_dunders_of(
	PyTypeObject * const type,
	PyObject * * const resolved_eq,
	PyObject * * const resolved_ne,
	PyObject * * const resolved_repr,
	PyTypeObject * * const eq_owner,
	PyTypeObject * * const ne_owner,
	PyTypeObject * * const repr_owner
) {
	*resolved_eq = NULL;
	*resolved_ne = NULL;
	*resolved_repr = NULL;
	*eq_owner = NULL;
	*ne_owner = NULL;
	*repr_owner = NULL;

	PY_OWNED(eq_name, PyUnicode_InternFromString("__eq__"));
	PY_OWNED(ne_name, PyUnicode_InternFromString("__ne__"));
	PY_OWNED(repr_name, PyUnicode_InternFromString("__repr__"));

	if (eq_name == NULL || ne_name == NULL || repr_name == NULL) {
		return RESULT_ERROR;
	}

	PyObject * const mro = type->tp_mro;

	for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		PY_OWNED(dict, struct_type_dict(entry));

		if (dict == NULL) {
			return RESULT_ERROR;
		}

		if (*resolved_eq == NULL) {
			PyObject * const found = PyDict_GetItem(dict, eq_name);

			if (found != NULL) {
				*resolved_eq = Py_NewRef(found);
				*eq_owner = entry;
			}
		}

		if (*resolved_ne == NULL) {
			PyObject * const found = PyDict_GetItem(dict, ne_name);

			if (found != NULL) {
				*resolved_ne = Py_NewRef(found);
				*ne_owner = entry;
			}
		}

		if (*resolved_repr == NULL) {
			PyObject * const found = PyDict_GetItem(dict, repr_name);

			if (found != NULL) {
				*resolved_repr = Py_NewRef(found);
				*repr_owner = entry;
			}
		}

		if (*resolved_eq != NULL && *resolved_ne != NULL && *resolved_repr != NULL) {
			break;
		}
	}

	return RESULT_OK;
}

/* Whether a binding's owner is one the single-base path would have honoured:
 * the first struct base's own MRO chain, or a non-struct base. A later struct
 * base's binding is shadowed by the record. */
static bool honoured_owner(PyTypeObject * const owner, PyTypeObject * const first_struct) {
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
static int honours_a_body_comparison(
	PyTypeObject * const type,
	PyTypeObject * const first_struct,
	PyObject * const original_namespace
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
			PyObject * const bound = PyDict_GetItemString(dict, (char const * const[6]){
				"__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__",
			}[name]);

			if (bound != NULL && bound != mixin_bindings[name] && bound != object_bindings[name]) {
				return 1;
			}
		}
	}

	return 0;
}

/* CPython copies the comparison slots from the first base only when the new
 * class's namespace overrides nothing, so a multi-base class whose namespace
 * carries salix's own bindings falls back to the default slots. With two
 * struct bases the real MRO can also answer a dunder from a base the
 * pre-build walk never read. Both are repaired here, where the class exists
 * and the MRO is the real one. */
static enum result settle_mro_bindings(
	StructType * const struct_class,
	PyObject * const bases,
	PyObject * const original_namespace,
	struct binding_plan const bindings,
	struct options const options
) {
	PyTypeObject * const type = (PyTypeObject *) struct_class;

	/* A co-base carrying its own C-level tp_setattro can divert the child's
	 * slot away from the frozen block, so the block is restored here, unless
	 * the child's own body __setattr__ answers (the single-base escape
	 * hatch). The mutable column needs no repair: CPython's own dispatch
	 * honours body and co-base hooks and foreign C-level slots exactly as it
	 * does for plain classes. Unlike the comparison repairs below, this runs
	 * for every struct class, single or multi base. */
	if (
		options.frozen &&
		PyDict_GetItemString(original_namespace, "__setattr__") == NULL &&
		type->tp_setattro != StructMixin_Type.tp_setattro
	) {
		type->tp_setattro = StructMixin_Type.tp_setattro;
	}

	if (struct_base_count(bases) <= 1) {
		return RESULT_OK;
	}

	PyTypeObject * const first_struct = (PyTypeObject *) find_behaviour_base(bases);

	PY_MOVABLE(mixin_eq, NULL);
	PY_MOVABLE(object_eq, NULL);
	PY_MOVABLE(mixin_ne, NULL);
	PY_MOVABLE(object_ne, NULL);
	PY_MOVABLE(mixin_repr, NULL);
	PY_MOVABLE(object_repr, NULL);
	PY_MOVABLE(resolved_eq, NULL);
	PY_MOVABLE(resolved_ne, NULL);
	PY_MOVABLE(resolved_repr, NULL);
	PyTypeObject * eq_owner = NULL;
	PyTypeObject * ne_owner = NULL;
	PyTypeObject * repr_owner = NULL;

	if (
		settle_candidates(
				&mixin_eq,
				&object_eq,
				&mixin_ne,
				&object_ne,
				&mixin_repr,
				&object_repr
			) <
			0 ||
		mro_dunders_of(
				type,
				&resolved_eq,
				&resolved_ne,
				&resolved_repr,
				&eq_owner,
				&ne_owner,
				&repr_owner
			) !=
			RESULT_OK
	) {
		return RESULT_ERROR;
	}

	int const body_answers = honours_a_body_comparison(type, first_struct, original_namespace);

	if (body_answers < 0) {
		return RESULT_ERROR;
	}

	if (!body_answers && options.eq && type->tp_richcompare != Struct_rich_compare) {
		type->tp_richcompare = Struct_rich_compare;
	}

	PyObject * const target_eq = options.eq ? mixin_eq : object_eq;
	bool const eq_is_salix_owned = resolved_eq == mixin_eq || resolved_eq == object_eq;

	if (
		resolved_eq != target_eq &&
		(eq_is_salix_owned || !honoured_owner(eq_owner, first_struct))
	) {
		if (
			settle_rebind(struct_class, original_namespace, rebind_comparison, options.eq) !=
			RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	bool const body_eq_answers = !eq_is_salix_owned && honoured_owner(eq_owner, first_struct);

	PyObject * const target_ne = body_eq_answers ? object_ne : options.eq ? mixin_ne : object_ne;
	bool const ne_is_salix_owned = resolved_ne == mixin_ne || resolved_ne == object_ne;

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
			) !=
			RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	PyObject * const target_repr = options.repr ? mixin_repr : object_repr;
	bool const repr_is_salix_owned = resolved_repr == mixin_repr || resolved_repr == object_repr;

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
			) !=
			RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	if (body_eq_answers && bindings.hash == HASH_BIND) {
		/* The plan bound the structural hash beside a honoured body __eq__ it
		 * never saw. Python's own rule pairs that equality with unhashability. */
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
		same_name == 0 ||
		same_bases == 0 ||
		find_struct_base(((PyTypeObject *) struct_class)->tp_bases) != base
	) {
		return refuse_unplanned(struct_class);
	}

	bool const carries_slot = carries_weakref_slot((PyTypeObject *) struct_class);

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
		PyDict_GetItemString(original_namespace, rebind_hash[0]) != NULL,
		PyDict_GetItemString(original_namespace, "__setattr__") != NULL
	);

	for (char const * const * const * tables = settle_tables; *tables != NULL; tables += 1) {
		for (char const * const * name = *tables; *name != NULL; name += 1) {
			if (
				PyDict_GetItemString(class_dict, *name) != NULL &&
				PyDict_GetItemString(original_namespace, *name) == NULL &&
				!settled_by_the_plan(*name, bindings)
			) {
				return refuse_unplanned(struct_class);
			}
		}
	}

	if (
		restore_stripped(struct_class, original_namespace, class_dict, settle_tables) !=
		RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_comparison &&
		settle_rebind(struct_class, original_namespace, rebind_comparison, options.eq) !=
			RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_not_equal &&
		settle_rebind(struct_class, original_namespace, rebind_not_equal, options.eq) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	/* bind_not_equal's answered case on a live class: the fresh build binds
	 * object's __ne__ when the body answered equality itself. */
	if (
		bindings.answered_by_body &&
		PyDict_GetItemString(original_namespace, rebind_not_equal[0]) == NULL &&
		settle_rebind(struct_class, original_namespace, rebind_not_equal, false) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_representation &&
		settle_rebind(struct_class, original_namespace, rebind_representation, options.repr) !=
			RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		bindings.rebind_mutability &&
		settle_rebind(struct_class, original_namespace, rebind_mutability, options.frozen) !=
			RESULT_OK
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
				settle_rebind(struct_class, original_namespace, rebind_hash, options.eq) !=
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
	char const * const * const * const tables
) {
	for (char const * const * const * table = tables; *table != NULL; table += 1) {
		for (char const * const * name = *table; *name != NULL; name += 1) {
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

static enum result install_post_init(StructType * const struct_class) {
	PyObject * const hook = optional_attribute((PyObject *) struct_class, "__post_init__");

	if (hook == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	Py_XSETREF(struct_class->struct_post_init, hook);

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

#ifdef TESTING

#	include "../testing.h"

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

static PyObject * struct_class_with_field(PyObject * bases, PyObject * keywords) {
	PY_OWNED(name, PyUnicode_FromString("Built"));
	PY_OWNED(namespace, PyDict_New());
	PY_OWNED(annotations, PyDict_New());

	if (
		PyDict_SetItemString(annotations, "x", (PyObject *) &PyLong_Type) < 0 ||
		PyDict_SetItemString(namespace, "__annotations__", annotations) < 0
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

	PY_OWNED(mutable_base, struct_class_with_field(struct_bases, mutable_keywords));
	TEST_ASSERT_NOT_NULL(mutable_base);

	PY_OWNED(raw_bases, PyTuple_Pack(2, (PyObject *) &SwallowingType, mutable_base));
	PY_OWNED(mutable_child, struct_class_with_field(raw_bases, NULL));
	TEST_ASSERT_NOT_NULL(mutable_child);

	PY_OWNED(instance, PyObject_CallFunction(mutable_child, "i", 1));
	TEST_ASSERT_NOT_NULL(instance);
	PY_OWNED(nine, PyLong_FromLong(9));
	PY_OWNED(field_name, PyUnicode_FromString("x"));
	TEST_ASSERT_EQUAL_INT(0, PyObject_SetAttr(instance, field_name, nine));
	TEST_ASSERT_EQUAL_INT(1, swallow_calls);
	TEST_ASSERT_EQUAL_INT(0, PyObject_DelAttr(instance, field_name));
	TEST_ASSERT_EQUAL_INT(2, swallow_calls);

	PY_OWNED(frozen_base, struct_class_with_field(struct_bases, NULL));
	TEST_ASSERT_NOT_NULL(frozen_base);

	PY_OWNED(frozen_bases, PyTuple_Pack(2, (PyObject *) &SwallowingType, frozen_base));
	PY_OWNED(frozen_child, struct_class_with_field(frozen_bases, NULL));
	TEST_ASSERT_NOT_NULL(frozen_child);
	TEST_ASSERT_EQUAL_PTR(
		StructMixin_Type.tp_setattro,
		((PyTypeObject *) frozen_child)->tp_setattro
	);

	PY_OWNED(frozen_instance, PyObject_CallFunction(frozen_child, "i", 1));
	TEST_ASSERT_NOT_NULL(frozen_instance);
	TEST_ASSERT_EQUAL_INT(-1, PyObject_SetAttr(frozen_instance, field_name, nine));
	PyErr_Clear();
	TEST_ASSERT_EQUAL_INT(-1, PyObject_DelAttr(frozen_instance, field_name));
	PyErr_Clear();
	TEST_ASSERT_EQUAL_INT(2, swallow_calls);
}

void class_tests(void) {
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_a_raw_tp_setattro_co_base_does_not_divert_the_struct_slot);
}

#endif
