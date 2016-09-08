#include <Python.h>

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>
#include <numpy/ndarraytypes.h>

#include <dlfcn.h>

#include <Rcpp.h>
using namespace Rcpp;

#include "tensorflow_types.hpp"

// helper class for ensuring decref of PyObject in the current scope
class PyObjectPtr {
public:
  // attach on creation, decref on destruction
  explicit PyObjectPtr(PyObject* object) : object_(object) {}
  virtual ~PyObjectPtr() {
    if (object_ != NULL)
      ::Py_DecRef(object_);
  }

  operator PyObject*() const { return object_; }

  PyObject* get() const { return object_; }

  PyObject* detach() {
    PyObject* object = object_;
    object_ = NULL;
    return object;
  }

  bool is_null() const { return object_ == NULL; }

private:
  // prevent copying
  PyObjectPtr(const PyObjectPtr&);
  PyObjectPtr& operator=(const PyObjectPtr&);

  // underlying object
  PyObject* object_;
};

// check whether a PyObject is None
bool py_is_none(PyObject* object) {
  return object == &::_Py_NoneStruct;
}

// wrap a PyObject in an XPtr
PyObjectXPtr py_xptr(PyObject* object, bool decref = true) {

  // wrap in XPtr
  PyObjectXPtr ptr(object, decref);

  // class attribute
  CharacterVector attrClass = CharacterVector::create();

  // determine underlying python class
  if (::PyObject_HasAttrString(object, "__class__")) {
    PyObjectPtr classPtr(::PyObject_GetAttrString(object, "__class__"));
    PyObjectPtr modulePtr(::PyObject_GetAttrString(classPtr, "__module__"));
    PyObjectPtr namePtr(::PyObject_GetAttrString(classPtr, "__name__"));
    std::ostringstream ostr;
    ostr << ::PyString_AsString(modulePtr) << "." <<
            ::PyString_AsString(namePtr);
    attrClass.push_back(ostr.str());
  }

  // add py_object
  attrClass.push_back("py_object");

  // set generic py_object class and additional class (if any)
  ptr.attr("class") = attrClass;

  // return XPtr
  return ptr;
}

// get a string representing the last python error
std::string py_fetch_error() {

  // determine error
  std::string error;
  PyObject *excType , *excValue , *excTraceback;
  ::PyErr_Fetch(&excType , &excValue , &excTraceback);
  PyObjectPtr pExcType(excType);
  PyObjectPtr pExcValue(excValue);
  PyObjectPtr pExcTraceback(excTraceback);

  if (!pExcValue.is_null()) {
    std::ostringstream ostr;
    PyObjectPtr pStr(::PyObject_Str(pExcValue));
    ostr << ::PyString_AsString(pStr);
    error = ostr.str();
  } else {
    error = "<unknown error>";
  }

  // return error
  return error;
}

// check whether the PyObject can be mapped to an R scalar type
int r_scalar_type(PyObject* x) {

  if (PyBool_Check(x))
    return LGLSXP;

  // integer
  else if (PyInt_Check(x) || PyLong_Check(x))
    return INTSXP;

  // double
  else if (PyFloat_Check(x))
    return REALSXP;

  // string
  else if (PyString_Check(x))
    return STRSXP;

  // not a scalar
  else
    return NILSXP;
}

// check whether the PyObject is a list of a single R scalar type
int scalar_list_type(PyObject* x) {

  Py_ssize_t len = PyList_Size(x);
  if (len == 0)
    return NILSXP;

  PyObject* first = PyList_GetItem(x, 0);
  int scalarType = r_scalar_type(first);
  if (scalarType == NILSXP)
    return NILSXP;

  for (Py_ssize_t i = 1; i<len; i++) {
    PyObject* next = PyList_GetItem(x, i);
    if (r_scalar_type(next) != scalarType)
      return NILSXP;
  }

  return scalarType;
}

// convert a tuple to a character vector
CharacterVector py_tuple_to_character(PyObject* tuple) {
  Py_ssize_t len = ::PyTuple_Size(tuple);
  CharacterVector vec(len);
  for (Py_ssize_t i = 0; i<len; i++)
    vec[i] = PyString_AsString(PyTuple_GetItem(tuple, i));
  return vec;
}

