#pragma once

#include <Python.h>
#include <stdbool.h>
#include <stddef.h>

#include "meta.h"
#include "options.h"

#if PY_VERSION_HEX < 0x030C0000
#	include <structmember.h>

enum { SLOT_MEMBER_TYPE = T_OBJECT_EX };
#else
enum { SLOT_MEMBER_TYPE = Py_T_OBJECT_EX };
#endif

enum parameter_kind : unsigned char {
	PARAMETER_FIELD,
	PARAMETER_INIT_VAR,
};

typedef struct StructType {
	PyHeapTypeObject heap_type;
	struct salix_state * struct_state;

	PyObject * struct_field_names;
	PyObject * struct_defaults;
	PyObject * struct_annotations;
	PyObject * struct_metadata;
	Py_ssize_t * struct_slot_offsets;
	Py_ssize_t * struct_member_offsets;
	PyObject * struct_post_init;
	PyObject * struct_singleton;
	PyObject * struct_signature;
	PyObject * struct_init_var_names;
	PyObject * struct_init_var_defaults;
	PyObject * struct_init_var_annotations;
	PyObject * struct_declared_names;
	PyObject * struct_class_var_positions;
	enum parameter_kind * struct_parameter_kinds;

	Py_ssize_t struct_field_count;
	Py_ssize_t struct_default_count;
	Py_ssize_t struct_member_count;
	Py_ssize_t struct_init_var_count;
	struct options struct_options;

	bool struct_resolves_body_eq;
	bool struct_author_new;
	bool struct_family_owned;
	bool struct_group_family;
	bool struct_own_init;
	bool struct_cannot_create;
	bool struct_copies_through_reduce;
	initproc struct_installed_init;
	Py_ssize_t struct_message_index;
	Py_ssize_t struct_exceptions_index;
} StructType;

static inline bool group_layout_has_excs_str(void) {
#if PY_VERSION_HEX >= 0x030D0C00
	return ((PyTypeObject *) PyExc_BaseExceptionGroup)->tp_basicsize >=
		(Py_ssize_t) (offsetof(PyBaseExceptionGroupObject, excs_str) + sizeof(PyObject *));
#endif

	return false;
}

static inline PyObject * group_excs_str(PyObject * const source) {
#if PY_VERSION_HEX >= 0x030D0C00
	if (group_layout_has_excs_str()) {
		return ((PyBaseExceptionGroupObject *) source)->excs_str;
	}
#endif

	return NULL;
}

static inline bool is_exception_struct(PyTypeObject * const cls) {
	return (
		PyType_FastSubclass(cls, Py_TPFLAGS_BASE_EXC_SUBCLASS) &&
		PyType_FastSubclass(cls->tp_base, Py_TPFLAGS_BASE_EXC_SUBCLASS)
	);
}

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

#if PY_VERSION_HEX < 0x030B0000
static inline PyObject * struct_type_qualname(StructType * const type) {
	return Py_NewRef(type->heap_type.ht_qualname);
}
#else
static inline PyObject * struct_type_qualname(StructType * const type) {
	return PyType_GetQualName(&type->heap_type.ht_type);
}
#endif

static inline PyObject * * struct_slot(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index
) {
	return (PyObject * *) ((char *) self + type->struct_slot_offsets[index]);
}

#if PY_VERSION_HEX < 0x030C0000
static inline PyObject * struct_type_dict(PyTypeObject * const type) {
	return Py_XNewRef(type->tp_dict);
}
#else
static inline PyObject * struct_type_dict(PyTypeObject * const type) {
	return PyType_GetDict(type);
}
#endif

static inline PyObject * dict_value_ref(PyObject * const mapping, PyObject * const key) {
#if PY_VERSION_HEX >= 0x030D0000
	PyObject * value = NULL;

	return PyDict_GetItemRef(mapping, key, &value) < 0 ? NULL : value;
#else
	PyObject * const value = PyDict_GetItemWithError(mapping, key);

	return value != NULL ? Py_XNewRef(value) : NULL;
#endif
}

static inline PyObject * dict_get_string(PyObject * const mapping, char const * const name) {
	PyObject * const key = PyUnicode_FromString(name);

	if (key == NULL) {
		return NULL;
	}

	PyObject * const value = PyDict_GetItemWithError(mapping, key);
	Py_DECREF(key);

	return value;
}

