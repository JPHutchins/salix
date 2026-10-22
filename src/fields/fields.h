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
	PyObject * keyword_only_marker;
	PyObject * class_var_name;
	PyObject * init_var_name;
	PyObject * keyword_only_marker_name;
};

PyObject * module_attribute(char const * module_name, char const * attribute);

struct special_form special_form_of(PyObject * annotation, struct form_probes const * probes);

bool class_var_top_level(PyObject * annotation, struct form_probes const * probes);

bool init_var_top_level(PyObject * annotation, struct form_probes const * probes);

bool marks_keyword_only(PyObject * annotation, struct form_probes const * probes);

extern struct special_form const CLASS_VAR_FORM;
extern struct special_form const INIT_VAR_FORM;

enum inheritance : int {
	INHERITANCE_ERROR = -1,
	INHERITANCE_NEW = 0,
	INHERITANCE_INHERITED = 1,
};

struct inheritance_lookup {
	enum inheritance tag;
	Py_ssize_t position;
};

struct parameter_lists {
	PyObject * names;
	PyObject * init_var_flags;
	PyObject * annotations;
	PyObject * metadata;
	PyObject * keyword_only_names;
};

enum result refuse_reserved_name(PyObject * field_name);
enum result insert_parameter(
	struct parameter_lists const * parameters,
	Py_ssize_t at,
	PyObject * parameter_name,
	PyObject * kind_flag,
	PyObject * annotation,
	PyObject * metadata
);
enum result append_parameter(
	struct parameter_lists const * parameters,
	PyObject * parameter_name,
	PyObject * kind_flag,
	PyObject * annotation,
	PyObject * metadata
);
struct inheritance_lookup inherited_position(
	PyObject * parameter_names,
	Py_ssize_t inherited_count,
	PyObject * field_name
);
enum result append_inherited(
	StructType const * base,
	struct parameter_lists const * parameters,
	PyObject * default_by_name,
	PyObject * empty_extras
);
enum result merge_secondary_class_vars(
	PyObject * bases,
	StructType const * base,
	struct parameter_lists const * parameters,
	PyObject * default_by_name,
	PyObject * empty_extras
);
enum result append_declared(
	PyObject * annotations,
	PyObject * namespace,
	Py_ssize_t inherited_count,
	struct parameter_lists const * parameters,
	PyObject * new_names,
	PyObject * default_by_name,
	PyObject * mro_default_names,
	PyObject * empty_extras,
	bool class_keyword_only
);
