#include <Python.h>
#include <stdbool.h>
#include <stddef.h>

#include "meta.h"
#include "../fields.h"
#include "../mixin.h"
#include "../options.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static char const * weakref_slot_name(void) {
	return "__weakref__";
}

static char const * instance_dict_slot_name(void) {
	return "__dict__";
}

enum slot_name_owner { SLOT_NAME_NONE, SLOT_NAME_WEAKREF, SLOT_NAME_INSTANCE_DICT };

static enum slot_name_owner slot_name_owner_of(PyObject * const name) {
	if (PyUnicode_CompareWithASCIIString(name, weakref_slot_name()) == 0) {
		return SLOT_NAME_WEAKREF;
	}

	if (PyUnicode_CompareWithASCIIString(name, instance_dict_slot_name()) == 0) {
		return SLOT_NAME_INSTANCE_DICT;
	}

	return SLOT_NAME_NONE;
}

static PyObject * build_slots(PyObject * new_names, bool weakref, PyObject * bases);
static enum result set_match_args(PyObject * namespace, PyObject * all_names, bool wanted);
static enum result apply_options(
	PyObject * namespace,
	struct options options,
	struct options inherited,
	bool frozen_across_bases,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal
);
static enum result rebind(PyObject * namespace, char const * const * names, bool from_mixin);
static enum result drop_class_variables(PyObject * namespace, PyObject * all_names);
static int defines_a_method(
	PyObject * bound,
	PyObject * field_name,
	PyObject * class_name,
	PyObject * * spelling
);
static int defined_in_this_body(
	PyObject * qualname,
	PyObject * field_name,
	PyObject * class_name,
	PyObject * * spelling
);
static int names_this_body(PyObject * qualname, PyObject * class_name, PyObject * name);
static PyObject * unmangled(PyObject * class_name, PyObject * field_name);

PyObject * build_class_namespace(
	PyObject * const original_namespace,
	PyObject * const all_names,
	PyObject * const new_names,
	struct options const options,
	StructType const * const base,
	PyObject * const bases,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal
) {
	PY_OWNED(slots, build_slots(new_names, options.weakref, bases));
	PY_MOVABLE(namespace, PyDict_Copy(original_namespace));

	if (
		slots != NULL &&
		namespace != NULL &&
		drop_class_variables(namespace, all_names) == RESULT_OK &&
		PyDict_SetItemString(namespace, "__slots__", slots) == 0 &&
		set_match_args(namespace, all_names, options.match_args) == RESULT_OK &&
		apply_options(
				namespace,
				options,
				inherited,
				frozen_across_bases,
				body_defines_eq,
				inherits_body_eq,
				derive_not_equal
			) ==
			RESULT_OK
	) {
		return py_move(&namespace);
	}

	return NULL;
}

enum result refuse_displaced_slots(
	PyObject * const original_namespace,
	PyObject * const all_names,
	PyObject * const bases,
	struct options const options
) {
	PyObject * const declared = PyDict_GetItemString(original_namespace, "__slots__");

	if (declared == NULL) {
		return PyErr_Occurred() ? RESULT_ERROR : RESULT_OK;
	}

	bool const carries_a_weakref_slot = weakref_expected(options, bases);
	bool const carries_an_instance_dict = any_base_has_instance_dict(bases);

	PY_OWNED(
		entries,
		PyUnicode_Check(declared) ? PyTuple_Pack(1, declared) : PySequence_Tuple(declared)
	);

	if (entries == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(entries); ++i) {
		PyObject * const entry = PyTuple_GET_ITEM(entries, i);

		if (!PyUnicode_Check(entry)) {
			PyErr_Format(
				PyExc_TypeError,
				"__slots__ items must be strings, not '%.200s'",
				Py_TYPE(entry)->tp_name
			);

			return RESULT_ERROR;
		}

		enum slot_name_owner const owner = slot_name_owner_of(entry);

		if (owner == SLOT_NAME_WEAKREF) {
			if (carries_a_weakref_slot) {
				continue;
			}

			PyErr_SetString(
				PyExc_TypeError,
				"__slots__ names __weakref__ and this class carries no weakref "
				"slot to name; a struct gets one from weakref=True or from a base "
				"that has one"
			);

			return RESULT_ERROR;
		}

		if (owner == SLOT_NAME_INSTANCE_DICT) {
			if (carries_an_instance_dict) {
				continue;
			}

			PyErr_SetString(
				PyExc_TypeError,
				"__slots__ names __dict__ and a struct carries no instance dict; "
				"a non-struct base that carries one gives the struct one"
			);

			return RESULT_ERROR;
		}

		int const named_by_a_field = PySequence_Contains(all_names, entry);

		if (named_by_a_field < 0) {
			return RESULT_ERROR;
		}

		if (named_by_a_field == 1) {
			continue;
		}

		if (reserved_metadata_name_of(entry) != NULL) {
			PyErr_Format(
				PyExc_TypeError,
				"'%U' is reserved for salix's metadata and cannot be named "
				"in __slots__",
				entry
			);

			return RESULT_ERROR;
		}

		PyErr_Format(
			PyExc_TypeError,
			"__slots__ names %R, which is not a field of this class; a struct's "
			"fields are its slots, so salix would drop it -- declare it as a field, "
			"or drop __slots__",
			entry
		);

		return RESULT_ERROR;
	}

	return RESULT_OK;
}