// convert a python object to an R object
SEXP py_to_r(PyObject* x) {

  // NULL for Python None
  if (py_is_none(x))
    return R_NilValue;

  // check for scalars
  int scalarType = r_scalar_type(x);
  if (scalarType != NILSXP) {
    // logical
    if (scalarType == LGLSXP)
      return wrap(x == Py_True);

    // integer
    else if (scalarType == INTSXP)
      return wrap(PyInt_AsLong(x));

    // double
    else if (scalarType == REALSXP)
      return wrap(PyFloat_AsDouble(x));

    // string
    else if (scalarType == STRSXP)
      return wrap(std::string(PyString_AsString(x)));

    else
      return R_NilValue; // keep compiler happy
  }

  // list
  else if (PyList_Check(x)) {

    Py_ssize_t len = PyList_Size(x);
    int scalarType = scalar_list_type(x);
    if (scalarType == REALSXP) {
      Rcpp::NumericVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = PyFloat_AsDouble(PyList_GetItem(x, i));
      return vec;
    } else if (scalarType == INTSXP) {
      Rcpp::IntegerVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = PyInt_AsLong(PyList_GetItem(x, i));
      return vec;
    } else if (scalarType == LGLSXP) {
      Rcpp::LogicalVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = PyList_GetItem(x, i) == Py_True;
      return vec;
    } else if (scalarType == STRSXP) {
      Rcpp::CharacterVector vec(len);
      for (Py_ssize_t i = 0; i<len; i++)
        vec[i] = PyString_AsString(PyList_GetItem(x, i));
      return vec;
    } else { // not a homegenous list of scalars, return a list
      Rcpp::List list(len);
      for (Py_ssize_t i = 0; i<len; i++)
        list[i] = py_to_r(PyList_GetItem(x, i));
      return list;
    }
  }

  // tuple
  else if (PyTuple_Check(x)) {
    Py_ssize_t len = ::PyTuple_Size(x);
    Rcpp::List list(len);
    for (Py_ssize_t i = 0; i<len; i++)
      list[i] = py_to_r(PyTuple_GetItem(x, i));
    // check for namedtuple
    if (::PyObject_HasAttrString(x, "_fields") == 1) {
      PyObjectPtr fieldsAttrPtr(::PyObject_GetAttrString(x, "_fields"));
      if (PyTuple_Check(fieldsAttrPtr) && PyTuple_Size(fieldsAttrPtr) == len)
        list.names() = py_tuple_to_character(fieldsAttrPtr);
    }
    return list;
  }

  // dict
  else if (PyDict_Check(x)) {
    // allocate R list
    Rcpp::List list;
    // iterate over dict
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(x, &pos, &key, &value))
      list[PyString_AsString(key)] = py_to_r(value);
    return list;
  }

  // numpy array
  else if (PyArray_Check(x)) {

    // get the array
    PyArrayObject* array = (PyArrayObject*)x;

    // get the dimensions
    npy_intp len = PyArray_SIZE(array);
    int nd = PyArray_NDIM(array);
    npy_intp *dims = PyArray_DIMS(array);
    IntegerVector dimsVector(nd);
    for (int i = 0; i<nd; i++)
      dimsVector[i] = dims[i];

    // determine the target type of the array
    int typenum = PyArray_TYPE(array);
    switch(typenum) {
    // logical
    case NPY_BOOL:
      typenum = NPY_BOOL;
      break;
    // integer
    case NPY_BYTE:
    case NPY_UBYTE:
    case NPY_SHORT:
    case NPY_USHORT:
    case NPY_INT:
    case NPY_LONG:
      typenum = NPY_LONG;
      break;
    // double
    case NPY_FLOAT:
    case NPY_DOUBLE:
      typenum = NPY_DOUBLE;
      break;
    // unsupported
    default:
      stop("Conversion from numpy array type %d is not supported", typenum);
      break;
    }

    // cast it to a fortran array (PyArray_CastToType steals the descr)
    // (note that we will decref the copied array below)
    PyArray_Descr* descr = PyArray_DescrFromType(typenum);
    array = (PyArrayObject*)PyArray_CastToType(array, descr, NPY_ARRAY_FARRAY);
    if (array == NULL)
      stop(py_fetch_error());

    // ensure we release it within this scope
    PyObjectPtr ptrArray((PyObject*)array);

    // R array to return
    SEXP rArray = R_NilValue;

    // copy the data as required per-type
    switch(typenum) {
      case NPY_BOOL: {
        npy_bool* pData = (npy_bool*)PyArray_DATA(array);
        rArray = Rf_allocArray(LGLSXP, dimsVector);
        for (int i=0; i<len; i++)
          LOGICAL(rArray)[i] = pData[i];
        break;
      }
      case NPY_LONG: {
        npy_long* pData = (npy_long*)PyArray_DATA(array);
        rArray = Rf_allocArray(INTSXP, dimsVector);
        for (int i=0; i<len; i++)
          INTEGER(rArray)[i] = pData[i];
        break;
      }
      case NPY_DOUBLE: {
        npy_double* pData = (npy_double*)PyArray_DATA(array);
        rArray = Rf_allocArray(REALSXP, dimsVector);
        for (int i=0; i<len; i++)
          REAL(rArray)[i] = pData[i];
        break;
      }
    }

    // return the R Array
    return rArray;
  }

  // default is to return opaque wrapper to python object
  else {
    ::Py_IncRef(x);
    return py_xptr(x);
  }
}


