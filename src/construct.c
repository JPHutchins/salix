#include <Python.h>

#include "construct.h"
#include "owned.h"
#include "result.h"
#include "types.h"

struct field_lookup {
	enum { FIELD_LOOKUP_FOUND, FIELD_LOOKUP_MISSING, FIELD_LOOKUP_ERROR } tag;
	Py_ssize_t index;
};

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
static enum result fill_defaults(
	StructType const * type,
	PyObject * self,
	Py_ssize_t positional_count
);
static struct field_lookup find_field(StructType const * type, PyObject * name);
static enum result write_slot(
	StructType const * type,
	PyObject * self,
	Py_ssize_t index,
	PyObject * value
);
static enum result run_post_init(StructType const * type, PyObject * self);

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

	PyTypeObject * const python_class = &type->heap_type.ht_type;

	PY_MOVABLE(self, python_class->tp_alloc(python_class, 0));

	if (self == NULL) {
		return NULL;
	}

	bind_positional(type, self, arguments, positional_count);

	if (
		bind_keywords(type, self, arguments, positional_count, keyword_names) != RESULT_OK ||
		fill_defaults(type, self, positional_count) != RESULT_OK ||
		run_post_init(type, self) != RESULT_OK
	) {
		return NULL;
	}

	return py_move(&self);
}

PyObject * Struct_new(
	PyTypeObject * const struct_class,
	PyObject * const arguments,
	PyObject * const keywords
) {
	PY_MOVABLE(self, struct_class->tp_alloc(struct_class, 0));

	if (self == NULL) {
		return NULL;
	}

	StructType const * const type = (StructType *) struct_class;

	return (
		fill_defaults(type, self, struct_required_count(type)) == RESULT_OK ? py_move(&self) :
		NULL
	);
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
		PyObject * const keyword = PyTuple_GET_ITEM(keyword_names, i);
		struct field_lookup const found = find_field(type, keyword);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return RESULT_ERROR;
			case FIELD_LOOKUP_MISSING:
				PyErr_Format(
					PyExc_TypeError,
					"%.200s() got an unexpected keyword argument '%U'",
					struct_type_name(type),
					keyword
				);

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
				keyword
			);

			return RESULT_ERROR;
		}

		*slot = Py_NewRef(arguments[positional_count + i]);
	}

	return RESULT_OK;
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

/* A list of statements rather than a static array of {type, constructor}: on
 * Windows a `PyTypeObject` is imported from python3.dll, and the address of a
 * dllimport symbol is not a compile-time constant, so the array version
 * compiles everywhere except the platform half the wheels are cross-built for.
 */
typedef PyObject * (*default_copier)(PyObject * declared);

static PyObject * copy_list(PyObject * const declared) {
	return PyList_GetSlice(declared, 0, PyList_GET_SIZE(declared));
}

static default_copier copies_default(PyTypeObject const * const kind) {
	if (kind == &PyList_Type) {
		return copy_list;
	}

	if (kind == &PyDict_Type) {
		return PyDict_Copy;
	}

	if (kind == &PySet_Type) {
		return PySet_New;
	}

	if (kind == &PyByteArray_Type) {
		return PyByteArray_FromObject;
	}

	return NULL;
}

bool struct_copies_default(PyTypeObject const * const kind) {
	return copies_default(kind) != NULL;
}

PyObject * struct_default_copy(PyObject * const declared) {
	default_copier const copy = copies_default(Py_TYPE(declared));

	return copy != NULL ? copy(declared) : Py_NewRef(declared);
}

