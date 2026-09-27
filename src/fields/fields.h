#pragma once

#include "../fields.h"

enum special_form_kind {
	SPECIAL_FORM_NONE,
	SPECIAL_FORM_CLASS_VAR,
	SPECIAL_FORM_INIT_VAR,
};

struct special_form {
	enum special_form_kind kind;
	char const * name;
	char const * instead;
};

struct form_probes {
	PyObject * class_var;
	PyObject * init_var;
	PyObject * class_var_name;
	PyObject * init_var_name;
};

PyObject * module_attribute(char const * module_name, char const * attribute);

struct special_form special_form_of(PyObject * annotation, struct form_probes const * probes);

bool class_var_top_level(PyObject * annotation, struct form_probes const * probes);

extern struct special_form const CLASS_VAR_FORM;
extern struct special_form const INIT_VAR_FORM;
