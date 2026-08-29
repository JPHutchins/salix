#include <Python.h>
#include <stdbool.h>

#if PY_VERSION_HEX < 0x030B0000
#	include <code.h>
#else
#	include <cpython/code.h>
#endif

#include "../../fields.h"
#include "../meta.h"
#include "../../options.h"
#include "../../owned.h"
#include "../../result.h"
#include "../../types.h"

struct chain_verdict {
	int accepts_all;
	int accepts_weakref;
	bool readable;
};
static int code_accepts_keyword(PyCodeObject * code, PyObject * varnames, char const * keyword);
static PyObject * metaclass_chain(PyTypeObject * winner);
static struct chain_verdict chain_probe(PyObject * chain, PyObject * keywords, bool weakref_column);
static StructType * create_class(
	PyTypeObject * metatype,
	PyTypeObject * handoff,
	PyObject * name,
	PyObject * bases,
	PyObject * namespace,
	PyObject * const * keyword_rungs,
	Py_ssize_t keyword_rung_count,
	PyObject * forwarded_keywords,
	bool laddered,
	PyObject * handoff_attempt,
	PyObject * handoff_declined,
	PyObject * handoff_new
);
static PyTypeObject * winning_metatype(PyTypeObject * requested, PyObject * bases);
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
	struct options_request request = options_read(keywords, inherited, survey.facts);

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

		verdict = chain_probe(chain, keywords, weakref_keyword_rides);

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
					PyDict_SetItemString(weakref_only, option_keywords[OPTION_WEAKREF], Py_True) <
						0
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
					PyDict_SetItemString(weakref_only, option_keywords[OPTION_WEAKREF], Py_True) <
						0
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

struct field_plan plan = field_plan_build(base, original_namespace);

	if (field_plan_failed(&plan)) {
		return NULL;
	}

	if (
		verify_settle_names_readable(original_namespace) != RESULT_OK ||
		refuse_reserved_metadata_names(original_namespace, plan.new_names) != RESULT_OK ||
		refuse_colliding_methods(original_namespace, plan.all_names, name) != RESULT_OK ||
		refuse_mixin_method_fields(plan.all_names, plan.class_var_names) != RESULT_OK ||
		refuse_slot_name_fields(plan.new_names) != RESULT_OK ||
		refuse_displaced_slots(
				original_namespace,
				plan.all_names,
				request.options,
				survey.facts.instance_dict_carried
			) !=
			RESULT_OK
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

			if (defines_hash < 0 || defines_setattr < 0) {
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
					settle_mro_bindings(
							struct_class,
							bases,
							original_namespace,
							bindings,
							request.options
						) !=
						RESULT_OK ||
					install_constructor(struct_class, bases_divert_setattro) != RESULT_OK
				) {
					Py_CLEAR(struct_class);
				}
			}
		}
	}

	field_plan_clear(&plan);

	return (PyObject *) struct_class;
}

static int code_accepts_keyword(
	PyCodeObject * const code,
	PyObject * const varnames,
	char const * const keyword
) {
	Py_ssize_t const named = code->co_argcount + code->co_kwonlyargcount;

	for (Py_ssize_t i = code->co_posonlyargcount; i < named; ++i) {
		int const compared = PyUnicode_CompareWithASCIIString(
			PyTuple_GET_ITEM(varnames, i),
			keyword
		);

		if (compared == 0) {
			return 1;
		}

		if (compared < 0 && PyErr_Occurred()) {
			return -1;
		}
	}

	return 0;
}

static struct chain_verdict chain_probe(
	PyObject * const chain,
	PyObject * const keywords,
	bool const weakref_column
) {
	struct chain_verdict verdict = {.accepts_all = 1, .accepts_weakref = 1, .readable = true};

	for (Py_ssize_t i = 0; i < PyList_GET_SIZE(chain); ++i) {
		PyObject * const link = PyList_GET_ITEM(chain, i);

		if (!PyFunction_Check(link)) {
			verdict.readable = false;
			verdict.accepts_all = 0;
			verdict.accepts_weakref = 0;
			continue;
		}

		PyCodeObject * const code = (PyCodeObject *) ((PyFunctionObject *) link)->func_code;

		if ((code->co_flags & CO_VARKEYWORDS) != 0) {
			continue;
		}

#if PY_VERSION_HEX >= 0x030B0000
		PY_OWNED(varnames, PyCode_GetVarnames(code));
#else
		PyObject * const varnames = code->co_varnames;
#endif

		if (varnames == NULL) {
			verdict.accepts_all = -1;

			return verdict;
		}

		Py_ssize_t position = 0;
		PyObject * key;
		PyObject * value;

		while (PyDict_Next(keywords, &position, &key, &value)) {
			char const * const name = PyUnicode_AsUTF8(key);

			if (name == NULL) {
				verdict.accepts_all = -1;

				return verdict;
			}

			int const accepts = code_accepts_keyword(code, varnames, name);

			if (accepts < 0) {
				verdict.accepts_all = -1;

				return verdict;
			}

			if (accepts == 0) {
				verdict.accepts_all = 0;
				break;
			}
		}

		if (weakref_column) {
			int const accepts_weakref = code_accepts_keyword(
				code,
				varnames,
				option_keywords[OPTION_WEAKREF]
			);

			if (accepts_weakref < 0) {
				verdict.accepts_all = -1;

				return verdict;
			}

			if (accepts_weakref == 0) {
				verdict.accepts_weakref = 0;
			}
		}
	}

	return verdict;
}

