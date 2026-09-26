#include <Python.h>

#include "construct.h"
#include "../meta/meta.h"
#include "../owned.h"
#include "../result.h"
#include "../types.h"

static void bind_positional(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count
);
static enum result bind_keywords(
	StructType const * type,
	PyObject * self,
	PyObject * const * arguments,
	Py_ssize_t positional_count,
	PyObject * keyword_names
);
static enum result bind_named(
	StructType const * type,
	PyObject * self,
	PyObject * name,
	PyObject * value,
	Py_ssize_t positional_count
);
static struct field_lookup named_field(StructType const * type, PyObject * name);
static enum result fill_defaults(
	StructType const * type,
	PyObject * self,
	Py_ssize_t positional_count
);
static enum result run_post_init(StructType const * type, PyObject * self);

static PyObject * interned_value(StructType const * const type, bool const no_arguments) {
	PyObject * const singleton = type->struct_singleton;

	return (singleton != NULL && no_arguments) ? Py_NewRef(singleton) : NULL;
}

#if PY_VERSION_HEX >= 0x030B0000
static int change_names_touch(
	StructType * const type,
	PyObject * const keyword_names,
	Py_ssize_t const change_count,
	Py_ssize_t const field_index
) {
	if (field_index < 0 || keyword_names == NULL) {
		return 0;
	}

	PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, field_index);

	for (Py_ssize_t i = 0; i < change_count; ++i) {
		int const matches = PyObject_RichCompareBool(
			PyTuple_GET_ITEM(keyword_names, i),
			name,
			Py_EQ
		);

		if (matches < 0) {
			return -1;
		}

		if (matches == 1) {
			return 1;
		}
	}

	return 0;
}

static enum result group_members_from_fields(
	StructType * const type,
	PyObject * const self,
	PyObject * const msg_fallback
) {
	/* The members' one writer for field-built instances: the message and
	 * the body come from the bound fields, and every body item is an
	 * exception -- the invariant the family's own __new__ enforces on its
	 * shapes, which the allocation fallbacks never reached. The fallback
	 * answers for a struct without a message field; it is consumed. */
	PY_MOVABLE(msg, NULL);

	if (type->struct_message_index >= 0) {
		PyObject * const bound = *struct_slot(type, self, type->struct_message_index);

		msg = bound != NULL ? PyObject_Str(bound) : PyUnicode_FromString("");
	} else if (msg_fallback != NULL) {
		msg = msg_fallback;
	} else {
		msg = PyUnicode_FromString("");
	}

	if (msg == NULL) {
		return RESULT_ERROR;
	}

	PY_MOVABLE(excs, NULL);

	if (type->struct_exceptions_index >= 0) {
		PyObject * const bound = *struct_slot(type, self, type->struct_exceptions_index);

		if (bound != NULL) {
			excs = PyTuple_Check(bound) ? Py_NewRef(bound) : PySequence_Tuple(bound);

			if (excs == NULL) {
				return RESULT_ERROR;
			}

			for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(excs); ++i) {
				if (!PyExceptionInstance_Check(PyTuple_GET_ITEM(excs, i))) {
					PyErr_Format(
						PyExc_ValueError,
						"Item %zd of second argument (exceptions) is not an exception",
						i
					);

					return RESULT_ERROR;
				}
			}
		}
	}

	if (excs == NULL) {
		excs = PyTuple_New(0);

		if (excs == NULL) {
			return RESULT_ERROR;
		}
	}

	/* The carry borrows both members and takes its own references; the
	 * locals here own them until this return releases them. */
	return carry_group_members(type, self, msg, excs, NULL, NULL, NULL);
}
#endif

enum result carry_group_members(
	StructType * const type,
	PyObject * self,
	PyObject * const msg,
	PyObject * const excs,
	PyObject * const excs_str,
	PyObject * const deepcopier,
	PyObject * const memo
) {
#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		/* The family's __new__ is the group members' only writer; every
		 * tp_alloc arm carries them itself so str()/repr()/raise never read
		 * NULL. The deep copy detaches the body through the memo. */
		PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;

		/* The overwrites release what the family's own __new__ wrote on a
		 * rebuild; a field-built shell's members are NULL, where a set is
		 * the same store. */
		Py_XSETREF(group->msg, (
			msg != NULL ? (
				deepcopier != NULL ? PyObject_CallFunctionObjArgs(deepcopier, msg, memo, NULL) :
				Py_XNewRef(msg)
			) :
			PyUnicode_FromString("")
		));
		Py_XSETREF(group->excs, (
			excs != NULL ? (
				deepcopier != NULL ? PyObject_CallFunctionObjArgs(deepcopier, excs, memo, NULL) :
				Py_XNewRef(excs)
			) :
			PyTuple_New(0)
		));
		bool complete = group->msg != NULL && group->excs != NULL;

#	if PY_VERSION_HEX >= 0x030D0C00
		if (group_layout_has_excs_str()) {
			/* 3.13.12+ backported the cached excs string: its repr
			 * dereferences it on 3.14 and, without it, reads args[1] past a
			 * one-item payload on 3.13.12+. The layout probe keeps a wheel
			 * built on newer headers off an older patch's shorter object.
			 * The source's cache answers when one exists; the repr is the
			 * rebuild. */
			Py_XSETREF(group->excs_str, (
				excs_str != NULL ? Py_NewRef(excs_str) :
				group->excs != NULL ? PyObject_Repr(group->excs) :
				NULL
			));
			complete = complete && group->excs_str != NULL;
		}
#	endif

		if (!complete) {
			/* The partially carried members belong to the instance; the
			 * caller's return frees them with it. Clearing here would hand
			 * the caller a freed pointer its cleanup attribute then
			 * touches again. */
			return RESULT_ERROR;
		}
	}
