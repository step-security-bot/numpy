#ifndef NUMPY_CORE_SRC_MULTIARRAY_ARRAY_METHOD_H_
#define NUMPY_CORE_SRC_MULTIARRAY_ARRAY_METHOD_H_

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE

#include <Python.h>
#include <numpy/ndarraytypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Flag for whether the GIL is required */
    NPY_METH_REQUIRES_PYAPI = 1 << 0,
    /*
     * Some functions cannot set floating point error flags, this flag
     * gives us the option (not requirement) to skip floating point error
     * setup/check. No function should set error flags and ignore them
     * since it would interfere with chaining operations (e.g. casting).
     */
    NPY_METH_NO_FLOATINGPOINT_ERRORS = 1 << 1,
    /* Whether the method supports unaligned access (not runtime) */
    NPY_METH_SUPPORTS_UNALIGNED = 1 << 2,
    /*
     * Used for reductions to allow reordering the operation.  At this point
     * assume that if set, it also applies to normal operations though!
     */
    NPY_METH_IS_REORDERABLE = 1 << 3,
    /*
     * Private flag for now for *logic* functions.  The logical functions
     * `logical_or` and `logical_and` can always cast the inputs to booleans
     * "safely" (because that is how the cast to bool is defined).
     * @seberg: I am not sure this is the best way to handle this, so its
     * private for now (also it is very limited anyway).
     * There is one "exception". NA aware dtypes cannot cast to bool
     * (hopefully), so the `??->?` loop should error even with this flag.
     * But a second NA fallback loop will be necessary.
     */
    _NPY_METH_FORCE_CAST_INPUTS = 1 << 17,

    /* All flags which can change at runtime */
    NPY_METH_RUNTIME_FLAGS = (
            NPY_METH_REQUIRES_PYAPI |
            NPY_METH_NO_FLOATINGPOINT_ERRORS),
} NPY_ARRAYMETHOD_FLAGS;


/*
 * It would be nice to just | flags, but in general it seems that 0 bits
 * probably should indicate "default".
 * And that is not necessarily compatible with `|`.
 *
 * NOTE: If made public, should maybe be a function to easier add flags?
 */
#define PyArrayMethod_MINIMAL_FLAGS NPY_METH_NO_FLOATINGPOINT_ERRORS
#define PyArrayMethod_COMBINED_FLAGS(flags1, flags2)  \
        ((NPY_ARRAYMETHOD_FLAGS)(  \
            ((flags1 | flags2) & ~PyArrayMethod_MINIMAL_FLAGS)  \
            | (flags1 & flags2)))


struct PyArrayMethodObject_tag;

/*
 * This struct is specific to an individual (possibly repeated) call of
 * the ArrayMethods strided operator, and as such is passed into the various
 * methods of the ArrayMethod object (the resolve_descriptors function,
 * the get_loop function and the individual lowlevel strided operator calls).
 * It thus has to be persistent for one end-user call, and then be discarded.
 *
 * TODO: Before making this public, we should review which information should
 *       be stored on the Context/BoundArrayMethod vs. the ArrayMethod.
 */
typedef struct {
    PyObject *caller;  /* E.g. the original ufunc, may be NULL */
    struct PyArrayMethodObject_tag *method;

    /* Operand descriptors, filled in by resolve_descriptors */
    PyArray_Descr **descriptors;
} PyArrayMethod_Context;


typedef int (PyArrayMethod_StridedLoop)(PyArrayMethod_Context *context,
        char *const *data, const npy_intp *dimensions, const npy_intp *strides,
        NpyAuxData *transferdata);


typedef NPY_CASTING (resolve_descriptors_function)(
        struct PyArrayMethodObject_tag *method,
        PyArray_DTypeMeta **dtypes,
        PyArray_Descr **given_descrs,
        PyArray_Descr **loop_descrs,
        npy_intp *view_offset);


typedef int (get_loop_function)(
        PyArrayMethod_Context *context,
        int aligned, int move_references,
        const npy_intp *strides,
        PyArrayMethod_StridedLoop **out_loop,
        NpyAuxData **out_transferdata,
        NPY_ARRAYMETHOD_FLAGS *flags);


