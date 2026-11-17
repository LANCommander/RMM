// Minimal, dynamically-resolved Python 2.x C API for the interpreter embedded
// in Refractor 2 servers (dice_py.dll / libdice_py.so, Python 2.3.x).
//
// We deliberately do not compile against Python headers: the exact build DICE
// shipped is not something we can link against, and only a handful of entry
// points are needed. Everything is resolved with GetProcAddress/dlsym at
// runtime, so a missing export degrades gracefully instead of failing to load.
#pragma once

#include <cstddef>
#include <string>

namespace rmm::py {

struct PyObject;  // opaque

struct PyMethodDef {
    const char* ml_name;
    PyObject* (*ml_meth)(PyObject* self, PyObject* args);
    int ml_flags;
    const char* ml_doc;
};

constexpr int METH_VARARGS = 0x0001;
constexpr int PYTHON_API_VERSION_23 = 1012;  // Python 2.3

struct Api {
    bool Resolve(void* module, std::string& missing);
    bool ok = false;

    void (*Py_Initialize)() = nullptr;
    int (*Py_IsInitialized)() = nullptr;
    PyObject* (*Py_InitModule4)(const char*, PyMethodDef*, const char*, PyObject*, int) = nullptr;
    int (*PyArg_ParseTuple)(PyObject*, const char*, ...) = nullptr;
    PyObject* (*Py_BuildValue)(const char*, ...) = nullptr;
    PyObject* (*PyImport_ImportModule)(const char*) = nullptr;
    PyObject* (*PyObject_CallMethod)(PyObject*, const char*, const char*, ...) = nullptr;
    PyObject* (*PyObject_GetAttrString)(PyObject*, const char*) = nullptr;
    PyObject* (*PyObject_CallObject)(PyObject*, PyObject*) = nullptr;
    int (*PyRun_SimpleString)(const char*) = nullptr;
    void (*PyErr_Print)() = nullptr;
    PyObject* (*PyErr_Occurred)() = nullptr;
    void (*PyErr_Clear)() = nullptr;
    char* (*PyString_AsString)(PyObject*) = nullptr;
    long (*PyInt_AsLong)(PyObject*) = nullptr;
    void (*Py_DecRef)(PyObject*) = nullptr;             // 2.4+, optional
    void* (*PyCObject_AsVoidPtr)(PyObject*) = nullptr;  // optional: host.Object tokens
    void* PyCObject_Type = nullptr;                     // optional: &PyCObject_Type
    PyObject* None = nullptr;                           // &_Py_NoneStruct

    /// The pointer inside a PyCObject (e.g. a host.Object `token`, which
    /// wraps the engine's IObject*), or nullptr if @p o isn't one.
    void* CObjectPtr(PyObject* o) const;

    /// Py_DECREF that works without Py_DecRef (Python 2.3): manipulates the
    /// object header directly - layout is stable across all 2.x release builds.
    void DecRef(PyObject* o) const;
    void IncRef(PyObject* o) const;

    PyObject* ReturnNone() const {
        IncRef(None);

        return None;
    }
};

Api& Get();

}  // namespace rmm::py
