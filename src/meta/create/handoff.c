#include <Python.h>
#include <stdbool.h>

#if PY_VERSION_HEX < 0x030B0000
#	include <code.h>
#else
#	include <cpython/code.h>
#endif

#include "create.h"
#include "../../options.h"
#include "../../owned.h"

static int code_accepts_keyword(
	PyCodeObject * const code,
	PyObject * const varnames,
	char const * const keyword
) {
	Py_ssize_t const named = code->co_argcount + code->co_kwonlyargcount;

	for (Py_ssize_t i = code->co_posonlyargcount; i < named; ++i) {
		int const compared = PyUnicode_CompareWithASCIIString(
			PyTuple_GET_ITEM(varnames, i),
			keyword
		);

		if (compared == 0) {
			return 1;
		}

		if (compared < 0 && PyErr_Occurred()) {
			return -1;
		}
	}

	return 0;
}

struct chain_verdict chain_probe(
	PyObject * const chain,
	PyObject * const keywords,
	bool const weakref_column,
	PyObject * * const declined
) {
	struct chain_verdict verdict = {.accepts_all = 1, .accepts_weakref = 1, .readable = true};

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(chain); ++i) {
		PyObject * const link = PyList_GET_ITEM(chain, i);

		if (!PyFunction_Check(link)) {
			verdict.readable = false;
			verdict.accepts_all = 0;
			verdict.accepts_weakref = 0;
			continue;
		}

		PyCodeObject * const code = (PyCodeObject *) ((PyFunctionObject *) link)->func_code;

		if ((code->co_flags & CO_VARKEYWORDS) != 0) {
			continue;
		}

#if PY_VERSION_HEX >= 0x030B0000
		PY_OWNED(varnames, PyCode_GetVarnames(code));
#else
		PyObject * const varnames = code->co_varnames;
#endif

		if (varnames == NULL) {
			verdict.accepts_all = -1;

			return verdict;
		}

		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(keywords, &position, &key, &value)) {
			char const * const name = PyUnicode_AsUTF8(key);

			if (name == NULL) {
				verdict.accepts_all = -1;

				return verdict;
			}

			int const accepts = code_accepts_keyword(code, varnames, name);

			if (accepts < 0) {
				verdict.accepts_all = -1;

				return verdict;
			}

			if (accepts == 0) {
				verdict.accepts_all = 0;

				if (declined != NULL) {
					*declined = key;
				}

				break;
			}
		}

		if (weakref_column) {
			int const accepts_weakref = code_accepts_keyword(
				code,
				varnames,
				option_keywords[OPTION_WEAKREF]
			);

			if (accepts_weakref < 0) {
				verdict.accepts_all = -1;

				return verdict;
			}

			if (accepts_weakref == 0) {
				verdict.accepts_weakref = 0;
			}
		}
	}

	return verdict;
}

PyObject * metaclass_chain(PyTypeObject * const winner) {
	PY_MOVABLE(chain, PyList_New(0));

	if (chain == NULL) {
		return NULL;
	}

	PyObject * const mro = winner->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const link = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);

		if (link == &StructMeta_Type || link == &PyType_Type) {
			break;
		}

		if (link->tp_new == StructMeta_new) {
			continue;
		}

		PY_OWNED(new, PyObject_GetAttrString((PyObject *) link, "__new__"));

		if (new == NULL || PyList_Append(chain, new) < 0) {
			return NULL;
		}
	}

	return py_move(&chain);
}

