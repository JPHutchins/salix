#include <Python.h>

#include "../fields.h"
#include "../meta.h"
#include "../result.h"

typedef struct {
	PyObject_HEAD
	PyObject * default_value;
	enum keyword_only_choice keyword_only;
} FieldSpecifierObject;

static PyTypeObject FieldSpecifier_Type;

static enum keyword_only_choice keyword_only_choice_of(
	PyObject * chosen,
	PyObject * missing,
	bool * failed
);

PyObject * Struct_field_specifier(
	PyObject * const module,
	PyObject * const arguments,
	PyObject * const keywords
) {
	if (PyTuple_GET_SIZE(arguments) > 0) {
		PyErr_SetString(
			PyExc_TypeError,
			"field() takes keyword arguments only: default and kw_only"
		);

		return NULL;
	}

	struct salix_state const * const state = (struct salix_state const *) PyModule_GetState(module);

	if (state == NULL) {
		return NULL;
	}

	PyObject * default_value = NULL;
	PyObject * keyword_only = NULL;
	Py_ssize_t position = 0;
	PyObject * name;
	PyObject * value;

	while (keywords != NULL && PyDict_Next(keywords, &position, &name, &value)) {
		if (PyUnicode_CompareWithASCIIString(name, "default") == 0) {
			default_value = value;
		} else if (PyUnicode_CompareWithASCIIString(name, "kw_only") == 0) {
			keyword_only = value;
		} else {
			PyErr_Format(
				PyExc_TypeError,
				"field() takes default and kw_only; salix does not support '%U'",
				name
			);

			return NULL;
		}
	}

	bool failed = false;
	enum keyword_only_choice const choice = keyword_only_choice_of(
		keyword_only,
		state->missing,
		&failed
	);

	if (failed) {
		return NULL;
	}

	FieldSpecifierObject * const specifier = PyObject_GC_New(
		FieldSpecifierObject,
		&FieldSpecifier_Type
	);

	if (specifier == NULL) {
		return NULL;
	}

	specifier->default_value = (
		default_value != NULL && default_value != state->missing ? Py_NewRef(default_value) :
		NULL
	);
	specifier->keyword_only = choice;
	PyObject_GC_Track(specifier);

	return (PyObject *) specifier;
}

struct field_specifier field_specifier_of(PyObject * const value) {
	if (value == NULL || !Py_IS_TYPE(value, &FieldSpecifier_Type)) {
		return (struct field_specifier){.tag = FIELD_SPECIFIER_ABSENT};
	}

	FieldSpecifierObject const * const specifier = (FieldSpecifierObject const *) value;

	return (struct field_specifier){
		.tag = FIELD_SPECIFIER_PRESENT,
		.default_value = specifier->default_value,
		.keyword_only = specifier->keyword_only,
	};
}

enum result field_specifier_install(void) {
	return PyType_Ready(&FieldSpecifier_Type) < 0 ? RESULT_ERROR : RESULT_OK;
}

static enum keyword_only_choice keyword_only_choice_of(
	PyObject * const chosen,
	PyObject * const missing,
	bool * const failed
) {
	if (chosen == NULL || chosen == missing) {
		return KEYWORD_ONLY_UNSPECIFIED;
	}

	int const truth = PyObject_IsTrue(chosen);

	*failed = truth < 0;

	return truth == 1 ? KEYWORD_ONLY_CHOSEN : KEYWORD_ONLY_DECLINED;
}

static int FieldSpecifier_traverse(PyObject * const self, visitproc const visit, void * const arg) {
	Py_VISIT(((FieldSpecifierObject *) self)->default_value);

	return 0;
}

static int FieldSpecifier_clear(PyObject * const self) {
	Py_CLEAR(((FieldSpecifierObject *) self)->default_value);

	return 0;
}

static void FieldSpecifier_dealloc(PyObject * const self) {
	PyObject_GC_UnTrack(self);
	FieldSpecifier_clear(self);
	Py_TYPE(self)->tp_free(self);
}

static PyTypeObject FieldSpecifier_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "salix._FieldSpecifier",
	.tp_basicsize = sizeof(FieldSpecifierObject),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
	.tp_dealloc = FieldSpecifier_dealloc,
	.tp_traverse = FieldSpecifier_traverse,
	.tp_clear = FieldSpecifier_clear,
};
