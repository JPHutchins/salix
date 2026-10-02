#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static PyObject * interned_value(StructType const * const type, bool const no_arguments) {
	PyObject * const singleton = type->struct_singleton;

	return (singleton != NULL && no_arguments) ? Py_NewRef(singleton) : NULL;
}

PyObject * Struct_vectorcall(
	PyObject * const struct_class,
	PyObject * const * const arguments,
	size_t const argument_count_and_flags,
	PyObject * const keyword_names
) {
	StructType * const type = (StructType *) struct_class;
	Py_ssize_t const positional_count = PyVectorcall_NARGS(argument_count_and_flags);

	if (positional_count > type->struct_field_count) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() takes at most %zd positional arguments but %zd were given",
			struct_type_name(type),
			type->struct_field_count,
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

	/* A body __new__ = None is the cannot-create marker; the cached flag
	 * answers here too, because a class carrying the vectorcall flag is
	 * called through its slot directly, past the metatype's dispatch. */
	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", python_class->tp_name);

		return NULL;
	}

	/* A genuinely NULL slot is the same refusal, whatever path left it. */
	if (python_class->tp_new == NULL) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", python_class->tp_name);

		return NULL;
	}

	PY_MOVABLE(self, NULL);
	bool fallback_allocated = false;

	if (exception_struct) {
		/* The inherited tp_new is the exception family's construction: it
		 * initializes args and the family's C members (errno, filename,
		 * ...), which tp_alloc would leave zeroed. It receives the call's
		 * real shape -- a user __new__ sees the keywords the caller passed,
		 * and a family tp_new with its own arity contract is not answered by
		 * a one-tuple. A non-exception struct's body __new__ is discarded by
		 * construction, the pinned contract; the plain allocation is the
		 * whole of it. */
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

		/* An author __new__ raising TypeError owns the construction and the
		 * failure; the install cached the answer. */
		bool const author_new = type->struct_author_new;

		self = python_class->tp_new(python_class, positionals, keywords);

		if (self == NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
			/* A family tp_new with its own arity contract rejects the
			 * field-constructor call shape; the allocation answers instead,
			 * the C members zeroed. */
			if (!author_new) {
				PyErr_Clear();
				fallback_allocated = true;
				self = python_class->tp_alloc(python_class, 0);

				if (self != NULL) {
					/* The family tp_new would have written args; the fallback
					 * writes the same payload before any hook runs, so a
					 * __post_init__ that formats the instance never reads
					 * NULL. */
					set_exception_args_from_positionals(python_class, self, positionals);
				}
			}
		}

		/* An author __new__ may return any object; the slot writes that
		 * follow assume the struct's own layout, so the type_call guard
		 * answers here. */
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
		/* The family's __new__ is the group members' one writer; a
		 * construction that reached the allocation without it -- the
		 * fallback, or an author __new__ that allocated elsewhere -- carries
		 * the members from the bound fields, validated, so str()/repr()/
		 * raise never read NULL and never see a non-exception body. A
		 * struct without a message field mirrors the first supplied value. */
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

	/* The family's tp_new already set args, so a hook that formats the
	 * exception never dereferences NULL; the rewrite follows __post_init__
	 * so the payload is the explicit field prefix and reflects the hook's
	 * mutations. */
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

	/* The own-init path runs the author's or the family's init after the
	 * allocation, so the fields are not bound here; the positional tuple
	 * is the payload when the family's tp_new wrote nothing (a keyword
	 * shaped family call normalizes its own). A captured NULL answers
	 * when the init was deleted from an ancestor, and the defaults fill
	 * above is all the construction needs. */
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

	/* A body __new__ = None is the cannot-create marker; the cached flag
	 * answers at every construction entry point, the metatype's dispatch
	 * included. */
	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", struct_type_name(type));

		return NULL;
	}

	/* The fallback acquires items once and validates every pair at the
	 * boundary, so the bind loop, the own-init kwargs and the pair-shape
	 * error all read the same list. A list is PyMapping_Check-true through
	 * its subscript slot and an ABC-style mapping carries the sequence
	 * slots through __len__ and __getitem__, so the items probe names a
	 * mapping where neither slot check does; dicts, the hot path, never
	 * take it. */
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
			/* The author's init owns validation on direct construction and
			 * replace; the mapping's keywords reach it the same way. */
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
