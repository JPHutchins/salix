#include <Python.h>
#include <stdbool.h>
#include <stddef.h>

#include "meta.h"
#include "../construct/construct.h"
#include "../mixin.h"
#include "../options.h"
#include "../owned.h"
#include "../types.h"

struct definition {
	enum { DEFINITION_READ, DEFINITION_UNREADABLE } tag;
	bool found;
};

static struct equality_source equality_from_the_co_bases(PyObject * bases, Py_ssize_t first);
static struct definition base_defines(PyObject * base, PyObject * name);
static struct definition any_base_defines(PyObject * bases, Py_ssize_t first, PyObject * name);
static struct options base_options(StructType const * base);
static bool any_base_satisfies(PyObject * bases, bool (*carries)(PyTypeObject const *));

StructType * find_struct_base(PyObject * const bases) {
	StructType * widest = NULL;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (!is_struct_class(base)) {
			continue;
		}

		StructType * const candidate = (StructType *) base;

		if (widest == NULL || struct_parameter_count(candidate) > struct_parameter_count(widest)) {
			widest = candidate;
		}
	}

	return widest;
}

static PyTypeObject * solid_base(PyTypeObject * type) {
	while (
		type->tp_base != NULL &&
		type->tp_basicsize == type->tp_base->tp_basicsize &&
		type->tp_itemsize == type->tp_base->tp_itemsize
	) {
		type = type->tp_base;
	}

	return type;
}

static bool fielded_struct(PyObject * const base) {
	return is_struct_class(base) && ((StructType const *) base)->struct_field_count > 0;
}

static bool metatypes_agree(PyObject * const bases, PyTypeObject * const winner) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i += 1) {
		if (!PyType_IsSubtype(winner, Py_TYPE(PyTuple_GET_ITEM(bases, i)))) {
			return false;
		}
	}

	return true;
}

enum result refuse_two_fielded_layouts(PyObject * const bases, PyTypeObject * const winner) {
	if (!metatypes_agree(bases, winner)) {
		return RESULT_OK;
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i += 1) {
		PyObject * const first = PyTuple_GET_ITEM(bases, i);

		for (Py_ssize_t j = i + 1; fielded_struct(first) && j < PyTuple_GET_SIZE(bases); j += 1) {
			PyObject * const second = PyTuple_GET_ITEM(bases, j);

			if (
				fielded_struct(second) &&
				!PyType_IsSubtype(
					solid_base((PyTypeObject *) first),
					solid_base((PyTypeObject *) second)
				) &&
				!PyType_IsSubtype(
					solid_base((PyTypeObject *) second),
					solid_base((PyTypeObject *) first)
				)
			) {
				PyErr_Format(
					PyExc_TypeError,
					"multiple bases have instance lay-out conflict: %.200s and %.200s "
					"each add fields, and a struct keeps its fields in slots, so like "
					"dataclass(slots=True) a class can extend only one base that adds slots",
					((PyTypeObject *) first)->tp_name,
					((PyTypeObject *) second)->tp_name
				);

				return RESULT_ERROR;
			}
		}
	}

	return RESULT_OK;
}

static int carries_parameter(StructType const * const carrier, PyObject * const name) {
	struct field_lookup const field = find_field(carrier, name);
	struct field_lookup const found = (
		field.tag == FIELD_LOOKUP_MISSING ? find_init_var(carrier, name) :
		field
	);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
			return -1;
		case FIELD_LOOKUP_MISSING:
			return 0;
		case FIELD_LOOKUP_FOUND:
			break;
	}

	return 1;
}

static int declares_a_parameter_of(
	StructType const * const declaring,
	StructType const * const carrier
) {
	PyObject * const declared = declaring->struct_declared_names;

	for (Py_ssize_t i = 0; declared != NULL && i < PyTuple_GET_SIZE(declared); i += 1) {
		int const carried = carries_parameter(carrier, PyTuple_GET_ITEM(declared, i));

		if (carried != 0) {
			return carried;
		}
	}

	return 0;
}

static int declares_its_own_or_a_parameter_of(
	StructType const * const declaring,
	StructType const * const carrier
) {
	int const own = declares_a_parameter_of(declaring, declaring);

	return own != 0 ? own : declares_a_parameter_of(declaring, carrier);
}

struct declarer {
	enum { DECLARER_NONE, DECLARER_FOUND, DECLARER_ERROR } tag;
	StructType const * entry;
};

static struct declarer an_unshared_declarer(
	PyTypeObject * const owner,
	PyTypeObject * const sharer,
	StructType const * const carrier,
	int ( * const declares)(StructType const * declaring, StructType const * carrier)
) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(owner->tp_mro); i += 1) {
		PyObject * const entry = PyTuple_GET_ITEM(owner->tp_mro, i);

		if (is_struct_class(entry) && !PyType_IsSubtype(sharer, (PyTypeObject *) entry)) {
			int const declared = declares((StructType const *) entry, carrier);

			if (declared < 0) {
				return (struct declarer){.tag = DECLARER_ERROR};
			}

			if (declared == 1) {
				return (struct declarer){
					.tag = DECLARER_FOUND,
					.entry = (StructType const *) entry,
				};
			}
		}
	}

	return (struct declarer){.tag = DECLARER_NONE};
}

static struct declarer unreachable_declarer(
	StructType const * const base,
	StructType const * const candidate
) {
	PyTypeObject * const base_type = (PyTypeObject *) base;
	PyTypeObject * const candidate_type = (PyTypeObject *) candidate;

	if (PyType_IsSubtype(base_type, candidate_type)) {
		return (struct declarer){.tag = DECLARER_NONE};
	}

	struct declarer const candidate_side = an_unshared_declarer(
		candidate_type,
		base_type,
		base,
		declares_its_own_or_a_parameter_of
	);

	return (
		candidate_side.tag != DECLARER_NONE ? candidate_side :
		an_unshared_declarer(base_type, candidate_type, candidate, declares_a_parameter_of)
	);
}

