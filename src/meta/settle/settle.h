#pragma once

#include "../meta.h"
#include "../../types.h"

enum result settle_cache_fill(struct salix_state * state);

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

enum result refuse_rebound_class_names(StructType * struct_class);

enum result settle_rebind_one(
	StructType * struct_class,
	PyObject * original_namespace,
	char const * name,
	bool from_mixin
);

enum result settle_rebind(
	StructType * struct_class,
	PyObject * original_namespace,
	char const * const * names,
	bool from_mixin
);

enum result restore_stripped(
	StructType * struct_class,
	PyObject * original_namespace,
	PyObject * class_dict,
	char const * const * const * tables
);

enum result resolve_dunder(
	PyObject * dict,
	PyObject * name,
	PyObject * * resolved,
	PyTypeObject * entry,
	PyTypeObject * * owner
);

enum result mro_dunders_of(
	PyTypeObject * type,
	PyObject * * resolved_eq,
	PyObject * * resolved_ne,
	PyObject * * resolved_repr,
	PyTypeObject * * eq_owner,
	PyTypeObject * * ne_owner,
	PyTypeObject * * repr_owner,
	PyObject * * resolved_lt,
	PyObject * * resolved_le,
	PyObject * * resolved_gt,
	PyObject * * resolved_ge,
	PyTypeObject * * lt_owner,
	PyTypeObject * * le_owner,
	PyTypeObject * * gt_owner,
	PyTypeObject * * ge_owner
);

bool honoured_owner(PyTypeObject * owner, PyTypeObject * first_struct);

int honours_a_body_comparison(
	PyTypeObject * type,
	PyTypeObject * first_struct,
	PyObject * original_namespace,
	struct salix_state const * state
);
