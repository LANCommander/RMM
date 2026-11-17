#include "pyapi.h"

#include "../../core/platform.h"

namespace rmm::py {

Api& Get() {
    static Api api;

    return api;
}

template <class T> static bool Bind(void* mod, const char* name, T& fn, std::string& missing, bool required = true) {
    fn = reinterpret_cast<T>(plat::GetSym(mod, name));

    if (!fn && required) {
        if (!missing.empty())
            missing += ", ";

        missing += name;
    }

    return fn != nullptr;
}

bool Api::Resolve(void* m, std::string& missing) {
    missing.clear();
    Bind(m, "Py_Initialize", Py_Initialize, missing);
    Bind(m, "Py_IsInitialized", Py_IsInitialized, missing);
    Bind(m, "Py_InitModule4", Py_InitModule4, missing);
    Bind(m, "PyArg_ParseTuple", PyArg_ParseTuple, missing);
    Bind(m, "Py_BuildValue", Py_BuildValue, missing);
    Bind(m, "PyImport_ImportModule", PyImport_ImportModule, missing);
    Bind(m, "PyObject_CallMethod", PyObject_CallMethod, missing);
    Bind(m, "PyObject_GetAttrString", PyObject_GetAttrString, missing);
    Bind(m, "PyObject_CallObject", PyObject_CallObject, missing);
    Bind(m, "PyRun_SimpleString", PyRun_SimpleString, missing);
    Bind(m, "PyErr_Print", PyErr_Print, missing);
    Bind(m, "PyErr_Occurred", PyErr_Occurred, missing);
    Bind(m, "PyErr_Clear", PyErr_Clear, missing);
    Bind(m, "PyString_AsString", PyString_AsString, missing);
    Bind(m, "PyInt_AsLong", PyInt_AsLong, missing);
    Bind(m, "Py_DecRef", Py_DecRef, missing, /*required=*/false);
    Bind(m, "PyCObject_AsVoidPtr", PyCObject_AsVoidPtr, missing, /*required=*/false);
    PyCObject_Type = plat::GetSym(m, "PyCObject_Type");
    None = static_cast<PyObject*>(plat::GetSym(m, "_Py_NoneStruct"));

    if (!None) {
        if (!missing.empty())
            missing += ", ";

        missing += "_Py_NoneStruct";
    }

    ok = missing.empty();

    return ok;
}

// Release-build PyObject header (Python 2.x):
//   struct { int ob_refcnt; PyTypeObject* ob_type; }
// PyTypeObject: { int ob_refcnt; PyTypeObject* ob_type; int ob_size;
//                 const char* tp_name; int tp_basicsize; int tp_itemsize;
//                 destructor tp_dealloc; ... }
// Python 2.3 uses `int` (not Py_ssize_t) for these fields.
namespace {
struct ObjHead {
    int refcnt;
    void* type;
};

struct TypeHead {
    int refcnt;
    void* type;
    int size;
    const char* name;
    int basicsize;
    int itemsize;
    void (*dealloc)(PyObject*);
};
}  // namespace

void* Api::CObjectPtr(PyObject* o) const {
    // Check the exact type first: PyCObject_AsVoidPtr on anything else sets a
    // Python error (and older builds don't check at all).
    if (!o || !PyCObject_AsVoidPtr || !PyCObject_Type || reinterpret_cast<ObjHead*>(o)->type != PyCObject_Type)
        return nullptr;

    return PyCObject_AsVoidPtr(o);
}

void Api::IncRef(PyObject* o) const {
    if (o)
        ++reinterpret_cast<ObjHead*>(o)->refcnt;
}

void Api::DecRef(PyObject* o) const {
    if (!o)
        return;

    if (Py_DecRef) {
        Py_DecRef(o);

        return;
    }

    auto* h = reinterpret_cast<ObjHead*>(o);

    if (--h->refcnt == 0)
        reinterpret_cast<TypeHead*>(h->type)->dealloc(o);
}

}  // namespace rmm::py
