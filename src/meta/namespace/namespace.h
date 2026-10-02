#pragma once

enum slot_name_owner { SLOT_NAME_NONE, SLOT_NAME_WEAKREF, SLOT_NAME_INSTANCE_DICT };

char const * weakref_slot_name(void);
char const * instance_dict_slot_name(void);
enum slot_name_owner slot_name_owner_of(PyObject * name);
PyObject * build_slots(PyObject * new_names, bool adds_weakref_slot);