// convert an R object to a python object (the returned object
// will have an active reference count on it)
PyObject* r_to_py(RObject x) {

  int type = x.sexp_type();
  SEXP sexp = x.get__();

  // NULL becomes python None (Py_IncRef since PyTuple_SetItem
  // will steal the passed reference)
  if (x.isNULL()) {
    ::Py_IncRef(&::_Py_NoneStruct);
    return &::_Py_NoneStruct;

  // pass python objects straight through (Py_IncRef since returning this
  // creates a new reference from the caller)
  } else if (x.inherits("py_object")) {
    PyObjectXPtr obj = as<PyObjectXPtr>(sexp);
    ::Py_IncRef(obj.get());
    return obj.get();

  // convert arrays and matrixes to numpy
  } else if (x.hasAttribute("dim")) {

    IntegerVector dimAttrib = x.attr("dim");
    int nd = dimAttrib.length();
    std::vector<npy_intp> dims(nd);
    for (int i = 0; i<nd; i++)
      dims[i] = dimAttrib[i];
    int typenum;
    void* data;
    if (type == INTSXP) {
      typenum = NPY_INT;
      data = &(INTEGER(sexp)[0]);
    } else if (type == REALSXP) {
      typenum = NPY_DOUBLE;
      data = &(REAL(sexp)[0]);
    } else if (type == LGLSXP) {
      typenum = NPY_BOOL;
      data = &(LOGICAL(sexp)[0]);
    } else {
      stop("Matrix type cannot be converted to python (only integer, "
           "numeric, and logical matrixes can be converted");
    }

    // create the matrix
    PyObject* matrix = PyArray_New(&PyArray_Type,
                                   nd,
                                   &(dims[0]),
                                   typenum,
                                   NULL,
                                   data,
                                   0,
                                   NPY_ARRAY_FARRAY_RO,
                                   NULL);

    // check for error
    if (matrix == NULL)
      stop(py_fetch_error());

    // return it
    return matrix;

  // integer (pass length 1 vectors as scalars, otherwise pass list)
  } else if (type == INTSXP) {
    if (LENGTH(sexp) == 1) {
      int value = INTEGER(sexp)[0];
      return ::PyInt_FromLong(value);
    } else {
      PyObjectPtr list(::PyList_New(LENGTH(sexp)));
      for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
        int value = INTEGER(sexp)[i];
        // NOTE: reference to added value is "stolen" by the list
        int res = ::PyList_SetItem(list, i, ::PyInt_FromLong(value));
        if (res != 0)
          stop(py_fetch_error());
      }
      return list.detach();
    }

  // numeric (pass length 1 vectors as scalars, otherwise pass list)
  } else if (type == REALSXP) {
    if (LENGTH(sexp) == 1) {
      double value = REAL(sexp)[0];
      return ::PyFloat_FromDouble(value);
    } else {
      PyObjectPtr list(PyList_New(LENGTH(sexp)));
      for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
        double value = REAL(sexp)[i];
        // NOTE: reference to added value is "stolen" by the list
        int res = ::PyList_SetItem(list, i, ::PyFloat_FromDouble(value));
        if (res != 0)
          stop(py_fetch_error());
      }
      return list.detach();
    }

  // logical (pass length 1 vectors as scalars, otherwise pass list)
  } else if (type == LGLSXP) {
    if (LENGTH(sexp) == 1) {
      int value = LOGICAL(sexp)[0];
      return ::PyBool_FromLong(value);
    } else {
      PyObjectPtr list(PyList_New(LENGTH(sexp)));
      for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
        int value = LOGICAL(sexp)[i];
        // NOTE: reference to added value is "stolen" by the list
        int res = ::PyList_SetItem(list, i, ::PyBool_FromLong(value));
        if (res != 0)
          stop(py_fetch_error());
      }
      return list.detach();
    }

  // character (pass length 1 vectors as scalars, otherwise pass list)
  } else if (type == STRSXP) {
    if (LENGTH(sexp) == 1) {
      const char* value = CHAR(STRING_ELT(sexp, 0));
      return ::PyString_FromString(value);
    } else {
      PyObjectPtr list(::PyList_New(LENGTH(sexp)));
      for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
        const char* value = CHAR(STRING_ELT(sexp, i));
        // NOTE: reference to added value is "stolen" by the list
        int res = ::PyList_SetItem(list, i, ::PyString_FromString(value));
        if (res != 0)
          stop(py_fetch_error());
      }
      return list.detach();
    }

  // list
  } else if (type == VECSXP) {
    // create a dict for names
    if (x.hasAttribute("names")) {
      PyObjectPtr dict(::PyDict_New());
      CharacterVector names = x.attr("names");
      for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
        const char* name = names.at(i);
        PyObjectPtr item(r_to_py(RObject(VECTOR_ELT(sexp, i))));
        int res = ::PyDict_SetItemString(dict, name, item);
        if (res != 0)
          stop(py_fetch_error());
      }
      return dict.detach();
    // create a tuple if there are no names
    } else {
      PyObjectPtr tuple(::PyTuple_New(LENGTH(sexp)));
      for (R_xlen_t i = 0; i<LENGTH(sexp); i++) {
        PyObject* item = r_to_py(RObject(VECTOR_ELT(sexp, i)));
        // NOTE: reference to added item is "stolen" by tuple
        int res = ::PyTuple_SetItem(tuple, i, item);
        if (res != 0)
          stop(py_fetch_error());
      }
      return tuple.detach();
    }
  } else {
    Rcpp::print(sexp);
    stop("Unable to convert R object to python type");
  }
}