#endif

	return RESULT_OK;
}

static void store_exception_args(PyObject * const self, PyObject * const args) {
	PyObject * old;

	/* The store holds the same lock every reader takes, and the old
	 * reference's release is deferred past the end of the section -- the
	 * PyMember_SetOne pattern the struct slots rely on. */
	STRUCT_BEGIN_CRITICAL_SECTION(self);
	old = ((PyBaseExceptionObject *) self)->args;
	((PyBaseExceptionObject *) self)->args = args;
	STRUCT_END_CRITICAL_SECTION();

	Py_XDECREF(old);
}

#if PY_VERSION_HEX >= 0x030B0000
static enum result store_group_args(StructType * const type, PyObject * const self) {
	/* The group payload is the members' shape -- (msg, excs) when both
	 * fields exist, the single member otherwise -- so pickle's positional
	 * reconstruction binds them by index back onto the same fields. A
	 * struct without either member field keeps the field-value payload the
	 * explicit-prefix writer produced. */
	bool const has_message = type->struct_message_index >= 0;
	bool const has_exceptions = type->struct_exceptions_index >= 0;

	if (!has_message && !has_exceptions) {
		return RESULT_OK;
	}

	PyBaseExceptionGroupObject * const group = (PyBaseExceptionGroupObject *) self;
	PY_MOVABLE(packed, (
		has_message && has_exceptions ? PyTuple_Pack(2, group->msg, group->excs) :
		has_message ? PyTuple_Pack(1, group->msg) :
		PyTuple_Pack(1, group->excs)
	));

	if (packed == NULL) {
		return RESULT_ERROR;
	}

	store_exception_args(self, py_move(&packed));

	return RESULT_OK;
}
#endif

enum result set_exception_args_from_fields(
	StructType * const type,
	PyObject * const self,
	Py_ssize_t const field_count
) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return RESULT_OK;
	}

	/* The exception's C-level args member sits at offset zero when the first
	 * base is the exception; str()/repr()/__reduce__ read it without a NULL
	 * check. The payload is the leading run of explicitly-supplied fields:
	 * __reduce__'s (cls, args) reconstructs them positionally, and the
	 * auto-filled defaults -- trailing or behind a gap -- stay out. Every
	 * arm computes the same explicit prefix and this is its one writer. */
	Py_ssize_t bound_count = 0;

	while (bound_count < field_count && *struct_slot(type, self, bound_count) != NULL) {
		bound_count += 1;
	}

	PY_MOVABLE(args, PyTuple_New(bound_count));

	if (args == NULL) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 0; i < bound_count; ++i) {
		PyTuple_SET_ITEM(args, i, Py_NewRef(*struct_slot(type, self, i)));
	}

	store_exception_args(self, py_move(&args));

	return RESULT_OK;
}

enum result set_exception_args_from_original(
	StructType * const type,
	PyObject * const copy,
	PyObject * const original,
	PyObject * const deepcopier,
	PyObject * const memo
) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return RESULT_OK;
	}

	/* The source's payload is the truth on every arm -- whatever produced
	 * it, the copy keeps it, so a copied exception formats like the
	 * original. The ref is taken under the source's own lock: args is a
	 * settable member whose store holds a critical section on the instance,
	 * and a load-then-incref outside it is the free-threaded use-after-free
	 * the struct slots fixed the same way. The deep copy detaches the items
	 * through the memo, so a deepcopy's payload is the deep field values,
	 * not the original's live objects. */
	PyObject * args;

	STRUCT_BEGIN_CRITICAL_SECTION(original);
	args = Py_XNewRef(((PyBaseExceptionObject *) original)->args);
	STRUCT_END_CRITICAL_SECTION();

	PY_MOVABLE(copied_args, NULL);

	if (args == NULL) {
		copied_args = PyTuple_New(0);
	} else if (deepcopier != NULL) {
		copied_args = PyObject_CallFunctionObjArgs(deepcopier, args, memo, NULL);
		Py_DECREF(args);
	} else {
		copied_args = args;
	}

	if (copied_args == NULL) {
		return RESULT_ERROR;
	}

	store_exception_args(copy, py_move(&copied_args));

	return RESULT_OK;
}

