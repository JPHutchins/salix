#include <Python.h>

#include "compare.h"
#include "hash.h"
#include "mixin.h"
#include "repr.h"
#include "result.h"
#include "types.h"

static int Struct_set_attribute(PyObject * self, PyObject * name, PyObject * value);
static PyObject * Struct_get_field_names(PyObject * self, void * closure);
static PyObject * Struct_get_defaults(PyObject * self, void * closure);
static PyObject * Struct_get_fields_as_msgspec(PyObject * self, void * closure);
static PyObject * Struct_get_defaults_as_msgspec(PyObject * self, void * closure);
static PyObject * metadata_of(PyObject * self, enum struct_metadata which, char const * name);
static PyGetSetDef Struct_getset[];

PyTypeObject StructMixin_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._StructMixin",
	.tp_basicsize = sizeof(PyObject),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_setattro = Struct_set_attribute,
	.tp_repr = Struct_repr,
	.tp_hash = Struct_hash,
	.tp_richcompare = Struct_rich_compare,
	.tp_getset = Struct_getset,
};

/*
 * A sunder for salix's own metadata, and the dunder beside it for msgspec's.
 *
 * The language reference reserves `__name__` for the interpreter and its
 * implementation, and PEP 8 says never to invent one -- only to use a
 * documented one. So the pair salix defines is `_struct_fields_`, and
 * `__struct_fields__` is here because msgspec spells it that way and code
 * written against msgspec reads it. Using another project's documented name is
 * the half PEP 8 permits; minting one is not.
 */
static PyGetSetDef Struct_getset[] = {
	{
		.name = "_struct_fields_",
		.get = Struct_get_field_names,
		.doc = "tuple of field names",
	},
	{
		.name = "_struct_defaults_",
		.get = Struct_get_defaults,
		.doc = "tuple of trailing defaults",
	},
	{
		.name = "__struct_fields__",
		.get = Struct_get_fields_as_msgspec,
		.doc = "tuple of field names, under msgspec's name for it",
	},
	{
		.name = "__struct_defaults__",
		.get = Struct_get_defaults_as_msgspec,
		.doc = "tuple of trailing defaults, under msgspec's name for it",
	},
	{.name = NULL},
};

/*
 * Everything below asks whether it is looking at a struct, because the mixin is
 * a permitted base and a subclass of it need not be one. Where there is no
 * struct, repr falls back to object's rather than raising -- raising out of
 * repr would turn an object that is merely useless into one a debugger cannot
 * print.
 *
 * object's, specifically, and not the co-base's: an impostor over `list` is
 * given object's repr, where a plain list subclass has its own. Deferring to
 * whatever the co-base defines would mean walking the MRO for something to
 * borrow, on a path reachable only by subclassing a private class.
 *
 * Hash is the exception: it cannot be a local decision, because hashing and
 * equality have to agree and equality here is the co-base's. It refuses
 * instead. See src/hash.c.
 *
 * Setattr takes object's too, and that one is worth saying out loud because it
 * *overrides* rather than falls back: a co-base with its own `__setattr__`
 * never sees the write. Measured, over a `list` subclass that records every
 * name set on it -- the co-base alone records `['z']`, the impostor records
 * nothing and the value lands anyway. Same reason as repr: honouring it means
 * walking the MRO for a setter to borrow, on a path reachable only by
 * subclassing a private class.
 *
 * Equality is left intransitive by all of this, and refusing to hash does not
 * change it. `rich_compare` answers NotImplemented for a non-struct, so two
 * content-equal impostors fall back to identity and compare unequal, while each
 * compares equal to the plain value they wrap through the co-base's reflected
 * `__eq__`. Pinned in tests/test_struct_identity.py rather than left to be
 * discovered, because it is a consequence of the fallback policy and not a
 * separate decision.
 *
 * The metadata getsets are the exception either way -- all four of them, the
 * two salix defines and the two it answers to for msgspec's sake: they report
 * struct metadata and nothing else, so there is nothing to fall back to and
 * they raise. An AttributeError rather than a TypeError, because "this object
 * does not have that attribute" is what happened and it is what `hasattr` and
 * `getattr`'s default are written to catch.
 */
static PyObject * metadata_of(
	PyObject * const self,
	enum struct_metadata const which,
	char const * const name
) {
	if (is_struct(self)) {
		return struct_metadata(struct_type_of(self), which);
	}

	PyErr_Format(
		PyExc_AttributeError,
		"%s is defined on structs, and %.200s is not one",
		name,
		Py_TYPE(self)->tp_name
	);

	return NULL;
}

/* Structs are frozen, so every write fails.  A NULL value is how CPython
 * spells `del`, which is the only thing the two messages differ over. */
static int Struct_set_attribute(
	PyObject * const self,
	PyObject * const name,
	PyObject * const value
) {
	if (!is_struct(self)) {
		return PyObject_GenericSetAttr(self, name, value);
	}

	PyErr_Format(
		PyExc_TypeError,
		"%.200s object does not support attribute %s",
		Py_TYPE(self)->tp_name,
		value == NULL ? "deletion" : "assignment"
	);

	return RESULT_ERROR;
}

/* Four entry points because the getset table names four attributes and hands
 * the getter no way to tell which one was asked for; there are two answers. The
 * name is spelled here rather than passed through `closure` so that a lookup
 * that fails is refused under the name the caller used. */
static PyObject * Struct_get_field_names(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_FIELD_NAMES, "_struct_fields_");
}

static PyObject * Struct_get_defaults(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_DEFAULTS, "_struct_defaults_");
}

static PyObject * Struct_get_fields_as_msgspec(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_FIELD_NAMES, "__struct_fields__");
}

static PyObject * Struct_get_defaults_as_msgspec(PyObject * const self, void * const closure) {
	return metadata_of(self, STRUCT_DEFAULTS, "__struct_defaults__");
}
