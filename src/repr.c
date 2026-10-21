#include <Python.h>

#include "owned.h"
#include "repr.h"
#include "result.h"
#include "types.h"

struct ascii_literal {
	char const * text;
	Py_ssize_t length;
};

#define ASCII_LITERAL(literal) {.text = "" literal, .length = sizeof("" literal) - 1}

#if PY_VERSION_HEX >= 0x030E0000
typedef PyUnicodeWriter repr_writer;

static int write_text(repr_writer * const writer, PyObject * const text) {
	return PyUnicodeWriter_WriteStr(writer, text);
}

static int write_ascii(repr_writer * const writer, struct ascii_literal const literal) {
	return PyUnicodeWriter_WriteASCII(writer, literal.text, literal.length);
}

static int write_value_repr(repr_writer * const writer, PyObject * const value) {
	return PyUnicodeWriter_WriteRepr(writer, value);
}
#else
typedef _PyUnicodeWriter repr_writer;

static int write_text(repr_writer * const writer, PyObject * const text) {
	return _PyUnicodeWriter_WriteStr(writer, text);
}

static int write_ascii(repr_writer * const writer, struct ascii_literal const literal) {
	return _PyUnicodeWriter_WriteASCIIString(writer, literal.text, literal.length);
}

static int write_value_repr(repr_writer * const writer, PyObject * const value) {
	PY_OWNED(rendered, PyObject_Repr(value));

	return rendered != NULL ? _PyUnicodeWriter_WriteStr(writer, rendered) : -1;
}
#endif

static struct ascii_literal const fields_open = ASCII_LITERAL("(");
static struct ascii_literal const fields_close = ASCII_LITERAL(")");
static struct ascii_literal const field_separator = ASCII_LITERAL(", ");
static struct ascii_literal const name_value_separator = ASCII_LITERAL("=");
static struct ascii_literal const unset_value = ASCII_LITERAL("<unset>");

static int write_value(repr_writer * const writer, PyObject * const value) {
	return (value != NULL ? write_value_repr(writer, value) : write_ascii(writer, unset_value));
}

static enum result write_field(
	repr_writer * const writer,
	StructType const * const type,
	PyObject * const self,
	Py_ssize_t const index
) {
	PY_OWNED(value, struct_slot_ref(type, self, index));
	bool const separated = (index == 0 || write_ascii(writer, field_separator) == 0);
	bool const named = (
		separated &&
		write_text(writer, PyTuple_GET_ITEM(type->struct_field_names, index)) == 0 &&
		write_ascii(writer, name_value_separator) == 0
	);

	return named && write_value(writer, value) == 0 ? RESULT_OK : RESULT_ERROR;
}

static enum result write_struct(
	repr_writer * const writer,
	StructType const * const type,
	PyObject * const self,
	PyObject * const qualname
) {
	if (write_text(writer, qualname) < 0 || write_ascii(writer, fields_open) < 0) {
		return RESULT_ERROR;
	}

	for (Py_ssize_t i = 0; i < type->struct_field_count; i += 1) {
		if (write_field(writer, type, self, i) != RESULT_OK) {
			return RESULT_ERROR;
		}
	}

	return (write_ascii(writer, fields_close) == 0 ? RESULT_OK : RESULT_ERROR);
}

#if PY_VERSION_HEX >= 0x030E0000
static PyObject * written_repr(
	StructType const * const type,
	PyObject * const self,
	PyObject * const qualname
) {
	PyUnicodeWriter * const writer = PyUnicodeWriter_Create(0);

	if (writer == NULL) {
		return NULL;
	}

	if (write_struct(writer, type, self, qualname) != RESULT_OK) {
		PyUnicodeWriter_Discard(writer);

		return NULL;
	}

	return PyUnicodeWriter_Finish(writer);
}
#else
static PyObject * written_repr(
	StructType const * const type,
	PyObject * const self,
	PyObject * const qualname
) {
	_PyUnicodeWriter writer;

	_PyUnicodeWriter_Init(&writer);
	writer.overallocate = 1;

	if (write_struct(&writer, type, self, qualname) != RESULT_OK) {
		_PyUnicodeWriter_Dealloc(&writer);

		return NULL;
	}

	return _PyUnicodeWriter_Finish(&writer);
}
#endif

PyObject * Struct_repr(PyObject * const self) {
	if (!is_struct(self)) {
		return PyBaseObject_Type.tp_repr(self);
	}

	int const recursive = Py_ReprEnter(self);

	if (recursive != 0) {
		return recursive < 0 ? NULL : PyUnicode_FromString("...");
	}

	StructType * const type = struct_type_of(self);
	PY_OWNED(qualname, struct_type_qualname(type));
	PyObject * const rendered = qualname != NULL ? written_repr(type, self, qualname) : NULL;

	Py_ReprLeave(self);

	return rendered;
}

#ifdef TESTING

#	include "testing.h"

static char const instances_source[] = {
#	embed "../tests/c/fixtures/repr/instances.py" suffix(, '\0')
};

static void test_an_unwritten_slot_renders_as_unset(void) {
	PyObject * const instance = testing_fixture(instances_source, "pair");
	StructType const * const type = struct_type_of(instance);
	PyObject * * const slot = struct_slot(type, instance, 1);
	PyObject * const written = *slot;

	*slot = NULL;

	PyObject * const rendered = Struct_repr(instance);

	*slot = written;

	TEST_ASSERT_NOT_NULL(rendered);
	TEST_ASSERT_EQUAL_STRING("P(x=1, y=<unset>)", PyUnicode_AsUTF8(rendered));

	Py_DECREF(rendered);
	Py_DECREF(instance);
}

static void test_a_written_slot_renders_its_repr(void) {
	PyObject * const instance = testing_fixture(instances_source, "text");
	PyObject * const rendered = Struct_repr(instance);

	TEST_ASSERT_NOT_NULL(rendered);
	TEST_ASSERT_EQUAL_STRING("Q(x='value')", PyUnicode_AsUTF8(rendered));

	Py_DECREF(rendered);
	Py_DECREF(instance);
}

void repr_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_an_unwritten_slot_renders_as_unset);
	RUN_TEST(test_a_written_slot_renders_its_repr);
}

#endif
