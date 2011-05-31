/*
 * Copyright (c) 2011, Edd Barrett <vext01@gmail.com>
 * Copyright (c) 2011, Martin Ellis <ellism88@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#ifdef HAVE_PYTHON

#include <Python.h> /* defines _GNU_SOURCE */
#include <structmember.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>

#include "hgd.h"
#include "py.h"
#include "db.h"

struct hgd_py_modules		 hgd_py_mods;
char				*hgd_py_dir;

/*
 * XXX - reference counts, all over the place!
 */

/*
 * methods exposed to python
 */

/*
 * get the contents of the playlist
 * XXX needs to lock database
 *
 * args:
 * ret: tuple of dicts
 *
 * XXX more error checks
 */
static PyObject *
hgd_py_meth_get_playlist(Hgd *self)
{
	struct hgd_playlist	  list;
	struct hgd_playlist_item *it;
	unsigned int		  i;
	PyObject		 *rec, *ret_list, *v_filename, *v_id, *v_user;
	PyObject		 *k_filename, *k_id, *k_user, *plist_item = NULL;
	PyObject		 *playlist_item_t, *ctor = NULL, *args = NULL;

	self = self;

	if (hgd_get_playlist(&list) == HGD_FAIL)
		Py_RETURN_NONE; /* XXX throw exception */

	ret_list = PyList_New(list.n_items);
	if (!ret_list) /* XXX exception? */
		DPRINTF(HGD_D_ERROR, "could not allocate python list");

	/* get ready to construct some stuff */
	ctor = PyObject_GetAttrString(hgd_py_mods.hgd_support, "PlaylistItem");
	if (!ctor) {
		DPRINTF(HGD_D_ERROR, "Couldn't find PlaylistItem constructor");
		hgd_exit_nicely();
	}

	if (!PyCallable_Check(ctor)) {
		PRINT_PY_ERROR();
		DPRINTF(HGD_D_WARN, "PlaylistItem constructor is not callable");
		hgd_exit_nicely();
	}

	for (i = 0; i < list.n_items; i++) {
		it = list.items[i];

		rec = Py_BuildValue("{sissssssss}",
		    "tid", it->id,
		    "filename", it->filename,
		    "tag_artist", it->tag_artist,
		    "tag_title", it->tag_title,
		    "user", it->user);
		if (rec == NULL)
			DPRINTF(HGD_D_ERROR, "could not allocate python dict");

		args = PyTuple_New(1);
		PyTuple_SetItem(args, 0, rec);

		plist_item = PyObject_CallObject(ctor, args);
		Py_XDECREF(rec); /* don't decrement tuple refct! */
		if (plist_item == NULL) {
			PRINT_PY_ERROR();
			DPRINTF(HGD_D_WARN, "failed to construct PlaylistItem");
			continue;
		}

		/* steals ref */
		if (PyList_SetItem(ret_list, i, plist_item) != 0) {
			PRINT_PY_ERROR();
			DPRINTF(HGD_D_ERROR, "can't add to list");
		}
	}

clean:
	Py_XDECREF(ctor);
	hgd_free_playlist(&list);
	return (ret_list);
}

/* make some stuff read only */
static int
hgd_py_meth_read_only_raise(Hgd *self, PyObject *value, void *closure)
{
	(void) PyErr_Format(PyExc_AttributeError, "attribute is read-only");
	return (-1);
}

static PyObject *
hgd_py_meth_get_hgd_version(Hgd *self, void *closure)
{
	Py_INCREF(self->hgd_version);
	return (self->hgd_version);
}

/* method table */
static PyMethodDef hgd_py_methods[] = {
	{"get_playlist",
	    (PyCFunction) hgd_py_meth_get_playlist,
	    METH_NOARGS, "get the current hgd playlist"},
	{ 0, 0, 0, 0 }
};

/* member table */
static PyMemberDef hgd_py_members[] = {
	{"proto_version",
	    T_INT, offsetof(Hgd, proto_version), 0, "protocol version"},
	{"debug_level",
	    T_INT, offsetof(Hgd, debug_level), 0, "debug level"},
	{"component",
	    T_OBJECT_EX, offsetof(Hgd, component), 0, "which hgd process?"},
	{0, 0, 0, 0, 0}
};

/* member get/set table */
static PyGetSetDef hgd_py_get_setters[] = {
	{"hgd_version", (getter) hgd_py_meth_get_hgd_version,
		(setter) hgd_py_meth_read_only_raise,
		"hgd version", NULL},
	{NULL, NULL, NULL, NULL, NULL}  /* Sentinel */
};

