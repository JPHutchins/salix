#pragma once

#include <Python.h>
#include <stdbool.h>

#include "../options.h"
#include "../result.h"

typedef struct StructType StructType;
struct field_plan;

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
PyObject * struct_create_root(
	PyObject * name,
	PyObject * bases,
	PyObject * namespace,
	PyObject * module
);

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

struct base_facts {
	bool fielded_frozen;
	bool weakref_carried;
	bool instance_dict_carried;
};

struct base_survey {
	StructType * behaviour;
	struct base_facts facts;
};

StructType * find_struct_base(PyObject * bases);
StructType * find_behaviour_base(PyObject * bases);
struct equality_source resolves_body_equality(PyObject * bases);
struct base_survey survey_bases(PyObject * bases);
struct options inherited_options(StructType const * behaviour, struct base_facts facts);
bool any_struct_base_is_mutable(PyObject * bases);
enum { SETTLE_BINDING_COUNT = 7 };

struct salix_state {
	PyObject * mixin_bindings[SETTLE_BINDING_COUNT];
	PyObject * object_bindings[SETTLE_BINDING_COUNT];
	PyObject * handoff_attempt;
	PyObject * handoff_declined;
	PyObject * handoff_new;
};

enum result settle_cache_fill(struct salix_state * state);
PyModuleDef * salix_module_def(void);
bool any_base_diverts_setattro(PyObject * bases);
bool carries_weakref_slot(PyTypeObject const * type);

struct binding_plan binding_plan(
	struct options options,
	struct options inherited,
	bool frozen_across_bases,
	bool bases_divert_setattro,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal,
	bool body_defines_hash,
	bool body_defines_setattr
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
	bool adds_weakref_slot,
	struct options inherited,
	bool frozen_across_bases,
	bool bases_divert_setattro,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal
);
enum result refuse_displaced_slots(
	PyObject * original_namespace,
	PyObject * all_names,
	struct options options,
	bool instance_dict_carried
);
enum result refuse_colliding_methods(
	PyObject * original_namespace,
	PyObject * all_names,
	PyObject * class_name
);
enum result refuse_mixin_method_fields(PyObject * all_names);
enum result refuse_slot_name_fields(PyObject * all_names);
enum result refuse_reserved_metadata_names(PyObject * original_namespace, PyObject * new_names);

PyObject * build_struct_class(
	PyTypeObject * metatype,
	StructType const * base,
	PyObject * name,
	PyObject * bases,
	PyObject * original_namespace,
	PyObject * keywords,
	struct salix_state * state
);
enum result settle_planned(
	StructType * struct_class,
	StructType const * base,
	PyObject * bases,
	PyObject * name,
	struct field_plan const * plan,
	PyObject * original_namespace,
	struct options options,
	struct options inherited,
	bool frozen_across_bases,
	bool body_defines_eq,
	bool inherits_body_eq,
	bool derive_not_equal
);
enum result settle_mro_bindings(
	StructType * struct_class,
	PyObject * bases,
	PyObject * original_namespace,
	struct binding_plan bindings,
	struct options options
);
enum result verify_settle_names_readable(PyObject * original_namespace);
enum result install_fields(
	StructType * struct_class,
	StructType const * base,
	struct field_plan const * plan,
	struct options options,
	bool resolves_body_eq
);
enum result install_post_init(StructType * struct_class);
bool defines_own_init(StructType const * struct_class);
enum result ensure_singleton(StructType * struct_class);