enum result refuse_slot_name_fields(PyObject * const all_names) {
	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(all_names); ++i) {
		PyObject * const field_name = PyList_GET_ITEM(all_names, i);
		enum slot_name_owner const owner = slot_name_owner_of(field_name);
		char const * const owner_name = (
			owner == SLOT_NAME_WEAKREF ? "the weakref slot's" :
			owner == SLOT_NAME_INSTANCE_DICT ? "the instance dict's" :
			NULL
		);

		if (owner_name != NULL) {
			PyErr_Format(
				PyExc_TypeError,
				"'%U' is %s name and cannot be a field",
				field_name,
				owner_name
			);

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

enum result refuse_reserved_metadata_names(
	PyObject * const original_namespace,
	PyObject * const new_names
) {
	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(new_names); ++i) {
		PyObject * const field_name = PyList_GET_ITEM(new_names, i);
		char const * const reserved = reserved_metadata_name_of(field_name);

		if (reserved == NULL) {
			continue;
		}

		PyErr_Format(
			PyExc_TypeError,
			"'%s' is reserved for salix's metadata and cannot be a field "
			"or a class-body binding",
			reserved
		);

		return RESULT_ERROR;
	}

	for (char const * const * reserved = reserved_metadata_names; *reserved != NULL; ++reserved) {
		PyObject * const bound = PyDict_GetItemString(original_namespace, *reserved);

		if (PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		if (bound != NULL) {
			PyErr_Format(
				PyExc_TypeError,
				"'%s' is reserved for salix's metadata and cannot be a field "
				"or a class-body binding",
				*reserved
			);

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static PyObject * build_slots(
	PyObject * const new_names,
	bool const weakref,
	PyObject * const bases
) {
	PY_OWNED(names, PySequence_List(new_names));

	if (names == NULL) {
		return NULL;
	}

	if (weakref && !any_base_has_weakref_slot(bases)) {
		PY_OWNED(weakref_name, PyUnicode_FromString(weakref_slot_name()));

		if (weakref_name == NULL || PyList_Append(names, weakref_name) < 0) {
			return NULL;
		}
	}

	return PyList_AsTuple(names);
}

static enum result set_match_args(
	PyObject * const namespace,
	PyObject * const all_names,
	bool const wanted
) {
	if (!wanted) {
		return RESULT_OK;
	}

	PY_OWNED(match_args, PyList_AsTuple(all_names));

	return (
		match_args != NULL && PyDict_SetItemString(
			namespace,
			"__match_args__",
			match_args
		) == 0 ? RESULT_OK :
		RESULT_ERROR
	);
}

struct binding_plan binding_plan(
	struct options const options,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal,
	bool const body_defines_hash,
	bool const body_defines_setattr
) {
	struct binding_plan plan = {
		.answered_by_body = body_defines_eq || derive_not_equal,
		.rebind_comparison = options.eq != inherited.eq,
		.rebind_not_equal = options.eq != inherited.eq && !(body_defines_eq || derive_not_equal),
		.rebind_representation = options.repr != inherited.repr,
		.rebind_mutability = options.frozen != inherited.frozen || frozen_across_bases,
		.match_args_wanted = options.match_args,
	};

	if (body_defines_hash) {
		plan.hash = HASH_BODY_DEFINED;
	} else if (inherits_body_eq) {
		plan.hash = HASH_INHERITED_EQ;
	} else if (body_defines_eq || (options.eq && (!options.frozen || body_defines_setattr))) {
		plan.hash = HASH_NONE;
	} else {
		plan.hash = HASH_BIND;
	}

	return plan;
}

static enum result apply_options(
	PyObject * const namespace,
	struct options const options,
	struct options const inherited,
	bool const frozen_across_bases,
	bool const body_defines_eq,
	bool const inherits_body_eq,
	bool const derive_not_equal
) {
	struct binding_plan const plan = binding_plan(
		options,
		inherited,
		frozen_across_bases,
		body_defines_eq,
		inherits_body_eq,
		derive_not_equal,
		PyDict_GetItemString(namespace, "__hash__") != NULL,
		PyDict_GetItemString(namespace, "__setattr__") != NULL
	);

	if (
		plan.answered_by_body &&
		PyDict_GetItemString(namespace, "__ne__") == NULL &&
		rebind(namespace, rebind_not_equal, false) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (plan.rebind_comparison && rebind(namespace, rebind_comparison, options.eq) != RESULT_OK) {
		return RESULT_ERROR;
	}

	if (plan.rebind_not_equal && rebind(namespace, rebind_not_equal, options.eq) != RESULT_OK) {
		return RESULT_ERROR;
	}

	if (
		plan.rebind_representation &&
		rebind(namespace, rebind_representation, options.repr) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	if (
		plan.rebind_mutability &&
		rebind(namespace, rebind_mutability, options.frozen) != RESULT_OK
	) {
		return RESULT_ERROR;
	}

	switch (plan.hash) {
		case HASH_BODY_DEFINED:
		case HASH_INHERITED_EQ:
			break;
		case HASH_NONE:
			if (PyDict_SetItemString(namespace, "__hash__", Py_None) != 0) {
				return RESULT_ERROR;
			}

			break;
		case HASH_BIND:
			if (rebind(namespace, rebind_hash, options.eq) != RESULT_OK) {
				return RESULT_ERROR;
			}

			break;
	}

	return RESULT_OK;
}

static enum result rebind(
	PyObject * const namespace,
	char const * const * const names,
	bool const from_mixin
) {
	PyObject * const source = (
		from_mixin ? (PyObject *) &StructMixin_Type :
		(PyObject *) &PyBaseObject_Type
	);

	for (char const * const * name = names; *name != NULL; ++name) {
		if (PyDict_GetItemString(namespace, *name) != NULL) {
			continue;
		}

		PY_OWNED(bound, PyObject_GetAttrString(source, *name));

		if (bound == NULL || PyDict_SetItemString(namespace, *name, bound) < 0) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result drop_class_variables(PyObject * const namespace, PyObject * const all_names) {
	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(all_names); ++i) {
		PyObject * const field_name = PyList_GET_ITEM(all_names, i);
		int const present = PyDict_Contains(namespace, field_name);

		if (present < 0 || (present == 1 && PyDict_DelItem(namespace, field_name) < 0)) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

enum result refuse_mixin_method_fields(PyObject * const all_names) {
	PY_OWNED(mixin_dict, struct_type_dict(&StructMixin_Type));

	if (mixin_dict == NULL) {
		return RESULT_ERROR;
	}

	Py_ssize_t position = 0;
	PyObject * field_name;
	PyObject * method;

	while (PyDict_Next(mixin_dict, &position, &field_name, &method)) {
		if (!PyObject_TypeCheck(method, &PyMethodDescr_Type)) {
			continue;
		}

		int const present = PySequence_Contains(all_names, field_name);

		if (present < 0) {
			return RESULT_ERROR;
		}

		if (present == 1) {
			PyErr_Format(
				PyExc_TypeError,
				"%U is a field, and the mixin defines a method of the same "
				"name which the field's descriptor would shadow; rename the field",
				field_name
			);

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

enum result refuse_colliding_methods(
	PyObject * const original_namespace,
	PyObject * const all_names,
	PyObject * const class_name
) {
	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(all_names); ++i) {
		PyObject * const field_name = PyList_GET_ITEM(all_names, i);
		PY_OWNED(bound, dict_value_ref(original_namespace, field_name));

		if (bound == NULL) {
			if (PyErr_Occurred()) {
				return RESULT_ERROR;
			}

			continue;
		}

		PY_MOVABLE(spelling, NULL);
		int const method = defines_a_method(bound, field_name, class_name, &spelling);

		if (method < 0) {
			return RESULT_ERROR;
		}

		if (method == 1) {
			PyErr_Format(
				PyExc_TypeError,
				"'%U' is a field, and the class body binds a %.100s to that name "
				"which salix would drop for the descriptor that reads the field; "
				"rename one of them",
				spelling != NULL ? spelling : field_name,
				Py_TYPE(bound)->tp_name
			);

			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static PyObject * qualname_of(PyObject * const value) {
	if (PyFunction_Check(value)) {
		return Py_XNewRef(((PyFunctionObject *) value)->func_qualname);
	}

	PY_MOVABLE(qualname, PyObject_GetAttrString(value, "__qualname__"));

	if (qualname != NULL) {
		return py_move(&qualname);
	}

	if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
		return NULL;
	}

	PyErr_Clear();

	PY_MOVABLE(func, PyObject_GetAttrString(value, "func"));

	if (func == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
			return NULL;
		}

		PyErr_Clear();

		return NULL;
	}

	return PyFunction_Check(func) ? Py_XNewRef(((PyFunctionObject *) func)->func_qualname) : NULL;
}

static int defines_a_method(
	PyObject * const bound,
	PyObject * const field_name,
	PyObject * const class_name,
	PyObject * * const spelling
) {
	if (
		PyObject_TypeCheck(bound, &PyProperty_Type) ||
		PyObject_TypeCheck(bound, &PyClassMethod_Type) ||
		PyObject_TypeCheck(bound, &PyStaticMethod_Type)
	) {
		return 1;
	}

	PY_OWNED(qualname, qualname_of(bound));

	if (qualname == NULL) {
		return PyErr_Occurred() ? -1 : 0;
	}

	return defined_in_this_body(qualname, field_name, class_name, spelling);
}

static int defined_in_this_body(
	PyObject * const qualname,
	PyObject * const field_name,
	PyObject * const class_name,
	PyObject * * const spelling
) {
	if (qualname == NULL || !PyUnicode_Check(qualname)) {
		return 0;
	}

	int const stored = names_this_body(qualname, class_name, field_name);

	if (stored != 0) {
		*spelling = stored == 1 ? Py_NewRef(field_name) : NULL;

		return stored;
	}

	PY_OWNED(source_name, unmangled(class_name, field_name));

	if (source_name == NULL) {
		return -1;
	}

	if (source_name == field_name) {
		return 0;
	}

	int const written = names_this_body(qualname, class_name, source_name);

	*spelling = written == 1 ? Py_NewRef(source_name) : NULL;

	return written;
}

static int names_this_body(
	PyObject * const qualname,
	PyObject * const class_name,
	PyObject * const name
) {
	PY_OWNED(own, PyUnicode_FromFormat("%U.%U", class_name, name));

	if (own == NULL) {
		return -1;
	}

	Py_ssize_t const length = PyUnicode_GET_LENGTH(qualname);
	Py_ssize_t const tail = PyUnicode_GET_LENGTH(own);
	Py_ssize_t const matched = PyUnicode_Tailmatch(qualname, own, 0, length, 1);

	if (matched <= 0) {
		return matched < 0 ? -1 : 0;
	}

	return length == tail || PyUnicode_ReadChar(qualname, length - tail - 1) == '.';
}

static PyObject * unmangled(PyObject * const class_name, PyObject * const field_name) {
	Py_ssize_t const class_length = PyUnicode_GET_LENGTH(class_name);
	Py_ssize_t leading = 0;

	while (leading < class_length && PyUnicode_ReadChar(class_name, leading) == '_') {
		++leading;
	}

	if (leading == class_length) {
		return Py_NewRef(field_name);
	}

	PY_OWNED(stripped, PyUnicode_Substring(class_name, leading, class_length));

	if (stripped == NULL) {
		return NULL;
	}

	PY_OWNED(prefix, PyUnicode_FromFormat("_%U__", stripped));

	if (prefix == NULL) {
		return NULL;
	}

	Py_ssize_t const mangled = PyUnicode_Tailmatch(field_name, prefix, 0, PY_SSIZE_T_MAX, -1);

	if (mangled < 0) {
		return NULL;
	}

	if (mangled == 0) {
		return Py_NewRef(field_name);
	}

	return PyUnicode_Substring(
		field_name,
		PyUnicode_GET_LENGTH(prefix) - 2,
		PyUnicode_GET_LENGTH(field_name)
	);
}
