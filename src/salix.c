#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "construct.h"
#include "meta.h"
#include "mixin.h"
#include "owned.h"
#include "result.h"

static int struct_exec(PyObject * module);
static enum result add_struct_base(PyObject * module);
static PyObject * create_struct_base(PyObject * module);
static PyObject * handoff_new(PyObject * self, PyObject * args);
static void struct_free(void * module);

/*
 * A type's field metadata is written once at class creation and then only
 * read. An instance's slots are written by the constructor before it returns,
 * and afterwards only through PyMember_SetOne, so a free-threaded build's
 * guarantees there are inherited rather than reimplemented.
 */
static PyModuleDef_Slot struct_slots[] = {
	{Py_mod_exec, struct_exec},
#ifdef Py_mod_multiple_interpreters
	{Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
#endif
#ifdef Py_mod_gil
	{Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
	{0, NULL},
};

static PyMethodDef struct_functions[] = {
	{
		.ml_name = "set_field",
		.ml_meth = Struct_set_field,
		.ml_flags = METH_VARARGS,
		.ml_doc = "set_field(instance, name, value) -- assign a field the class declared.",
	},
	{
		.ml_name = "from_mapping",
		.ml_meth = Struct_from_mapping,
		.ml_flags = METH_VARARGS,
		.ml_doc = "from_mapping(cls, values) -- construct a struct from a mapping of field values.",
	},
	{.ml_name = NULL},
};

static PyModuleDef struct_module = {
	PyModuleDef_HEAD_INIT,
	.m_name = "salix",
	.m_doc = "A minimal C-backed inheritable Struct base class.",
	.m_size = sizeof(struct salix_state),
	.m_slots = struct_slots,
	.m_methods = struct_functions,
	.m_free = struct_free,
};

PyMODINIT_FUNC PyInit_salix(void) {
	return PyModuleDef_Init(&struct_module);
}

PyModuleDef * salix_module_def(void) {
	return &struct_module;
}

static void struct_free(void * const module) {
	/* m_free receives the module object on every supported version, so the
	 * state is reached back through it. */
	struct salix_state * const state = (struct salix_state *) PyModule_GetState(
		(PyObject *) module
	);

	for (Py_ssize_t i = 0; i < SETTLE_BINDING_COUNT; ++i) {
		Py_CLEAR(state->mixin_bindings[i]);
		Py_CLEAR(state->object_bindings[i]);
	}

	Py_CLEAR(state->handoff_attempt);
	Py_CLEAR(state->handoff_declined);
	Py_CLEAR(state->handoff_new);
}

static PyMethodDef handoff_new_method = {
	.ml_name = "_handoff_new",
	.ml_meth = handoff_new,
	.ml_flags = METH_VARARGS,
	.ml_doc = NULL,
};

static PyObject * build_handoff_machinery(struct salix_state * const state) {
	static char const * const source =
		"class _HandoffDeclined(TypeError):\n"
		"    pass\n"
		"\n"
		"\n"
		"def _handoff_attempt(\n"
		"    new, builder, type_args, keywords, declined=_HandoffDeclined, own_name=\"_handoff_attempt\"\n"
		"):\n"
		"    try:\n"
		"        return new(builder, type_args, keywords)\n"
		"    except TypeError as error:\n"
		"        tb = error.__traceback__\n"
		"        while tb is not None and tb.tb_next is not None:\n"
		"            tb = tb.tb_next\n"
		"        if (\n"
		"            tb is not None\n"
		"            and tb.tb_frame is not None\n"
		"            and tb.tb_frame.f_code.co_name == own_name\n"
		"            and tb.tb_frame.f_code.co_filename == \"<salix handoff>\"\n"
		"        ):\n"
		"            raise declined(str(error)).with_traceback(error.__traceback__) from None\n"
		"        raise\n";

	PY_OWNED(code, Py_CompileString(source, "<salix handoff>", Py_file_input));
	PY_OWNED(globals_dict, PyDict_New());
	PY_OWNED(new_fn, PyCFunction_New(&handoff_new_method, NULL));
	PY_OWNED(module_name, PyUnicode_FromString(struct_module.m_name));

	if (
		code == NULL ||
		globals_dict == NULL ||
		new_fn == NULL ||
		module_name == NULL ||
		PyDict_SetItemString(globals_dict, "__name__", module_name) < 0
	) {
		return NULL;
	}

	PY_OWNED(result, PyEval_EvalCode(code, globals_dict, globals_dict));

	if (result == NULL) {
		return NULL;
	}

	PyObject * const attempt = PyDict_GetItemString(globals_dict, "_handoff_attempt");
	PyObject * const declined = PyDict_GetItemString(globals_dict, "_HandoffDeclined");

	if (attempt == NULL || declined == NULL) {
		return NULL;
	}

	Py_XSETREF(state->handoff_attempt, Py_NewRef(attempt));
	Py_XSETREF(state->handoff_declined, Py_NewRef(declined));
	Py_XSETREF(state->handoff_new, Py_NewRef(new_fn));

	return state->handoff_attempt;
}

static PyObject * handoff_new(PyObject * const self, PyObject * const args) {
	PyObject * builder;
	PyObject * type_args;
	PyObject * keywords;

	if (!PyArg_ParseTuple(args, "OO!O", &builder, &PyTuple_Type, &type_args, &keywords)) {
		return NULL;
	}

	if (!PyType_Check(builder)) {
		PyErr_SetString(PyExc_TypeError, "the hand-off builder is not a type");

		return NULL;
	}

	return PyType_Type.tp_new(
		(PyTypeObject *) builder,
		type_args,
		keywords == Py_None ? NULL : keywords
	);
}

static int struct_exec(PyObject * const module) {
	StructMeta_Type.tp_base = &PyType_Type;

	if (PyType_Ready(&StructMeta_Type) < 0 || PyType_Ready(&StructMixin_Type) < 0) {
		return RESULT_ERROR;
	}

	struct salix_state * const state = (struct salix_state *) PyModule_GetState(module);

	if (state == NULL || settle_cache_fill(state) != RESULT_OK) {
		return RESULT_ERROR;
	}

	if (build_handoff_machinery(state) == NULL) {
		return RESULT_ERROR;
	}

	return add_struct_base(module);
}

static enum result add_struct_base(PyObject * const module) {
	PyObject * const struct_base = create_struct_base(module);

	if (struct_base == NULL) {
		return RESULT_ERROR;
	}

	int const added = PyModule_AddObjectRef(module, "Struct", struct_base);
	Py_DECREF(struct_base);

	return added < 0 ? RESULT_ERROR : RESULT_OK;
}

static PyObject * create_struct_base(PyObject * const module) {
	PY_OWNED(name, PyUnicode_FromString("Struct"));
	PY_OWNED(module_name, PyUnicode_FromString(struct_module.m_name));
	PY_OWNED(bases, PyTuple_Pack(1, (PyObject *) &StructMixin_Type));
	PY_OWNED(namespace, PyDict_New());

	if (name == NULL || module_name == NULL || bases == NULL || namespace == NULL) {
		return NULL;
	}

	if (PyDict_SetItemString(namespace, "__module__", module_name) < 0) {
		return NULL;
	}

	return struct_create_root(name, bases, namespace, module);
}
