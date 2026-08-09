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

static inline bool options_agree(struct options const left, struct options const right) {
	return (
		left.frozen == right.frozen &&
		left.eq == right.eq &&
		left.order == right.order &&
		left.repr == right.repr &&
		left.match_args == right.match_args &&
		left.weakref == right.weakref
	);
}

struct options_request {
	enum { OPTIONS_RESOLVED, OPTIONS_REJECTED } tag;
	struct options options;
};

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
