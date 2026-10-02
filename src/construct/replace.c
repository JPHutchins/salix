#include <Python.h>

#include "construct.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

PyObject * Struct_replace(
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const nargs,
	PyObject * const keyword_names
) {
	/* METH_FASTCALL methods receive self as the first parameter, so the
	 * positional count here excludes it: anything past zero is a second
	 * positional argument. */
	if (nargs != 0) {
		PyErr_Format(
			PyExc_TypeError,
			"%s.__replace__() takes exactly one positional argument (%zd given)",
			Py_TYPE(self)->tp_name,
			nargs
		);

		return NULL;
	}

	if (!is_struct(self)) {
		PyErr_Format(PyExc_TypeError, "%s object is not replaceable", Py_TYPE(self)->tp_name);

		return NULL;
	}

	StructType * const type = struct_type_of(self);
	Py_ssize_t const change_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;

	/* A body __new__ = None is the cannot-create marker; the cached flag
	 * answers at every construction entry point, the metatype's dispatch
	 * included. */
	if (type->struct_cannot_create) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", struct_type_name(type));

		return NULL;
	}

	if (change_count == 0 && type->struct_options.frozen) {
		return Py_NewRef(self);
	}

	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (type->struct_own_init) {
		for (Py_ssize_t i = 0; i < change_count; ++i) {
			if (named_field(type, PyTuple_GET_ITEM(keyword_names, i)).tag != FIELD_LOOKUP_FOUND) {
				return NULL;
			}
		}

		PY_OWNED(values, PyTuple_New(type->struct_field_count));

		if (values == NULL) {
			return NULL;
		}

		struct_slots_ref_into(type, self, values, NULL);

		PY_MOVABLE(replaced, NULL);

		if (type->struct_family_owned || type->struct_group_family) {
			/* The family's construction owns the call shape and rejects field
			 * keywords; the source's positional payload reconstructs the
			 * members and args, and the changes land on the fields
			 * directly. */
			PY_MOVABLE(positionals, NULL);

			STRUCT_BEGIN_CRITICAL_SECTION(self);
			positionals = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
			STRUCT_END_CRITICAL_SECTION();

			if (positionals == NULL) {
				positionals = PyTuple_New(0);
			}

			if (positionals == NULL) {
				return NULL;
			}

			bool allocated_route = false;

			if (PyTuple_GET_SIZE(positionals) > 0) {
				replaced = cls->tp_new(cls, positionals, NULL);

				if (
					replaced == NULL &&
					PyErr_ExceptionMatches(PyExc_TypeError) &&
					!type->struct_author_new
				) {
					/* A payload the family's arity rejects -- the group's
					 * exact two, an author init's narrower shape -- takes the
					 * allocation instead, the vectorcall's fallback pattern;
					 * the members and args below answer what the family's
					 * parse would have written. Only tp_new's own rejection
					 * falls back: an author init's TypeError propagates. */
					PyErr_Clear();
					allocated_route = true;
					replaced = cls->tp_alloc(cls, 0);
				} else if (replaced != NULL && !PyObject_TypeCheck(replaced, cls)) {
					/* The type_call sequence this mirrors hands an author
					 * __new__'s foreign object back without initializing it;
					 * the slot writes that follow assume the struct's own
					 * layout. */
					PyErr_Format(
						PyExc_TypeError,
						"%s.__new__(%s) is not safe, use %s.__new__()",
						Py_TYPE(replaced)->tp_name,
						cls->tp_name,
						cls->tp_name
					);
					Py_CLEAR(replaced);
				} else if (replaced != NULL) {
					if (cls->tp_init != NULL && cls->tp_init(replaced, positionals, NULL) < 0) {
						Py_CLEAR(replaced);
					}
				}
			} else {
				/* An empty payload marks a from_mapping-built source: the
				 * family's parse has nothing to reconstruct, and the plain
				 * allocation keeps the members exactly as the source left
				 * them -- unset, not fabricated. */
				allocated_route = true;
				replaced = cls->tp_alloc(cls, 0);
			}

			if (replaced != NULL) {
				/* The constructor pre-filled the defaults; the source's
				 * values -- mutations and prior replaces included --
				 * overwrite them, then the changes overwrite those, and
				 * every overwrite releases the pre-filled reference. */
				for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
					PyObject * const value = PyTuple_GET_ITEM(values, i);

					if (value != NULL) {
						Py_XSETREF(*struct_slot(type, replaced, i), Py_NewRef(value));
					}
				}

				for (Py_ssize_t i = 0; i < change_count; ++i) {
					struct field_lookup const found = find_field(
						type,
						PyTuple_GET_ITEM(keyword_names, i)
					);

					if (found.tag != FIELD_LOOKUP_FOUND) {
						return NULL;
					}

					Py_XSETREF(
						*struct_slot(type, replaced, found.index),
						Py_NewRef(arguments[nargs + i])
					);
				}
#if PY_VERSION_HEX >= 0x030B0000
				if (type->struct_group_family) {
					PyBaseExceptionGroupObject * const source_group =
						(PyBaseExceptionGroupObject *) self;
					int const touched = change_names_touch(
						type,
						keyword_names,
						change_count,
						type->struct_exceptions_index
					);

					if (touched < 0) {
						return NULL;
					}

					if (touched == 1) {
						if (
							group_members_from_fields(
								type,
								replaced,
								type->struct_message_index >= 0 ? NULL :
								Py_XNewRef(source_group->msg)
							) != RESULT_OK
						) {
							return NULL;
						}

						if (store_group_args(type, replaced, false) != RESULT_OK) {
							return NULL;
						}
					} else if (allocated_route) {
						if (
							carry_group_members(
								type,
								replaced,
								source_group->msg,
								source_group->excs,
								group_excs_str(self),
								NULL,
								NULL
							) != RESULT_OK
						) {
							return NULL;
						}

						/* The allocation wrote no args -- the family's parse
						 * never ran -- so the source's payload answers; a
						 * member-less struct whose graded pack no-ops keeps
						 * its args here. */
						if (
							set_exception_args_from_original(
								type,
								replaced,
								self,
								NULL,
								NULL
							) != RESULT_OK
						) {
							return NULL;
						}
					}
				} else
#endif
				if (allocated_route) {
					if (
						set_exception_args_from_original(
							type,
							replaced,
							self,
							NULL,
							NULL
						) != RESULT_OK
					) {
						return NULL;
					}
				}
			}
		} else {
			/* The author's init owns the call shape; the field values and
			 * the changes reach it as keywords, positionals stay out so a
			 * field-named parameter binds once. */
			PY_OWNED(changed, PyDict_New());

			if (changed == NULL) {
				return NULL;
			}

			for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
				PyObject * const value = PyTuple_GET_ITEM(values, i);

				if (value == NULL) {
					continue;
				}

				PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, i);
				int const present = PyDict_Contains(changed, name);

				if (present < 0) {
					return NULL;
				}

				if (present == 0 && PyDict_SetItem(changed, name, value) < 0) {
					return NULL;
				}
			}

			for (Py_ssize_t i = 0; i < change_count; ++i) {
				if (
					PyDict_SetItem(
						changed,
						PyTuple_GET_ITEM(keyword_names, i),
						arguments[nargs + i]
					) < 0
				) {
					return NULL;
				}
			}

			PY_OWNED(no_arguments, PyTuple_New(0));

			if (no_arguments == NULL) {
				return NULL;
			}

			replaced = PyObject_Call((PyObject *) cls, no_arguments, changed);
		}

		if (replaced == NULL) {
			return NULL;
		}

		if (!PyObject_TypeCheck(replaced, cls)) {
			PyErr_SetString(
				PyExc_SystemError,
				"salix internal error: the replace construction returned a different type"
			);

			return NULL;
		}

		if (!type->struct_family_owned) {
			for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
				PyObject * const value = PyTuple_GET_ITEM(values, i);
				PyObject * * const slot = struct_slot(type, replaced, i);

				if (value != NULL && *slot == NULL) {
					*slot = Py_NewRef(value);
				}
			}
		}

		PY_MOVABLE(source_dict, NULL);
		struct_slots_copy_into(type, self, replaced, &source_dict);

		if (source_dict != NULL && struct_dict_copy_merged(source_dict, replaced) < 0) {
			return NULL;
		}

		return py_move(&replaced);
	}

	PY_MOVABLE(copy, cls->tp_alloc(cls, 0));

	if (copy == NULL) {
		return NULL;
	}

	if (bind_keywords(type, copy, arguments, 0, keyword_names) != RESULT_OK) {
		return NULL;
	}

	PY_MOVABLE(source_dict, NULL);
	struct_slots_copy_into(type, self, copy, &source_dict);

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyBaseExceptionGroupObject * const source_group = (PyBaseExceptionGroupObject *) self;
		int const touched = change_names_touch(
			type,
			keyword_names,
			change_count,
			type->struct_exceptions_index
		);

		if (touched < 0) {
			return NULL;
		}

		if (touched == 1) {
			/* An exceptions change replaces what is raised, so the members
			 * rebuild from the resulting fields instead of carrying the
			 * source's; the message field answers the message. */
			if (
				group_members_from_fields(
					type,
					copy,
					type->struct_message_index >= 0 ? NULL : Py_XNewRef(source_group->msg)
				) != RESULT_OK
			) {
				return NULL;
			}
		} else if (
			carry_group_members(
				type,
				copy,
				source_group->msg,
				source_group->excs,
				group_excs_str(self),
				NULL,
				NULL
			) != RESULT_OK
		) {
			return NULL;
		}
	}
#endif

	/* The source's payload was the explicit prefix at its construction; the
	 * replaced instance carries it forward, so a change behind a gap keeps
	 * the leading fields in the payload and the result pickles. */
	Py_ssize_t const explicit_count = (
		is_exception_struct(
			cls
		) ? explicit_field_prefix(type, 0, keyword_names, carried_payload_count(type, self)) :
		0
	);

	if (explicit_count < 0) {
		return NULL;
	}

	if (
		set_exception_args_from_fields(type, copy, explicit_count) != RESULT_OK ||
		run_post_init(type, copy) != RESULT_OK ||
		(
			type->struct_post_init != NULL &&
			set_exception_args_from_fields(type, copy, explicit_count) != RESULT_OK
		)
	) {
		return NULL;
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		/* The payload mirrors the carried or rebuilt members, so args and
		 * str() never disagree about the message; a struct without either
		 * member field keeps the field-value payload written above. The
		 * locked store answers because the post-init hook may have
		 * published the copy. */
		if (store_group_args(type, copy, false) != RESULT_OK) {
			return NULL;
		}
	}
#endif

	if (source_dict != NULL && struct_dict_copy_merged(source_dict, copy) < 0) {
		return NULL;
	}

	return py_move(&copy);
}
