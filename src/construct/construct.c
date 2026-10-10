#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static PyObject * interned_value(StructType const * const type, bool const no_arguments) {
	PyObject * const singleton = type->struct_singleton;

	return (singleton != NULL && no_arguments) ? Py_NewRef(singleton) : NULL;
}

static PyObject * construct_with_init_vars(
	StructType * const type,
	PyObject * const * const arguments,
	Py_ssize_t const positional_count,
	PyObject * const keyword_names
) {
	PyTypeObject * const python_class = &type->heap_type.ht_type;
	PY_MOVABLE(self, python_class->tp_alloc(python_class, 0));

	if (self == NULL) {
		return NULL;
	}

	PY_OWNED(post_init_arguments, post_init_arguments_for(type, self));

	return (
		(
			post_init_arguments != NULL &&
			(
				bind_parameters(
					type,
					self,
					post_init_arguments,
					arguments,
					positional_count,
					keyword_names
				) == RESULT_OK
			) &&
			refuse_missing_parameters(type, self, post_init_arguments) == RESULT_OK &&
			fill_defaults(type, self, true) == RESULT_OK &&
			fill_init_var_defaults(type, post_init_arguments) == RESULT_OK &&
			run_post_init_with(type, post_init_arguments) == RESULT_OK
		) ? py_move(&self) :
		NULL
	);
}

static PyObject * from_mapping_with_init_vars(
	StructType * const type,
	PyObject * const dict_values,
	PyObject * const items
) {
	PyTypeObject * const cls = &type->heap_type.ht_type;
	PY_MOVABLE(built, cls->tp_alloc(cls, 0));

	if (built == NULL) {
		return NULL;
	}

	PY_OWNED(post_init_arguments, post_init_arguments_for(type, built));

	if (post_init_arguments == NULL) {
		return NULL;
	}

	if (dict_values != NULL) {
		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(dict_values, &position, &key, &value)) {
			if (bind_parameter_named(type, built, post_init_arguments, key, value) != RESULT_OK) {
				return NULL;
			}
		}
	} else {
		for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(items); i += 1) {
			PyObject * const pair = PySequence_Fast_GET_ITEM(items, i);

			if (
				bind_parameter_named(
					type,
					built,
					post_init_arguments,
					PyTuple_GET_ITEM(pair, 0),
					PyTuple_GET_ITEM(pair, 1)
				) != RESULT_OK
			) {
				return NULL;
			}
		}
	}

	return (
		(
			refuse_missing_parameters(type, built, post_init_arguments) == RESULT_OK &&
			fill_defaults(type, built, true) == RESULT_OK &&
			fill_init_var_defaults(type, post_init_arguments) == RESULT_OK &&
			run_post_init_with(type, post_init_arguments) == RESULT_OK
		) ? py_move(&built) :
		NULL
	);
}

