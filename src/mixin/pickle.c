#include <Python.h>
#include <stdbool.h>

#include "mixin.h"
#include "../construct/construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"
#include "../meta/meta.h"

static PyObject * pickle_cached_build(enum pickle_cached const kind) {
	static char const * const names[] = {
		[PICKLE_REDUCE_EX_NAME] = "__reduce_ex__",
		[PICKLE_REDUCE_NAME] = "__reduce__",
		[PICKLE_SETSTATE_NAME] = "__setstate__",
	};

	return (
		kind == PICKLE_OBJECT_REDUCE_EX ? PyObject_GetAttrString(
			(PyObject *) &PyBaseObject_Type,
			"__reduce_ex__"
		) :
		PyUnicode_InternFromString(names[kind])
	);
}

enum result pickle_cache_fill(struct salix_state * const state) {
	for (enum pickle_cached kind = 0; kind < PICKLE_CACHED_COUNT; kind += 1) {
		Py_XSETREF(state->pickle_cache[kind], pickle_cached_build(kind));

		if (state->pickle_cache[kind] == NULL) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static PyObject * pickle_cached(PyObject * const self, enum pickle_cached const kind) {
	return (
		is_struct(self) ? Py_NewRef(struct_type_of(self)->struct_state->pickle_cache[kind]) :
		pickle_cached_build(kind)
	);
}

struct definition {
	enum { DEFINITION_MISSING, DEFINITION_FOUND, DEFINITION_ERROR } tag;
	PyTypeObject * owner;
};

static struct definition nearest_definition(PyTypeObject * const cls, PyObject * const key) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(cls->tp_mro); i += 1) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(cls->tp_mro, i);

		if (entry == &StructMixin_Type) {
			continue;
		}

		PY_OWNED(entry_dict, struct_type_dict(entry));

		if (entry_dict == NULL) {
			return (struct definition){.tag = DEFINITION_ERROR};
		}

		int const present = PyDict_Contains(entry_dict, key);

		if (present < 0) {
			return (struct definition){.tag = DEFINITION_ERROR};
		}

		if (present == 1) {
			return (struct definition){.tag = DEFINITION_FOUND, .owner = entry};
		}
	}

	return (struct definition){.tag = DEFINITION_MISSING};
}

static int defined_by_user_code(PyTypeObject * const cls, char const * const name) {
	PY_OWNED(key, PyUnicode_InternFromString(name));

	if (key == NULL) {
		return -1;
	}

	struct definition const found = nearest_definition(cls, key);

	switch (found.tag) {
		case DEFINITION_ERROR:
			return -1;
		case DEFINITION_MISSING:
			return 0;
		case DEFINITION_FOUND:
			break;
	}

	return (found.owner->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0;
}

static bool foreign_storage(StructType * const type) {
	PyTypeObject * const cls = &type->heap_type.ht_type;
	Py_ssize_t const pointer = (Py_ssize_t) sizeof(PyObject *);
	Py_ssize_t const replicated = (
		PyBaseObject_Type.tp_basicsize +
		pointer * (type->struct_field_count + type->struct_member_count) +
		(cls->tp_dictoffset > 0 ? pointer : 0) +
		(cls->tp_weaklistoffset > 0 ? pointer : 0)
	);

	return (
		!PyType_FastSubclass(cls, Py_TPFLAGS_BASE_EXC_SUBCLASS) &&
		(cls->tp_basicsize > replicated || cls->tp_itemsize > 0)
	);
}

int copies_through_reduce(StructType * const type) {
	PyTypeObject * const cls = &type->heap_type.ht_type;
	static char const * const hooks[] = {
		"__reduce_ex__",
		"__reduce__",
		"__getstate__",
		"__setstate__",
	};

	if (foreign_storage(type)) {
		return 1;
	}

	for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i += 1) {
		int const defined = defined_by_user_code(cls, hooks[i]);

		if (defined != 0) {
			return defined;
		}
	}

	return 0;
}

static int overrides_reduce(PyObject * const self) {
	PY_OWNED(key, pickle_cached(self, PICKLE_REDUCE_NAME));

	if (key == NULL) {
		return -1;
	}

	struct definition const found = nearest_definition(Py_TYPE(self), key);

	switch (found.tag) {
		case DEFINITION_ERROR:
			return -1;
		case DEFINITION_MISSING:
			return 0;
		case DEFINITION_FOUND:
			break;
	}

	return found.owner != &PyBaseObject_Type;
}

