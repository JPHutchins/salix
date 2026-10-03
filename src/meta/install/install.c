#include <Python.h>
#include <stdbool.h>

#include "../../construct.h"
#include "../../construct/construct.h"
#include "../../fields.h"
#include "../meta.h"
#include "../../options.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"
#include "install.h"

static Py_ssize_t * resolve_slot_offsets(
	StructType * struct_class,
	StructType const * base,
	PyObject * field_names,
	PyObject * new_names
);
static Py_ssize_t * resolve_member_offsets(
	StructType * struct_class,
	Py_ssize_t const * slot_offsets,
	Py_ssize_t field_count,
	Py_ssize_t * member_count
);

static enum parameter_kind * parameter_kinds_of(PyObject * const init_var_flags) {
	Py_ssize_t const parameter_count = PyTuple_GET_SIZE(init_var_flags);
	enum parameter_kind * const kinds = PyMem_New(enum parameter_kind, parameter_count);

	if (kinds == NULL) {
		PyErr_NoMemory();

		return NULL;
	}

	for (Py_ssize_t i = 0; i < parameter_count; i += 1) {
		kinds[i] = (
			PyTuple_GET_ITEM(init_var_flags, i) == Py_True ? PARAMETER_INIT_VAR :
			PARAMETER_FIELD
		);
	}

	return kinds;
}

enum result install_init_vars(
	StructType * const struct_class,
	struct field_plan const * const plan
) {
	Py_ssize_t const init_var_count = PyTuple_GET_SIZE(plan->init_var_names);
	enum parameter_kind * const kinds = (
		init_var_count > 0 ? parameter_kinds_of(plan->init_var_flags) :
		NULL
	);

	if (init_var_count > 0 && kinds == NULL) {
		return RESULT_ERROR;
	}

	PyMem_Free(struct_class->struct_parameter_kinds);
	struct_class->struct_parameter_kinds = kinds;
	Py_XSETREF(struct_class->struct_init_var_names, Py_NewRef(plan->init_var_names));
	Py_XSETREF(struct_class->struct_init_var_defaults, Py_NewRef(plan->init_var_defaults));
	Py_XSETREF(struct_class->struct_init_var_annotations, Py_NewRef(plan->init_var_annotations));
	struct_class->struct_init_var_count = init_var_count;

	return RESULT_OK;
}

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
		resolve_slot_offsets(struct_class, base, field_names, plan->new_names);

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

	if (install_init_vars(struct_class, plan) != RESULT_OK) {
		PyMem_Free(member_offsets);
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
	for (PyTypeObject * entry = cls; entry != NULL; entry = entry->tp_base) {
		if ((entry->tp_flags & Py_TPFLAGS_HEAPTYPE) == 0) {
			break;
		}

		int const present = dict_has_string(entry->tp_dict, "__new__");

		if (present < 0) {
			PyErr_Clear();

			return false;
		}

		if (present != 0) {
			return true;
		}
	}

	return false;
}

static bool resolves_new_to_none(PyTypeObject * const cls) {
	PyObject * const mro = cls->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i += 1) {
		PY_OWNED(entry_dict, struct_type_dict((PyTypeObject *) PyTuple_GET_ITEM(mro, i)));
		PyObject * const entry = (
			entry_dict != NULL ? dict_get_string(entry_dict, "__new__") :
			NULL
		);

		if (entry != NULL) {
			return entry == Py_None;
		}

		PyErr_Clear();
	}

	return false;
}

