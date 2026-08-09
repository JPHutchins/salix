#include <Python.h>
#include <stdbool.h>

#include "annotations.h"
#include "owned.h"

enum annotation_format {
	ANNOTATION_FORMAT_VALUE = 1,
	ANNOTATION_FORMAT_VALUE_WITH_FAKE_GLOBALS = 2,
	ANNOTATION_FORMAT_FORWARDREF = 3,
	ANNOTATION_FORMAT_STRING = 4,
};

static PyObject * borrow_annotate(PyObject * namespace);
static PyObject * evaluate(PyObject * annotate);

#if PY_VERSION_HEX >= 0x030E0000
enum symbol_verdict {
	SYMBOL_UNRESOLVED,
	SYMBOL_NOT_A_SYMBOL,
	SYMBOL_UNREADABLE,
};

static enum symbol_verdict names_an_unresolved_symbol(PyObject * error);
static PyObject * escalate(PyObject * annotate);
static void raise_over(PyObject * displaced, PyObject * failure);
static bool context_slot_is_free(PyObject * error);
#endif

PyObject * struct_annotations(PyObject * const namespace) {
	PyObject * const declared = PyDict_GetItemString(namespace, "__annotations__");

	if (declared != NULL) {
		return Py_NewRef(declared);
	}

	/* Owned rather than borrowed: on 3.14+ evaluate imports a module, which runs
	 * arbitrary Python the first time, and the namespace this came out of is
	 * within that code's reach. */
	PY_OWNED(annotate, Py_XNewRef(borrow_annotate(namespace)));

	if (annotate == NULL) {
		return PyDict_New();
	}

	return evaluate(annotate);
}

static PyObject * borrow_annotate(PyObject * const namespace) {
	PyObject * const annotate = PyDict_GetItemString(namespace, "__annotate__");

	return annotate != NULL ? annotate : PyDict_GetItemString(namespace, "__annotate_func__");
}

static PyObject * evaluate(PyObject * const annotate) {
	PY_OWNED(format, PyLong_FromLong(ANNOTATION_FORMAT_VALUE));
	PY_MOVABLE(resolved, format != NULL ? PyObject_CallOneArg(annotate, format) : NULL);

#if PY_VERSION_HEX >= 0x030E0000
	if (resolved == NULL && PyFunction_Check(annotate) && PyErr_ExceptionMatches(PyExc_NameError)) {
		PY_MOVABLE(unresolved, PyErr_GetRaisedException());

		switch (names_an_unresolved_symbol(unresolved)) {
			case SYMBOL_UNRESOLVED: {
				PY_MOVABLE(escalated, escalate(annotate));

				if (escalated != NULL) {
					return py_move(&escalated);
				}

				raise_over(py_move(&unresolved), PyErr_GetRaisedException());

				return NULL;
			}

			case SYMBOL_UNREADABLE:
				raise_over(py_move(&unresolved), PyErr_GetRaisedException());

				return NULL;

			case SYMBOL_NOT_A_SYMBOL:
				PyErr_SetRaisedException(py_move(&unresolved));

				return NULL;
		}
	}
#endif

	return py_move(&resolved);
}

#if PY_VERSION_HEX >= 0x030E0000
/* The import is a plain path-based one and could in principle find a user
 * module of that name; a checkout that shadows a stdlib module has larger
 * problems.
 *
 * No `owner`: the class does not exist yet, so annotationlib builds every
 * ForwardRef unowned and class-scope resolution is off the table. Only the keys
 * are read here, so nothing depends on it. */
static PyObject * escalate(PyObject * const annotate) {
	PY_OWNED(annotationlib, PyImport_ImportModule("annotationlib"));

	return (
		annotationlib == NULL ? NULL :
		PyObject_CallMethod(
			annotationlib,
			"call_annotate_function",
			"Oi",
			annotate,
			ANNOTATION_FORMAT_FORWARDREF
		)
	);
}

/*
 * Both arguments are owned, and neither is NULL: every caller takes its
 * `failure` from PyErr_GetRaisedException straight after a call that returned
 * NULL, and CPython sets an exception on every one of those.
 */
static void raise_over(PyObject * const displaced, PyObject * const failure) {
	bool const exits = !PyErr_GivenExceptionMatches(failure, PyExc_Exception);
	PY_MOVABLE(primary, exits ? failure : displaced);
	PY_MOVABLE(behind, exits ? displaced : failure);

	if (context_slot_is_free(primary)) {
		PyException_SetContext(primary, py_move(&behind));
	}

	PyErr_SetRaisedException(py_move(&primary));
}

static bool context_slot_is_free(PyObject * const error) {
	PY_OWNED(context, PyException_GetContext(error));
	PY_OWNED(cause, PyException_GetCause(error));

	return (
		context == NULL &&
		cause == NULL &&
		!((PyBaseExceptionObject *) error)->suppress_context
	);
}

static enum symbol_verdict names_an_unresolved_symbol(PyObject * const error) {
	PyObject * found = NULL;

	if (PyObject_GetOptionalAttrString(error, "name", &found) < 0) {
		return SYMBOL_UNREADABLE;
	}

	PY_OWNED(name, found);

	return (
		name != NULL && PyUnicode_Check(
			name
		) && PyUnicode_GET_LENGTH(name) > 0 ? SYMBOL_UNRESOLVED :
		SYMBOL_NOT_A_SYMBOL
	);
}
#endif