PyObject * Struct_set_field(PyObject * const module, PyObject * const arguments) {
	PyObject * self = NULL;
	PyObject * name = NULL;
	PyObject * value = NULL;

	if (!PyArg_UnpackTuple(arguments, "set_field", 3, 3, &self, &name, &value)) {
		return NULL;
	}

	if (!is_struct(self)) {
		PyErr_Format(
			PyExc_TypeError,
			"set_field() expects a struct, not %.200s",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	if (!PyUnicode_Check(name)) {
		PyErr_Format(
			PyExc_TypeError,
			"set_field() field name must be str, not %.200s",
			Py_TYPE(name)->tp_name
		);

		return NULL;
	}

	StructType * const type = struct_type_of(self);
	struct field_lookup const found = find_field(type, name);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
			return NULL;
		case FIELD_LOOKUP_MISSING:
			PyErr_Format(
				PyExc_AttributeError,
				"%.200s has no field '%U'",
				struct_type_name(type),
				name
			);

			return NULL;
		case FIELD_LOOKUP_FOUND:
			if (write_slot(type, self, found.index, value) != RESULT_OK) {
				return NULL;
			}
	}

	Py_RETURN_NONE;
}

/*
 * Through CPython's own member setter rather than a store of our own, so the
 * free-threading guarantee is inherited here exactly as it is for `self.x = v`.
 * A plain Py_XSETREF is a load, a store and a decref: two threads read the same
 * previous value, both store, and both release it -- one reference, two
 * releases, and 3.14t dies on it. PyMember_SetOne takes a critical section on
 * the instance and defers the release past the end of it.
 *
 * The offset is the one type.__new__ gave this field, so the descriptor here
 * describes a slot that already exists rather than looking one up.
 */
static enum result write_slot(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index,
	PyObject * const value
) {
	char const * const name =
		PyUnicode_AsUTF8(PyTuple_GET_ITEM(type->struct_field_names, index));

	if (name == NULL) {
		return RESULT_ERROR;
	}

	PyMemberDef slot = {
		.name = name,
		.type = SLOT_MEMBER_TYPE,
		.offset = type->struct_slot_offsets[index],
	};

	return PyMember_SetOne((char *) self, &slot, value) == 0 ? RESULT_OK : RESULT_ERROR;
}

static struct field_lookup find_field(StructType const * const type, PyObject * const name) {
	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		if (name == PyTuple_GET_ITEM(type->struct_field_names, i)) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_FOUND, .index = i};
		}
	}

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		int const compared = PyUnicode_Compare(name, PyTuple_GET_ITEM(type->struct_field_names, i));

		if (compared == 0) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_FOUND, .index = i};
		}

		if (compared == -1 && PyErr_Occurred()) {
			return (struct field_lookup){.tag = FIELD_LOOKUP_ERROR};
		}
	}

	return (struct field_lookup){.tag = FIELD_LOOKUP_MISSING};
}