static PyObject * object_reduce_ex(PyObject * const self, PyObject * const protocol) {
	PY_OWNED(method, pickle_cached(self, PICKLE_OBJECT_REDUCE_EX));
	PyObject * const arguments[] = {self, protocol};

	return method != NULL ? PyObject_Vectorcall(method, arguments, 2, NULL) : NULL;
}

static PyObject * reduce_fallback(PyObject * const self) {
	int const overridden = overrides_reduce(self);

	if (overridden < 0) {
		return NULL;
	}

	if (overridden == 0 && !foreign_storage(struct_type_of(self))) {
		return NULL;
	}

	PY_OWNED(reductor, optional_attribute(self, "__reduce__"));

	return reductor != NULL && reductor != Py_None ? PyObject_CallNoArgs(reductor) : NULL;
}

PyObject * copy_reduction(PyObject * const self, PyObject * const copier) {
	if (copier != NULL) {
		return PyObject_CallOneArg(copier, self);
	}

	PY_OWNED(reductor, optional_attribute(self, "__reduce_ex__"));

	if (reductor == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (reductor == NULL || reductor == Py_None) {
		return reduce_fallback(self);
	}

	PY_OWNED(protocol, PyLong_FromLong(4));

	return protocol != NULL ? PyObject_CallOneArg(reductor, protocol) : NULL;
}

PyObject * Struct_reduce_ex(PyObject * const self, PyObject * const protocol) {
	long const protocol_number = PyLong_AsLong(protocol);

	if (protocol_number == -1 && PyErr_Occurred()) {
		return NULL;
	}

	PY_OWNED(name, pickle_cached(self, PICKLE_REDUCE_EX_NAME));

	if (name == NULL) {
		return NULL;
	}

	PY_OWNED(deferred, co_base_override(self, name));

	if (deferred == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (deferred != NULL) {
		return PyObject_CallOneArg(deferred, protocol);
	}

	int const overridden = overrides_reduce(self);

	if (overridden < 0) {
		return NULL;
	}

	if (overridden == 1 || !is_struct(self)) {
		return object_reduce_ex(self, protocol);
	}

	StructType const * const type = struct_type_of(self);

	if (type->struct_singleton != NULL) {
		return Py_BuildValue("(O())", (PyObject *) Py_TYPE(self));
	}

	if (protocol_number < 2) {
		PyErr_Format(
			PyExc_TypeError,
			"struct '%.200s' pickles with protocol 2 or higher, not %ld",
			struct_type_name(type),
			protocol_number
		);

		return NULL;
	}

	return object_reduce_ex(self, protocol);
}

struct state_parts {
	PyObject * instance_dict;
	PyObject * slots;
};

static enum result split_state(PyObject * const state, struct state_parts * const parts) {
	if (state == Py_None) {
		return RESULT_OK;
	}

	if (PyDict_Check(state)) {
		parts->instance_dict = state;

		return RESULT_OK;
	}

	if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
		PyObject * const instance_dict = PyTuple_GET_ITEM(state, 0);
		PyObject * const slots = PyTuple_GET_ITEM(state, 1);

		if (
			(instance_dict == Py_None || PyDict_Check(instance_dict)) &&
			(slots == Py_None || PyDict_Check(slots))
		) {
			parts->instance_dict = instance_dict != Py_None ? instance_dict : NULL;
			parts->slots = slots != Py_None ? slots : NULL;

			return RESULT_OK;
		}
	}

	PyErr_Format(
		PyExc_TypeError,
		"struct state must be None, a dict, or a (dict, slots dict) pair, not %.200s",
		Py_TYPE(state)->tp_name
	);

	return RESULT_ERROR;
}

static PyObject * member_descriptor(PyTypeObject * const cls, PyObject * const name) {
	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(cls->tp_mro); i += 1) {
		PY_OWNED(entry_dict, struct_type_dict((PyTypeObject *) PyTuple_GET_ITEM(cls->tp_mro, i)));

		if (entry_dict == NULL) {
			return NULL;
		}

		PY_MOVABLE(value, dict_value_ref(entry_dict, name));

		if (value == NULL && PyErr_Occurred()) {
			return NULL;
		}

		if (value != NULL && PyObject_TypeCheck(value, &PyMemberDescr_Type)) {
			return py_move(&value);
		}
	}

	return NULL;
}

static enum result refuse_unknown_slots(StructType const * const type, PyObject * const slots) {
	Py_ssize_t position = 0;
	PyObject * name;
	PyObject * value;

