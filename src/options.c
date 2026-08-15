#include <Python.h>
#include <stdbool.h>

#include "options.h"
#include "owned.h"

enum : Py_ssize_t {
	OPTION_COUNT = OPTION_WEAKREF + 1,
};

struct option_lookup {
	enum { OPTION_LOOKUP_FOUND, OPTION_LOOKUP_UNKNOWN } tag;
	enum option option;
};

char const * const option_keywords[OPTION_COUNT] = {
	[OPTION_FROZEN] = "frozen",
	[OPTION_EQ] = "eq",
	[OPTION_ORDER] = "order",
	[OPTION_REPR] = "repr",
	[OPTION_MATCH_ARGS] = "match_args",
	[OPTION_WEAKREF] = "weakref",
};

static struct option_lookup find_option(PyObject * keyword);
static struct options with_option(struct options options, enum option which, bool value);
static struct options_request checked(struct options requested, bool fielded_base_is_frozen);
static struct options_request reject_unknown(PyObject * keyword);
static PyObject * accepted_keywords(void);

struct options_request options_read(
	PyObject * const keywords,
	struct options const inherited,
	bool const fielded_base_is_frozen
) {
	if (keywords == NULL) {
		return (struct options_request){.tag = OPTIONS_RESOLVED, .options = inherited};
	}

	struct options requested = inherited;
	PyObject * keyword;
	PyObject * value;
	Py_ssize_t position = 0;

	while (PyDict_Next(keywords, &position, &keyword, &value)) {
		struct option_lookup const found = find_option(keyword);

		switch (found.tag) {
			case OPTION_LOOKUP_UNKNOWN:
				return reject_unknown(keyword);
			case OPTION_LOOKUP_FOUND:
				break;
		}

		int const truth = PyObject_IsTrue(value);

		if (truth < 0) {
			return (struct options_request){.tag = OPTIONS_REJECTED};
		}

		requested = with_option(requested, found.option, truth != 0);
	}

	return checked(requested, fielded_base_is_frozen);
}

static struct option_lookup find_option(PyObject * const keyword) {
	for (Py_ssize_t i = 0; i < OPTION_COUNT; ++i) {
		if (PyUnicode_CompareWithASCIIString(keyword, option_keywords[i]) == 0) {
			return (struct option_lookup){.tag = OPTION_LOOKUP_FOUND, .option = (enum option) i};
		}
	}

	return (struct option_lookup){.tag = OPTION_LOOKUP_UNKNOWN};
}

static struct options with_option(
	struct options const options,
	enum option const which,
	bool const value
) {
	struct options updated = options;

	switch (which) {
		case OPTION_FROZEN:
			updated.frozen = value;
			break;
		case OPTION_EQ:
			updated.eq = value;
			break;
		case OPTION_ORDER:
			updated.order = value;
			break;
		case OPTION_REPR:
			updated.repr = value;
			break;
		case OPTION_MATCH_ARGS:
			updated.match_args = value;
			break;
		case OPTION_WEAKREF:
			updated.weakref = value;
			break;
	}

	return updated;
}

static struct options_request checked(
	struct options const requested,
	bool const fielded_base_is_frozen
) {
	if (!requested.frozen && fielded_base_is_frozen) {
		PyErr_SetString(PyExc_TypeError, "mutable struct cannot inherit from a frozen one");

		return (struct options_request){.tag = OPTIONS_REJECTED};
	}

	if (requested.order && !requested.eq) {
		PyErr_SetString(PyExc_TypeError, "order=True needs eq=True");

		return (struct options_request){.tag = OPTIONS_REJECTED};
	}

	return (struct options_request){.tag = OPTIONS_RESOLVED, .options = requested};
}

static struct options_request reject_unknown(PyObject * const keyword) {
	PY_OWNED(accepted, accepted_keywords());

	if (accepted != NULL) {
		PyErr_Format(
			PyExc_TypeError,
			"'%U' is not a struct class keyword; expected one of %U",
			keyword,
			accepted
		);
	}

	return (struct options_request){.tag = OPTIONS_REJECTED};
}

/* Listed from the table rather than spelled out, so a new option cannot leave
 * the message behind. */
static PyObject * accepted_keywords(void) {
	PY_OWNED(names, PyList_New(0));
	PY_OWNED(separator, PyUnicode_FromString(", "));

	if (names == NULL || separator == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < OPTION_COUNT; ++i) {
		PY_OWNED(name, PyUnicode_FromString(option_keywords[i]));

		if (name == NULL || PyList_Append(names, name) < 0) {
			return NULL;
		}
	}

	return PyUnicode_Join(separator, names);
}

#ifdef TESTING

#	include "testing.h"

static PyObject * keywords_of(char const * const name, bool const value) {
	PyObject * const keywords = PyDict_New();

	PyDict_SetItemString(keywords, name, value ? Py_True : Py_False);

	return keywords;
}

static void test_no_keywords_inherit_the_base(void) {
	struct options inherited = options_initial();
	inherited.eq = false;

	struct options_request const request = options_read(NULL, inherited, false);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_FALSE(request.options.eq);
	TEST_ASSERT_TRUE(request.options.frozen);
}

static void test_a_keyword_replaces_only_the_flag_it_names(void) {
	PyObject * const keywords = keywords_of("order", true);
	struct options_request const request = options_read(keywords, options_initial(), false);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_TRUE(request.options.order);
	TEST_ASSERT_TRUE(request.options.eq);
	TEST_ASSERT_TRUE(request.options.frozen);

	Py_DECREF(keywords);
}

static void test_an_unknown_keyword_is_rejected(void) {
	PyObject * const keywords = keywords_of("frozn", true);
	struct options_request const request = options_read(keywords, options_initial(), false);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, request.tag);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));

	PyErr_Clear();
	Py_DECREF(keywords);
}

static void test_ordering_without_equality_is_rejected(void) {
	PyObject * const keywords = keywords_of("order", true);
	struct options inherited = options_initial();
	inherited.eq = false;

	struct options_request const request = options_read(keywords, inherited, false);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, request.tag);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));

	PyErr_Clear();
	Py_DECREF(keywords);
}

static void test_a_fielded_frozen_base_pins_frozen(void) {
	PyObject * const keywords = keywords_of("frozen", false);
	struct options_request const constrained = options_read(keywords, options_initial(), true);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, constrained.tag);
	PyErr_Clear();

	struct options_request const free = options_read(keywords, options_initial(), false);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, free.tag);
	TEST_ASSERT_FALSE(free.options.frozen);

	Py_DECREF(keywords);
}

static void test_frozen_true_resolves_over_the_fielded_promise(void) {
	PyObject * const keywords = keywords_of("frozen", true);
	struct options_request const request = options_read(keywords, options_initial(), true);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_TRUE(request.options.frozen);

	Py_DECREF(keywords);
}

void options_tests(void) {
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_no_keywords_inherit_the_base);
	RUN_TEST(test_a_keyword_replaces_only_the_flag_it_names);
	RUN_TEST(test_an_unknown_keyword_is_rejected);
	RUN_TEST(test_ordering_without_equality_is_rejected);
	RUN_TEST(test_a_fielded_frozen_base_pins_frozen);
	RUN_TEST(test_frozen_true_resolves_over_the_fielded_promise);
}

#endif