PyObject * Struct_get_state(PyObject * const self, PyObject * const noargs) {
	if (!is_struct(self)) {
		PyErr_Format(
			PyExc_AttributeError,
			"__getstate__ is defined on structs, and %.200s is not one",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	StructType const * const type = struct_type_of(self);
	PY_OWNED(values, PyDict_New());
	PY_OWNED(unset_names, PyList_New(0));

	if (values == NULL || unset_names == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, i);
		PY_OWNED(value, struct_slot_ref(type, self, i));

		if (value == NULL) {
			if (PyList_Append(unset_names, name) < 0) {
				return NULL;
			}

			continue;
		}

		if (PyDict_SetItem(values, name, value) < 0) {
			return NULL;
		}
	}

	if (Py_TYPE(self)->tp_dictoffset != 0) {
		PyObject * * const dict_slot = _PyObject_GetDictPtr(self);
		PY_MOVABLE(dict, NULL);

		STRUCT_BEGIN_CRITICAL_SECTION(self);
		dict = Py_XNewRef(*dict_slot);

		if (dict == NULL) {
			dict = PyDict_New();

			if (dict != NULL) {
				*dict_slot = dict;
			}
		}
		STRUCT_END_CRITICAL_SECTION();

		if (dict == NULL) {
			return NULL;
		}

		PY_OWNED(snapshot, PyDict_Copy(dict));

		if (snapshot == NULL) {
			return NULL;
		}

		Py_ssize_t position = 0;
		PyObject * name = NULL;
		PyObject * entry = NULL;

		while (PyDict_Next(snapshot, &position, &name, &entry)) {
			if (PyUnicode_Check(name)) {
				struct field_lookup const found = find_field(type, name);

				switch (found.tag) {
					case FIELD_LOOKUP_ERROR:
						return NULL;
					case FIELD_LOOKUP_FOUND:
						continue;
					case FIELD_LOOKUP_MISSING:
						break;
				}
			}

			if (PyDict_SetItem(values, name, entry) < 0) {
				return NULL;
			}
		}
	}

	PY_MOVABLE(unset, PyList_AsTuple(unset_names));

	if (unset == NULL) {
		return NULL;
	}

	return PyTuple_Pack(2, values, unset);
}

static enum result restore_unset(
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index
) {
	enum result outcome = RESULT_OK;

	STRUCT_BEGIN_CRITICAL_SECTION(self);

	if (*struct_slot(type, self, index) != NULL) {
		outcome = write_slot(type, self, index, NULL);
	}

	STRUCT_END_CRITICAL_SECTION();

	return outcome;
}

static enum result validate_state(
	StructType const * const type,
	PyObject * const values,
	PyObject * const unset_names,
	bool const has_dict
) {
	Py_ssize_t position = 0;
	PyObject * name = NULL;
	PyObject * value = NULL;

	while (PyDict_Next(values, &position, &name, &value)) {
		if (!PyUnicode_Check(name)) {
			if (!has_dict) {
				PyErr_Format(
					PyExc_TypeError,
					"__setstate__() field name must be str, not %.200s",
					Py_TYPE(name)->tp_name
				);

				return RESULT_ERROR;
			}

			continue;
		}

		struct field_lookup const found = find_field(type, name);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return RESULT_ERROR;
			case FIELD_LOOKUP_MISSING:
				if (!has_dict) {
					PyErr_Format(
						PyExc_AttributeError,
						"%.200s has no field '%U'",
						struct_type_name(type),
						name
					);

					return RESULT_ERROR;
				}

				break;
			case FIELD_LOOKUP_FOUND:
				int const also_unset = PySequence_Contains(unset_names, name);

				if (also_unset < 0) {
					return RESULT_ERROR;
				}

				if (also_unset == 1) {
					PyErr_Format(PyExc_TypeError, "field '%U' listed as both set and unset", name);

					return RESULT_ERROR;
				}
		}
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(unset_names); ++i) {
		PyObject * const unset_name = PyTuple_GET_ITEM(unset_names, i);

		if (!PyUnicode_Check(unset_name)) {
			PyErr_Format(
				PyExc_TypeError,
				"__setstate__() field name must be str, not %.200s",
				Py_TYPE(unset_name)->tp_name
			);

			return RESULT_ERROR;
		}

		struct field_lookup const found = find_field(type, unset_name);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return RESULT_ERROR;
			case FIELD_LOOKUP_MISSING:
				PyErr_Format(
					PyExc_AttributeError,
					"%.200s has no field '%U'",
					struct_type_name(type),
					unset_name
				);

				return RESULT_ERROR;
			case FIELD_LOOKUP_FOUND:
				break;
		}
	}

	return RESULT_OK;
}

static enum result restore_dict_entry(
	PyObject * const self,
	PyObject * const name,
	PyObject * const value
) {
	PyObject * * const dict_slot = _PyObject_GetDictPtr(self);
	PyObject * dict = NULL;

	STRUCT_BEGIN_CRITICAL_SECTION(self);
	dict = *dict_slot;

	if (dict == NULL) {
		dict = PyDict_New();

		if (dict != NULL) {
			*dict_slot = dict;
		}
	}
	STRUCT_END_CRITICAL_SECTION();

	if (dict == NULL) {
		return RESULT_ERROR;
	}

	return PyObject_SetItem(dict, name, value) == 0 ? RESULT_OK : RESULT_ERROR;
}

