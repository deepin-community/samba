/*
   Unix SMB/CIFS implementation.
   Samba utility functions
   Copyright (C) David Mulder <dmulder@samba.org> 2021

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "lib/replace/system/python.h"
#include "includes.h"
#include "param/param.h"
#include "param/pyparam.h"
#include "param/loadparm.h"
#include "param/s3_param.h"
#include <pytalloc.h>

#define PyErr_FromString(str) Py_BuildValue("(s)", discard_const_p(char, str))
#define PyLoadparmContext_AsLoadparmContext(obj) pytalloc_get_type(obj, struct loadparm_context)

_PUBLIC_ struct loadparm_context *lpcfg_from_py_object(TALLOC_CTX *mem_ctx, PyObject *py_obj)
{
	PyObject *param_mod;
	PyTypeObject *lp_type;
	bool is_lpobj;
	const struct loadparm_s3_helpers *s3_context;
	struct loadparm_context *s4_context;

	if (py_obj == Py_None) {
		s3_context = loadparm_s3_helpers();

		s4_context = loadparm_init_s3(mem_ctx, s3_context);
		if (s4_context == NULL) {
			PyErr_NoMemory();
			return NULL;
		}

		if (!lpcfg_load_default(s4_context)) {
			PyErr_FromString("Failed to load defaults\n");
			return NULL;
		}

		return s4_context;
	}

	param_mod = PyImport_ImportModule("samba.param");
	if (param_mod == NULL) {
		return NULL;
	}

	lp_type = (PyTypeObject *)PyObject_GetAttrString(param_mod, "LoadParm");
	Py_DECREF(param_mod);
	if (lp_type == NULL) {
		PyErr_SetString(PyExc_RuntimeError, "Unable to import LoadParm");
		return NULL;
	}

	is_lpobj = PyObject_TypeCheck(py_obj, lp_type);
	Py_DECREF(lp_type);
	if (is_lpobj) {
		return talloc_reference(mem_ctx, PyLoadparmContext_AsLoadparmContext(py_obj));
	}

	PyErr_SetNone(PyExc_TypeError);
	return NULL;
}