	while (slots != NULL && PyDict_Next(slots, &position, &name, &value)) {
		if (!PyUnicode_Check(name)) {
			PyErr_Format(
				PyExc_TypeError,
				"%.200s state names a slot with a %.200s, not a str",
				struct_type_name(type),
				Py_TYPE(name)->tp_name
			);

			return RESULT_ERROR;
		}

		struct field_lookup const field = find_field(type, name);

		if (field.tag == FIELD_LOOKUP_ERROR) {
			return RESULT_ERROR;
		}

		if (field.tag == FIELD_LOOKUP_FOUND) {
			continue;
		}

		PY_OWNED(descriptor, member_descriptor((PyTypeObject *) type, name));

		if (descriptor == NULL) {
			if (!PyErr_Occurred()) {
				PyErr_Format(
					PyExc_TypeError,
					"%.200s state names '%U', which is not a slot of the struct",
					struct_type_name(type),
					name
				);
			}

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static bool any_field_set(StructType const * const type, PyObject * const self) {
	bool set = false;

	STRUCT_BEGIN_CRITICAL_SECTION(self);

	for (Py_ssize_t i = 0; i < type->struct_field_count && !set; i += 1) {
		set = *struct_slot(type, self, i) != NULL;
	}

	STRUCT_END_CRITICAL_SECTION();

	return set;
}

static enum result restore_slots(
	StructType const * const type,
	PyObject * const self,
	PyObject * const slots
) {
	Py_ssize_t position = 0;
	PyObject * name;
	PyObject * value;

	while (slots != NULL && PyDict_Next(slots, &position, &name, &value)) {
		struct field_lookup const field = find_field(type, name);

		if (field.tag == FIELD_LOOKUP_FOUND) {
			if (write_slot(type, self, field.index, value) != RESULT_OK) {
				return RESULT_ERROR;
			}

			continue;
		}

		PY_OWNED(descriptor, member_descriptor((PyTypeObject *) type, name));

		if (descriptor == NULL || Py_TYPE(descriptor)->tp_descr_set(descriptor, self, value) < 0) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result restore_impostor(PyObject * const self, struct state_parts const parts) {
	Py_ssize_t position = 0;
	PyObject * name;
	PyObject * value;

	while (parts.slots != NULL && PyDict_Next(parts.slots, &position, &name, &value)) {
		if (PyObject_SetAttr(self, name, value) < 0) {
			return RESULT_ERROR;
		}
	}

	if (parts.instance_dict == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(instance_dict, PyObject_GenericGetDict(self, NULL));

	return (
		instance_dict != NULL && PyDict_Update(
			instance_dict,
			parts.instance_dict
		) == 0 ? RESULT_OK :
		RESULT_ERROR
	);
}

PyObject * Struct_setstate(PyObject * const self, PyObject * const state) {
	PY_OWNED(name, pickle_cached(self, PICKLE_SETSTATE_NAME));

	if (name == NULL) {
		return NULL;
	}

	PY_OWNED(deferred, co_base_override(self, name));

	if (deferred == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (deferred != NULL) {
		return PyObject_CallOneArg(deferred, state);
	}

	struct state_parts parts = {0};

	if (split_state(state, &parts) != RESULT_OK) {
		return NULL;
	}

	if (!is_struct(self)) {
		return restore_impostor(self, parts) == RESULT_OK ? Py_NewRef(Py_None) : NULL;
	}

	StructType const * const type = struct_type_of(self);

	if (refuse_unknown_slots(type, parts.slots) != RESULT_OK) {
		return NULL;
	}

	if (type->struct_options.frozen && any_field_set(type, self)) {
		PyErr_Format(
			PyExc_TypeError,
			"cannot restore state into a live frozen '%.200s'; only a fresh allocation restores",
			struct_type_name(type)
		);

		return NULL;
	}

	bool const restores_dict = (
		parts.instance_dict != NULL &&
		PyDict_GET_SIZE(parts.instance_dict) > 0
	);

	if (restores_dict && Py_TYPE(self)->tp_dictoffset == 0) {
		PyErr_Format(
			PyExc_AttributeError,
			"struct '%.200s' has no instance __dict__ to restore dict state into",
			struct_type_name(type)
		);

		return NULL;
	}

	PY_OWNED(instance_dict, restores_dict ? PyObject_GenericGetDict(self, NULL) : NULL);

	if (instance_dict == NULL && PyErr_Occurred()) {
		return NULL;
	}

	if (
		restore_slots(type, self, parts.slots) != RESULT_OK ||
		(instance_dict != NULL && PyDict_Update(instance_dict, parts.instance_dict) < 0)
	) {
		return NULL;
	}

	Py_RETURN_NONE;
}
