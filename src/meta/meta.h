#pragma once

#include <Python.h>
#include <stdbool.h>

#include "../options.h"
#include "../result.h"

typedef struct StructType StructType;

enum hash_binding {
	HASH_BODY_DEFINED,
	HASH_INHERITED_EQ,
	HASH_NONE,
	HASH_BIND,
};

struct binding_plan {
	bool answered_by_body;
	bool rebind_comparison;
	bool rebind_not_equal;
	bool rebind_representation;
	bool rebind_mutability;
	enum hash_binding hash;
	bool match_args_wanted;
};

extern PyTypeObject StructMeta_Type;

PyObject * StructMeta_new(PyTypeObject * metatype, PyObject * args, PyObject * keywords);

/* The public ``Struct`` base, built once at module init. It is the one class
 * with no struct base among its own, which is exactly what StructMeta_new
 * refuses -- so it is built here instead of through a metaclass call. */
PyObject * struct_create_root(PyObject * name, PyObject * bases, PyObject * namespace);

struct member_lookup {
	enum { MEMBER_LOOKUP_FOUND, MEMBER_LOOKUP_MISSING, MEMBER_LOOKUP_ERROR } tag;
	Py_ssize_t slot_offset;
};

struct member_lookup find_member(
	struct PyMemberDef const * members,
	Py_ssize_t member_count,
	PyObject * name
);

struct equality_source {
	enum { EQUALITY_RESOLVED, EQUALITY_FAILED } tag;
	bool from_a_body;
	bool needs_derived_not_equal;
};

StructType * find_struct_base(PyObject * bases);
StructType * find_behaviour_base(PyObject * bases);
struct equality_source resolves_body_equality(PyObject * bases);
struct options inherited_options(PyObject * bases, StructType const * behaviour);
bool any_struct_base_is_mutable(PyObject * bases);
bool has_weakref_slot(StructType const * base);
bool weakref_expected(struct options options, PyObject * bases);
bool any_base_has_instance_dict(PyObject * bases);

struct binding_plan binding_plan(
	struct options options,
	struct options inherited,
	bool frozen_across_bases,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal,
	bool body_defines_hash
);

extern char const * const rebind_comparison[];
extern char const * const rebind_not_equal[];
extern char const * const rebind_representation[];
extern char const * const rebind_mutability[];
extern char const * const rebind_hash[];

PyObject * build_class_namespace(
	PyObject * original_namespace,
	PyObject * all_names,
	PyObject * new_names,
	struct options options,
	StructType const * base,
	struct options inherited,
	bool frozen_across_bases,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal
);
enum result refuse_displaced_slots(
	PyObject * original_namespace,
	PyObject * all_names,
	bool carries_a_weakref_slot,
	bool carries_an_instance_dict
);
enum result refuse_colliding_methods(
	PyObject * original_namespace,
	PyObject * all_names,
	PyObject * class_name
);
enum result refuse_mixin_method_fields(PyObject * all_names);
enum result refuse_slot_name_fields(PyObject * all_names);

PyObject * build_struct_class(
	PyTypeObject * metatype,
	StructType const * base,
	PyObject * name,
	PyObject * bases,
	PyObject * original_namespace,
	PyObject * keywords
);
