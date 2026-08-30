#include <Python.h>
#include <stdbool.h>

#include "meta.h"
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
static struct options_request checked(
	struct options requested,
	struct base_facts facts,
	bool weakref_written
);

struct options_request options_read(
	PyObject * const keywords,
	struct options const inherited,
	struct base_facts const facts
) {
	/* The record a reader hands in must already carry the base-derived facts;
	 * inherited_options is the only producer and forces both columns. A caller
	 * that skips the force would otherwise get a refusal for a keyword nobody
	 * wrote, or a record that contradicts the settle verify downstream. */
	if (
		(!inherited.frozen && facts.fielded_frozen) ||
		(!inherited.weakref && facts.weakref_carried)
	) {
		PyErr_SetString(
			PyExc_SystemError,
			"salix internal error: the inherited options contradict the base facts"
		);

		return (struct options_request){.tag = OPTIONS_REJECTED};
	}

	if (keywords == NULL) {
		return (struct options_request){
			.tag = OPTIONS_RESOLVED,
			.options = inherited,
			.weakref_written = false,
		};
	}

	struct options requested = inherited;
	bool weakref_written = false;
	PyObject * keyword;
	PyObject * value;
	Py_ssize_t position = 0;

	while (PyDict_Next(keywords, &position, &keyword, &value)) {
		struct option_lookup const found = find_option(keyword);

		if (found.tag == OPTION_LOOKUP_UNKNOWN) {
			continue;
		}

		int const truth = PyObject_IsTrue(value);

		if (truth < 0) {
			return (struct options_request){.tag = OPTIONS_REJECTED};
		}

		weakref_written |= found.option == OPTION_WEAKREF;
		requested = with_option(requested, found.option, truth != 0);
	}

	return checked(requested, facts, weakref_written);
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
	struct base_facts const facts,
	bool const weakref_written
) {
	if (!requested.frozen && facts.fielded_frozen) {
		PyErr_SetString(PyExc_TypeError, "mutable struct cannot inherit from a frozen one");

		return (struct options_request){.tag = OPTIONS_REJECTED};
	}

	if (requested.order && !requested.eq) {
		PyErr_SetString(PyExc_TypeError, "order=True needs eq=True");

		return (struct options_request){.tag = OPTIONS_REJECTED};
	}

	if (!requested.weakref && facts.weakref_carried) {
		PyErr_SetString(PyExc_TypeError, "weakref=False cannot drop a weakref slot a base carries");

		return (struct options_request){.tag = OPTIONS_REJECTED};
	}

	return (struct options_request){
		.tag = OPTIONS_RESOLVED,
		.options = requested,
		.weakref_written = weakref_written,
	};
}

PyObject * options_forwarded(PyObject * const keywords) {
	if (keywords == NULL) {
		return NULL;
	}

	PY_MOVABLE(forwarded, PyDict_New());

	if (forwarded == NULL) {
		return NULL;
	}

	Py_ssize_t position = 0;
	PyObject * keyword;
	PyObject * value;

	while (PyDict_Next(keywords, &position, &keyword, &value)) {
		if (find_option(keyword).tag == OPTION_LOOKUP_UNKNOWN) {
			if (PyDict_SetItem(forwarded, keyword, value) < 0) {
				return NULL;
			}
		}
	}

	return py_move(&forwarded);
}

#ifdef TESTING

#	include "testing.h"

static PyObject * keywords_of(char const * const name, bool const value) {
	PyObject * const keywords = PyDict_New();

	PyDict_SetItemString(keywords, name, value ? Py_True : Py_False);

	return keywords;
}

static struct base_facts facts_of(
	bool const fielded_frozen,
	bool const weakref_carried,
	bool const instance_dict_carried
) {
	return (struct base_facts){
		.fielded_frozen = fielded_frozen,
		.weakref_carried = weakref_carried,
		.instance_dict_carried = instance_dict_carried,
	};
}

