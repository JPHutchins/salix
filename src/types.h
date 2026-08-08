#pragma once

#include <Python.h>
#include <stdbool.h>

#include "meta.h"
#include "options.h"

/* PyMemberDef only became visible through Python.h in 3.12, and the member
 * type constants gained their Py_ prefix in the same move. Both halves of that
 * rename live here, so the whole adaptation goes away together whenever the
 * floor reaches 3.12. SLOT_MEMBER_TYPE is what type.__new__ makes a __slots__
 * entry, and so what addresses one by offset. */
#if PY_VERSION_HEX < 0x030C0000
#	include <structmember.h>

enum { SLOT_MEMBER_TYPE = T_OBJECT_EX };
#else
enum { SLOT_MEMBER_TYPE = Py_T_OBJECT_EX };
#endif

/* An instance of StructMeta *is* a struct class.  We extend the heap-type
 * object with the per-type field metadata needed for fast construction and
 * the dunder methods. */
typedef struct {
	PyHeapTypeObject heap_type;

	/* tuple[str]: every field name, in order */
	PyObject * struct_field_names;

	/* tuple: defaults for the trailing fields */
	PyObject * struct_defaults;

	/* malloc'd array[field_count] of slot offsets */
	Py_ssize_t * struct_slot_offsets;

	/* Resolved __post_init__, or NULL for a class that declares none */
	PyObject * struct_post_init;

	Py_ssize_t struct_field_count;
	Py_ssize_t struct_default_count;

	/* What the class body asked for, and what a subclass inherits */
	struct options struct_options;

	/* Whether this class resolves an __eq__ that came from a class body rather
	 * than from salix. Answered once, here, because a subclass needs it and
	 * cannot re-derive it: `__hash__ is None` on the base does not say which
	 * rule put it there. */
	bool struct_resolves_body_eq;
} StructType;

/*
 * Only an instance of StructMeta has the storage declared above; every other
 * type stops at PyHeapTypeObject, and reading a field off one is a read past
 * the end of its allocation. _StructMixin is a permitted base and Struct.__mro__
 * hands it out, so a subclass of it whose metaclass is plain `type` reaches
 * every slot the mixin installs while being no such thing.
 */
static inline bool is_struct_class(PyObject * const object) {
	return PyObject_TypeCheck(object, &StructMeta_Type);
}

static inline bool is_struct(PyObject * const self) {
	return is_struct_class((PyObject *) Py_TYPE(self));
}

static inline StructType * struct_type_of(PyObject * const self) {
	return (StructType *) Py_TYPE(self);
}

static inline char const * struct_type_name(StructType const * const type) {
	return type->heap_type.ht_type.tp_name;
}

/*
 * Where field `index` lives inside an instance. Fields are plain slots, so a
 * struct is just its values laid end to end, and every read or write of one
 * goes through here.
 */
static inline PyObject * * struct_slot(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index
) {
	return (PyObject * *) ((char *) self + type->struct_slot_offsets[index]);
}

/*
 * Py_BEGIN_CRITICAL_SECTION arrived in 3.13, and expands to a bare scope on a
 * build with the GIL -- which is every build below it.
 *
 * A macro because a critical section is a lexical scope holding a stack frame
 * the interpreter links into a per-thread stack; no function can open one and
 * return with it still open. Same reason owned.h is macros.
 */
/* A class's own dict, as a strong reference either way. PyType_GetDict arrived
 * in 3.12; before it, tp_dict is the same object and the caller owns nothing,
 * so a reference is taken to make the two spellings interchangeable. */
#if PY_VERSION_HEX < 0x030C0000
static inline PyObject * struct_type_dict(PyTypeObject * const type) {
	return Py_XNewRef(type->tp_dict);
}
#else
static inline PyObject * struct_type_dict(PyTypeObject * const type) {
	return PyType_GetDict(type);
}
#endif

/*
 * A strong reference to the value, or NULL if the key is gone.
 *
 * PyDict_GetItem hands back a borrowed one, and taking a reference to it is two
 * steps: on a free-threaded build another thread can remove the key and drop
 * the last reference in between. PyDict_GetItemRef takes it under the dict's
 * own lock instead -- 3.13+, which is also every version that can have the
 * race, so the older branch is the plain read it always was.
 *
 * It also reports a failed lookup rather than swallowing it, which is why the
 * caller separates "gone" from "could not tell".
 */
static inline PyObject * dict_value_ref(PyObject * const mapping, PyObject * const key) {
#if PY_VERSION_HEX >= 0x030D0000
	PyObject * value = NULL;

	return PyDict_GetItemRef(mapping, key, &value) < 0 ? NULL : value;
#else
	return Py_XNewRef(PyDict_GetItem(mapping, key));
#endif
}

#if PY_VERSION_HEX < 0x030D0000
#	define STRUCT_BEGIN_CRITICAL_SECTION(object) {
#	define STRUCT_END_CRITICAL_SECTION() }
#	define STRUCT_BEGIN_CRITICAL_SECTION2(first, second) {
#	define STRUCT_END_CRITICAL_SECTION2() }
#else
#	define STRUCT_BEGIN_CRITICAL_SECTION(object) Py_BEGIN_CRITICAL_SECTION(object)
#	define STRUCT_END_CRITICAL_SECTION() Py_END_CRITICAL_SECTION()
#	define STRUCT_BEGIN_CRITICAL_SECTION2(first, second) Py_BEGIN_CRITICAL_SECTION2(first, second)
#	define STRUCT_END_CRITICAL_SECTION2() Py_END_CRITICAL_SECTION2()
#endif