PyObject * Struct_vectorcall(
	PyObject * const struct_class,
	PyObject * const * const arguments,
	size_t const argument_count_and_flags,
	PyObject * const keyword_names
) {
	StructType * const type = (StructType *) struct_class;
	Py_ssize_t const positional_count = PyVectorcall_NARGS(argument_count_and_flags);

	if (positional_count > struct_parameter_count(type)) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() takes at most %zd positional arguments but %zd were given",
			struct_type_name(type),
			struct_parameter_count(type),
			positional_count
		);

		return NULL;
	}

	PyObject * const interned = interned_value(
		type,
		positional_count == 0 &&
			(keyword_names == NULL || PyTuple_GET_SIZE(keyword_names) == 0)
	);

	if (interned != NULL) {
		return interned;
	}

	PyTypeObject * const python_class = &type->heap_type.ht_type;
	bool const exception_struct = is_exception_struct(python_class);

	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", python_class->tp_name);

		return NULL;
	}

	if (python_class->tp_new == NULL) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", python_class->tp_name);

		return NULL;
	}

	if (type->struct_init_var_count > 0) {
		return construct_with_init_vars(type, arguments, positional_count, keyword_names);
	}

	PY_MOVABLE(self, NULL);
	bool fallback_allocated = false;

	if (exception_struct) {
		PY_OWNED(positionals, PyTuple_New(positional_count));

		if (positionals == NULL) {
			return NULL;
		}

		for (Py_ssize_t i = 0; i < positional_count; ++i) {
			PyTuple_SET_ITEM(positionals, i, Py_NewRef(arguments[i]));
		}

		PY_MOVABLE(keywords, NULL);

		if (keyword_names != NULL && PyTuple_GET_SIZE(keyword_names) > 0) {
			keywords = PyDict_New();

			if (keywords == NULL) {
				return NULL;
			}

			for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(keyword_names); ++i) {
				if (
					PyDict_SetItem(
						keywords,
						PyTuple_GET_ITEM(keyword_names, i),
						arguments[positional_count + i]
					) < 0
				) {
					return NULL;
				}
			}
		}

		bool const author_new = type->struct_author_new;

		self = python_class->tp_new(python_class, positionals, keywords);

		if (self == NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
			if (!author_new) {
				PyErr_Clear();
				fallback_allocated = true;
				self = python_class->tp_alloc(python_class, 0);

				if (self != NULL) {
					set_exception_args_from_positionals(python_class, self, positionals);
				}
			}
		}

		if (self != NULL && !PyObject_TypeCheck(self, python_class)) {
			PyErr_Format(
				PyExc_TypeError,
				"%s.__new__(%s) is not safe, use %s.__new__()",
				Py_TYPE(self)->tp_name,
				python_class->tp_name,
				python_class->tp_name
			);
			Py_CLEAR(self);
		}

		if (self == NULL) {
			return NULL;
		}
	} else {
		self = python_class->tp_alloc(python_class, 0);

		if (self == NULL) {
			return NULL;
		}
	}

	if (self == NULL) {
		return NULL;
	}

	bind_positional(type, self, arguments, positional_count, !fallback_allocated);

	if (
		bind_keywords(type, self, arguments, positional_count, keyword_names) != RESULT_OK ||
		fill_defaults(type, self, true) != RESULT_OK
	) {
		return NULL;
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family && ((PyBaseExceptionGroupObject *) self)->msg == NULL) {
		PY_MOVABLE(group_msg_fallback, NULL);

		if (type->struct_message_index < 0) {
			group_msg_fallback = (
				positional_count > 0 ? PyObject_Str(arguments[0]) :
				keyword_names != NULL && PyTuple_GET_SIZE(
					keyword_names
				) > 0 ? PyObject_Str(arguments[positional_count]) :
				PyUnicode_FromString("")
			);
		}

		if (
			(group_msg_fallback == NULL && PyErr_Occurred()) ||
			group_members_from_fields(type, self, py_move(&group_msg_fallback)) != RESULT_OK
		) {
			Py_CLEAR(self);

			return NULL;
		}
	}
#endif

	Py_ssize_t explicit_count = 0;

	if (exception_struct) {
		explicit_count = explicit_field_prefix(type, positional_count, keyword_names, 0);

		if (explicit_count < 0) {
			return NULL;
		}
	}

	if (run_post_init(type, self) != RESULT_OK) {
		return NULL;
	}

	if (exception_struct) {
#if PY_VERSION_HEX >= 0x030B0000
		if (
			type->struct_group_family &&
			(type->struct_message_index >= 0 || type->struct_exceptions_index >= 0)
		) {
			if (store_group_args(type, self, true) != RESULT_OK) {
				return NULL;
			}
		} else
#endif
		if (set_exception_args_from_fields(type, self, explicit_count) != RESULT_OK) {
			return NULL;
		}
	}

	return py_move(&self);
}

enum result refuse_a_builtin_init(StructType const * const type, char const * const operation) {
	PyTypeObject const * const cls = &type->heap_type.ht_type;
	initproc const init = (
		cls->tp_init == Struct_init_wrapper ? type->struct_installed_init :
		cls->tp_init
	);

	if (init == NULL || init == PyBaseObject_Type.tp_init) {
		return RESULT_OK;
	}

	PY_OWNED(mro, Py_XNewRef(cls->tp_mro));

	for (Py_ssize_t i = 0; mro != NULL && i < PyTuple_GET_SIZE(mro); i += 1) {
		PyTypeObject * const entry = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);

		if (entry->tp_init != init || (entry->tp_base != NULL && entry->tp_base->tp_init == init)) {
			continue;
		}

		if (PyType_FastSubclass(entry, Py_TPFLAGS_BASE_EXC_SUBCLASS)) {
			return RESULT_OK;
		}

