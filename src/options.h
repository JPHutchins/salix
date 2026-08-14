#pragma once

#include <Python.h>
#include <stdbool.h>

struct options {
	bool frozen;
	bool eq;
	bool order;
	bool repr;
	bool match_args;
	bool weakref;
};

struct options_request {
	enum { OPTIONS_RESOLVED, OPTIONS_REJECTED } tag;
	struct options options;
};

enum option {
	OPTION_FROZEN,
	OPTION_EQ,
	OPTION_ORDER,
	OPTION_REPR,
	OPTION_MATCH_ARGS,
	OPTION_WEAKREF,
};

extern char const * const option_keywords[];

static inline struct options options_initial(void) {
	return (struct options){
		.frozen = true,
		.eq = true,
		.order = false,
		.repr = true,
		.match_args = true,
		.weakref = false,
	};
}

struct options_request options_read(
	PyObject * keywords,
	struct options inherited,
	bool base_is_constraining
);