/**
 * Query an ArrayMethod for the initial value for use in reduction.
 *
 * @param context The arraymethod context, mainly to access the descriptors.
 * @param reduction_is_empty Whether the reduction is empty. When it is, the
 *     value returned may differ.  In this case it is a "default" value that
 *     may differ from the "identity" value normally used.  For example:
 *     - `0.0` is the default for `sum([])`.  But `-0.0` is the correct
 *       identity otherwise as it preserves the sign for `sum([-0.0])`.
 *     - We use no identity for object, but return the default of `0` and `1`
 *       for the empty `sum([], dtype=object)` and `prod([], dtype=object)`.
 *       This allows `np.sum(np.array(["a", "b"], dtype=object))` to work.
 *     - `-inf` or `INT_MIN` for `max` is an identity, but at least `INT_MIN`
 *       not a good *default* when there are no items.
 * @param initial Pointer to initial data to be filled (if possible)
 *
 * @returns -1, 0, or 1 indicating error, no initial value, and initial being
 *     successfully filled.  Errors must not be given where 0 is correct, NumPy
 *     may call this even when not strictly necessary.
 */
typedef int (get_reduction_initial_function)(
        PyArrayMethod_Context *context, npy_bool reduction_is_empty,
        char *initial);

/*
 * The following functions are only used be the wrapping array method defined
 * in umath/wrapping_array_method.c
 */

/*
 * The function to convert the given descriptors (passed in to
 * `resolve_descriptors`) and translates them for the wrapped loop.
 * The new descriptors MUST be viewable with the old ones, `NULL` must be
 * supported (for outputs) and should normally be forwarded.
 *
 * The function must clean up on error.
 *
 * NOTE: We currently assume that this translation gives "viewable" results.
 *       I.e. there is no additional casting related to the wrapping process.
 *       In principle that could be supported, but not sure it is useful.
 *       This currently also means that e.g. alignment must apply identically
 *       to the new dtypes.
 *
 * TODO: Due to the fact that `resolve_descriptors` is also used for `can_cast`
 *       there is no way to "pass out" the result of this function.  This means
 *       it will be called twice for every ufunc call.
 *       (I am considering including `auxdata` as an "optional" parameter to
 *       `resolve_descriptors`, so that it can be filled there if not NULL.)
 */
typedef int translate_given_descrs_func(int nin, int nout,
        PyArray_DTypeMeta *wrapped_dtypes[],
        PyArray_Descr *given_descrs[], PyArray_Descr *new_descrs[]);

/**
 * The function to convert the actual loop descriptors (as returned by the
 * original `resolve_descriptors` function) to the ones the output array
 * should use.
 * This function must return "viewable" types, it must not mutate them in any
 * form that would break the inner-loop logic.  Does not need to support NULL.
 *
 * The function must clean up on error.
 *
 * @param nargs Number of arguments
 * @param new_dtypes The DTypes of the output (usually probably not needed)
 * @param given_descrs Original given_descrs to the resolver, necessary to
 *        fetch any information related to the new dtypes from the original.
 * @param original_descrs The `loop_descrs` returned by the wrapped loop.
 * @param loop_descrs The output descriptors, compatible to `original_descrs`.
 *
 * @returns 0 on success, -1 on failure.
 */
typedef int translate_loop_descrs_func(int nin, int nout,
        PyArray_DTypeMeta *new_dtypes[], PyArray_Descr *given_descrs[],
        PyArray_Descr *original_descrs[], PyArray_Descr *loop_descrs[]);


/*
 * This struct will be public and necessary for creating a new ArrayMethod
 * object (casting and ufuncs).
 * We could version the struct, although since we allow passing arbitrary
 * data using the slots, and have flags, that may be enough?
 * (See also PyBoundArrayMethodObject.)
 */
typedef struct {
    const char *name;
    int nin, nout;
    NPY_CASTING casting;
    NPY_ARRAYMETHOD_FLAGS flags;
    PyArray_DTypeMeta **dtypes;
    PyType_Slot *slots;
} PyArrayMethod_Spec;


/*
 * Structure of the ArrayMethod. This structure should probably not be made
 * public. If necessary, we can make certain operations on it public
 * (e.g. to allow users indirect access to `get_strided_loop`).
 *
 * NOTE: In some cases, it may not be clear whether information should be
 * stored here or on the bound version. E.g. `nin` and `nout` (and in the
 * future the gufunc `signature`) is already stored on the ufunc so that
 * storing these here duplicates the information.
 */
