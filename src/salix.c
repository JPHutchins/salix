#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "construct.h"
#include "meta.h"
#include "mixin.h"
#include "owned.h"
#include "result.h"

static int struct_exec(PyObject * module);
static enum result add_struct_base(PyObject * module);
static PyObject * create_struct_base(void);
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

struct salix_state * settle_state(void) {
	PY_OWNED(name, PyUnicode_FromString(struct_module.m_name));

	if (name == NULL) {
		return NULL;
	}

	PyObject * const module = PyImport_GetModule(name);

	return module == NULL ? NULL : (struct salix_state *) PyModule_GetState(module);
}

static void struct_free(void * const module) {
#if PY_VERSION_HEX < 0x030C0000
	/* Before 3.12 m_free receives the module object, not the module state. */
	struct salix_state * const state = (struct salix_state *) PyModule_GetState(
		(PyObject *) module
	);
#else
	struct salix_state * const state = (struct salix_state *) module;
#endif

	for (Py_ssize_t i = 0; i < 7; ++i) {
		Py_CLEAR(state->mixin_bindings[i]);
		Py_CLEAR(state->object_bindings[i]);
	}
}

static int struct_exec(PyObject * const module) {
	StructMeta_Type.tp_base = &PyType_Type;

	if (PyType_Ready(&StructMeta_Type) < 0 || PyType_Ready(&StructMixin_Type) < 0) {
		return RESULT_ERROR;
	}

	struct salix_state * const state = (struct salix_state *) PyModule_GetState(module);

	if (state == NULL) {
		return RESULT_ERROR;
	}

	settle_cache_fill(state);

	return add_struct_base(module);
}

static enum result add_struct_base(PyObject * const module) {
	PyObject * const struct_base = create_struct_base();

	if (struct_base == NULL) {
		return RESULT_ERROR;
	}

	int const added = PyModule_AddObjectRef(module, "Struct", struct_base);
	Py_DECREF(struct_base);

	return added < 0 ? RESULT_ERROR : RESULT_OK;
}

static PyObject * create_struct_base(void) {
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

	return struct_create_root(name, bases, namespace);
}
