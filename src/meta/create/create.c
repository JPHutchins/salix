#include <Python.h>
#include <stdbool.h>

#include "../../fields.h"
#include "../settle/settle.h"
#include "../meta.h"
#include "../../options.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"
#include "create.h"

PyObject * build_struct_class(
	PyTypeObject * const metatype,
	StructType const * const base,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const original_namespace,
	PyObject * const keywords,
	struct salix_state * const state
) {
	struct base_survey const survey = survey_bases(bases);
	struct options const inherited = inherited_options(survey.behaviour, survey.facts);
	PY_MOVABLE(forwarded_options, NULL);
	struct options_request request = options_read(
		keywords,
		inherited,
		survey.facts,
		&forwarded_options
	);

	if (request.tag == OPTIONS_REJECTED) {
		return NULL;
	}

	bool const adds_weakref_slot = request.options.weakref && !survey.facts.weakref_carried;
	bool const weakref_keyword_rides = (
		(request.weakref_written && request.options.weakref) ||
		(survey.behaviour != NULL && survey.behaviour->struct_options.weakref)
	);

	PyTypeObject * const handoff = winning_metatype(metatype, bases);
	PyObject * forwarded_keywords = NULL;
	PY_MOVABLE(weakref_only, NULL);
	PY_MOVABLE(chain, NULL);
	PyObject * keyword_rungs_storage[3] = {NULL, NULL, NULL};
	PyObject * const * keyword_rungs = keyword_rungs_storage;
	Py_ssize_t keyword_rung_count = 1;
	bool laddered = false;
	struct chain_verdict verdict = {.accepts_all = 1, .accepts_weakref = 1, .readable = true};

	if (
		handoff != metatype &&
		handoff->tp_new != StructMeta_new &&
		keywords != NULL &&
		PyDict_GET_SIZE(keywords) > 0
	) {
		chain = metaclass_chain(handoff);

		if (chain == NULL) {
			return NULL;
		}

		verdict = chain_probe(chain, keywords, weakref_keyword_rides, NULL);

		if (verdict.accepts_all < 0) {
			return NULL;
		}

		if (verdict.readable) {
			if (verdict.accepts_all == 1) {
				forwarded_keywords = keywords;
			} else if (weakref_keyword_rides && verdict.accepts_weakref == 1) {
				weakref_only = PyDict_New();

				if (
					weakref_only == NULL ||
					(
						PyDict_SetItemString(
							weakref_only,
							option_keywords[OPTION_WEAKREF],
							Py_True
						) < 0
					)
				) {
					return NULL;
				}

				forwarded_keywords = weakref_only;
			}
		} else {
			keyword_rungs_storage[0] = keywords;
			laddered = true;

			if (weakref_keyword_rides) {
				weakref_only = PyDict_New();

				if (
					weakref_only == NULL ||
					(
						PyDict_SetItemString(
							weakref_only,
							option_keywords[OPTION_WEAKREF],
							Py_True
						) < 0
					)
				) {
					return NULL;
				}

				keyword_rungs_storage[1] = weakref_only;
				keyword_rung_count = 3;
			} else {
				keyword_rung_count = 2;
			}
		}
	}

	if (
		forwarded_options != NULL &&
		PyDict_GET_SIZE(forwarded_options) > 0 &&
		verdict.accepts_all == 0
	) {
		PyObject * declined = NULL;
		struct chain_verdict const unowned_verdict = chain_probe(
			chain,
			forwarded_options,
			false,
			&declined
		);

		if (unowned_verdict.accepts_all < 0) {
			return NULL;
		}

		if (unowned_verdict.accepts_all == 0) {
			if (declined == NULL) {
				PyObject * unowned_value = NULL;
				Py_ssize_t unowned_position = 0;
				PyDict_Next(forwarded_options, &unowned_position, &declined, &unowned_value);
			}

			PyErr_Format(
				PyExc_TypeError,
				"'%U' cannot reach __init_subclass__ through %.200s.__new__",
				declined,
				handoff->tp_name
			);

			return NULL;
		}

		if (forwarded_keywords == NULL) {
			forwarded_keywords = forwarded_options;
		} else {
			if (PyDict_Update(forwarded_keywords, forwarded_options) < 0) {
				return NULL;
			}
		}
	}

	if (
		weakref_keyword_rides &&
		!survey.facts.weakref_carried &&
		handoff->tp_new != StructMeta_new &&
		verdict.readable &&
		verdict.accepts_weakref == 0
	) {
		PyErr_SetString(
			PyExc_TypeError,
			"weakref=True cannot cross a metaclass __new__ that hands the build "
			"off: the re-entered call cannot add the weakref slot"
		);

		return NULL;
	}

	if (refuse_two_fielded_layouts(bases, handoff) != RESULT_OK) {
		return NULL;
	}

	struct field_plan plan = field_plan_build(base, bases, original_namespace);

	if (field_plan_failed(&plan)) {
		return NULL;
	}

	if (
		verify_settle_names_readable(original_namespace) != RESULT_OK ||
		refuse_reserved_metadata_names(original_namespace, plan.new_names) != RESULT_OK ||
		refuse_unreachable_parameters(bases, handoff, base, plan.init_var_names) != RESULT_OK ||
		refuse_colliding_methods(original_namespace, plan.parameter_names, name) != RESULT_OK ||
		refuse_mixin_method_fields(plan.all_names) != RESULT_OK ||
		refuse_slot_name_fields(plan.new_names) != RESULT_OK ||
		(
			refuse_displaced_slots(
				original_namespace,
				plan.all_names,
				request.options,
				survey.facts.instance_dict_carried
			) != RESULT_OK
		)
	) {
		field_plan_clear(&plan);

		return NULL;
	}

	int const defines_eq = dict_has_string(original_namespace, "__eq__");

	if (defines_eq < 0) {
		field_plan_clear(&plan);

		return NULL;
	}

	bool const body_defines_eq = defines_eq == 1;

	struct equality_source const inherited_equality = (
		request.options.eq == inherited.eq && !body_defines_eq ? resolves_body_equality(bases) :
		(struct equality_source){.tag = EQUALITY_RESOLVED, .from_a_body = false}
	);

	if (inherited_equality.tag == EQUALITY_FAILED) {
		field_plan_clear(&plan);

		return NULL;
	}

	bool const inherits_body_eq = inherited_equality.from_a_body;
	bool const frozen_across_bases = request.options.frozen && any_struct_base_is_mutable(bases);
	bool const bases_divert_setattro = any_base_diverts_setattro(bases);

	PY_OWNED(
		namespace,
		build_class_namespace(
			original_namespace,
			plan.all_names,
			plan.parameter_names,
			plan.new_names,
			request.options,
			base,
			adds_weakref_slot,
			inherited,
			frozen_across_bases,
			bases_divert_setattro,
			body_defines_eq,
			inherits_body_eq,
			inherited_equality.needs_derived_not_equal
		)
	);
	StructType * struct_class = (
		namespace != NULL ? create_class(
			metatype,
			handoff,
			name,
			bases,
			namespace,
			keyword_rungs,
			keyword_rung_count,
			forwarded_keywords,
			forwarded_options,
			laddered,
			state->handoff_attempt,
			state->handoff_declined,
			state->handoff_new
		) :
		NULL
	);

	if (struct_class != NULL) {
		struct_class->struct_state = base != NULL ? base->struct_state : NULL;
		enum result const settled = (
			(
				field_plan_resolve_mro_defaults(
					&plan,
					&struct_class->heap_type.ht_type
				) != RESULT_OK
			) ? RESULT_ERROR :
			struct_class->struct_field_names == NULL ? install_fields(
				struct_class,
				base,
				&plan,
				request.options,
				body_defines_eq || inherits_body_eq
			) :
			settle_planned(
				struct_class,
				base,
				bases,
				name,
				&plan,
				original_namespace,
				request.options,
				inherited,
				frozen_across_bases,
				body_defines_eq,
				inherits_body_eq,
				inherited_equality.needs_derived_not_equal
			)
		);

		if (settled != RESULT_OK) {
			Py_CLEAR(struct_class);
		} else {
			int const defines_hash = dict_has_string(original_namespace, rebind_hash[0]);
			int const defines_setattr = dict_has_string(original_namespace, "__setattr__");

			if (
				defines_hash < 0 ||
				defines_setattr < 0 ||
				refuse_rebound_class_names(struct_class) != RESULT_OK
			) {
				Py_CLEAR(struct_class);
			} else {
				struct binding_plan const bindings = binding_plan(
					request.options,
					inherited,
					frozen_across_bases,
					bases_divert_setattro,
					body_defines_eq,
					inherits_body_eq,
					inherited_equality.needs_derived_not_equal,
					defines_hash == 1,
					defines_setattr == 1
				);

				if (
					(
						settle_mro_bindings(
							struct_class,
							bases,
							original_namespace,
							bindings,
							request.options
						) != RESULT_OK
					) ||
					(
						install_constructor(
							struct_class,
							original_namespace,
							bases_divert_setattro
						) != RESULT_OK
					)
				) {
					Py_CLEAR(struct_class);
				}
			}
		}
	}

	field_plan_clear(&plan);

	return (PyObject *) struct_class;
}