enum result install_constructor(
	StructType * const struct_class,
	PyObject * const namespace,
	bool const bases_divert_setattro
) {
	bool const own_init = defines_own_init(struct_class, namespace);
	struct_class->struct_own_init = own_init;

	bool const cannot_create = resolves_new_to_none(&struct_class->heap_type.ht_type);

	struct_class->struct_cannot_create = cannot_create;

	if (own_init) {
		initproc captured_init = struct_class->heap_type.ht_type.tp_init;

		for (
			PyTypeObject * chain = struct_class->heap_type.ht_type.tp_base;
			(
				captured_init == Struct_init_wrapper &&
				chain != NULL &&
				is_struct_class((PyObject *) chain)
			);
			chain = chain->tp_base
		) {
			captured_init = ((StructType *) chain)->struct_installed_init;
		}

		struct_class->struct_installed_init = captured_init;
		struct_class->heap_type.ht_type.tp_init = Struct_init_wrapper;
		struct_class->heap_type.ht_type.tp_vectorcall = NULL;
	} else {
		if (!cannot_create && struct_class->heap_type.ht_type.tp_new == NULL) {
			struct_class->heap_type.ht_type.tp_new = PyBaseObject_Type.tp_new;
		}

		struct_class->heap_type.ht_type.tp_vectorcall = Struct_vectorcall;
	}

	struct_class->struct_author_new = author_new_in_chain(&struct_class->heap_type.ht_type);
	struct_class->struct_family_owned = family_owns_in_mro(&struct_class->heap_type.ht_type);
	struct_class->struct_group_family = group_family_in_mro(&struct_class->heap_type.ht_type);

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
	int const new_present = dict_has_string(namespace, "__new__");

	if (new_present < 0) {
		PyErr_Clear();
	}

	bool const qualifies = (
		struct_class->struct_options.frozen &&
		!struct_class->struct_options.weakref &&
		struct_class->struct_field_count == 0 &&
		struct_class->struct_init_var_count == 0 &&
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

enum result install_post_init(StructType * const struct_class) {
	PyObject * const hook = optional_attribute((PyObject *) struct_class, "__post_init__");

	if (hook == NULL && PyErr_Occurred()) {
		return RESULT_ERROR;
	}

	Py_XSETREF(struct_class->struct_post_init, hook);

	return RESULT_OK;
}

static struct member_lookup inherited_member(
	StructType const * const base,
	PyObject * const field_name
) {
	struct field_lookup const found = (
		base != NULL ? find_field(base, field_name) :
		(struct field_lookup){.tag = FIELD_LOOKUP_MISSING}
	);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
			return (struct member_lookup){.tag = MEMBER_LOOKUP_ERROR};
		case FIELD_LOOKUP_MISSING:
			return (struct member_lookup){.tag = MEMBER_LOOKUP_MISSING};
		case FIELD_LOOKUP_FOUND:
			break;
	}

	return (struct member_lookup){
		.tag = MEMBER_LOOKUP_FOUND,
		.slot_offset = base->struct_slot_offsets[found.index],
	};
}

static Py_ssize_t * resolve_slot_offsets(
	StructType * const struct_class,
	StructType const * const base,
	PyObject * const field_names,
	PyObject * const new_names
) {
	Py_ssize_t const field_count = PyTuple_GET_SIZE(field_names);
	Py_ssize_t * const offsets = PyMem_New(Py_ssize_t, field_count > 0 ? field_count : 1);

	if (offsets == NULL) {
		PyErr_NoMemory();

		return NULL;
	}

	PyMemberDef const * const members = struct_heap_type_members(struct_class);
	Py_ssize_t const member_count = Py_SIZE(struct_class);

	for (Py_ssize_t i = 0; i < field_count; i += 1) {
		PyObject * const field_name = PyTuple_GET_ITEM(field_names, i);
		int const declared_here = PySequence_Contains(new_names, field_name);

		if (declared_here < 0) {
			PyMem_Free(offsets);

			return NULL;
		}

		struct member_lookup const found = (
			declared_here == 1 ? find_member(members, member_count, field_name) :
			inherited_member(base, field_name)
		);

		switch (found.tag) {
			case MEMBER_LOOKUP_ERROR:
				PyMem_Free(offsets);
				return NULL;
			case MEMBER_LOOKUP_MISSING:
				PyErr_Format(PyExc_RuntimeError, "could not find slot offset for %R", field_name);
				PyMem_Free(offsets);

				return NULL;
			case MEMBER_LOOKUP_FOUND:
				offsets[i] = found.slot_offset;
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