static void set_exception_args_from_positionals(
	PyTypeObject * const cls,
	PyObject * const self,
	PyObject * const positionals
) {
	if (!is_exception_struct(cls)) {
		return;
	}

	/* The own-init path runs the author's __init__ after the allocation, so
	 * the fields are not bound here; BaseException_new's contract holds: the
	 * positional tuple is the payload. */
	store_exception_args(self, Py_XNewRef(positionals));
}

static Py_ssize_t explicit_field_prefix(
	StructType const * const type,
	Py_ssize_t const positional_count,
	PyObject * const keyword_names,
	Py_ssize_t const carried_count
) {
	Py_ssize_t const keyword_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		bool explicit = prefix < positional_count || prefix < carried_count;

		for (Py_ssize_t i = 0; !explicit && i < keyword_count; ++i) {
			struct field_lookup const found = find_field(type, PyTuple_GET_ITEM(keyword_names, i));

			if (found.tag == FIELD_LOOKUP_ERROR) {
				return -1;
			}

			explicit = found.tag == FIELD_LOOKUP_FOUND && found.index == prefix;
		}

		if (!explicit) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

static Py_ssize_t carried_payload_count(StructType * const type, PyObject * const self) {
	PyTypeObject * const cls = &type->heap_type.ht_type;

	if (!is_exception_struct(cls)) {
		return 0;
	}

	PyObject * args;

	STRUCT_BEGIN_CRITICAL_SECTION(self);
	args = Py_XNewRef(((PyBaseExceptionObject *) self)->args);
	STRUCT_END_CRITICAL_SECTION();

	Py_ssize_t const count = args != NULL ? PyTuple_GET_SIZE(args) : 0;
	Py_XDECREF(args);

	return count;
}

static Py_ssize_t explicit_dict_prefix(StructType const * const type, PyObject * const values) {
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		int const present = PyDict_Contains(
			values,
			PyTuple_GET_ITEM(type->struct_field_names, prefix)
		);

		if (present < 0) {
			return -1;
		}

		if (present == 0) {
			break;
		}

		prefix += 1;
	}

	return prefix;
}

static Py_ssize_t explicit_items_prefix(
	StructType const * const type,
	PyObject * const items,
	Py_ssize_t const entry_count
) {
	Py_ssize_t prefix = 0;

	while (prefix < type->struct_field_count) {
		PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, prefix);
		bool explicit = false;

		for (Py_ssize_t i = 0; i < entry_count; ++i) {
			PyObject * const pair = PySequence_Fast_GET_ITEM(items, i);
			int const compared = PyObject_RichCompareBool(name, PyTuple_GET_ITEM(pair, 0), Py_EQ);

			if (compared < 0) {
				return -1;
			}

			if (compared == 1) {
				explicit = true;
				break;
			}
		}

		if (!explicit) {
			break;
		}

		prefix += 1;
	}

	return prefix;
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

	/* A body __new__ = None is the cannot-create marker the install
	 * normalized to a NULL slot; every struct reads it, exception or
	 * not. */
	if (python_class->tp_new == NULL) {
		PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", python_class->tp_name);

		return NULL;
	}

	PY_MOVABLE(self, NULL);

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
					) <
					0
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

	bind_positional(type, self, arguments, positional_count);

	if (
		bind_keywords(type, self, arguments, positional_count, keyword_names) != RESULT_OK ||
		fill_defaults(type, self, positional_count) != RESULT_OK
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
			if (store_group_args(type, self) != RESULT_OK) {
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

	if (fill_defaults(type, self, struct_required_count(type)) != RESULT_OK) {
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
							) !=
							RESULT_OK
						) {
							return NULL;
						}

						if (store_group_args(type, replaced) != RESULT_OK) {
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
							) !=
							RESULT_OK
						) {
							return NULL;
						}
					}
				} else
#endif
				if (allocated_route) {
					if (
						set_exception_args_from_original(type, replaced, self, NULL, NULL) !=
						RESULT_OK
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
					) <
					0
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
				) !=
				RESULT_OK
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
			) !=
			RESULT_OK
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
		if (store_group_args(type, copy) != RESULT_OK) {
			return NULL;
		}
	}