/* One field from each of two instances, for the readers that walk them in
 * pairs. */
struct slot_pair {
	PyObject * mine;
	PyObject * theirs;
};

/*
 * A new reference to a field, taken under the same lock the writer takes.
 *
 * The write side is safe because it goes through PyMember_SetOne, which holds
 * a critical section on the instance over the store and defers the release past
 * the end of it. Every reader here loaded the slot and then increffed what it
 * found, which is two steps with a window between them: on a free-threaded
 * build a concurrent write frees the pointer inside that window, and repr and
 * == segfault. Measured, 3.14t, four writers and three readers of one kind:
 * `repr` exited 134/139/134 and `==` 139/134/134, while the same loop reading
 * through CPython's own member descriptor survived every time.
 *
 * Every acquisition is in this file, so a fifth reader cannot forget one. The
 * macros are a bare scope on a build with the GIL, so all three of these are
 * the same load and incref they always were there.
 *
 * What a reader gets is per-slot atomicity, not a snapshot of the struct: a
 * write landing between two of repr's fields renders a pair the object never
 * held. That is what reading two member descriptors in a row gives as well.
 *
 * Hash is the exception, and the only one: its loop stays in C, so one
 * acquisition spans the whole of it and what it hashes is a state the struct
 * really held. repr and the comparisons run PyObject_Repr and
 * PyObject_RichCompareBool between fields, and a section held across arbitrary
 * Python is suspended the moment the thread detaches -- so hoisting one around
 * those loops would read as a guarantee and not be one.
 *
 * A slot stays NULL until something writes it, which is observable on a
 * half-built struct, and this hands that back as NULL: repr is the caller that
 * renders it.
 */
static inline PyObject * struct_slot_ref(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index
) {
	PyObject * value;

	STRUCT_BEGIN_CRITICAL_SECTION(self);
	value = Py_XNewRef(*struct_slot(type, self, index));
	STRUCT_END_CRITICAL_SECTION();

	return value;
}

/*
 * Both sides of one field, under a single acquisition covering both instances.
 * The comparison paths walk them in pairs, and one section where there were two
 * is the whole of what the lock can be made to cost them: 3.14t, an eight-field
 * struct, `a == b` 134.5ns per field-pair section against 110.7 for one.
 *
 * Reading an unwritten slot as None keeps == total instead of making each
 * caller re-derive the guard.
 */
static inline struct slot_pair struct_slot_pair_ref(
	StructType const * const self_type,
	PyObject * const self,
	StructType const * const other_type,
	PyObject * const other,
	Py_ssize_t const index
) {
	PyObject * mine;
	PyObject * theirs;

	STRUCT_BEGIN_CRITICAL_SECTION2(self, other);
	mine = *struct_slot(self_type, self, index);
	theirs = *struct_slot(other_type, other, index);
	mine = Py_NewRef(mine != NULL ? mine : Py_None);
	theirs = Py_NewRef(theirs != NULL ? theirs : Py_None);
	STRUCT_END_CRITICAL_SECTION2();

	return (struct slot_pair){.mine = mine, .theirs = theirs};
}

/*
 * Every field into `values`, which must be a tuple of struct_field_count and
 * whose items must be unset. One acquisition rather than one per field, and so
 * a real snapshot -- hash is the reader that can have both, because nothing in
 * this loop calls back into Python. 3.14t, eight fields: 111.4ns to 81.1,
 * against 77.5 for the unsynchronised read this replaced.
 */
static inline void struct_slots_ref_or_none_into(
	StructType const * const type,
	PyObject * const self,
	PyObject * const values
) {
	STRUCT_BEGIN_CRITICAL_SECTION(self);

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * const value = *struct_slot(type, self, i);

		PyTuple_SET_ITEM(values, i, Py_NewRef(value != NULL ? value : Py_None));
	}

	STRUCT_END_CRITICAL_SECTION();
}

/* Fields below this index have no default and must be supplied by the caller. */
static inline Py_ssize_t struct_required_count(StructType const * const type) {
	return type->struct_field_count - type->struct_default_count;
}

/* Both tuples are NULL on the mixin itself, which has no fields; an empty
 * tuple is the honest answer rather than None. */
static inline PyObject * struct_tuple_or_empty(PyObject * const tuple) {
	return tuple != NULL ? Py_NewRef(tuple) : PyTuple_New(0);
}

/* Which of the two metadata tuples is being asked for. The class answers
 * through the metaclass and the instance through the mixin, so without this the
 * same read is written out four times. */
enum struct_metadata : int {
	STRUCT_FIELD_NAMES,
	STRUCT_DEFAULTS,
};

static inline PyObject * struct_metadata(
	StructType const * const type,
	enum struct_metadata const which
) {
	/* Matched rather than tested, so that a third kind of metadata is a
	 * compiler error here instead of whatever the false arm happened to be. */
	switch (which) {
		case STRUCT_FIELD_NAMES:
			return struct_tuple_or_empty(type->struct_field_names);
		case STRUCT_DEFAULTS:
			return struct_tuple_or_empty(type->struct_defaults);
	}

	Py_UNREACHABLE();
}

/* Access the PyMemberDef array that floats behind a heap type. Mirrors
 * msgspec's MS_PyHeapType_GET_MEMBERS: the members live just past the type
 * object, which (for a custom metaclass) is sized by the metaclass basicsize. */
static inline PyMemberDef * struct_heap_type_members(StructType * const type) {
	return (PyMemberDef *) ((char *) type + Py_TYPE(type)->tp_basicsize);
}