		PY_OWNED(entry_dict, struct_type_dict(entry));
		PyObject * const init_entry = (
			entry_dict != NULL ? dict_get_string(entry_dict, "__init__") :
			NULL
		);

		if (PyErr_Occurred()) {
			return RESULT_ERROR;
		}

		if (init_entry == NULL || !Py_IS_TYPE(init_entry, &PyWrapperDescr_Type)) {
			return RESULT_OK;
		}

		PyErr_Format(
			PyExc_TypeError,
			"%s: '%.200s' takes its __init__ from %.200s, a built-in that salix does not pass "
			"struct fields to",
			operation,
			struct_type_name(type),
			entry->tp_name
		);

		return RESULT_ERROR;
	}

	return RESULT_OK;
}

int Struct_init_wrapper(
	PyObject * const self,
	PyObject * const arguments,
	PyObject * const keywords
) {
	StructType * const type = (StructType *) Py_TYPE(self);
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (fill_defaults(type, self, false) != RESULT_OK) {
		return -1;
	}

	if (type->struct_installed_init == NULL) {
		return 0;
	}

	if (is_exception_struct(cls)) {
		PyObject * args;

		STRUCT_BEGIN_CRITICAL_SECTION(self);
		args = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
		STRUCT_END_CRITICAL_SECTION();

		if (args == NULL) {
			set_exception_args_from_positionals(cls, self, arguments);
		}

		Py_XDECREF(args);
	}

	return type->struct_installed_init(self, arguments, keywords);
}