#endif

	if (source_dict != NULL && struct_dict_copy_merged(source_dict, copy) < 0) {
		return NULL;
	}

	return py_move(&copy);
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
			if (bind_named(type, built, key, value, 0) != RESULT_OK) {
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
					PyTuple_GET_ITEM(pair, 1),
					0
				) !=
				RESULT_OK
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

	if (fill_defaults(type, built, 0) != RESULT_OK) {
		return NULL;
	}

	if (set_exception_args_from_fields(type, built, explicit_count) != RESULT_OK) {
		return NULL;
	}

#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		PyObject * const args = ((PyBaseExceptionObject *) built)->args;
		PY_MOVABLE(group_msg_fallback, NULL);

		if (type->struct_message_index < 0) {
			group_msg_fallback = (
				args != NULL && PyTuple_GET_SIZE(
					args
				) > 0 ? PyObject_Str(PyTuple_GET_ITEM(args, 0)) :
				PyUnicode_FromString("")
			);
		}

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
		if (store_group_args(type, built) != RESULT_OK) {
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

static enum result run_post_init(StructType const * const type, PyObject * const self) {
	if (type->struct_post_init == NULL) {
		return RESULT_OK;
	}

	PY_OWNED(returned, PyObject_CallOneArg(type->struct_post_init, self));

	return returned != NULL ? RESULT_OK : RESULT_ERROR;
}

static void bind_positional(
	StructType const * const type,
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const positional_count
) {
#if PY_VERSION_HEX >= 0x030B0000
	if (type->struct_group_family) {
		/* A group struct's positional shape is the family's -- (msg, excs)
		 * -- so the first two positionals bind by the resolved member
		 * indexes, whatever the declaration order; further positionals bind
		 * in declaration order, and a slot already written keeps its value. */
		for (Py_ssize_t i = 0; i < positional_count; ++i) {
			Py_ssize_t const target = (
				i == 0 && type->struct_message_index >= 0 ? type->struct_message_index :
				i == 1 && type->struct_exceptions_index >= 0 ? type->struct_exceptions_index :
				i
			);

			if (*struct_slot(type, self, target) == NULL) {
				*struct_slot(type, self, target) = Py_NewRef(arguments[i]);
			}
		}

		return;
	}
#endif

	for (Py_ssize_t i = 0; i < positional_count; ++i) {
		*struct_slot(type, self, i) = Py_NewRef(arguments[i]);
	}
}

static enum result bind_keywords(
	StructType const * const type,
	PyObject * const self,
	PyObject * const * const arguments,
	Py_ssize_t const positional_count,
	PyObject * const keyword_names
) {
	Py_ssize_t const keyword_count = keyword_names != NULL ? PyTuple_GET_SIZE(keyword_names) : 0;

	for (Py_ssize_t i = 0; i < keyword_count; ++i) {
		if (
			bind_named(
				type,
				self,
				PyTuple_GET_ITEM(keyword_names, i),
				arguments[positional_count + i],
				positional_count
			) !=
			RESULT_OK
		) {
			return RESULT_ERROR;
		}
	}

	return RESULT_OK;
}

static enum result bind_named(
	StructType const * const type,
	PyObject * const self,
	PyObject * const name,
	PyObject * const value,
	Py_ssize_t const positional_count
) {
	struct field_lookup const found = named_field(type, name);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
		case FIELD_LOOKUP_MISSING:
			return RESULT_ERROR;
		case FIELD_LOOKUP_FOUND:
			break;
	}

	PyObject * * const slot = struct_slot(type, self, found.index);

	if (*slot != NULL || found.index < positional_count) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() got multiple values for argument '%U'",
			struct_type_name(type),
			name
		);

		return RESULT_ERROR;
	}

	*slot = Py_NewRef(value);

	return RESULT_OK;
}

static struct field_lookup named_field(StructType const * const type, PyObject * const name) {
	struct field_lookup const found = find_field(type, name);

	if (found.tag == FIELD_LOOKUP_MISSING) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s() got an unexpected keyword argument '%U'",
			struct_type_name(type),
			name
		);
	}

	return found;
}
static enum result fill_defaults(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const positional_count
) {
	Py_ssize_t const required_count = struct_required_count(type);

	for (Py_ssize_t i = positional_count; i < type->struct_field_count; ++i) {
		PyObject * * const slot = struct_slot(type, self, i);

		if (*slot != NULL) {
			continue;
		}

		if (i < required_count) {
			PyErr_Format(
				PyExc_TypeError,
				"%.200s() missing required argument '%U'",
				struct_type_name(type),
				PyTuple_GET_ITEM(type->struct_field_names, i)
			);

			return RESULT_ERROR;
		}

		PyObject * const value = struct_default_copy(
			PyTuple_GET_ITEM(type->struct_defaults, i - required_count)
		);

		if (value == NULL) {
			return RESULT_ERROR;
		}

		*slot = value;
	}

	return RESULT_OK;
}