/* __new__ */
static PyObject *
hgd_py_meth_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	Hgd			*self;

	DPRINTF(HGD_D_INFO, "__new__ hgd object");

	/* quiet */
	args = args;
	kwds = kwds;

	self = (Hgd *)type->tp_alloc(type, 0);

	if (self != NULL) {
		self->hgd_version = PyString_FromString(HGD_VERSION);
		if (self->hgd_version == NULL) {
			DPRINTF(HGD_D_ERROR, "couldn't init self.hgd_version");
			Py_DECREF(self);
			return NULL;
		}

		self->component = PyString_FromString(hgd_component);
		if (self->component == NULL) {
			DPRINTF(HGD_D_ERROR, "couldn't init self.componentn");
			Py_DECREF(self);
			return NULL;
		}
	}

	self->proto_version = HGD_PROTO_VERSION;
	self->debug_level = hgd_debug;

	return (PyObject *)self;
}

/* __init__ */
static int
hgd_py_meth_init(Hgd *self, PyObject *args, PyObject *kwds)
{
	DPRINTF(HGD_D_INFO, "__init__ hgd object");

	/* quiet */
	self = self;
	args = args;
	kwds = kwds;

	/* does nothing for now, but may need it later? */

	return (0);
}

static void
hgd_py_meth_dealloc(Hgd *self)
{
	Py_XDECREF(self->hgd_version);
	Py_XDECREF(self->component);
	self->ob_type->tp_free((PyObject*)self);
}


/*
 * Describe the hgd object type
 * This is for Python 2.6 and will probably make warnings on other versions
 */
static PyTypeObject HgdType = {
	PyObject_HEAD_INIT(NULL)
	0,				/* ob_size */
	"hgd.Hgd",			/* tp_name */
	sizeof(Hgd),			/* tp_basicsize */
	0,				/* tp_itemsize */
	(destructor) hgd_py_meth_dealloc,	/* tp_dealloc */
	0,				/* tp_print */
	0,				/* tp_getattr */
	0,				/* tp_setattr */
	0,				/* tp_compare */
	0,				/* tp_repr */
	0,				/* tp_as_number */
	0,				/* tp_as_sequence */
	0,				/* tp_as_mapping */
	0,				/* tp_hash */
	0,				/* tp_call */
	0,				/* tp_str */
	0,				/* tp_getattro */
	0,				/* tp_setattro */
	0,				/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	"Hackathon Gunther Daemon",	/* tp_doc */
	0,				/* tp_traverse */
	0,				/* tp_clear */
	0,				/* tp_richcompare */
	0,				/* tp_weaklistoffset */
	0,				/* tp_iter */
	0,				/* tp_iternext */
	hgd_py_methods,			/* tp_methods */
	hgd_py_members,			/* tp_members */
	hgd_py_get_setters,		/* tp_getset */
	0,				/* tp_base */
	0,				/* tp_dict */
	0,				/* tp_descr_get */
	0,				/* tp_descr_set */
	0,				/* tp_dictoffset */
	(initproc) hgd_py_meth_init,	/* tp_init */
	0,				/* tp_alloc */
	hgd_py_meth_new,		/* tp_new */
	0,				/* tp_free */
	0,				/* tp_is_gc */
	0,				/* tp_bases */
	0,				/* tp_mro */
	0,				/* tp_cache */
	0,				/* tp_subclasses */
	0,				/* tp_weaklis */
	0,				/* destructor */
	0,				/* tp_version_tag */
#ifdef COUNT_ALLOCS
	0,				/* tp_allocs */
	0,				/* tp_frees */
	0,				/* tp_maxalloc */
	0,				/* tp_prev */
	0,				/* tp_next */
#endif
};

/*
 * initialise hgd module
 */
#ifndef PyMODINIT_FUN
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
hgd_init_hgd_mod(void)
{
    PyObject* m;

    HgdType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&HgdType) < 0)
        return;

    m = Py_InitModule3("hgd", hgd_py_methods,
                       "Hackathon Gunther Daemon Extensions");

    Py_INCREF(&HgdType);
    PyModule_AddObject(m, "Hgd", (PyObject *) &HgdType);
}

/*
 * Back to HGD land
 */