// import numpy array api
bool py_import_numpy_array_api() {
  import_array1(false);
  return true;
}

// [[Rcpp::export]]
void py_initialize(const std::string& pythonSharedLibrary) {

#ifndef __APPLE__
  // force RTLD_GLOBAL of libpython symbols for numpy on Linux, see:
  //
  //   https://bugs.kde.org/show_bug.cgi?id=330032)
  //   http://stackoverflow.com/questions/29880931/
  //
  void * lib = ::dlopen(pythonSharedLibrary.c_str(), RTLD_NOW|RTLD_GLOBAL);
  if (lib == NULL) {
    const char* err = ::dlerror();
    stop(err);
  }
#endif

  // initialize python
  ::Py_Initialize();

  // required to populate sys.
  const char *argv[1] = {"python"};
  PySys_SetArgv(1, const_cast<char**>(argv));

  // import numpy array api
  if (!py_import_numpy_array_api())
    stop(py_fetch_error());
}

// [[Rcpp::export]]
void py_finalize() {

  // multiple calls to PyFinalize are likely to cause problems so
  // we comment this out to play better with other packages that include
  // python embedding code.

  // ::Py_Finalize();
}

// [[Rcpp::export]]
bool py_is_none(PyObjectXPtr x) {
  return py_is_none(x.get());
}

// [[Rcpp::export]]
CharacterVector py_str(PyObjectXPtr x) {
  PyObjectPtr str(PyObject_Str(x));
  if (str.is_null())
    stop(py_fetch_error());
  return ::PyString_AsString(str);
}

// [[Rcpp::export]]
void py_print(PyObjectXPtr x) {
  PyObjectPtr str(PyObject_Str(x));
  if (str.is_null())
    stop(py_fetch_error());
  Rcout << ::PyString_AsString(str) << std::endl;
}

// [[Rcpp::export]]
bool py_is_callable(PyObjectXPtr x) {
  return ::PyCallable_Check(x) == 1;
}

// [[Rcpp::export]]
bool py_is_null_xptr(PyObjectXPtr x) {
  return !x;
}


// [[Rcpp::export]]
std::vector<std::string> py_list_attributes(PyObjectXPtr x) {
  std::vector<std::string> attributes;
  PyObjectPtr attrs(::PyObject_Dir(x));
  if (attrs.is_null())
    stop(py_fetch_error());

  Py_ssize_t len = ::PyList_Size(attrs);
  for (Py_ssize_t index = 0; index<len; index++) {
    PyObject* item = ::PyList_GetItem(attrs, index);
    const char* value = ::PyString_AsString(item);
    attributes.push_back(value);
  }

  return attributes;
}


