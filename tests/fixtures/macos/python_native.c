/* Minimal public CPython declarations, authored for the 64-bit guest ABI. */
typedef struct PyObject PyObject;
typedef PyObject *(*PyCFunction)(PyObject *, PyObject *);
typedef struct {
    const char *name;
    PyCFunction function;
    int flags;
    const char *doc;
} PyMethodDef;
extern int PyArg_ParseTuple(PyObject *, const char *, ...);
typedef struct {
    long reference_count;
    void *type;
    PyObject *(*initialize)(void);
    long index;
    PyObject *copy;
} PyModuleDefBase;
typedef struct {
    PyModuleDefBase base;
    const char *name;
    const char *doc;
    long state_size;
    PyMethodDef *methods;
    void *slots;
    void *traverse;
    void *clear;
    void *free;
} PyModuleDef;
_Static_assert(sizeof(PyMethodDef) == 32, "CPython 64-bit method declaration layout");
_Static_assert(sizeof(PyModuleDefBase) == 40, "CPython 64-bit definition base layout");
_Static_assert(sizeof(PyModuleDef) == 104, "CPython 64-bit module definition layout");
extern PyObject *PyModule_Create2(PyModuleDef *, int);
extern int PyModule_AddIntConstant(PyObject *, const char *, long);
extern PyObject *PyLong_FromLong(long);
extern PyObject *PyObject_CallFunctionObjArgs(PyObject *, ...);
extern void Py_DecRef(PyObject *);
extern long alchemy_python_add(long);
extern int alchemy_python_initialized(void);

static PyObject *compute(PyObject *self, PyObject *arguments) {
    (void)self;
    long value;
    PyObject *callback;
    if (!PyArg_ParseTuple(arguments, "lO:compute", &value, &callback)) return 0;
    PyObject *number = PyLong_FromLong(alchemy_python_add(value));
    if (!number) return 0;
    PyObject *result = PyObject_CallFunctionObjArgs(callback, number, (PyObject *)0);
    Py_DecRef(number);
    return result;
}

static PyMethodDef methods[] = {
    {"compute", compute, 1, "Call an authored guest dylib and then a Python callback."},
    {0, 0, 0, 0}
};
static PyModuleDef definition = {
    {1, 0, 0, 0, 0}, "_alchemy_native", 0, 0, methods, 0, 0, 0, 0
};

PyObject *PyInit__alchemy_native(void) {
    PyObject *module = PyModule_Create2(&definition, 1013);
    if (!module) return 0;
    if (PyModule_AddIntConstant(module, "initialized", alchemy_python_initialized()) < 0) {
        Py_DecRef(module); return 0;
    }
    return module;
}
