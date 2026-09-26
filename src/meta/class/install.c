#include <Python.h>
#include <stdbool.h>

#include "../../construct.h"
#include "../../fields.h"
#include "../meta.h"
#include "../../options.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"

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

enum result install_fields(
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
	struct_class->struct_annotations = Py_NewRef(plan->annotations);
	struct_class->struct_metadata = Py_NewRef(plan->metadata);
	struct_class->struct_slot_offsets = offsets;
	struct_class->struct_member_offsets = member_offsets;
	struct_class->struct_member_count = member_count;
	struct_class->struct_field_count = field_count;
	struct_class->struct_default_count = PyTuple_GET_SIZE(plan->defaults);
	struct_class->struct_options = options;
	struct_class->struct_resolves_body_eq = resolves_body_eq;

	return RESULT_OK;
}

static bool author_new_in_chain(PyTypeObject * const cls) {
	/* The effective tp_new comes down the solid-base chain (type_new copies
	 * it from the first solid base), so the first heap entry in that chain
	 * whose dict defines __new__ is the one whose __new__ answers; a static
	 * builtin ends the chain and its tp_new is the family's. */
	for (PyTypeObject * entry = cls; entry != NULL; entry = entry->tp_base) {
		if ((entry->tp_flags & Py_TPFLAGS_HEAPTYPE) == 0) {
			break;
		}

		int const present = dict_has_string(entry->tp_dict, "__new__");

		if (present < 0) {
			/* A probe that cannot see has not learned presence; the
			 * conservative answer keeps the fallback, and the probe error
			 * cannot ride the class statement. */
			PyErr_Clear();

			return false;
		}

		if (present != 0) {
			return true;
		}
	}

	return false;
}

static bool family_owns_in_mro(PyTypeObject * const cls);
static bool group_family_in_mro(PyTypeObject * const cls);

enum result install_constructor(
	StructType * const struct_class,
	PyObject * const namespace,
	bool const bases_divert_setattro
) {
	bool const own_init = defines_own_init(struct_class, namespace);
	struct_class->struct_own_init = own_init;

	/* A body __new__ = None is the cannot-create marker; the flag is the
	 * one record of it, so a subclass inheriting the NULL slot inherits
	 * the refusal instead of the mixin's slotless NULL being re-set to
	 * object's own. The class's own dict is the record -- the settle may
	 * move the body's entries out of the original namespace before this
	 * probe runs. */
	PyObject * const new_entry = dict_get_string(
		struct_class->heap_type.ht_type.tp_dict,
		"__new__"
	);

	if (new_entry == NULL && PyErr_Occurred()) {
		/* A probe that cannot see has not learned a marker; the error
		 * cannot ride the class statement. */
		PyErr_Clear();
	}

	bool cannot_create = new_entry == Py_None;

	/* The nearest struct class whose dict defines __new__ decides the
	 * marker, CPython's own slot semantics: a body __new__ overrides it
	 * and the override is inheritable. The ancestors answer only when
	 * the class's own dict carries no entry. */
	if (new_entry == NULL) {
		for (
			PyTypeObject * chain = struct_class->heap_type.ht_type.tp_base;
			chain != NULL && is_struct_class((PyObject *) chain);
			chain = chain->tp_base
		) {
			PyObject * const entry = dict_get_string(chain->tp_dict, "__new__");

			if (entry == NULL && PyErr_Occurred()) {
				PyErr_Clear();

				continue;
			}

			if (entry == Py_None) {
				cannot_create = true;
			}

			break;
		}
	}

	struct_class->struct_cannot_create = cannot_create;

	if (own_init) {
		/* The wrapped init fills the defaults and writes the positional
		 * payload before the author's or the family's own init answers;
		 * tp_new stays the pre-install slot -- the body's, the family's
		 * (whose C member writes are the construction), or object's own.
		 * No class ever carries a salix slot as tp_new, so a body
		 * __new__'s super() chain passes object_new's own guard in every
		 * subclass shape. A struct base that is itself own-init hands
		 * down the wrapper, so the capture resolves through the struct
		 * ancestors to the init the first own-init ancestor captured. */
		initproc captured_init = struct_class->heap_type.ht_type.tp_init;

		for (
			PyTypeObject * chain = struct_class->heap_type.ht_type.tp_base;
			captured_init == Struct_init_wrapper &&
				chain != NULL &&
				is_struct_class((PyObject *) chain);
			chain = chain->tp_base
		) {
			captured_init = ((StructType *) chain)->struct_installed_init;
		}

		struct_class->struct_installed_init = captured_init;
		struct_class->heap_type.ht_type.tp_init = Struct_init_wrapper;
		struct_class->heap_type.ht_type.tp_vectorcall = NULL;

		/* A marker class keeps the slot type_new gave it, CPython's own
		 * shape: the plain class's dispatch slot, which subtype_new's
		 * staticbase walk climbs past. The construction guards read the
		 * cached flag instead of the slot. */
	} else {
		/* The root inherits the mixin's NULL tp_new, which object_new's
		 * guard refuses on a body __new__'s super() chain; object's own
		 * answers. The marker's slot stays whatever type_new gave it, the
		 * same rule the own-init arm follows. */
		if (!cannot_create && struct_class->heap_type.ht_type.tp_new == NULL) {
			struct_class->heap_type.ht_type.tp_new = PyBaseObject_Type.tp_new;
		}

		struct_class->heap_type.ht_type.tp_vectorcall = Struct_vectorcall;
	}

	/* Both answers are fixed at class creation, so the construction and
	 * copy paths read them instead of re-walking per call. */
	struct_class->struct_author_new = author_new_in_chain(&struct_class->heap_type.ht_type);
	struct_class->struct_family_owned = family_owns_in_mro(&struct_class->heap_type.ht_type);
	struct_class->struct_group_family = group_family_in_mro(&struct_class->heap_type.ht_type);

	/* The group members' field sources resolve once: the carry reads the
	 * bound message and exceptions fields by index, so no construction
	 * allocates the names or re-scans the field table. */
	struct_class->struct_message_index = -1;
	struct_class->struct_exceptions_index = -1;

	if (struct_class->struct_field_names != NULL) {
		PY_OWNED(message_name, PyUnicode_FromString("message"));
		PY_OWNED(exceptions_name, PyUnicode_FromString("exceptions"));

		if (message_name == NULL || exceptions_name == NULL) {
			return RESULT_ERROR;
		}

		for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(struct_class->struct_field_names); ++i) {
			PyObject * const name = PyTuple_GET_ITEM(struct_class->struct_field_names, i);

			if (struct_class->struct_message_index < 0) {
				int const matches = PyObject_RichCompareBool(name, message_name, Py_EQ);

				if (matches < 0) {
					return RESULT_ERROR;
				}

				struct_class->struct_message_index = matches == 1 ? i : -1;
			}

			if (struct_class->struct_exceptions_index < 0) {
				int const matches = PyObject_RichCompareBool(name, exceptions_name, Py_EQ);

				if (matches < 0) {
					return RESULT_ERROR;
				}

				struct_class->struct_exceptions_index = matches == 1 ? i : -1;
			}
		}
	}

	if (install_post_init(struct_class) != RESULT_OK) {
		return RESULT_ERROR;
	}

	return ensure_singleton(struct_class, namespace, bases_divert_setattro);
}