// [[Rcpp::export]]
PyObjectXPtr py_get_attr(PyObjectXPtr x, const std::string& name) {
  PyObject* attr = ::PyObject_GetAttrString(x, name.c_str());
  if (attr == NULL)
    stop(py_fetch_error());
  return py_xptr(attr);
}

// [[Rcpp::export]]
IntegerVector py_get_attribute_types(
    PyObjectXPtr x,
    const std::vector<std::string>& attributes) {

  //const int UNKNOWN     =  0;
  const int VECTOR      =  1;
  const int ARRAY       =  2;
  const int LIST        =  4;
  //const int ENVIRONMENT =  5;
  const int FUNCTION    =  6;

  IntegerVector types(attributes.size());
  for (size_t i = 0; i<attributes.size(); i++) {
    PyObjectXPtr attr = py_get_attr(x, attributes[i]);
    if (::PyCallable_Check(attr))
      types[i] = FUNCTION;
    else if (PyList_Check(attr)  ||
             PyTuple_Check(attr) ||
             PyDict_Check(attr))
      types[i] = LIST;
    else if (PyArray_Check(attr))
      types[i] = ARRAY;
    else if (PyBool_Check(attr)   ||
             PyInt_Check(attr)    ||
             PyLong_Check(attr)   ||
             PyFloat_Check(attr)  ||
             PyString_Check(attr))
      types[i] = VECTOR;
    else
      // presume that other types are objects
      types[i] = LIST;
  }

  return types;
}

// [[Rcpp::export]]
SEXP py_to_r(PyObjectXPtr x) {
  return py_to_r(x.get());
}

// [[Rcpp::export]]
SEXP py_call(PyObjectXPtr x, List args, List keywords = R_NilValue) {

  // unnamed arguments
  PyObjectPtr pyArgs(::PyTuple_New(args.length()));
  for (R_xlen_t i = 0; i<args.size(); i++) {
    PyObject* arg = r_to_py(args.at(i));
    // NOTE: reference to arg is "stolen" by the tuple
    int res = ::PyTuple_SetItem(pyArgs, i, arg);
    if (res != 0)
      stop(py_fetch_error());
  }

  // named arguments
  PyObjectPtr pyKeywords(::PyDict_New());
  if (keywords.length() > 0) {
    CharacterVector names = keywords.names();
    for (R_xlen_t i = 0; i<keywords.length(); i++) {
      const char* name = names.at(i);
      PyObjectPtr arg(r_to_py(keywords.at(i)));
      int res = ::PyDict_SetItemString(pyKeywords, name, arg);
      if (res != 0)
        stop(py_fetch_error());
    }
  }

  // call the function
  PyObjectPtr res(::PyObject_Call(x, pyArgs, pyKeywords));

  // check for error
  if (res.is_null())
    stop(py_fetch_error());

  // return as r object
  return py_to_r(res);
}


// [[Rcpp::export]]
PyObjectXPtr py_import(const std::string& module) {
  PyObject* pModule = ::PyImport_ImportModule(module.c_str());
  if (pModule == NULL)
    stop(py_fetch_error());
  return py_xptr(pModule);
}


// [[Rcpp::export]]
PyObjectXPtr py_dict(const List& keys, const List& items) {
  PyObject* dict = ::PyDict_New();
  for (auto i = 0; i<keys.length(); i++) {
    PyObjectPtr key(r_to_py(keys.at(i)));
    PyObjectPtr item(r_to_py(items.at(i)));
    ::PyDict_SetItem(dict, key, item);
  }
  return py_xptr(dict);
}

// [[Rcpp::export]]
void py_run_string(const std::string& code)
{
  PyObject* dict = ::PyModule_GetDict(::PyImport_AddModule("__main__"));
  PyObjectPtr res(::PyRun_StringFlags(code.c_str(), Py_file_input, dict, dict, NULL));
  if (res.is_null())
    stop(py_fetch_error());
}


// [[Rcpp::export]]
void py_run_file(const std::string& file)
{
  // expand path
  Function pathExpand("path.expand");
  std::string expanded = as<std::string>(pathExpand(file));

  // open and run
  FILE* fp = ::fopen(expanded.c_str(), "r");
  if (fp) {
    int res = ::PyRun_SimpleFile(fp, expanded.c_str());
    if (res != 0)
      stop(py_fetch_error());
  }
  else
    stop("Unable to read script file '%s' (does the file exist?)", file);
}