static inline int dict_has_string(PyObject * const mapping, char const * const name) {
	PyObject * const value = dict_get_string(mapping, name);

	if (value != NULL) {
		return 1;
	}

	return PyErr_Occurred() ? -1 : 0;
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

struct slot_pair {
	PyObject * mine;
	PyObject * theirs;
};

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

static inline struct slot_pair struct_slot_pair_ref(
	StructType const * const type,
	PyObject * const self,
	PyObject * const other,
	Py_ssize_t const index
) {
	PyObject * mine;
	PyObject * theirs;

	STRUCT_BEGIN_CRITICAL_SECTION2(self, other);
	mine = *struct_slot(type, self, index);
	theirs = *struct_slot(type, other, index);
	mine = Py_NewRef(mine != NULL ? mine : Py_None);
	theirs = Py_NewRef(theirs != NULL ? theirs : Py_None);
	STRUCT_END_CRITICAL_SECTION2();

	return (struct slot_pair){.mine = mine, .theirs = theirs};
}

static inline void struct_slots_ref_into(
	StructType const * const type,
	PyObject * const self,
	PyObject * const values,
	PyObject * const missing
) {
	STRUCT_BEGIN_CRITICAL_SECTION(self);

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * const value = *struct_slot(type, self, i);

		PyTuple_SET_ITEM(values, i, value != NULL ? Py_NewRef(value) : Py_XNewRef(missing));
	}

	STRUCT_END_CRITICAL_SECTION();
}

static inline void struct_slots_copy_into(
	StructType const * const type,
	PyObject * const source,
	PyObject * const destination,
	PyObject * * const dict
) {
	STRUCT_BEGIN_CRITICAL_SECTION(source);

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * const value = *struct_slot(type, source, i);
		PyObject * * const destination_slot = struct_slot(type, destination, i);

		if (value != NULL && *destination_slot == NULL) {
			*destination_slot = Py_NewRef(value);
		}
	}

	for (Py_ssize_t i = 0; i < type->struct_member_count; ++i) {
		Py_ssize_t const offset = type->struct_member_offsets[i];
		PyObject * const value = *(PyObject * *) ((char *) source + offset);
		PyObject * * const destination_slot = (PyObject * *) ((char *) destination + offset);

		if (value != NULL && *destination_slot == NULL) {
			*destination_slot = Py_NewRef(value);
		}
	}

	PyObject * * const dict_slot = _PyObject_GetDictPtr(source);

	if (dict_slot != NULL) {
		*dict = Py_XNewRef(*dict_slot);
	}

	STRUCT_END_CRITICAL_SECTION();
}

static inline int struct_dict_copy_merged(
	PyObject * const source_dict,
	PyObject * const destination
) {
	PyObject * const copied = PyDict_Copy(source_dict);

	if (copied == NULL) {
		return -1;
	}

	PyObject * * const own_slot = _PyObject_GetDictPtr(destination);

	if (own_slot != NULL && *own_slot != NULL && PyDict_Update(copied, *own_slot) < 0) {
		Py_DECREF(copied);

		return -1;
	}

	int const installed = PyObject_GenericSetDict(destination, copied, NULL);
	Py_DECREF(copied);

	return installed;
}

static inline Py_ssize_t struct_required_count(StructType const * const type) {
	return type->struct_field_count - type->struct_default_count;
}

static inline Py_ssize_t struct_required_init_var_count(StructType const * const type) {
	return type->struct_init_var_count - PyTuple_GET_SIZE(type->struct_init_var_defaults);
}

static inline Py_ssize_t struct_parameter_count(StructType const * const type) {
	return type->struct_field_count + type->struct_init_var_count;
}

static inline enum parameter_kind struct_parameter_kind(
	StructType const * const type,
	Py_ssize_t const position
) {
	return (
		type->struct_parameter_kinds != NULL ? type->struct_parameter_kinds[position] :
		PARAMETER_FIELD
	);
}

static inline PyObject * struct_tuple_or_empty(PyObject * const tuple) {
	return tuple != NULL ? Py_NewRef(tuple) : PyTuple_New(0);
}

enum struct_metadata : int {
	STRUCT_FIELD_NAMES,
	STRUCT_DEFAULTS,
	STRUCT_ANNOTATIONS,
	STRUCT_METADATA,
};

static inline PyObject * struct_metadata(
	StructType const * const type,
	enum struct_metadata const which
) {
	switch (which) {
		case STRUCT_FIELD_NAMES:
			return struct_tuple_or_empty(type->struct_field_names);
		case STRUCT_DEFAULTS:
			return struct_tuple_or_empty(type->struct_defaults);
		case STRUCT_ANNOTATIONS:
			return struct_tuple_or_empty(type->struct_annotations);
		case STRUCT_METADATA:
			return struct_tuple_or_empty(type->struct_metadata);
	}

	Py_UNREACHABLE();
}

static inline PyMemberDef * struct_heap_type_members(StructType * const type) {
	return (PyMemberDef *) ((char *) type + Py_TYPE(type)->tp_basicsize);
}