enum result ensure_singleton(
	StructType * const struct_class,
	PyObject * const namespace,
	bool const bases_divert_setattro
) {
	/* A body __new__ = None is the cannot-create marker, not the
	 * slotless allocation the singleton interns; a probe error forfeits
	 * the intern without riding the class statement. */
	int const new_present = dict_has_string(namespace, "__new__");

	if (new_present < 0) {
		PyErr_Clear();
	}

	bool const qualifies = (
		struct_class->struct_options.frozen &&
		!struct_class->struct_options.weakref &&
		struct_class->struct_field_count == 0 &&
		!struct_class->struct_own_init &&
		!struct_class->struct_cannot_create &&
		(
			struct_class->heap_type.ht_type.tp_new == NULL ||
			struct_class->heap_type.ht_type.tp_new == PyBaseObject_Type.tp_new
		) &&
		new_present == 0 &&
		struct_class->struct_member_count == 0 &&
		!bases_divert_setattro &&
		Py_TYPE(struct_class)->tp_call == StructMeta_Type.tp_call
	);

	if (!qualifies) {
		Py_CLEAR(struct_class->struct_singleton);

		return RESULT_OK;
	}

	if (struct_class->struct_singleton != NULL) {
		return RESULT_OK;
	}

	PY_MOVABLE(singleton, PyObject_CallNoArgs((PyObject *) struct_class));

	if (singleton == NULL) {
		return RESULT_ERROR;
	}

	if (Py_TYPE(singleton) != (PyTypeObject *) struct_class) {
		PyErr_SetString(
			PyExc_SystemError,
			"salix internal error: the singleton build returned a different type"
		);

		return RESULT_ERROR;
	}

	struct_class->struct_singleton = py_move(&singleton);

	return RESULT_OK;
}

enum init_owner { INIT_OWNER_NONE, INIT_OWNER_AUTHOR, INIT_OWNER_FIELD_CONSTRUCTOR };

static enum init_owner static_exception_owner(PyTypeObject * const base) {
	if (base->tp_init == PyBaseObject_Type.tp_init) {
		return INIT_OWNER_NONE;
	}

	PyTypeObject * const base_exception = (PyTypeObject *) PyExc_BaseException;
	bool const args_only_init = (base->tp_init == NULL || base->tp_init == base_exception->tp_init);
	bool const group_new =
#if PY_VERSION_HEX >= 0x030B0000
		PyExc_BaseExceptionGroup != NULL &&
		base->tp_new == ((PyTypeObject *) PyExc_BaseExceptionGroup)->tp_new
#else
		false
#endif
		;

