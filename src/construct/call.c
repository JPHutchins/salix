#include <Python.h>

#include "../construct.h"
#include "../owned.h"
#include "../types.h"

static PyObject * call_with_keywords(
	PyObject * struct_class,
	PyObject * arguments,
	PyObject * keywords
);

PyObject * Struct_call(
	PyObject * const struct_class,
	PyObject * const arguments,
	PyObject * const keywords
) {
	return (
		keywords == NULL || PyDict_GET_SIZE(
			keywords
		) == 0 ? Struct_vectorcall(
			struct_class,
			PySequence_Fast_ITEMS(arguments),
			PyTuple_GET_SIZE(arguments),
			NULL
		) :
		call_with_keywords(struct_class, arguments, keywords)
	);
}

static PyObject * call_with_keywords(
	PyObject * const struct_class,
	PyObject * const arguments,
	PyObject * const keywords
) {
	Py_ssize_t const positional_count = PyTuple_GET_SIZE(arguments);
	Py_ssize_t const keyword_count = PyDict_GET_SIZE(keywords);
	PY_OWNED(stack, PyTuple_New(positional_count + keyword_count));
	PY_OWNED(keyword_names, PyTuple_New(keyword_count));

	if (stack == NULL || keyword_names == NULL) {
		return NULL;
	}

	for (Py_ssize_t i = 0; i < positional_count; i += 1) {
		PyTuple_SET_ITEM(stack, i, Py_NewRef(PyTuple_GET_ITEM(arguments, i)));
	}

	Py_ssize_t copied = 0;

	STRUCT_BEGIN_CRITICAL_SECTION(keywords);
	Py_ssize_t position = 0;
	PyObject * name = NULL;
	PyObject * value = NULL;

	while (copied < keyword_count && PyDict_Next(keywords, &position, &name, &value)) {
		PyTuple_SET_ITEM(keyword_names, copied, Py_NewRef(name));
		PyTuple_SET_ITEM(stack, positional_count + copied, Py_NewRef(value));
		copied += 1;
	}
	STRUCT_END_CRITICAL_SECTION();

	return (
		copied == keyword_count ? Struct_vectorcall(
			struct_class,
			PySequence_Fast_ITEMS(stack),
			positional_count,
			keyword_names
		) :
		PyErr_Format(PyExc_RuntimeError, "the keyword arguments changed size during the call")
	);
}