static void test_no_keywords_inherit_the_base(void) {
	struct options inherited = options_initial();
	inherited.eq = false;

	struct options_request const request = options_read(
		NULL,
		inherited,
		facts_of(false, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_FALSE(request.options.eq);
	TEST_ASSERT_TRUE(request.options.frozen);
}

static void test_a_keyword_replaces_only_the_flag_it_names(void) {
	PyObject * const keywords = keywords_of("order", true);
	struct options_request const request = options_read(
		keywords,
		options_initial(),
		facts_of(false, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_TRUE(request.options.order);
	TEST_ASSERT_TRUE(request.options.eq);
	TEST_ASSERT_TRUE(request.options.frozen);

	Py_DECREF(keywords);
}

static void test_an_unknown_keyword_is_skipped(void) {
	PyObject * const keywords = keywords_of("frozn", true);
	struct options_request const request = options_read(
		keywords,
		options_initial(),
		facts_of(false, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_TRUE(request.options.frozen);
	TEST_ASSERT_FALSE(PyErr_Occurred());

	Py_DECREF(keywords);
}

static void test_only_unowned_keywords_are_forwarded(void) {
	PyObject * const keywords = PyDict_New();

	PyDict_SetItemString(keywords, "frozn", Py_True);
	PyDict_SetItemString(keywords, option_keywords[OPTION_FROZEN], Py_False);

	PY_OWNED(forwarded, options_forwarded(keywords));

	TEST_ASSERT_NOT_NULL(forwarded);
	TEST_ASSERT_EQUAL_INT(1, PyDict_Size(forwarded));
	TEST_ASSERT_EQUAL_INT(1, PyObject_IsTrue(PyDict_GetItemString(forwarded, "frozn")));

	Py_DECREF(keywords);
}

static void test_the_owned_keywords_leave_nothing_to_forward(void) {
	PyObject * const keywords = keywords_of("frozen", false);

	PY_OWNED(forwarded, options_forwarded(keywords));

	TEST_ASSERT_NOT_NULL(forwarded);
	TEST_ASSERT_EQUAL_INT(0, PyDict_Size(forwarded));

	Py_DECREF(keywords);
}

static void test_null_keywords_forward_nothing(void) {
	TEST_ASSERT_NULL(options_forwarded(NULL));
	TEST_ASSERT_FALSE(PyErr_Occurred());
}

static void test_ordering_without_equality_is_rejected(void) {
	PyObject * const keywords = keywords_of("order", true);
	struct options inherited = options_initial();
	inherited.eq = false;

	struct options_request const request = options_read(
		keywords,
		inherited,
		facts_of(false, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, request.tag);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));

	PyErr_Clear();
	Py_DECREF(keywords);
}

static void test_a_fielded_frozen_base_pins_frozen(void) {
	PyObject * const keywords = keywords_of("frozen", false);
	struct options_request const constrained = options_read(
		keywords,
		options_initial(),
		facts_of(true, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, constrained.tag);
	PyErr_Clear();

	struct options_request const free = options_read(
		keywords,
		options_initial(),
		facts_of(false, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, free.tag);
	TEST_ASSERT_FALSE(free.options.frozen);

	Py_DECREF(keywords);
}

static void test_a_carried_weakref_slot_refuses_the_explicit_drop(void) {
	PyObject * const keywords = keywords_of("weakref", false);
	struct options inherited = options_initial();
	inherited.weakref = true;

	struct options_request const refused = options_read(
		keywords,
		inherited,
		facts_of(false, true, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, refused.tag);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
	PyErr_Clear();

	Py_DECREF(keywords);
}

static void test_weakref_false_without_a_carried_slot_still_resolves(void) {
	PyObject * const keywords = keywords_of("weakref", false);
	struct options_request const request = options_read(
		keywords,
		options_initial(),
		facts_of(false, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_FALSE(request.options.weakref);

	Py_DECREF(keywords);
}

static void test_frozen_true_resolves_over_the_fielded_promise(void) {
	PyObject * const keywords = keywords_of("frozen", true);
	struct options_request const request = options_read(
		keywords,
		options_initial(),
		facts_of(true, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_TRUE(request.options.frozen);

	Py_DECREF(keywords);
}

static void test_an_unwritten_weakref_over_a_carried_slot_resolves_without_a_refusal(void) {
	PyObject * const keywords = keywords_of("frozen", true);
	struct options inherited = options_initial();
	inherited.weakref = true;

	struct options_request const request = options_read(
		keywords,
		inherited,
		facts_of(false, true, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_TRUE(request.options.weakref);
	TEST_ASSERT_FALSE(request.weakref_written);

	Py_DECREF(keywords);
}

static void test_the_request_reports_that_weakref_was_written(void) {
	PyObject * const keywords = keywords_of("weakref", true);
	struct options_request const request = options_read(
		keywords,
		options_initial(),
		facts_of(false, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_RESOLVED, request.tag);
	TEST_ASSERT_TRUE(request.options.weakref);
	TEST_ASSERT_TRUE(request.weakref_written);

	Py_DECREF(keywords);
}

static void test_an_unforced_inherited_record_over_carried_facts_is_an_internal_error(void) {
	struct options_request const weakref = options_read(
		NULL,
		options_initial(),
		facts_of(false, true, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, weakref.tag);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_SystemError));
	PyErr_Clear();

	struct options inherited = options_initial();
	inherited.frozen = false;

	struct options_request const frozen = options_read(
		NULL,
		inherited,
		facts_of(true, false, false)
	);

	TEST_ASSERT_EQUAL_INT(OPTIONS_REJECTED, frozen.tag);
	TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_SystemError));
	PyErr_Clear();
}

void options_tests(void) {
	/* Unity takes its file from UNITY_BEGIN, which is the runner's. */
	Unity.TestFile = __FILE__;

	RUN_TEST(test_no_keywords_inherit_the_base);
	RUN_TEST(test_a_keyword_replaces_only_the_flag_it_names);
	RUN_TEST(test_an_unknown_keyword_is_skipped);
	RUN_TEST(test_only_unowned_keywords_are_forwarded);
	RUN_TEST(test_the_owned_keywords_leave_nothing_to_forward);
	RUN_TEST(test_null_keywords_forward_nothing);
	RUN_TEST(test_ordering_without_equality_is_rejected);
	RUN_TEST(test_a_fielded_frozen_base_pins_frozen);
	RUN_TEST(test_a_carried_weakref_slot_refuses_the_explicit_drop);
	RUN_TEST(test_weakref_false_without_a_carried_slot_still_resolves);
	RUN_TEST(test_frozen_true_resolves_over_the_fielded_promise);
	RUN_TEST(test_an_unwritten_weakref_over_a_carried_slot_resolves_without_a_refusal);
	RUN_TEST(test_the_request_reports_that_weakref_was_written);
	RUN_TEST(test_an_unforced_inherited_record_over_carried_facts_is_an_internal_error);
}

#endif