	/* The field constructor answers beside the args-only construction --
	 * BaseException's own init, or the inherited NULL slot -- and beside
	 * BaseExceptionGroup's two-argument __new__, which is the construction
	 * itself: the vectorcall invokes it with the real shape and the rejected
	 * shapes fall back to the allocation. A family whose own C init writes
	 * members (SyntaxError, UnicodeDecodeError, OSError, ...) owns the
	 * construction. */
	return (args_only_init || group_new) ? INIT_OWNER_FIELD_CONSTRUCTOR : INIT_OWNER_AUTHOR;
}

static enum init_owner base_init_owner(PyTypeObject * const base) {
	PyObject * const dict = base->tp_dict;

	if (dict == NULL) {
		/* Static builtins expose no tp_dict to C; their C slots carry the
		 * classification. */
		if (PyType_FastSubclass(base, Py_TPFLAGS_BASE_EXC_SUBCLASS)) {
			return static_exception_owner(base);
		}

		if (base->tp_init != PyBaseObject_Type.tp_init) {
			return INIT_OWNER_AUTHOR;
		}

		return INIT_OWNER_NONE;
	}

	if (base->tp_init == PyBaseObject_Type.tp_init) {
		/* Whatever the dict says, the effective init is object's -- the
		 * generated-constructor case. */
		return INIT_OWNER_NONE;
	}

	PY_OWNED(init_value, Py_XNewRef(dict_get_string(dict, "__init__")));

	if (init_value == NULL) {
		if (PyErr_Occurred()) {
			/* A probe that cannot see has not learned absence; the
			 * conservative answer owns the construction, with the probe
			 * error cleared so it cannot ride the class statement. */
			PyErr_Clear();

			return INIT_OWNER_AUTHOR;
		}

		/* No entry: an exception base keeps walking toward the family; any
		 * other base with a non-object init owns it -- the same answer the
		 * NULL-dict branch gives, so a static C base whose dict is readable
		 * on one version and not on another flips nothing. */
		return PyType_FastSubclass(base, Py_TPFLAGS_BASE_EXC_SUBCLASS) ? INIT_OWNER_NONE :
			INIT_OWNER_AUTHOR;
	}

	if (PyType_FastSubclass(base, Py_TPFLAGS_BASE_EXC_SUBCLASS)) {
		/* A heap-type exception base carries only its own members: the
		 * entry is the author's. A static builtin's entry is the family's
		 * own wrapper, which the family's C slots classify. */
		return (base->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0 ? INIT_OWNER_AUTHOR :
			static_exception_owner(base);
	}

	return INIT_OWNER_AUTHOR;
}

static enum init_owner namespace_init_owner(PyTypeObject * const type, PyObject * const namespace) {
	int const present = dict_has_string(namespace, "__init__");

	if (present < 0) {
		/* The probe error cannot ride the class statement; the conservative
		 * answer owns the construction. */
		PyErr_Clear();

		return INIT_OWNER_AUTHOR;
	}

	return present == 1 ? INIT_OWNER_AUTHOR : INIT_OWNER_NONE;
}

static bool group_family_in_mro(PyTypeObject * const cls) {
#if PY_VERSION_HEX >= 0x030B0000
	PyObject * const mro = cls->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		if (
			PyExc_BaseExceptionGroup != NULL &&
			(PyTypeObject *) PyTuple_GET_ITEM(mro, i) == (PyTypeObject *) PyExc_BaseExceptionGroup
		) {
			return true;
		}
	}
#endif

	return false;
}

static bool family_owns_in_mro(PyTypeObject * const cls) {
	PyObject * const mro = cls->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		enum init_owner const owner = base_init_owner(entry);

		if (owner == INIT_OWNER_NONE) {
			continue;
		}

		return (
			(entry->tp_flags & Py_TPFLAGS_HEAPTYPE) == 0 &&
			PyType_FastSubclass(entry, Py_TPFLAGS_BASE_EXC_SUBCLASS) &&
			owner == INIT_OWNER_AUTHOR
		);
	}

	return false;
}

bool defines_own_init(StructType * const struct_class, PyObject * const namespace) {
	PyTypeObject * const type = &struct_class->heap_type.ht_type;

	if (type->tp_init == PyBaseObject_Type.tp_init) {
		return false;
	}

	/* One C3 walk answers creation and runtime alike: the nearest class
	 * whose dict defines __init__ owns the construction sequence. Pointer
	 * identity cannot find it: every Python __init__ shares slot_tp_init.
	 * An author __init__ is a Python function in the dict; an exception's
	 * own init is a wrapper descriptor whose family the C slots classify;
	 * anything else owns it too. The class body is the namespace while the
	 * type is being built, the installed tp_dict after. */
	PyObject * const mro = type->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);
		enum init_owner const owner = (
			i == 0 && namespace != NULL ? namespace_init_owner(entry, namespace) :
			base_init_owner(entry)
		);

		if (owner != INIT_OWNER_NONE) {
			return owner == INIT_OWNER_AUTHOR;
		}
	}

	/* Nothing in the chain defines an init: the generated constructor is
	 * what answers. */
	return false;
}

enum result install_post_init(StructType * const struct_class) {
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