StructType * create_class(
	PyTypeObject * const metatype,
	PyTypeObject * const handoff,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const namespace,
	PyObject * const * const keyword_rungs,
	Py_ssize_t const keyword_rung_count,
	PyObject * forwarded_keywords,
	PyObject * forwarded_options,
	bool const laddered,
	PyObject * const handoff_attempt,
	PyObject * const handoff_declined,
	PyObject * const handoff_new
) {
	PY_OWNED(type_args, PyTuple_Pack(3, name, bases, namespace));

	if (type_args == NULL) {
		return NULL;
	}

	PyTypeObject * const builder = handoff->tp_new == StructMeta_new ? handoff : metatype;
	PyObject * created = NULL;

	if (!laddered) {
		created = PyType_Type.tp_new(
			builder,
			type_args,
			builder == handoff ? forwarded_options : forwarded_keywords
		);
	}

	for (
		Py_ssize_t attempt = 0;
		created == NULL && laddered && attempt < keyword_rung_count;
		++attempt
	) {
		created = PyObject_CallFunctionObjArgs(
			handoff_attempt,
			handoff_new,
			builder,
			type_args,
			builder == handoff || keyword_rungs[attempt] == NULL ? Py_None :
			keyword_rungs[attempt],
			NULL
		);

		if (created != NULL) {
			break;
		}

		PyObject * exception_type = NULL;
		PyObject * exception_value = NULL;
		PyObject * exception_tb = NULL;
		PyErr_Fetch(&exception_type, &exception_value, &exception_tb);

		bool const declined = (
			exception_value != NULL &&
			PyErr_GivenExceptionMatches(exception_value, handoff_declined)
		);

		if (declined && attempt + 1 < keyword_rung_count) {
			Py_XDECREF(exception_type);
			Py_XDECREF(exception_value);
			Py_XDECREF(exception_tb);
			continue;
		}

		if (declined) {
			PyObject * const original_tb = PyException_GetTraceback(exception_value);
			PY_OWNED(message, PyObject_Str(exception_value));
			Py_XDECREF(exception_type);
			Py_XDECREF(exception_value);
			Py_XDECREF(exception_tb);

			if (message != NULL) {
				PyErr_SetObject(PyExc_TypeError, message);

				if (original_tb != NULL) {
					PyObject * exc_type = NULL;
					PyObject * exc_value = NULL;
					PyObject * exc_tb = NULL;
					PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
					PyException_SetTraceback(exc_value, Py_NewRef(original_tb));
					PyErr_Restore(exc_type, exc_value, exc_tb);
					Py_DECREF(original_tb);
				}
			} else {
				Py_XDECREF(original_tb);
			}

			break;
		}

		PyErr_Restore(exception_type, exception_value, exception_tb);
		break;
	}

	if (created == NULL) {
		return NULL;
	}

	if (!is_struct_class(created)) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s.__new__ returned %.200s, which is not a struct class",
			handoff->tp_name,
			Py_TYPE(created)->tp_name
		);

		Py_DECREF(created);

		return NULL;
	}

	return (StructType *) py_move(&created);
}

/* Measured, and smaller than the measurement: replacing the body with
 * `return requested` leaves class creation at 9.77-9.87 us for a 16-field
 * class either way, so the walk is bounded by the width of that band rather
 * than shown to be free. It is one Py_TYPE and one PyType_IsSubtype per base,
 * and a class has one. */
PyTypeObject * winning_metatype(PyTypeObject * const requested, PyObject * const bases) {
	PyTypeObject * winner = requested;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyTypeObject * const candidate = Py_TYPE(PyTuple_GET_ITEM(bases, i));

		if (PyType_IsSubtype(candidate, winner)) {
			winner = candidate;
		}
	}

	return winner;
}

#ifdef TESTING

#	include "../../testing.h"

static char const probes_source[] = {
#	embed "../../../tests/c/fixtures/handoff/probes.py" suffix(, '\0')
};

static char const metatypes_source[] = {
#	embed "../../../tests/c/fixtures/handoff/metatypes.py" suffix(, '\0')
};

static char const metaclasses_source[] = {
#	embed "../../../tests/c/fixtures/handoff/metaclasses.py" suffix(, '\0')
};

static void assert_verdict(
	struct chain_verdict const expected,
	struct chain_verdict const actual,
	UNITY_LINE_TYPE const line
) {
	UNITY_TEST_ASSERT_EQUAL_INT(expected.accepts_all, actual.accepts_all, line, "accepts_all");
	UNITY_TEST_ASSERT_EQUAL_INT(
		expected.accepts_weakref,
		actual.accepts_weakref,
		line,
		"accepts_weakref"
	);
	UNITY_TEST_ASSERT_EQUAL_INT(expected.readable, actual.readable, line, "readable");
}

static void test_the_winning_metatype_is_the_most_derived(void) {
	PyObject * const metatypes = testing_evaluate(metatypes_source);

	TEST_ASSERT_EQUAL_PTR(
		testing_entry(metatypes, "inner"),
		winning_metatype(&PyType_Type, testing_entry(metatypes, "from_both"))
	);
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(metatypes, "outer"),
		winning_metatype(
			(PyTypeObject *) testing_entry(metatypes, "outer"),
			testing_entry(metatypes, "from_object")
		)
	);

	Py_DECREF(metatypes);
}

