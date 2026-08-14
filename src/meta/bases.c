#include <Python.h>
#include <stdbool.h>
#include <stddef.h>

#include "meta.h"
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

StructType * find_struct_base(PyObject * const bases) {
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

bool any_struct_base_is_mutable(PyObject * const bases) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (is_struct_class(base) && !((StructType *) base)->struct_options.frozen) {
			return true;
		}
	}

	return false;
}

struct options inherited_options(
	PyObject * const bases,
	StructType const * const behaviour,
	bool * const promised_frozen
) {
	struct options const from_behaviour = base_options(behaviour);

	*promised_frozen = false;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);
		StructType const * const struct_base = (StructType *) base;

		*promised_frozen |= (
			is_struct_class(base) &&
			struct_base->struct_field_count > 0 &&
			struct_base->struct_options.frozen
		);
	}

	return (struct options){
		.frozen = from_behaviour.frozen || *promised_frozen,
		.eq = from_behaviour.eq,
		.order = from_behaviour.order,
		.repr = from_behaviour.repr,
		.match_args = from_behaviour.match_args,
		.weakref = from_behaviour.weakref,
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

static bool any_base_satisfies(PyObject * const bases, bool (*carries)(PyTypeObject const *)) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyObject * const base = PyTuple_GET_ITEM(bases, i);

		if (PyType_Check(base) && carries((PyTypeObject *) base)) {
			return true;
		}
	}

	return false;
}

bool any_base_has_weakref_slot(PyObject * const bases) {
	return any_base_satisfies(bases, carries_weakref_slot);
}

bool weakref_expected(struct options const options, PyObject * const bases) {
	return options.weakref || any_base_satisfies(bases, carries_weakref_slot);
}

bool weakref_slot_is_new(struct options const options, PyObject * const bases) {
	return options.weakref && !any_base_satisfies(bases, carries_weakref_slot);
}

bool any_base_has_instance_dict(PyObject * const bases) {
	return any_base_satisfies(bases, carries_instance_dict);
}