static PyObject * metaclass_chain(PyTypeObject * const winner) {
	PY_MOVABLE(chain, PyList_New(0));

	if (chain == NULL) {
		return NULL;
	}

	PyObject * const mro = winner->tp_mro;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
		PyTypeObject * const link = (PyTypeObject *) PyTuple_GET_ITEM(mro, i);

		if (link == &StructMeta_Type || link == &PyType_Type) {
			break;
		}

		if (link->tp_new == StructMeta_new) {
			continue;
		}

		PY_OWNED(new, PyObject_GetAttrString((PyObject *) link, "__new__"));

		if (new == NULL || PyList_Append(chain, new) < 0) {
			return NULL;
		}
	}

	return py_move(&chain);
}

static StructType * create_class(
	PyTypeObject * const metatype,
	PyTypeObject * const handoff,
	PyObject * const name,
	PyObject * const bases,
	PyObject * const namespace,
	PyObject * const * const keyword_rungs,
	Py_ssize_t const keyword_rung_count,
	PyObject * forwarded_keywords,
	bool const laddered,
	PyObject * const handoff_attempt,
	PyObject * const handoff_declined,
	PyObject * const handoff_new
) {
	PY_OWNED(type_args, PyTuple_Pack(3, name, bases, namespace));

	if (type_args == NULL) {
		return NULL;
	}

	PyTypeObject * const builder = handoff->tp_new == StructMeta_new ? handoff : metatype;
	PyObject * created = NULL;

	if (!laddered) {
		created = PyType_Type.tp_new(
			builder,
			type_args,
			builder == handoff ? NULL : forwarded_keywords
		);
	}

	for (
		Py_ssize_t attempt = 0;
		created == NULL && laddered && attempt < keyword_rung_count;
		++attempt
	) {
		created = PyObject_CallFunctionObjArgs(
			handoff_attempt,
			handoff_new,
			builder,
			type_args,
			builder == handoff || keyword_rungs[attempt] == NULL ? Py_None :
			keyword_rungs[attempt],
			NULL
		);

		if (created != NULL) {
			break;
		}

		PyObject * exception_type = NULL;
		PyObject * exception_value = NULL;
		PyObject * exception_tb = NULL;
		PyErr_Fetch(&exception_type, &exception_value, &exception_tb);

		bool const declined = (
			exception_value != NULL &&
			PyErr_GivenExceptionMatches(exception_value, handoff_declined)
		);

		if (declined && attempt + 1 < keyword_rung_count) {
			Py_XDECREF(exception_type);
			Py_XDECREF(exception_value);
			Py_XDECREF(exception_tb);
			continue;
		}

		if (declined) {
			PyObject * const original_tb = PyException_GetTraceback(exception_value);
			PY_OWNED(message, PyObject_Str(exception_value));
			Py_XDECREF(exception_type);
			Py_XDECREF(exception_value);
			Py_XDECREF(exception_tb);

			if (message != NULL) {
				PyErr_SetObject(PyExc_TypeError, message);

				if (original_tb != NULL) {
					PyObject * exc_type = NULL;
					PyObject * exc_value = NULL;
					PyObject * exc_tb = NULL;
					PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
					PyException_SetTraceback(exc_value, Py_NewRef(original_tb));
					PyErr_Restore(exc_type, exc_value, exc_tb);
					Py_DECREF(original_tb);
				}
			} else {
				Py_XDECREF(original_tb);
			}

			break;
		}

		PyErr_Restore(exception_type, exception_value, exception_tb);
		break;
	}

	if (created == NULL) {
		return NULL;
	}

	if (!is_struct_class(created)) {
		PyErr_Format(
			PyExc_TypeError,
			"%.200s.__new__ returned %.200s, which is not a struct class",
			handoff->tp_name,
			Py_TYPE(created)->tp_name
		);

		Py_DECREF(created);

		return NULL;
	}

	return (StructType *) py_move(&created);
}

/* Measured, and smaller than the measurement: replacing the body with
 * `return requested` leaves class creation at 9.77-9.87 us for a 16-field
 * class either way, so the walk is bounded by the width of that band rather
 * than shown to be free. It is one Py_TYPE and one PyType_IsSubtype per base,
 * and a class has one. */
static PyTypeObject * winning_metatype(PyTypeObject * const requested, PyObject * const bases) {
	PyTypeObject * winner = requested;

	for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
		PyTypeObject * const candidate = Py_TYPE(PyTuple_GET_ITEM(bases, i));

		if (PyType_IsSubtype(candidate, winner)) {
			winner = candidate;
		}
	}

	return winner;
}
