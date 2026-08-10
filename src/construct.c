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
static enum result require_field(
	StructType const * type,
	PyObject * name,
	char const * operation,
	Py_ssize_t * index
);
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
	if (!defines_own_init((StructType *) struct_class)) {
		Py_ssize_t const argument_count = (
			PyTuple_GET_SIZE(arguments) +
			(keywords != NULL ? PyDict_GET_SIZE(keywords) : 0)
		);

		if (argument_count > 0) {
			PyErr_Format(PyExc_TypeError, "%.200s() takes no arguments", struct_class->tp_name);

			return NULL;
		}
	}

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

	StructType * const type = struct_type_of(self);
	Py_ssize_t index;

	if (require_field(type, name, "set_field()", &index) != RESULT_OK) {
		return NULL;
	}

	if (write_slot(type, self, index, value) != RESULT_OK) {
		return NULL;
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

static enum result require_field(
	StructType const * const type,
	PyObject * const name,
	char const * const operation,
	Py_ssize_t * const index
) {
	if (!PyUnicode_Check(name)) {
		PyErr_Format(
			PyExc_TypeError,
			"%s field name must be str, not %.200s",
			operation,
			Py_TYPE(name)->tp_name
		);

		return RESULT_ERROR;
	}

	struct field_lookup const found = find_field(type, name);

	switch (found.tag) {
		case FIELD_LOOKUP_ERROR:
			return RESULT_ERROR;
		case FIELD_LOOKUP_MISSING:
			PyErr_Format(
				PyExc_AttributeError,
				"%.200s has no field '%U'",
				struct_type_name(type),
				name
			);

			return RESULT_ERROR;
		case FIELD_LOOKUP_FOUND:
			*index = found.index;
			return RESULT_OK;
	}

	Py_UNREACHABLE();
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

	enum result collected = RESULT_OK;

	STRUCT_BEGIN_CRITICAL_SECTION(self);

	for (Py_ssize_t i = 0; i < type->struct_field_count; ++i) {
		PyObject * const name = PyTuple_GET_ITEM(type->struct_field_names, i);
		PyObject * const value = *struct_slot(type, self, i);

		if (value == NULL) {
			if (PyList_Append(unset_names, name) < 0) {
				collected = RESULT_ERROR;
				break;
			}
		} else if (PyDict_SetItem(values, name, value) < 0) {
			collected = RESULT_ERROR;
			break;
		}
	}

	STRUCT_END_CRITICAL_SECTION();

	if (collected != RESULT_OK) {
		return NULL;
	}

	PY_MOVABLE(instance_dict, NULL);

	if (Py_TYPE(self)->tp_dictoffset != 0) {
		PY_MOVABLE(dict, struct_instance_dict_ref(self));

		if (dict == NULL && PyErr_Occurred()) {
			return NULL;
		}

		if (dict != NULL) {
			instance_dict = PyDict_Copy(dict);

			if (instance_dict == NULL) {
				return NULL;
			}
		}
	}

	PY_MOVABLE(unset, PyList_AsTuple(unset_names));

	if (unset == NULL) {
		return NULL;
	}

	return PyTuple_Pack(3, values, unset, instance_dict != NULL ? instance_dict : Py_None);
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
	Py_ssize_t * * const values_indices,
	Py_ssize_t * * const unset_indices
) {
	Py_ssize_t const value_count = PyDict_GET_SIZE(values);
	Py_ssize_t const unset_count = PyTuple_GET_SIZE(unset_names);
	Py_ssize_t * const indices = PyMem_Calloc(value_count + unset_count, sizeof(Py_ssize_t));

	if (indices == NULL) {
		return RESULT_ERROR;
	}

	Py_ssize_t position = 0;
	Py_ssize_t slot = 0;
	PyObject * name = NULL;
	PyObject * value = NULL;
	Py_ssize_t index;

	while (PyDict_Next(values, &position, &name, &value)) {
		if (require_field(type, name, "__setstate__()", &index) != RESULT_OK) {
			PyMem_Free(indices);
			return RESULT_ERROR;
		}

		int const also_unset = PySequence_Contains(unset_names, name);

		if (also_unset < 0) {
			PyMem_Free(indices);
			return RESULT_ERROR;
		}

		if (also_unset == 1) {
			PyErr_Format(PyExc_TypeError, "field '%U' listed as both set and unset", name);
			PyMem_Free(indices);
			return RESULT_ERROR;
		}

		indices[slot++] = index;
	}

	for (Py_ssize_t i = 0; i < unset_count; ++i) {
		if (
			require_field(type, PyTuple_GET_ITEM(unset_names, i), "__setstate__()", &index) !=
			RESULT_OK
		) {
			PyMem_Free(indices);
			return RESULT_ERROR;
		}

		indices[slot++] = index;
	}

	*values_indices = indices;
	*unset_indices = indices + value_count;

	return RESULT_OK;
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
	PyObject * instance_dict = NULL;

	if (
		!PyArg_ParseTuple(
			state,
			"O!O!O:__setstate__",
			&PyDict_Type,
			&values,
			&PyTuple_Type,
			&unset_names,
			&instance_dict
		)
	) {
		return NULL;
	}

	if (instance_dict != Py_None && !PyDict_Check(instance_dict)) {
		PyErr_Format(
			PyExc_TypeError,
			"__setstate__() argument 3 must be a dict or None, not %.200s",
			Py_TYPE(instance_dict)->tp_name
		);

		return NULL;
	}

	if (
		Py_TYPE(self)->tp_dictoffset == 0 &&
		instance_dict != Py_None &&
		PyDict_GET_SIZE(instance_dict) > 0
	) {
		PyErr_SetString(PyExc_TypeError, "cannot set an instance dict on a struct without one");

		return NULL;
	}

	StructType const * const type = struct_type_of(self);
	Py_ssize_t * values_indices = NULL;
	Py_ssize_t * unset_indices = NULL;

	if (validate_state(type, values, unset_names, &values_indices, &unset_indices) != RESULT_OK) {
		return NULL;
	}

	if (Py_TYPE(self)->tp_dictoffset != 0) {
		PY_MOVABLE(dict, struct_instance_dict_slot_ref(self));

		if (dict == NULL) {
			PyMem_Free(values_indices);
			return NULL;
		}

		/* The replace is built up front and swapped only on full success, so a
		 * failing merge (a key whose __hash__/__eq__ raises during the copy)
		 * leaves the instance's dict untouched. The swap replaces the dict
		 * object in the slot, stranding any external reference to the old one;
		 * that is the accepted cost of atomicity. The None branch clears in
		 * place. */
		if (instance_dict == Py_None) {
			STRUCT_BEGIN_CRITICAL_SECTION(self);
			PyDict_Clear(dict);
			STRUCT_END_CRITICAL_SECTION();
		} else if (instance_dict != dict) {
			PY_MOVABLE(fresh, PyDict_New());

			if (fresh == NULL || PyDict_Update(fresh, instance_dict) < 0) {
				PyMem_Free(values_indices);
				return NULL;
			}

			int swapped;

			STRUCT_BEGIN_CRITICAL_SECTION(self);
			swapped = PyObject_GenericSetDict(self, fresh, NULL);
			STRUCT_END_CRITICAL_SECTION();

			if (swapped < 0) {
				PyMem_Free(values_indices);
				return NULL;
			}
		}
	}

	Py_ssize_t position = 0;
	Py_ssize_t slot = 0;
	PyObject * name = NULL;
	PyObject * value = NULL;

	while (PyDict_Next(values, &position, &name, &value)) {
		if (write_slot(type, self, values_indices[slot++], value) != RESULT_OK) {
			PyMem_Free(values_indices);
			return NULL;
		}
	}

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(unset_names); ++i) {
		if (restore_unset(type, self, unset_indices[i]) != RESULT_OK) {
			PyMem_Free(values_indices);
			return NULL;
		}
	}

	PyMem_Free(values_indices);
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