/* embed the Python interpreter */
int
hgd_embed_py()
{
	DIR			*script_dir;
	struct dirent		*ent;
	PyObject		*mod;
	char			*search_path;

	DPRINTF(HGD_D_INFO, "Initialising Python");

	if (hgd_py_dir == NULL) {
		hgd_py_dir = xstrdup(HGD_DFL_PY_DIR);
	}

	/* ensure we find our modules */
	xasprintf(&search_path, "%s:%s:%s",
	    PREFIX "/share/hgd/pylib", BUILD_DIR "/share/pylib", hgd_py_dir);
	DPRINTF(HGD_D_DEBUG, "Python search path is '%s'", search_path);

	if (setenv("PYTHONPATH", search_path, 0) == -1) {
		DPRINTF(HGD_D_ERROR, "Can't set python search path: %s", SERROR);
		free(search_path);
		hgd_exit_nicely();
	}
	free(search_path);

	Py_Initialize();
	memset(&hgd_py_mods, 0, sizeof(hgd_py_mods));

	/* always import the hgd support stuff from hgd.py */
	mod = PyImport_ImportModule("hgd");
	if (!mod) {
		PRINT_PY_ERROR();
		hgd_exit_nicely();
	}
	hgd_py_mods.hgd_support = mod;

	script_dir = opendir(HGD_DFL_PY_DIR);
	if (script_dir == NULL) {
		DPRINTF(HGD_D_ERROR, "Can't read script dir '%s': %s",
		    HGD_DFL_PY_DIR, SERROR);
		hgd_exit_nicely();
	}

	/* loop over script dir loading modules */
	while ((ent = readdir(script_dir)) != NULL) {

		if ((strcmp(ent->d_name, ".") == 0) ||
		    (strcmp(ent->d_name, "..") == 0) ||
		    (strcmp(ent->d_name, "hgd.py") == 0)) {
			continue;
		}

		if (hgd_py_mods.n_mods == HGD_MAX_PY_MODS) {
			DPRINTF(HGD_D_WARN, "too many python modules loaded");
			break;
		}

		if (strlen(ent->d_name) < 4)
			continue;

		ent->d_name[strlen(ent->d_name) - 3] = 0;
		DPRINTF(HGD_D_DEBUG, "Loading '%s'", ent->d_name);
		mod = PyImport_ImportModule(ent->d_name);

		if (!mod) {
			PRINT_PY_ERROR();
			continue;
		}

		hgd_py_mods.mods[hgd_py_mods.n_mods] = mod;
		hgd_py_mods.mod_names[hgd_py_mods.n_mods] = xstrdup(ent->d_name);
		hgd_py_mods.n_mods++;

	}

	(void) closedir(script_dir);

	hgd_init_hgd_mod(); /* init hgd module */
	hgd_py_mods.hgd_o = hgd_py_meth_new(&HgdType, NULL, NULL); /* stash an instance */

	hgd_execute_py_hook("init");

	return (HGD_OK);
}

void
hgd_free_py()
{
	DPRINTF(HGD_D_INFO, "Clearing up python stuff");
	hgd_py_meth_dealloc((Hgd *) hgd_py_mods.hgd_o);
	if (hgd_py_dir != NULL) free (hgd_py_dir);
	Py_Finalize();
	while (hgd_py_mods.n_mods)
		free(hgd_py_mods.mod_names[--hgd_py_mods.n_mods]);

}

int
hgd_execute_py_hook(char *hook)
{
	PyObject		*func, *ret, *args;
	int			 i, c_ret, any_errors = HGD_OK;
	char			*func_name = NULL;

	DPRINTF(HGD_D_INFO, "Executing Python hooks for '%s'", hook);

	xasprintf(&func_name, "hgd_hook_%s", hook);

	for (i = 0; i < hgd_py_mods.n_mods; i++) {
		func = PyObject_GetAttrString(hgd_py_mods.mods[i], func_name);

		/* if a hook func is not defined, that is fine, skip */
		if (!func) {
			DPRINTF(HGD_D_DEBUG, "Python hook '%s.%s' undefined",
			    hgd_py_mods.mod_names[i], func_name);
			continue;
		}

		if (!PyCallable_Check(func)) {
			PRINT_PY_ERROR();
			DPRINTF(HGD_D_WARN,
			    "Python hook '%s.%s' is not callable",
			    hgd_py_mods.mod_names[i], func_name);
			continue;
		}

		args = PyTuple_New(1);
		PyTuple_SetItem(args, 0, hgd_py_mods.hgd_o);

		DPRINTF(HGD_D_INFO, "Calling Python hook '%s.%s'",
		    hgd_py_mods.mod_names[i], func_name);

		ret = PyObject_CallObject(func, args);
		if (ret == NULL) {
			PRINT_PY_ERROR();
			DPRINTF(HGD_D_WARN,
			    "failed to call Python hook '%s.%s'",
			    hgd_py_mods.mod_names[i], func_name);
			continue;
		}

		c_ret = PyInt_AsLong(ret);

		/* if the user returns non HGD_OK (non-zero), indicates fail */
		if (c_ret != HGD_OK) {
			DPRINTF(HGD_D_WARN, "%s.%s returned non-zero",
			    hgd_py_mods.mod_names[i], func_name);
			any_errors = HGD_FAIL;
		}
	}

	free(func_name);

	return (any_errors);
}

#endif