static void test_the_metaclass_chain_holds_each_python_new_above_struct_meta(void) {
	PyObject * const metaclasses = testing_evaluate(metaclasses_source);
	PyObject * const handing_chain = metaclass_chain(
		(PyTypeObject *) testing_entry(metaclasses, "handing")
	);
	PyObject * const plain_chain = metaclass_chain(
		(PyTypeObject *) testing_entry(metaclasses, "plain")
	);

	TEST_ASSERT_NOT_NULL(handing_chain);
	TEST_ASSERT_NOT_NULL(plain_chain);
	TEST_ASSERT_EQUAL_INT(1, PyList_GET_SIZE(handing_chain));
	TEST_ASSERT_EQUAL_PTR(
		testing_entry(metaclasses, "handing_new"),
		PyList_GET_ITEM(handing_chain, 0)
	);
	TEST_ASSERT_EQUAL_INT(0, PyList_GET_SIZE(plain_chain));

	Py_DECREF(plain_chain);
	Py_DECREF(handing_chain);
	Py_DECREF(metaclasses);
}

static void test_the_chain_probe_declines_a_positional_only_name(void) {
	PyObject * const fixtures = testing_evaluate(probes_source);
	PyObject * declined = NULL;

	assert_verdict(
		(struct chain_verdict){.accepts_all = 1, .accepts_weakref = 1, .readable = true},
		chain_probe(
			testing_entry(fixtures, "positional_only"),
			testing_entry(fixtures, "named"),
			false,
			NULL
		),
		__LINE__
	);
	assert_verdict(
		(struct chain_verdict){.accepts_all = 0, .accepts_weakref = 1, .readable = true},
		chain_probe(
			testing_entry(fixtures, "positional_only"),
			testing_entry(fixtures, "positional"),
			false,
			&declined
		),
		__LINE__
	);
	TEST_ASSERT_NOT_NULL(declined);
	TEST_ASSERT_EQUAL_INT(0, PyUnicode_CompareWithASCIIString(declined, "a"));

	Py_DECREF(fixtures);
}

static void test_a_variadic_keyword_link_accepts_every_keyword(void) {
	PyObject * const fixtures = testing_evaluate(probes_source);

	assert_verdict(
		(struct chain_verdict){.accepts_all = 1, .accepts_weakref = 1, .readable = true},
		chain_probe(
			testing_entry(fixtures, "takes_every_keyword"),
			testing_entry(fixtures, "anything"),
			true,
			NULL
		),
		__LINE__
	);

	Py_DECREF(fixtures);
}

static void test_a_link_written_in_c_makes_the_chain_unreadable(void) {
	PyObject * const fixtures = testing_evaluate(probes_source);

	assert_verdict(
		(struct chain_verdict){.accepts_all = 0, .accepts_weakref = 0, .readable = false},
		chain_probe(
			testing_entry(fixtures, "written_in_c"),
			testing_entry(fixtures, "anything"),
			true,
			NULL
		),
		__LINE__
	);

	Py_DECREF(fixtures);
}

static void test_the_weakref_column_reads_each_links_weakref_keyword(void) {
	PyObject * const fixtures = testing_evaluate(probes_source);

	assert_verdict(
		(struct chain_verdict){.accepts_all = 0, .accepts_weakref = 1, .readable = true},
		chain_probe(
			testing_entry(fixtures, "names_weakref"),
			testing_entry(fixtures, "named"),
			true,
			NULL
		),
		__LINE__
	);
	assert_verdict(
		(struct chain_verdict){.accepts_all = 1, .accepts_weakref = 0, .readable = true},
		chain_probe(
			testing_entry(fixtures, "positional_only"),
			testing_entry(fixtures, "named"),
			true,
			NULL
		),
		__LINE__
	);

	Py_DECREF(fixtures);
}

void handoff_tests(void) {
	Unity.TestFile = __FILE__;

	RUN_TEST(test_the_winning_metatype_is_the_most_derived);
	RUN_TEST(test_the_metaclass_chain_holds_each_python_new_above_struct_meta);
	RUN_TEST(test_the_chain_probe_declines_a_positional_only_name);
	RUN_TEST(test_a_variadic_keyword_link_accepts_every_keyword);
	RUN_TEST(test_a_link_written_in_c_makes_the_chain_unreadable);
	RUN_TEST(test_the_weakref_column_reads_each_links_weakref_keyword);
}

#endif
