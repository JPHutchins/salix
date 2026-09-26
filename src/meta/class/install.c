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

enum result install_constructor(
	StructType * const struct_class,
	PyObject * const namespace,
	bool const bases_divert_setattro
) {
	if (defines_own_init(struct_class, namespace)) {
		/* A body __new__ owns the allocation: the author's slot stays, and
		 * its super() chain reaches the family's __new__. The probe error
		 * keeps the slot too -- the conservative answer runs the author's. */
		int const defines_new = dict_has_string(namespace, "__new__");

		if (defines_new == 0) {
			struct_class->heap_type.ht_type.tp_new = Struct_new;
		}

		struct_class->heap_type.ht_type.tp_vectorcall = NULL;
	} else {
		struct_class->heap_type.ht_type.tp_vectorcall = Struct_vectorcall;
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
	bool const qualifies = (
		struct_class->struct_options.frozen &&
		!struct_class->struct_options.weakref &&
		struct_class->struct_field_count == 0 &&
		!defines_own_init(struct_class, namespace) &&
		struct_class->heap_type.ht_type.tp_new == NULL &&
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