enum result refuse_unreachable_parameters(
	PyObject * const bases,
	PyTypeObject * const winner,
	StructType const * const base,
	PyObject * const init_var_names
) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i += 1) {
		PyObject * const candidate = PyTuple_GET_ITEM(bases, i);

		if (
			PyTuple_GET_SIZE(init_var_names) > 0 &&
			PyType_FastSubclass((PyTypeObject *) candidate, Py_TPFLAGS_BASE_EXC_SUBCLASS)
		) {
			PyErr_Format(
				PyExc_TypeError,
				"an exception struct cannot take InitVar '%U': its args are built from its fields",
				PyTuple_GET_ITEM(init_var_names, 0)
			);

			return RESULT_ERROR;
		}

		if (
			base == NULL ||
			!is_struct_class(candidate) ||
			(StructType const *) candidate == base ||
			!metatypes_agree(bases, winner)
		) {
			continue;
		}

		struct declarer const declarer = unreachable_declarer(base, (StructType const *) candidate);

		switch (declarer.tag) {
			case DECLARER_ERROR:
				return RESULT_ERROR;
			case DECLARER_NONE:
				continue;
			case DECLARER_FOUND:
				break;
		}

		StructType const * const separate = (
			PyType_IsSubtype((PyTypeObject *) candidate, (PyTypeObject *) declarer.entry) ? base :
			(StructType const *) candidate
		);

		PyErr_Format(
			PyExc_TypeError,
			"%.200s declares parameters on a path %.200s does not inherit: a struct inherits "
			"its fields and InitVars from one struct base, here %.200s",
			struct_type_name(declarer.entry),
			struct_type_name(separate),
			struct_type_name(base)
		);

		return RESULT_ERROR;
	}

	return RESULT_OK;
}

static struct options base_options(StructType const * const base) {
	return base != NULL ? base->struct_options : options_initial();
}

StructType * find_behaviour_base(PyObject * const bases) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base)) {
			return (StructType *) base;
		}
	}

	return NULL;
}

struct equality_source resolves_body_equality(PyObject * const bases) {
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

bool any_struct_base_is_mutable(PyObject * const bases) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base) && !((StructType *) base)->struct_options.frozen) {
			return true;
		}
	}

	return false;
}

static bool carries_fielded_frozen(PyTypeObject const * const base) {
	if (!is_struct_class((PyObject *) base)) {
		return false;
	}

	StructType const * const struct_base = (StructType *) base;

	return struct_base->struct_field_count > 0 && struct_base->struct_options.frozen;
}

static bool carries_instance_dict(PyTypeObject const * const base) {
	if (base->tp_dictoffset != 0) {
		return true;
	}

#if PY_VERSION_HEX >= 0x030C0000
	return (base->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0 &&
		(base->tp_flags & Py_TPFLAGS_MANAGED_DICT) != 0;
#else
	return false;
#endif
}

struct base_survey survey_bases(PyObject * const bases) {
	struct base_survey survey = {
		.behaviour = NULL,
		.facts = {
			.fielded_frozen = false,
			.weakref_carried = false,
			.instance_dict_carried = false,
		},
	};

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (!PyType_Check(base)) {
			continue;
		}

		if (survey.behaviour == NULL && is_struct_class(base)) {
			survey.behaviour = (StructType *) base;
		}

		survey.facts.fielded_frozen |= carries_fielded_frozen((PyTypeObject *) base);
		survey.facts.weakref_carried |= carries_weakref_slot((PyTypeObject *) base);
		survey.facts.instance_dict_carried |= carries_instance_dict((PyTypeObject *) base);

		if (
			survey.behaviour != NULL &&
			survey.facts.fielded_frozen &&
			survey.facts.weakref_carried &&
			survey.facts.instance_dict_carried
		) {
			break;
		}
	}

	return survey;
}

struct options inherited_options(
	StructType const * const behaviour,
	struct base_facts const facts
) {
	struct options const from_behaviour = base_options(behaviour);

	return (struct options){
		.frozen = from_behaviour.frozen || facts.fielded_frozen,
		.eq = from_behaviour.eq,
		.order = from_behaviour.order,
		.repr = from_behaviour.repr,
		.match_args = from_behaviour.match_args,
		.weakref = from_behaviour.weakref || facts.weakref_carried,
	};
}

bool carries_weakref_slot(PyTypeObject const * const base) {
	if (base->tp_weaklistoffset != 0) {
		return true;
	}

#if PY_VERSION_HEX >= 0x030C0000
	return (base->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0 &&
		(base->tp_flags & Py_TPFLAGS_MANAGED_WEAKREF) != 0;
#else
	return false;
#endif
}

static bool any_base_satisfies(PyObject * const bases, bool (*carries)(PyTypeObject const *)) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (PyType_Check(base) && carries((PyTypeObject *) base)) {
			return true;
		}
	}

	return false;
}

static bool diverts(PyTypeObject const * const base, char const * const name) {
	switch (setter_source_of(base, name)) {
		case SETTER_SOURCE_ERROR:
			PyErr_Clear();

			return true;
		case SETTER_SOURCE_OBJECT:
		case SETTER_SOURCE_STRUCT:
			return false;
		case SETTER_SOURCE_OTHER:
			return true;
	}

	Py_UNREACHABLE();
}

static bool carries_diverting_setattro(PyTypeObject const * const base) {
	return diverts(base, "__setattr__") || diverts(base, "__delattr__");
}

bool any_base_diverts_setattro(PyObject * const bases) {
	return any_base_satisfies(bases, carries_diverting_setattro);
}