PyObject * Struct_set_state(PyObject * const self, PyObject * const state) {
	if (!is_struct(self)) {
		PyErr_Format(
			PyExc_AttributeError,
			"__setstate__ is defined on structs, and %.200s is not one",
			Py_TYPE(self)->tp_name
		);

		return NULL;
	}

	if (!PyTuple_Check(state)) {
		PyErr_Format(
			PyExc_TypeError,
			"__setstate__() argument 1 must be a tuple, not %.200s",
			Py_TYPE(state)->tp_name
		);

		return NULL;
	}

	PyObject * values = NULL;
	PyObject * unset_names = NULL;

	if (
		!PyArg_ParseTuple(
			state,
			"O!O!:__setstate__",
			&PyDict_Type,
			&values,
			&PyTuple_Type,
			&unset_names
		)
	) {
		return NULL;
	}

	StructType const * const type = struct_type_of(self);
	bool const has_dict = Py_TYPE(self)->tp_dictoffset != 0;

	if (validate_state(type, values, unset_names, has_dict) != RESULT_OK) {
		return NULL;
	}

	Py_ssize_t position = 0;
	PyObject * name = NULL;
	PyObject * value = NULL;

	while (PyDict_Next(values, &position, &name, &value)) {
		struct field_lookup const found = (
			PyUnicode_Check(name) ? find_field(type, name) :
			(struct field_lookup){.tag = FIELD_LOOKUP_MISSING}
		);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return NULL;
			case FIELD_LOOKUP_MISSING:
				break;
			case FIELD_LOOKUP_FOUND:
				if (write_slot(type, self, found.index, value) != RESULT_OK) {
					return NULL;
				}
		}
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(unset_names); ++i) {
		struct field_lookup const found = find_field(type, PyTuple_GET_ITEM(unset_names, i));

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return NULL;
			case FIELD_LOOKUP_MISSING:
				Py_UNREACHABLE();
			case FIELD_LOOKUP_FOUND:
				if (restore_unset(type, self, found.index) != RESULT_OK) {
					return NULL;
				}
		}
	}

	position = 0;

	while (PyDict_Next(values, &position, &name, &value)) {
		struct field_lookup const found = (
			PyUnicode_Check(name) ? find_field(type, name) :
			(struct field_lookup){.tag = FIELD_LOOKUP_MISSING}
		);

		switch (found.tag) {
			case FIELD_LOOKUP_ERROR:
				return NULL;
			case FIELD_LOOKUP_MISSING:
				if (restore_dict_entry(self, name, value) != RESULT_OK) {
					return NULL;
				}
				break;
			case FIELD_LOOKUP_FOUND:
				break;
		}
	}

	Py_RETURN_NONE;
}

#ifdef TESTING

#	include "testing.h"

static PyObject * two_field_instance(void) {
	return testing_evaluate("class P(Struct):\n    alpha: int\n    beta: int\nresult = P(1, 2)\n");
}

/* The identity scan is the fast path; the equality scan exists only for a name
 * that was not interned, which Python-level tests reach only by accident. */
static void test_an_interned_name_resolves_by_identity(void) {
	PyObject * const instance = two_field_instance();
	PyObject * const name = PyUnicode_InternFromString("beta");
	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(1, found.index);

	Py_DECREF(name);
	Py_DECREF(instance);
}

static void test_a_name_assembled_at_runtime_resolves_by_comparison(void) {
	PyObject * const instance = two_field_instance();
	PyObject * const fields = struct_type_of(instance)->struct_field_names;
	PyObject * const name = PyUnicode_FromFormat("%s%s", "al", "pha");

	TEST_ASSERT_NOT_EQUAL(PyTuple_GET_ITEM(fields, 0), name);

	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_FOUND, found.tag);
	TEST_ASSERT_EQUAL_INT(0, found.index);

	Py_DECREF(name);
	Py_DECREF(instance);
}

static void test_a_name_that_is_not_a_field_is_missing(void) {
	PyObject * const instance = two_field_instance();
	PyObject * const name = PyUnicode_FromString("gamma");
	struct field_lookup const found = find_field(struct_type_of(instance), name);

	TEST_ASSERT_EQUAL_INT(FIELD_LOOKUP_MISSING, found.tag);

	Py_DECREF(name);
	Py_DECREF(instance);
}

void construct_tests(void) {
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_interned_name_resolves_by_identity);
	RUN_TEST(test_a_name_assembled_at_runtime_resolves_by_comparison);
	RUN_TEST(test_a_name_that_is_not_a_field_is_missing);
}

#endif