PyObject * Struct_from_mapping(PyObject * const module, PyObject * const arguments) {
	PyObject * struct_class = NULL;
	PyObject * values = NULL;

	if (!PyArg_UnpackTuple(arguments, "from_mapping", 2, 2, &struct_class, &values)) {
		return NULL;
	}

	if (!is_struct_class(struct_class)) {
		PyErr_Format(
			PyExc_TypeError,
			"from_mapping() expects a struct class, not %.200s",
			Py_TYPE(struct_class)->tp_name
		);

		return NULL;
	}

	StructType * const type = (StructType *) struct_class;

	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", struct_type_name(type));

		return NULL;
	}

	if (
		type->struct_own_init &&
		!type->struct_family_owned &&
		!type->struct_group_family &&
		refuse_a_builtin_init(type, "from_mapping()") != RESULT_OK
	) {
		return NULL;
	}

	PyObject * const dict_values = PyDict_Check(values) ? values : NULL;
	PY_MOVABLE(items, NULL);

	if (dict_values == NULL) {
		PY_MOVABLE(items_call, optional_attribute(values, "items"));

		if (items_call == NULL && PyErr_Occurred()) {
			return NULL;
		}

		if (items_call == NULL || !PyMapping_Check(values)) {
			PyErr_Format(
				PyExc_TypeError,
				"from_mapping() values must be a mapping, not %.200s",
				Py_TYPE(values)->tp_name
			);

			return NULL;
		}

		PY_MOVABLE(items_result, PyObject_CallNoArgs(items_call));

		if (items_result == NULL) {
			return NULL;
		}

		items = PySequence_Fast(items_result, "from_mapping() items() must return a sequence");

		if (items == NULL) {
			return NULL;
		}

		for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(items); ++i) {
			PyObject * const pair = PySequence_Fast_GET_ITEM(items, i);

			if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
				PyErr_Format(
					PyExc_TypeError,
					"from_mapping() items() must yield (str, value) pairs"
				);

				return NULL;
			}

			if (!PyUnicode_Check(PyTuple_GET_ITEM(pair, 0))) {
				PyErr_SetString(PyExc_TypeError, "keywords must be strings");

				return NULL;
			}
		}
	}

	PY_MOVABLE(init_keywords, NULL);

	if (type->struct_own_init && !type->struct_family_owned) {
		if (dict_values != NULL) {
			init_keywords = Py_NewRef(dict_values);
		} else {
			init_keywords = PyDict_New();

			if (init_keywords == NULL) {
				return NULL;
			}

			for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(items); ++i) {
				PyObject * const pair = PySequence_Fast_GET_ITEM(items, i);
				PyObject * const name = PyTuple_GET_ITEM(pair, 0);
				int const present = PyDict_Contains(init_keywords, name);

				if (present < 0) {
					return NULL;
				}

				if (present == 1) {
					PyErr_Format(
						PyExc_TypeError,
						"%.200s() got multiple values for argument '%U'",
						struct_type_name(type),
						name
					);

					return NULL;
				}

				if (PyDict_SetItem(init_keywords, name, PyTuple_GET_ITEM(pair, 1)) < 0) {
					return NULL;
				}
			}
		}

		if (!type->struct_group_family) {
			PY_OWNED(no_arguments, PyTuple_New(0));

			return (
				no_arguments != NULL ? PyObject_Call(struct_class, no_arguments, init_keywords) :
				NULL
			);
		}
	}

	Py_ssize_t const entry_count = (
		dict_values != NULL ? PyDict_GET_SIZE(dict_values) :
		PySequence_Fast_GET_SIZE(items)
	);

	PyObject * const interned = interned_value(type, entry_count == 0);

	if (interned != NULL) {
		return interned;
	}

	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (dict_values != NULL) {
		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(dict_values, &position, &key, &value)) {
			if (!PyUnicode_Check(key)) {
				PyErr_SetString(PyExc_TypeError, "keywords must be strings");

				return NULL;
			}
		}
	}

	if (type->struct_init_var_count > 0) {
		return from_mapping_with_init_vars(type, dict_values, items);
	}

	PY_MOVABLE(built, cls->tp_alloc(cls, 0));

	if (built == NULL) {
		return NULL;
	}

	if (dict_values != NULL) {
		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(dict_values, &position, &key, &value)) {
			if (bind_named(type, built, key, value) != RESULT_OK) {
				return NULL;
			}
		}
	} else {
		for (Py_ssize_t i = 0; i < entry_count; ++i) {
			PyObject * const pair = PySequence_Fast_GET_ITEM(items, i);

			if (
				bind_named(
					type,
					built,
					PyTuple_GET_ITEM(pair, 0),
					PyTuple_GET_ITEM(pair, 1)
				) != RESULT_OK
			) {
				return NULL;
			}
		}
	}

	Py_ssize_t const explicit_count = (
		is_exception_struct(
			cls
		) && !type->struct_family_owned ? (
			dict_values != NULL ? explicit_dict_prefix(type, dict_values) :
			explicit_items_prefix(type, items, entry_count)
		) :
		0
	);

	if (explicit_count < 0) {
		return NULL;
	}

	if (fill_defaults(type, built, true) != RESULT_OK) {
		return NULL;
	}

	if (set_exception_args_from_fields(type, built, explicit_count) != RESULT_OK) {
		return NULL;
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyObject * args;

		STRUCT_BEGIN_CRITICAL_SECTION(built);
		args = Py_XNewRef(((PyBaseExceptionObject *) built)->args);
		STRUCT_END_CRITICAL_SECTION();

		PY_MOVABLE(group_msg_fallback, NULL);

		if (type->struct_message_index < 0) {
			group_msg_fallback = (
				args != NULL && PyTuple_GET_SIZE(
					args
				) > 0 ? PyObject_Str(PyTuple_GET_ITEM(args, 0)) :
				PyUnicode_FromString("")
			);
		}

		Py_XDECREF(args);

		if (
			(group_msg_fallback == NULL && PyErr_Occurred()) ||
			group_members_from_fields(type, built, py_move(&group_msg_fallback)) != RESULT_OK
		) {
			return NULL;
		}

		if (
			type->struct_own_init &&
			!type->struct_family_owned &&
			type->struct_installed_init != NULL
		) {
			PY_OWNED(no_arguments, PyTuple_New(0));

			if (
				no_arguments == NULL ||
				type->struct_installed_init(built, no_arguments, init_keywords) < 0
			) {
				return NULL;
			}
		}
	}
#endif

	if (
		(!type->struct_own_init || !type->struct_group_family) &&
		run_post_init(type, built) != RESULT_OK
	) {
		return NULL;
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (
		type->struct_group_family &&
		(type->struct_message_index >= 0 || type->struct_exceptions_index >= 0)
	) {
		if (store_group_args(type, built, true) != RESULT_OK) {
			return NULL;
		}
	} else
#endif
	if (
		type->struct_post_init != NULL &&
		set_exception_args_from_fields(type, built, explicit_count) != RESULT_OK
	) {
		return NULL;
	}

	return py_move(&built);
}