typedef struct PyArrayMethodObject_tag {
    PyObject_HEAD
    char *name;
    int nin, nout;
    /* Casting is normally "safe" for functions, but is important for casts */
    NPY_CASTING casting;
    /* default flags. The get_strided_loop function can override these */
    NPY_ARRAYMETHOD_FLAGS flags;
    resolve_descriptors_function *resolve_descriptors;
    get_loop_function *get_strided_loop;
    get_reduction_initial_function  *get_reduction_initial;
    /* Typical loop functions (contiguous ones are used in current casts) */
    PyArrayMethod_StridedLoop *strided_loop;
    PyArrayMethod_StridedLoop *contiguous_loop;
    PyArrayMethod_StridedLoop *unaligned_strided_loop;
    PyArrayMethod_StridedLoop *unaligned_contiguous_loop;
    /* Chunk only used for wrapping array method defined in umath */
    struct PyArrayMethodObject_tag *wrapped_meth;
    PyArray_DTypeMeta **wrapped_dtypes;
    translate_given_descrs_func *translate_given_descrs;
    translate_loop_descrs_func *translate_loop_descrs;
    /* Chunk reserved for use by the legacy fallback arraymethod */
    char legacy_initial[sizeof(npy_clongdouble)];  /* initial value storage */
} PyArrayMethodObject;


/*
 * We will sometimes have to create a ArrayMethod and allow passing it around,
 * similar to `instance.method` returning a bound method, e.g. a function like
 * `ufunc.resolve()` can return a bound object.
 * The current main purpose of the BoundArrayMethod is that it holds on to the
 * `dtypes` (the classes), so that the `ArrayMethod` (e.g. for casts) will
 * not create references cycles.  In principle, it could hold any information
 * which is also stored on the ufunc (and thus does not need to be repeated
 * on the `ArrayMethod` itself.
 */
typedef struct {
    PyObject_HEAD
    PyArray_DTypeMeta **dtypes;
    PyArrayMethodObject *method;
} PyBoundArrayMethodObject;


extern NPY_NO_EXPORT PyTypeObject PyArrayMethod_Type;
extern NPY_NO_EXPORT PyTypeObject PyBoundArrayMethod_Type;

/*
 * SLOTS IDs For the ArrayMethod creation, one public, the IDs are fixed.
 * TODO: Before making it public, consider adding a large constant to private
 *       slots.
 */
#define NPY_METH_resolve_descriptors 1
#define NPY_METH_get_loop 2
#define NPY_METH_get_reduction_initial 3
/* specific loops for constructions/default get_loop: */
#define NPY_METH_strided_loop 4
#define NPY_METH_contiguous_loop 5
#define NPY_METH_unaligned_strided_loop 6
#define NPY_METH_unaligned_contiguous_loop 7


/*
 * Used internally (initially) for real to complex loops only
 */
NPY_NO_EXPORT int
npy_default_get_strided_loop(
        PyArrayMethod_Context *context,
        int aligned, int NPY_UNUSED(move_references), const npy_intp *strides,
        PyArrayMethod_StridedLoop **out_loop, NpyAuxData **out_transferdata,
        NPY_ARRAYMETHOD_FLAGS *flags);


NPY_NO_EXPORT int
PyArrayMethod_GetMaskedStridedLoop(
        PyArrayMethod_Context *context,
        int aligned,
        npy_intp *fixed_strides,
        PyArrayMethod_StridedLoop **out_loop,
        NpyAuxData **out_transferdata,
        NPY_ARRAYMETHOD_FLAGS *flags);



NPY_NO_EXPORT PyObject *
PyArrayMethod_FromSpec(PyArrayMethod_Spec *spec);


/*
 * TODO: This function is the internal version, and its error paths may
 *       need better tests when a public version is exposed.
 */
NPY_NO_EXPORT PyBoundArrayMethodObject *
PyArrayMethod_FromSpec_int(PyArrayMethod_Spec *spec, int priv);

#ifdef __cplusplus
}
#endif

#endif  /* NUMPY_CORE_SRC_MULTIARRAY_ARRAY_METHOD_H_ */
