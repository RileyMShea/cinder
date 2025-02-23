/* Generator object implementation */

#include "Python.h"
#include "pycore_object.h"
#include "pycore_pystate.h"
#include "frameobject.h"
#include "structmember.h"
#include "opcode.h"
#include "Jit/frame.h"
#include "Jit/pyjit.h"

#define UNLIKELY(x) __builtin_expect((x), 0)

static PyObject *gen_close(PyGenObject *, PyObject *);
static PyObject *async_gen_asend_new(PyAsyncGenObject *, PyObject *);
static PyObject *async_gen_athrow_new(PyAsyncGenObject *, PyObject *);

static const char *NON_INIT_CORO_MSG = "can't send non-None value to a "
                                 "just-started coroutine";

static const char *ASYNC_GEN_IGNORED_EXIT_MSG =
                                 "async generator ignored GeneratorExit";

typedef struct {
    PyGenObject *free_list;
    int numfree;
} gen_free_list;

#define FREE_LIST_GEN 0
#define FREE_LIST_CORO 1
#define FREE_LIST_ASYNC_GEN 2
#define FREE_LIST_MAX 3

gen_free_list freelists[FREE_LIST_MAX];

int _PyGen_FreeListEnabled = 0;

#define PyGen_MAXFREELIST 200

static inline int
exc_state_traverse(_PyErr_StackItem *exc_state, visitproc visit, void *arg)
{
    Py_VISIT(exc_state->exc_type);
    Py_VISIT(exc_state->exc_value);
    Py_VISIT(exc_state->exc_traceback);
    return 0;
}

static int
gen_traverse(PyGenObject *gen, visitproc visit, void *arg)
{
    Py_VISIT((PyObject *)gen->gi_frame);
    Py_VISIT(gen->gi_code);
    Py_VISIT(gen->gi_name);
    Py_VISIT(gen->gi_qualname);
    if (gen->gi_jit_data) {
        int r = _PyJIT_GenVisitRefs(gen, visit, arg);
        if (r) {
            return r;
        }
    }
    /* No need to visit cr_origin, because it's just tuples/str/int, so can't
       participate in a reference cycle. */
    return exc_state_traverse(&gen->gi_exc_state, visit, arg);
}

__inline__ static int
gen_is_completed(PyGenObject *gen)
{
    if (gen->gi_jit_data) {
        return _PyJIT_GenState(gen) == _PyJitGenState_Completed;
    }
    return gen->gi_frame == NULL || gen->gi_frame->f_stacktop == NULL;
}

__inline__ static int
gen_is_just_started(PyGenObject* gen) {
    if (gen->gi_jit_data) {
        return _PyJIT_GenState(gen) == _PyJitGenState_JustStarted;
    }
    return gen->gi_frame->f_lasti == -1;
}

void
_PyGen_Finalize(PyObject *self)
{
    PyGenObject *gen = (PyGenObject *)self;
    PyObject *res = NULL;
    PyObject *error_type, *error_value, *error_traceback;

    if (gen_is_completed(gen)) {
        /* Generator isn't paused, so no need to close */
        return;
    }

    if (PyAsyncGen_CheckExact(self)) {
        PyAsyncGenObject *agen = (PyAsyncGenObject*)self;
        PyObject *finalizer = agen->ag_finalizer;
        if (finalizer && !agen->ag_closed) {
            /* Save the current exception, if any. */
            PyErr_Fetch(&error_type, &error_value, &error_traceback);

            res = _PyObject_Call1Arg(finalizer, self);

            if (res == NULL) {
                PyErr_WriteUnraisable(self);
            } else {
                Py_DECREF(res);
            }
            /* Restore the saved exception. */
            PyErr_Restore(error_type, error_value, error_traceback);
            return;
        }
    }

    /* Save the current exception, if any. */
    PyErr_Fetch(&error_type, &error_value, &error_traceback);

    /* If `gen` is a coroutine, and if it was never awaited on,
       issue a RuntimeWarning. */
    if (gen->gi_code != NULL &&
        ((PyCodeObject *)gen->gi_code)->co_flags & CO_COROUTINE &&
        gen_is_just_started(gen))
    {
        _PyErr_WarnUnawaitedCoroutine((PyObject *)gen);
    }
    else {
        res = gen_close(gen, NULL);
    }

    if (res == NULL) {
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    else {
        Py_DECREF(res);
    }

    /* Restore the saved exception. */
    PyErr_Restore(error_type, error_value, error_traceback);
}

static inline void
exc_state_clear(_PyErr_StackItem *exc_state)
{
    PyObject *t, *v, *tb;
    t = exc_state->exc_type;
    v = exc_state->exc_value;
    tb = exc_state->exc_traceback;
    exc_state->exc_type = NULL;
    exc_state->exc_value = NULL;
    exc_state->exc_traceback = NULL;
    Py_XDECREF(t);
    Py_XDECREF(v);
    Py_XDECREF(tb);
}

static void
gen_dealloc(PyGenObject *gen)
{
    PyObject *self = (PyObject *) gen;

    _PyObject_GC_UNTRACK(gen);

    if (gen->gi_weakreflist != NULL)
        PyObject_ClearWeakRefs(self);

    _PyObject_GC_TRACK(self);

    if (PyObject_CallFinalizerFromDealloc(self))
        return;                     /* resurrected.  :( */

    _PyObject_GC_UNTRACK(self);
    if (PyAsyncGen_CheckExact(gen)) {
        /* We have to handle this case for asynchronous generators
           right here, because this code has to be between UNTRACK
           and GC_Del. */
        Py_CLEAR(((PyAsyncGenObject*)gen)->ag_finalizer);
    }
    if (gen->gi_frame != NULL) {
        gen->gi_frame->f_gen = NULL;
        Py_CLEAR(gen->gi_frame);
    }
    if (gen->gi_jit_data) {
        _PyJIT_GenDealloc(gen);
    }
    if (((PyCodeObject *)gen->gi_code)->co_flags & CO_COROUTINE) {
        Py_CLEAR(((PyCoroObject *)gen)->cr_origin);
    }
    Py_CLEAR(gen->gi_code);
    Py_CLEAR(gen->gi_name);
    Py_CLEAR(gen->gi_qualname);
    exc_state_clear(&gen->gi_exc_state);

    gen_free_list *list = NULL;
    if (PyGen_CheckExact(gen)) {
        list = &freelists[FREE_LIST_GEN];
    } else if (PyCoro_CheckExact(gen)) {
        list = &freelists[FREE_LIST_CORO];
    } else if (PyAsyncGen_CheckExact(gen)) {
        list = &freelists[FREE_LIST_ASYNC_GEN];
    } else {
        assert(0);
    }

    if (_PyGen_FreeListEnabled && list->numfree < PyGen_MAXFREELIST) {
        ++list->numfree;
        gen->gi_code = (PyObject *)list->free_list;
        list->free_list = gen;
    } else {
        PyObject_GC_Del(gen);
    }
}

static PyObject *
gen_send_ex_with_finish_yf(PyGenObject *gen,
                           PyObject *arg,
                           int exc,
                           int closing,
                           int finish_yield_from)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyFrameObject *f = gen->gi_frame;
    PyObject *result;

    if (gen->gi_running) {
        const char *msg = "generator already executing";
        if (PyCoro_CheckExact(gen)) {
            msg = "coroutine already executing";
        }
        else if (PyAsyncGen_CheckExact(gen)) {
            msg = "async generator already executing";
        }
        PyErr_SetString(PyExc_ValueError, msg);
        return NULL;
    }

    if (gen_is_completed(gen))  {
        if (PyCoro_CheckExact(gen) && !closing) {
            /* `gen` is an exhausted coroutine: raise an error,
               except when called from gen_close(), which should
               always be a silent method. */
            PyErr_SetString(
                PyExc_RuntimeError,
                "cannot reuse already awaited coroutine");
        }
        else if (arg && !exc) {
            /* `gen` is an exhausted generator:
               only set exception if called from send(). */
            if (PyAsyncGen_CheckExact(gen)) {
                PyErr_SetNone(PyExc_StopAsyncIteration);
            }
            else {
                PyErr_SetNone(PyExc_StopIteration);
            }
        }
        return NULL;
    }

    if (gen_is_just_started(gen)) {
        if (arg && arg != Py_None) {
            const char *msg = "can't send non-None value to a "
                              "just-started generator";
            if (PyCoro_CheckExact(gen)) {
                msg = NON_INIT_CORO_MSG;
            }
            else if (PyAsyncGen_CheckExact(gen)) {
                msg = "can't send non-None value to a "
                      "just-started async generator";
            }
            PyErr_SetString(PyExc_TypeError, msg);
            return NULL;
        }
    } else {
        arg = arg ? arg : Py_None;
        Py_INCREF(arg);
        if (!gen->gi_jit_data) {
            /* Push arg onto the frame's value stack */
            *(f->f_stacktop++) = arg;
        }
    }

    if (f) {
        /* Generators always return to their most recent caller, not
        * necessarily their creator. */
        Py_XINCREF(tstate->frame);
        assert(f->f_back == NULL);
        f->f_back = tstate->frame;
    }

    gen->gi_running = 1;
    gen->gi_exc_state.previous_item = tstate->exc_info;
    tstate->exc_info = &gen->gi_exc_state;
    if (gen->gi_jit_data) {
        result = _PyJIT_GenSend(gen, arg, exc, f, tstate, finish_yield_from);
        /* We might get a frame in no-frame mode if a deopt occurs. */
        assert(!f || f == gen->gi_frame);
        f = gen->gi_frame;
    } else {
        result = PyEval_EvalFrameEx(f, exc);
    }
    tstate->exc_info = gen->gi_exc_state.previous_item;
    gen->gi_exc_state.previous_item = NULL;
    gen->gi_running = 0;

    if (f) {
        /* Don't keep the reference to f_back any longer than necessary.  It
        * may keep a chain of frames alive or it could create a reference
        * cycle. */
        assert(f->f_back == tstate->frame);
        Py_CLEAR(f->f_back);
    }

    /* If the generator just returned (as opposed to yielding), signal
     * that the generator is exhausted. */
    if (result && gen_is_completed(gen)) {
        if (result == Py_None) {
            /* Delay exception instantiation if we can */
            if (PyAsyncGen_CheckExact(gen)) {
                PyErr_SetNone(PyExc_StopAsyncIteration);
            }
            else {
                PyErr_SetNone(PyExc_StopIteration);
            }
        }
        else {
            /* Async generators cannot return anything but None */
            assert(!PyAsyncGen_CheckExact(gen));
            _PyGen_SetStopIterationValue(result);
        }
        Py_CLEAR(result);
    }
    else if (!result && PyErr_ExceptionMatches(PyExc_StopIteration)) {
        const char *msg = "generator raised StopIteration";
        if (PyCoro_CheckExact(gen)) {
            msg = "coroutine raised StopIteration";
        }
        else if PyAsyncGen_CheckExact(gen) {
            msg = "async generator raised StopIteration";
        }
        _PyErr_FormatFromCause(PyExc_RuntimeError, "%s", msg);

    }
    else if (!result && PyAsyncGen_CheckExact(gen) &&
             PyErr_ExceptionMatches(PyExc_StopAsyncIteration))
    {
        /* code in `gen` raised a StopAsyncIteration error:
           raise a RuntimeError.
        */
        const char *msg = "async generator raised StopAsyncIteration";
        _PyErr_FormatFromCause(PyExc_RuntimeError, "%s", msg);
    }

    if (!result || gen_is_completed(gen)) {
        /* generator can't be rerun, so release the frame */
        /* first clean reference cycle through stored exception traceback */
        exc_state_clear(&gen->gi_exc_state);
        if (gen->gi_frame) {
            gen->gi_frame->f_gen = NULL;
            gen->gi_frame = NULL;
            Py_DECREF(f);
        } else {
            assert(!f);
        }
    }

    return result;
}

static PyObject *
gen_send_ex(PyGenObject *gen, PyObject *arg, int exc, int closing)
{
    return gen_send_ex_with_finish_yf(gen, arg, exc, closing, 0);
}

int
_PyGen_IsSuspended(PyGenObject *gen)
{
    if (gen->gi_running) {
        return 0;
    }
    if (gen->gi_jit_data) {
        return _PyJIT_GenState(gen) == _PyJitGenState_Running;
    } else {
        PyFrameObject *f = gen->gi_frame;
        return f && f->f_lasti != -1;
    }
}

PyObject *
_PyGen_Send_NoStopIteration(PyThreadState *tstate,
                            PyGenObject *gen,
                            PyObject *arg,
                            PyObject **pStopIterationValue)
{
    PyFrameObject *f = gen->gi_frame;
    PyObject *result;

    if (UNLIKELY(gen->gi_running)) {
        const char *msg = "generator already executing";
        if (PyCoro_CheckExact(gen)) {
            msg = "coroutine already executing";
        } else if (PyAsyncGen_CheckExact(gen)) {
            msg = "async generator already executing";
        }
        PyErr_SetString(PyExc_ValueError, msg);
        return NULL;
    }
    if (UNLIKELY(gen_is_completed(gen))) {
        if (PyCoro_CheckExact(gen)) {

            /* `gen` is an exhausted coroutine: raise an error,
               except when called from gen_close(), which should
               always be a silent method. */
            PyErr_SetString(PyExc_RuntimeError,
                            "cannot reuse already awaited coroutine");
        } else if (arg) {
            /* `gen` is an exhausted generator:
               only set exception if called from send(). */
            Py_INCREF(Py_None);
            *pStopIterationValue = Py_None;
        }
        return NULL;
    }

    if (UNLIKELY(gen_is_just_started(gen))) {
        if (arg && arg != Py_None) {
            const char *msg = "can't send non-None value to a "
                              "just-started generator";
            if (PyCoro_CheckExact(gen)) {
                msg = NON_INIT_CORO_MSG;
            }
            PyErr_SetString(PyExc_TypeError, msg);
            return NULL;
        }
    } else {
        arg = arg ? arg : Py_None;
        Py_INCREF(arg);
        if (!gen->gi_jit_data) {
            /* Push arg onto the frame's value stack */
            *(f->f_stacktop++) = arg;
        }
    }

    /* Generators always return to their most recent caller, not
     * necessarily their creator. */
    if (f) {
        Py_XINCREF(tstate->frame);
        assert(f->f_back == NULL);
        f->f_back = tstate->frame;
    }

    gen->gi_running = 1;
    gen->gi_exc_state.previous_item = tstate->exc_info;
    tstate->exc_info = &gen->gi_exc_state;
    if (gen->gi_jit_data) {
        result = _PyJIT_GenSend(gen, arg, 0, f, tstate, 0);
        /* We might get a frame in no-frame mode if a deopt occurs. */
        assert(!f || f == gen->gi_frame);
        f = gen->gi_frame;
    } else {
        result = PyEval_EvalFrameEx(f, 0);
    }
    tstate->exc_info = gen->gi_exc_state.previous_item;
    gen->gi_exc_state.previous_item = NULL;
    gen->gi_running = 0;

    if (f) {
        /* Don't keep the reference to f_back any longer than necessary.  It
        * may keep a chain of frames alive or it could create a reference
        * cycle. */
        assert(f->f_back == tstate->frame);
        Py_CLEAR(f->f_back);
    }

    /* If the generator just returned (as opposed to yielding), signal
     * that the generator is exhausted. */
    if (result && gen_is_completed(gen)) {
        // steal ref
        *pStopIterationValue = result;
        result = NULL;
    } else if (UNLIKELY(!result &&
                            PyErr_ExceptionMatches(PyExc_StopIteration))) {
        const char *msg = "generator raised StopIteration";
        if (PyCoro_CheckExact(gen)) {
            msg = "coroutine raised StopIteration";
        }
        _PyErr_FormatFromCause(PyExc_RuntimeError, "%s", msg);
    }

    if (!result || gen_is_completed(gen)) {
        /* generator can't be rerun, so release the frame */
        /* first clean reference cycle through stored exception traceback */
        exc_state_clear(&gen->gi_exc_state);
        if (gen->gi_frame) {
            gen->gi_frame->f_gen = NULL;
            gen->gi_frame = NULL;
            Py_DECREF(f);
        } else {
            assert(!f);
        }
    }

    return result;
}

PyDoc_STRVAR(send_doc,
"send(arg) -> send 'arg' into generator,\n\
return next yielded value or raise StopIteration.");

PyObject *
_PyGen_Send(PyGenObject *gen, PyObject *arg)
{
    return gen_send_ex(gen, arg, 0, 0);
}

PyDoc_STRVAR(close_doc,
"close() -> raise GeneratorExit inside generator.");

/*
 *   This helper function is used by gen_close and gen_throw to
 *   close a subiterator being delegated to by yield-from.
 */

static int
gen_close_iter(PyObject *yf)
{
    PyObject *retval = NULL;
    _Py_IDENTIFIER(close);

    if (PyGen_CheckExact(yf) || PyCoro_CheckExact(yf)) {
        retval = gen_close((PyGenObject *)yf, NULL);
        if (retval == NULL)
            return -1;
    }
    else {
        PyObject *meth;
        if (_PyObject_LookupAttrId(yf, &PyId_close, &meth) < 0) {
            PyErr_WriteUnraisable(yf);
        }
        if (meth) {
            retval = _PyObject_CallNoArg(meth);
            Py_DECREF(meth);
            if (retval == NULL)
                return -1;
        }
    }
    Py_XDECREF(retval);
    return 0;
}

int
_PyGen_close_yf(PyObject *yf)
{
    return gen_close_iter(yf);
}

PyObject *
_PyGen_yf(PyGenObject *gen)
{
    PyObject *yf = NULL;
    PyFrameObject *f = gen->gi_frame;

    if (gen->gi_jit_data) {
        return _PyJIT_GenYieldFromValue(gen);
    }

    if (f && f->f_stacktop) {
        PyObject *bytecode = f->f_code->co_code;
        unsigned char *code = (unsigned char *)PyBytes_AS_STRING(bytecode);

        if (f->f_lasti < 0) {
            /* Return immediately if the frame didn't start yet. YIELD_FROM
               always come after LOAD_CONST: a code object should not start
               with YIELD_FROM */
            assert(code[0] != YIELD_FROM);
            return NULL;
        }

        if (code[f->f_lasti + sizeof(_Py_CODEUNIT)] != YIELD_FROM)
            return NULL;
        yf = f->f_stacktop[-1];
        Py_INCREF(yf);
    }

    return yf;
}

static PyObject *
gen_close(PyGenObject *gen, PyObject *args)
{
    PyObject *retval;
    PyObject *yf = _PyGen_yf(gen);
    int err = 0;

    if (yf) {
        gen->gi_running = 1;
        err = gen_close_iter(yf);
        gen->gi_running = 0;
        Py_DECREF(yf);
    }
    if (err == 0)
        PyErr_SetNone(PyExc_GeneratorExit);
    retval = gen_send_ex(gen, Py_None, 1, 1);
    if (retval) {
        const char *msg = "generator ignored GeneratorExit";
        if (PyCoro_CheckExact(gen)) {
            msg = "coroutine ignored GeneratorExit";
        } else if (PyAsyncGen_CheckExact(gen)) {
            msg = ASYNC_GEN_IGNORED_EXIT_MSG;
        }
        Py_DECREF(retval);
        PyErr_SetString(PyExc_RuntimeError, msg);
        return NULL;
    }
    if (PyErr_ExceptionMatches(PyExc_StopIteration)
        || PyErr_ExceptionMatches(PyExc_GeneratorExit)) {
        PyErr_Clear();          /* ignore these errors */
        Py_RETURN_NONE;
    }
    return NULL;
}

static int
_gen_restore_error(PyObject *typ, PyObject *val, PyObject *tb)
{
    /* First, check the traceback argument, replacing None with
       NULL. */
    if (tb == Py_None) {
        tb = NULL;
    }
    else if (tb != NULL && !PyTraceBack_Check(tb)) {
        PyErr_SetString(PyExc_TypeError,
            "throw() third argument must be a traceback object");
        return -1;
    }

    Py_INCREF(typ);
    Py_XINCREF(val);
    Py_XINCREF(tb);

    if (PyExceptionClass_Check(typ))
        PyErr_NormalizeException(&typ, &val, &tb);

    else if (PyExceptionInstance_Check(typ)) {
        /* Raising an instance.  The value should be a dummy. */
        if (val && val != Py_None) {
            PyErr_SetString(PyExc_TypeError,
              "instance exception may not have a separate value");
            goto failed_throw;
        }
        else {
            /* Normalize to raise <class>, <instance> */
            Py_XDECREF(val);
            val = typ;
            typ = PyExceptionInstance_Class(typ);
            Py_INCREF(typ);

            if (tb == NULL)
                /* Returns NULL if there's no traceback */
                tb = PyException_GetTraceback(val);
        }
    }
    else {
        /* Not something you can raise.  throw() fails. */
        PyErr_Format(PyExc_TypeError,
                     "exceptions must be classes or instances "
                     "deriving from BaseException, not %s",
                     Py_TYPE(typ)->tp_name);
            goto failed_throw;
    }

    PyErr_Restore(typ, val, tb);
    return 0;

failed_throw:
    /* Didn't use our arguments, so restore their original refcounts */
    Py_DECREF(typ);
    Py_XDECREF(val);
    Py_XDECREF(tb);
    return -1;
}

int
_PyGen_restore_error(PyObject *et, PyObject *ev, PyObject *tb)
{
    return _gen_restore_error(et, ev, tb);
}

PyDoc_STRVAR(throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in generator,\n\
return next yielded value or raise StopIteration.");

static PyObject *
_gen_throw(PyGenObject *gen, int close_on_genexit,
           PyObject *typ, PyObject *val, PyObject *tb)
{
    PyObject *yf = _PyGen_yf(gen);
    _Py_IDENTIFIER(throw);

    if (yf) {
        PyObject *ret;
        int err;
        if (PyErr_GivenExceptionMatches(typ, PyExc_GeneratorExit) &&
            close_on_genexit
        ) {
            /* Asynchronous generators *should not* be closed right away.
               We have to allow some awaits to work it through, hence the
               `close_on_genexit` parameter here.
            */
            gen->gi_running = 1;
            err = gen_close_iter(yf);
            gen->gi_running = 0;
            Py_DECREF(yf);
            if (err < 0)
                return gen_send_ex(gen, Py_None, 1, 0);
            goto throw_here;
        }
        if (PyGen_CheckExact(yf) || PyCoro_CheckExact(yf)) {
            /* `yf` is a generator or a coroutine. */
            gen->gi_running = 1;
            /* Close the generator that we are currently iterating with
               'yield from' or awaiting on with 'await'. */
            ret = _gen_throw((PyGenObject *)yf, close_on_genexit,
                             typ, val, tb);
            gen->gi_running = 0;
        } else {
            /* `yf` is an iterator or a coroutine-like object. */
            PyObject *meth;
            if (_PyObject_LookupAttrId(yf, &PyId_throw, &meth) < 0) {
                Py_DECREF(yf);
                return NULL;
            }
            if (meth == NULL) {
                Py_DECREF(yf);
                goto throw_here;
            }
            gen->gi_running = 1;
            ret = PyObject_CallFunctionObjArgs(meth, typ, val, tb, NULL);
            gen->gi_running = 0;
            Py_DECREF(meth);
        }
        Py_DECREF(yf);
        if (!ret) {
            PyObject *val;
            if (!gen->gi_jit_data) {
                /* Pop subiterator from stack */
                ret = *(--gen->gi_frame->f_stacktop);
                assert(ret == yf);
                Py_DECREF(ret);
                /* Termination repetition of YIELD_FROM */
                assert(gen->gi_frame->f_lasti >= 0);
                gen->gi_frame->f_lasti += sizeof(_Py_CODEUNIT);
            }
            if (_PyGen_FetchStopIterationValue(&val) == 0) {
                ret = gen_send_ex_with_finish_yf(gen, val, 0, 0, 1);
                Py_DECREF(val);
            } else {
                ret = gen_send_ex_with_finish_yf(gen, Py_None, 1, 0, 1);
            }
        }
        return ret;
    }

throw_here:
    if (_gen_restore_error(typ, val, tb) == -1) {
        return NULL;
    }
    return gen_send_ex(gen, Py_None, 1, 0);
}


static PyObject *
gen_throw(PyGenObject *gen, PyObject *args)
{
    PyObject *typ;
    PyObject *tb = NULL;
    PyObject *val = NULL;

    if (!PyArg_UnpackTuple(args, "throw", 1, 3, &typ, &val, &tb)) {
        return NULL;
    }

    return _gen_throw(gen, 1, typ, val, tb);
}

static PyObject *
gen_throw_fastcall(PyGenObject *gen, PyObject **args, Py_ssize_t nargs)
{
    PyObject *typ;
    PyObject *tb = NULL;
    PyObject *val = NULL;

    if (!_PyArg_UnpackStack(args, nargs, "throw", 1, 3, &typ, &val, &tb)) {
        return NULL;
    }

    return _gen_throw(gen, 1, typ, val, tb);
}

static PyObject *
gen_iternext(PyGenObject *gen)
{
    return gen_send_ex(gen, NULL, 0, 0);
}

/*
 * Set StopIteration with specified value.  Value can be arbitrary object
 * or NULL.
 *
 * Returns 0 if StopIteration is set and -1 if any other exception is set.
 */
int
_PyGen_SetStopIterationValue(PyObject *value)
{
    PyObject *e;

    if (value == NULL ||
        (!PyTuple_Check(value) && !PyExceptionInstance_Check(value)))
    {
        /* Delay exception instantiation if we can */
        PyErr_SetObject(PyExc_StopIteration, value);
        return 0;
    }
    /* Construct an exception instance manually with
     * PyObject_CallFunctionObjArgs and pass it to PyErr_SetObject.
     *
     * We do this to handle a situation when "value" is a tuple, in which
     * case PyErr_SetObject would set the value of StopIteration to
     * the first element of the tuple.
     *
     * (See PyErr_SetObject/_PyErr_CreateException code for details.)
     */
    e = _PyObject_Call1Arg(PyExc_StopIteration, value);
    if (e == NULL) {
        return -1;
    }
    PyErr_SetObject(PyExc_StopIteration, e);
    Py_DECREF(e);
    return 0;
}

/*
 *   If StopIteration exception is set, fetches its 'value'
 *   attribute if any, otherwise sets pvalue to None.
 *
 *   Returns 0 if no exception or StopIteration is set.
 *   If any other exception is set, returns -1 and leaves
 *   pvalue unchanged.
 */

int
_PyGen_FetchStopIterationValue(PyObject **pvalue)
{
    PyObject *et, *ev, *tb;
    PyObject *value = NULL;

    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Fetch(&et, &ev, &tb);
        if (ev) {
            /* exception will usually be normalised already */
            if (PyObject_TypeCheck(ev, (PyTypeObject *) et)) {
                value = ((PyStopIterationObject *)ev)->value;
                Py_INCREF(value);
                Py_DECREF(ev);
            } else if (et == PyExc_StopIteration && !PyTuple_Check(ev)) {
                /* Avoid normalisation and take ev as value.
                 *
                 * Normalization is required if the value is a tuple, in
                 * that case the value of StopIteration would be set to
                 * the first element of the tuple.
                 *
                 * (See _PyErr_CreateException code for details.)
                 */
                value = ev;
            } else {
                /* normalisation required */
                PyErr_NormalizeException(&et, &ev, &tb);
                if (!PyObject_TypeCheck(ev, (PyTypeObject *)PyExc_StopIteration)) {
                    PyErr_Restore(et, ev, tb);
                    return -1;
                }
                value = ((PyStopIterationObject *)ev)->value;
                Py_INCREF(value);
                Py_DECREF(ev);
            }
        }
        Py_XDECREF(et);
        Py_XDECREF(tb);
    } else if (PyErr_Occurred()) {
        return -1;
    }
    if (value == NULL) {
        value = Py_None;
        Py_INCREF(value);
    }
    *pvalue = value;
    return 0;
}

static PyObject *
gen_repr(PyGenObject *gen)
{
    return PyUnicode_FromFormat("<generator object %S at %p>",
                                gen->gi_qualname, gen);
}

static PyObject *
gen_get_name(PyGenObject *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->gi_name);
    return op->gi_name;
}

static int
gen_set_name(PyGenObject *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del gen.gi_name or to set it to anything
     * other than a string object. */
    if (value == NULL || !PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__name__ must be set to a string object");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(op->gi_name, value);
    return 0;
}

static PyObject *
gen_get_qualname(PyGenObject *op, void *Py_UNUSED(ignored))
{
    Py_INCREF(op->gi_qualname);
    return op->gi_qualname;
}

static int
gen_set_qualname(PyGenObject *op, PyObject *value, void *Py_UNUSED(ignored))
{
    /* Not legal to del gen.__qualname__ or to set it to anything
     * other than a string object. */
    if (value == NULL || !PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "__qualname__ must be set to a string object");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(op->gi_qualname, value);
    return 0;
}

static PyObject *
gen_getyieldfrom(PyGenObject *gen, void *Py_UNUSED(ignored))
{
    PyObject *yf = _PyGen_yf(gen);
    if (yf == NULL)
        Py_RETURN_NONE;
    return yf;
}

static PyObject *
gen_getframe(PyGenObject *gen, void *Py_UNUSED(ignored))
{
    PyFrameObject *frame = gen->gi_frame;
    if (frame == NULL) {
        Py_RETURN_NONE;
    }

    frame = JIT_MaterializeToFrame(PyThreadState_GET(), frame);
    gen->gi_frame = frame;
    Py_INCREF(frame);
    return (PyObject *)frame;
}


static PyGetSetDef gen_getsetlist[] = {
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the generator")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the generator")},
    {"gi_yieldfrom", (getter)gen_getyieldfrom, NULL,
     PyDoc_STR("object being iterated by yield from, or None")},
    {"gi_frame", (getter)gen_getframe, NULL, NULL},
    {NULL} /* Sentinel */
};

static PyMemberDef gen_memberlist[] = {
    {"gi_running",   T_BOOL,   offsetof(PyGenObject, gi_running),  READONLY},
    {"gi_code",      T_OBJECT, offsetof(PyGenObject, gi_code),     READONLY},
    {NULL}      /* Sentinel */
};

static PyMethodDef gen_methods[] = {
    {"send",(PyCFunction)_PyGen_Send, METH_O, send_doc},
    {"throw",(PyCFunction)gen_throw_fastcall, METH_FASTCALL, throw_doc},
    {"close",(PyCFunction)gen_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject PyGen_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "generator",                                /* tp_name */
    sizeof(PyGenObject),                        /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)gen_dealloc,                    /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)gen_repr,                         /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)gen_traverse,                 /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    offsetof(PyGenObject, gi_weakreflist),      /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)gen_iternext,                 /* tp_iternext */
    gen_methods,                                /* tp_methods */
    gen_memberlist,                             /* tp_members */
    gen_getsetlist,                             /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */

    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
    0,                                          /* tp_free */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    0,                                          /* tp_del */
    0,                                          /* tp_version_tag */
    _PyGen_Finalize,                            /* tp_finalize */
};

static PyGenObject *
gen_alloc(PyTypeObject *type, int free_list)
{
    PyGenObject *gen;

    gen_free_list *list = &freelists[free_list];
    if (list->free_list == NULL) {
        return PyObject_GC_New(PyGenObject, type);
    }

    assert(list->numfree > 0);
    --list->numfree;
    gen = list->free_list;
    list->free_list = (PyGenObject *)gen->gi_code;

    assert(((PyObject *)gen)->ob_type == type);

    _Py_NewReference((PyObject *)gen);
    _PyGC_UNSET_FINALIZED(gen);

    return gen;
}

static PyObject *
gen_new_with_qualname(PyGenObject *gen,
                      PyFrameObject *f,
                      PyCodeObject *code,
                      PyObject *name,
                      PyObject *qualname)
{
    if (gen == NULL) {
        Py_XDECREF(f);
        return NULL;
    }

    gen->gi_jit_data = NULL;

    gen->gi_frame = f;
    if (f) {
        assert(!PyTinyFrame_Check(f));
        f->f_gen = (PyObject *) gen;
    }
    Py_INCREF(code);
    gen->gi_code = (PyObject*)code;
    gen->gi_running = 0;
    gen->gi_weakreflist = NULL;
    gen->gi_exc_state.exc_type = NULL;
    gen->gi_exc_state.exc_value = NULL;
    gen->gi_exc_state.exc_traceback = NULL;
    gen->gi_exc_state.previous_item = NULL;
    if (name != NULL)
        gen->gi_name = name;
    else
        gen->gi_name = ((PyCodeObject *)gen->gi_code)->co_name;
    Py_INCREF(gen->gi_name);
    if (qualname != NULL)
        gen->gi_qualname = qualname;
    else
        gen->gi_qualname = gen->gi_name;
    Py_INCREF(gen->gi_qualname);
    _PyObject_GC_TRACK(gen);
    return (PyObject *)gen;
}

PyObject *
_PyGen_NewNoFrame(PyCodeObject *code)
{
    return gen_new_with_qualname(
        // TODO(jbower) use qualname when this is available
        gen_alloc(&PyGen_Type, FREE_LIST_GEN), NULL, code, code->co_name, NULL);
}

PyObject *
PyGen_NewWithQualName(PyFrameObject *f, PyObject *name, PyObject *qualname)
{
    return gen_new_with_qualname(
        gen_alloc(&PyGen_Type, FREE_LIST_GEN), f, f->f_code, name, qualname);
}

PyObject *
PyGen_New(PyFrameObject *f)
{
    return gen_new_with_qualname(
        gen_alloc(&PyGen_Type, FREE_LIST_GEN), f, f->f_code, NULL, NULL);
}

int
PyGen_NeedsFinalizing(PyGenObject *gen)
{
    PyFrameObject *f = gen->gi_frame;

    if (gen_is_completed(gen))
        return 0; /* no frame or empty blockstack == no finalization */

    /* Any (exception-handling) block type requires cleanup. */
    // TODO(jbower): I guess this is broken already for generators? I'm not sure
    // what this function is used for. It seems to be for C-API only and I can't
    // find it being actively used in zbgs .
    if (f && f->f_iblock > 0)
        return 1;

    /* No blocks, it's safe to skip finalization. */
    return 0;
}

/* Coroutine Object */

typedef struct {
    PyObject_HEAD
    PyCoroObject *cw_coroutine;
} PyCoroWrapper;

static int
gen_is_coroutine(PyObject *o)
{
    if (PyGen_CheckExact(o)) {
        PyCodeObject *code = (PyCodeObject *)((PyGenObject*)o)->gi_code;
        if (code->co_flags & CO_ITERABLE_COROUTINE) {
            return 1;
        }
    }
    return 0;
}

/*
 *   This helper function returns an awaitable for `o`:
 *     - `o` if `o` is a coroutine-object;
 *     - `type(o)->tp_as_async->am_await(o)`
 *
 *   Raises a TypeError if it's not possible to return
 *   an awaitable and returns NULL.
 */
PyObject *
_PyCoro_GetAwaitableIter(PyObject *o)
{
    unaryfunc getter = NULL;
    PyTypeObject *ot;

    if (PyCoro_CheckExact(o) || gen_is_coroutine(o)) {
        /* 'o' is a coroutine. */
        Py_INCREF(o);
        return o;
    }

    ot = Py_TYPE(o);
    if (ot->tp_as_async != NULL) {
        getter = ot->tp_as_async->am_await;
    }
    if (getter != NULL) {
        PyObject *res = (*getter)(o);
        if (res != NULL) {
            if (PyCoro_CheckExact(res) || gen_is_coroutine(res)) {
                /* __await__ must return an *iterator*, not
                   a coroutine or another awaitable (see PEP 492) */
                PyErr_SetString(PyExc_TypeError,
                                "__await__() returned a coroutine");
                Py_CLEAR(res);
            } else if (!PyIter_Check(res)) {
                PyErr_Format(PyExc_TypeError,
                             "__await__() returned non-iterator "
                             "of type '%.100s'",
                             Py_TYPE(res)->tp_name);
                Py_CLEAR(res);
            }
        }
        return res;
    }

    PyErr_Format(PyExc_TypeError,
                 "object %.100s can't be used in 'await' expression",
                 ot->tp_name);
    return NULL;
}

static PyObject *
coro_repr(PyCoroObject *coro)
{
    return PyUnicode_FromFormat("<coroutine object %S at %p>",
                                coro->cr_qualname, coro);
}

static PyObject *
coro_await(PyCoroObject *coro)
{
    PyCoroWrapper *cw = PyObject_GC_New(PyCoroWrapper, &_PyCoroWrapper_Type);
    if (cw == NULL) {
        return NULL;
    }
    Py_INCREF(coro);
    cw->cw_coroutine = coro;
    _PyObject_GC_TRACK(cw);
    return (PyObject *)cw;
}

static PyObject *
coro_get_cr_await(PyCoroObject *coro, void *Py_UNUSED(ignored))
{
    PyObject *yf = _PyGen_yf((PyGenObject *) coro);
    if (yf == NULL)
        Py_RETURN_NONE;
    return yf;
}

static PyGetSetDef coro_getsetlist[] = {
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the coroutine")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the coroutine")},
    {"cr_await", (getter)coro_get_cr_await, NULL,
     PyDoc_STR("object being awaited on, or None")},
    {"cr_frame", (getter)gen_getframe, NULL, NULL},
    {NULL} /* Sentinel */
};

static PyMemberDef coro_memberlist[] = {
    {"cr_running",   T_BOOL,   offsetof(PyCoroObject, cr_running),  READONLY},
    {"cr_code",      T_OBJECT, offsetof(PyCoroObject, cr_code),     READONLY},
    {"cr_origin",    T_OBJECT, offsetof(PyCoroObject, cr_origin),   READONLY},
    {NULL}      /* Sentinel */
};

PyDoc_STRVAR(coro_send_doc,
"send(arg) -> send 'arg' into coroutine,\n\
return next iterated value or raise StopIteration.");

PyDoc_STRVAR(coro_throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in coroutine,\n\
return next iterated value or raise StopIteration.");

PyDoc_STRVAR(coro_close_doc,
"close() -> raise GeneratorExit inside coroutine.");

static PyMethodDef coro_methods[] = {
    {"send",(PyCFunction)_PyGen_Send, METH_O, coro_send_doc},
    {"throw",(PyCFunction)gen_throw_fastcall, METH_FASTCALL, coro_throw_doc},
    {"close",(PyCFunction)gen_close, METH_NOARGS, coro_close_doc},
    {NULL, NULL}        /* Sentinel */
};

static PyAsyncMethods coro_as_async = {
    (unaryfunc)coro_await,                      /* am_await */
    0,                                          /* am_aiter */
    0                                           /* am_anext */
};

PyTypeObject PyCoro_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "coroutine",                                /* tp_name */
    sizeof(PyCoroObject),                       /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)gen_dealloc,                    /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    &coro_as_async,                             /* tp_as_async */
    (reprfunc)coro_repr,                        /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)gen_traverse,                 /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    offsetof(PyCoroObject, cr_weakreflist),     /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    coro_methods,                               /* tp_methods */
    coro_memberlist,                            /* tp_members */
    coro_getsetlist,                            /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
    0,                                          /* tp_free */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    0,                                          /* tp_del */
    0,                                          /* tp_version_tag */
    _PyGen_Finalize,                            /* tp_finalize */
};

static void
coro_wrapper_dealloc(PyCoroWrapper *cw)
{
    _PyObject_GC_UNTRACK((PyObject *)cw);
    Py_CLEAR(cw->cw_coroutine);
    PyObject_GC_Del(cw);
}

static PyObject *
coro_wrapper_iternext(PyCoroWrapper *cw)
{
    return gen_send_ex((PyGenObject *)cw->cw_coroutine, NULL, 0, 0);
}

static PyObject *
coro_wrapper_send(PyCoroWrapper *cw, PyObject *arg)
{
    return gen_send_ex((PyGenObject *)cw->cw_coroutine, arg, 0, 0);
}

static PyObject *
coro_wrapper_throw(PyCoroWrapper *cw, PyObject *args)
{
    return gen_throw((PyGenObject *)cw->cw_coroutine, args);
}

static PyObject *
coro_wrapper_close(PyCoroWrapper *cw, PyObject *args)
{
    return gen_close((PyGenObject *)cw->cw_coroutine, args);
}

static int
coro_wrapper_traverse(PyCoroWrapper *cw, visitproc visit, void *arg)
{
    Py_VISIT((PyObject *)cw->cw_coroutine);
    return 0;
}

static PyMethodDef coro_wrapper_methods[] = {
    {"send",(PyCFunction)coro_wrapper_send, METH_O, coro_send_doc},
    {"throw",(PyCFunction)coro_wrapper_throw, METH_VARARGS, coro_throw_doc},
    {"close",(PyCFunction)coro_wrapper_close, METH_NOARGS, coro_close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject _PyCoroWrapper_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "coroutine_wrapper",
    sizeof(PyCoroWrapper),                      /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)coro_wrapper_dealloc,           /* destructor tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    "A wrapper object implementing __await__ for coroutines.",
    (traverseproc)coro_wrapper_traverse,        /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)coro_wrapper_iternext,        /* tp_iternext */
    coro_wrapper_methods,                       /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
    0,                                          /* tp_free */
};

static PyObject *
compute_cr_origin(int origin_depth)
{
    PyFrameObject *frame = PyEval_GetFrame();
    /* First count how many frames we have */
    int frame_count = 0;
    for (; frame && frame_count < origin_depth; ++frame_count) {
        frame = JIT_MaterializePrevFrame(frame);
    }

    /* Now collect them */
    PyObject *cr_origin = PyTuple_New(frame_count);
    if (cr_origin == NULL) {
        return NULL;
    }
    frame = PyEval_GetFrame();
    for (int i = 0; i < frame_count; ++i) {
        PyObject *frameinfo = Py_BuildValue(
            "OiO",
            frame->f_code->co_filename,
            PyFrame_GetLineNumber(frame),
            frame->f_code->co_name);
        if (!frameinfo) {
            Py_DECREF(cr_origin);
            return NULL;
        }
        PyTuple_SET_ITEM(cr_origin, i, frameinfo);
        frame = frame->f_back;
    }

    return cr_origin;
}

static PyObject *
coro_new(
    PyThreadState *tstate,
    PyFrameObject *f,
    PyCodeObject *code,
    PyObject *name,
    PyObject *qualname)
{
    PyObject *coro = gen_new_with_qualname(
        gen_alloc(&PyCoro_Type, FREE_LIST_CORO), f, code, name, qualname);
    if (!coro) {
        return NULL;
    }

    int origin_depth = tstate->coroutine_origin_tracking_depth;
    ((PyCoroObject *)coro)->creator = NULL;

    if (origin_depth == 0) {
        ((PyCoroObject *)coro)->cr_origin = NULL;
    } else {
        PyObject *cr_origin = compute_cr_origin(origin_depth);
        ((PyCoroObject *)coro)->cr_origin = cr_origin;
        if (!cr_origin) {
            Py_DECREF(coro);
            return NULL;
        }
    }

    return coro;
}

PyObject *
_PyCoro_NewNoFrame(PyThreadState *tstate, PyCodeObject *code)
{
    // TODO(jbower): Add qualname when it becomes available
    return coro_new(tstate, NULL, code, code->co_name, NULL);
}


PyObject *
PyCoro_New(PyFrameObject *f, PyObject *name, PyObject *qualname)
{
    return coro_new(PyThreadState_GET(), f, f->f_code, name, qualname);
}

PyObject *
_PyCoro_NewTstate(PyThreadState *tstate,
                   PyFrameObject *f,
                   PyObject *name,
                   PyObject *qualname)
{
    return coro_new(tstate, f, f->f_code, name, qualname);
}

PyObject *
_PyCoro_ForFrame(PyThreadState *tstate,
                 PyFrameObject *f,
                 PyObject *name,
                 PyObject *qualname)
{
    /* Don't need to keep the reference to f_back, it will be set
     * when the generator is resumed. */
    Py_CLEAR(f->f_back);

    /* Create a new generator that owns the ready to run frame
     * and return that as the value. */
    PyObject *gen = coro_new(tstate, f, f->f_code, name, qualname);
    if (gen == NULL) {
        return NULL;
    }

    JIT_MaterializeTopFrame(tstate);

    PyFrameObject *parent_f = tstate->frame;
    const char *UTF8_name = PyUnicode_AsUTF8(parent_f->f_code->co_name);
    if (UTF8_name[0] == '<' &&
        (!strcmp(UTF8_name, "<genexpr>") || !strcmp(UTF8_name, "<listcomp>") ||
         !strcmp(UTF8_name, "<dictcomp>"))) {
        JIT_MaterializePrevFrame(parent_f);
        ((PyCoroObject *)gen)->creator = parent_f->f_back;
    } else {
        ((PyCoroObject *)gen)->creator = parent_f;
    }

    assert(!_PyObject_GC_IS_TRACKED(f));
    _PyObject_GC_TRACK(f);

    return gen;
}

PyTypeObject PyWaitHandle_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "wait handle",
    .tp_basicsize = sizeof(PyWaitHandleObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

/* ========= Wait Handles ========= */
PyWaitHandleObject PyWaitHandle = {
    {_PyObject_EXTRA_INIT
    kImmortalInitialCount, &PyWaitHandle_Type}, NULL, NULL
};

PyObject *
_PyWaitHandle_New(PyObject *coro_or_result, PyObject *waiter)
{
    if (PyWaitHandle.wh_coro_or_result != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "_PyWaitHandle_New is called when singleton wait handle is still in use.");
        return NULL;
    }
    PyWaitHandle.wh_coro_or_result = coro_or_result;
    PyWaitHandle.wh_waiter = waiter;
    return (PyObject *)&PyWaitHandle;
}

void
_PyWaitHandle_Release(PyObject *wait_handle)
{
    assert(_PyWaitHandle_CheckExact(wait_handle));
    ((PyWaitHandleObject *)wait_handle)->wh_coro_or_result = NULL;
    ((PyWaitHandleObject *)wait_handle)->wh_waiter = NULL;
}

/* ========= Asynchronous Generators ========= */


typedef enum {
    AWAITABLE_STATE_INIT,   /* new awaitable, has not yet been iterated */
    AWAITABLE_STATE_ITER,   /* being iterated */
    AWAITABLE_STATE_CLOSED, /* closed */
} AwaitableState;


typedef struct {
    PyObject_HEAD
    PyAsyncGenObject *ags_gen;

    /* Can be NULL, when in the __anext__() mode
       (equivalent of "asend(None)") */
    PyObject *ags_sendval;

    AwaitableState ags_state;
} PyAsyncGenASend;


typedef struct {
    PyObject_HEAD
    PyAsyncGenObject *agt_gen;

    /* Can be NULL, when in the "aclose()" mode
       (equivalent of "athrow(GeneratorExit)") */
    PyObject *agt_args;

    AwaitableState agt_state;
} PyAsyncGenAThrow;


typedef struct {
    PyObject_HEAD
    PyObject *agw_val;
} _PyAsyncGenWrappedValue;


#ifndef _PyAsyncGen_MAXFREELIST
#define _PyAsyncGen_MAXFREELIST 80
#endif

/* Freelists boost performance 6-10%; they also reduce memory
   fragmentation, as _PyAsyncGenWrappedValue and PyAsyncGenASend
   are short-living objects that are instantiated for every
   __anext__ call.
*/

static _PyAsyncGenWrappedValue *ag_value_freelist[_PyAsyncGen_MAXFREELIST];
static int ag_value_freelist_free = 0;

static PyAsyncGenASend *ag_asend_freelist[_PyAsyncGen_MAXFREELIST];
static int ag_asend_freelist_free = 0;

#define _PyAsyncGenWrappedValue_CheckExact(o) \
                    (Py_TYPE(o) == &_PyAsyncGenWrappedValue_Type)

#define PyAsyncGenASend_CheckExact(o) \
                    (Py_TYPE(o) == &_PyAsyncGenASend_Type)


static int
async_gen_traverse(PyAsyncGenObject *gen, visitproc visit, void *arg)
{
    Py_VISIT(gen->ag_finalizer);
    return gen_traverse((PyGenObject*)gen, visit, arg);
}


static PyObject *
async_gen_repr(PyAsyncGenObject *o)
{
    return PyUnicode_FromFormat("<async_generator object %S at %p>",
                                o->ag_qualname, o);
}


static int
async_gen_init_hooks(PyAsyncGenObject *o)
{
    PyThreadState *tstate;
    PyObject *finalizer;
    PyObject *firstiter;

    if (o->ag_hooks_inited) {
        return 0;
    }

    o->ag_hooks_inited = 1;

    tstate = _PyThreadState_GET();

    finalizer = tstate->async_gen_finalizer;
    if (finalizer) {
        Py_INCREF(finalizer);
        o->ag_finalizer = finalizer;
    }

    firstiter = tstate->async_gen_firstiter;
    if (firstiter) {
        PyObject *res;

        Py_INCREF(firstiter);
        res = _PyObject_Call1Arg(firstiter, (PyObject *)o);
        Py_DECREF(firstiter);
        if (res == NULL) {
            return 1;
        }
        Py_DECREF(res);
    }

    return 0;
}


static PyObject *
async_gen_anext(PyAsyncGenObject *o)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_asend_new(o, NULL);
}


static PyObject *
async_gen_asend(PyAsyncGenObject *o, PyObject *arg)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_asend_new(o, arg);
}


static PyObject *
async_gen_aclose(PyAsyncGenObject *o, PyObject *arg)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_athrow_new(o, NULL);
}

static PyObject *
async_gen_athrow(PyAsyncGenObject *o, PyObject *args)
{
    if (async_gen_init_hooks(o)) {
        return NULL;
    }
    return async_gen_athrow_new(o, args);
}


static PyGetSetDef async_gen_getsetlist[] = {
    {"__name__", (getter)gen_get_name, (setter)gen_set_name,
     PyDoc_STR("name of the async generator")},
    {"__qualname__", (getter)gen_get_qualname, (setter)gen_set_qualname,
     PyDoc_STR("qualified name of the async generator")},
    {"ag_await", (getter)coro_get_cr_await, NULL,
     PyDoc_STR("object being awaited on, or None")},
    {"ag_frame", (getter)gen_getframe, NULL, NULL},
    {NULL} /* Sentinel */
};

static PyMemberDef async_gen_memberlist[] = {
    {"ag_running", T_BOOL,   offsetof(PyAsyncGenObject, ag_running_async),
        READONLY},
    {"ag_code",    T_OBJECT, offsetof(PyAsyncGenObject, ag_code),    READONLY},
    {NULL}      /* Sentinel */
};

PyDoc_STRVAR(async_aclose_doc,
"aclose() -> raise GeneratorExit inside generator.");

PyDoc_STRVAR(async_asend_doc,
"asend(v) -> send 'v' in generator.");

PyDoc_STRVAR(async_athrow_doc,
"athrow(typ[,val[,tb]]) -> raise exception in generator.");

static PyMethodDef async_gen_methods[] = {
    {"asend", (PyCFunction)async_gen_asend, METH_O, async_asend_doc},
    {"athrow",(PyCFunction)async_gen_athrow, METH_VARARGS, async_athrow_doc},
    {"aclose", (PyCFunction)async_gen_aclose, METH_NOARGS, async_aclose_doc},
    {NULL, NULL}        /* Sentinel */
};


static PyAsyncMethods async_gen_as_async = {
    0,                                          /* am_await */
    PyObject_SelfIter,                          /* am_aiter */
    (unaryfunc)async_gen_anext                  /* am_anext */
};

PyTypeObject PyAsyncGen_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "async_generator",                          /* tp_name */
    sizeof(PyAsyncGenObject),                   /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)gen_dealloc,                    /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    &async_gen_as_async,                        /* tp_as_async */
    (reprfunc)async_gen_repr,                   /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)async_gen_traverse,           /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    offsetof(PyAsyncGenObject, ag_weakreflist), /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    async_gen_methods,                          /* tp_methods */
    async_gen_memberlist,                       /* tp_members */
    async_gen_getsetlist,                       /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
    0,                                          /* tp_free */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    0,                                          /* tp_del */
    0,                                          /* tp_version_tag */
    _PyGen_Finalize,                            /* tp_finalize */
};

static PyObject* async_gen_init(PyAsyncGenObject* o) {
    if (o == NULL) {
        return NULL;
    }
    o->ag_finalizer = NULL;
    o->ag_closed = 0;
    o->ag_hooks_inited = 0;
    o->ag_running_async = 0;
    return (PyObject*)o;
}

PyAPI_FUNC(PyObject *) _PyAsyncGen_NewNoFrame(PyCodeObject *code) {
    PyAsyncGenObject *o;
    o = (PyAsyncGenObject *)gen_new_with_qualname(
        gen_alloc(&PyAsyncGen_Type, FREE_LIST_ASYNC_GEN),
        NULL,
        code,
        code->co_name,
        // TODO(jbower) use qualname when this is available
        NULL);
    return async_gen_init(o);
}

PyObject *
PyAsyncGen_New(PyFrameObject *f, PyObject *name, PyObject *qualname)
{
    PyAsyncGenObject *o;
    o = (PyAsyncGenObject *)gen_new_with_qualname(
        gen_alloc(&PyAsyncGen_Type, FREE_LIST_ASYNC_GEN),
        f,
        f->f_code,
        name,
        qualname);
    return async_gen_init(o);
}

int
PyAsyncGen_ClearFreeLists(void)
{
    int ret = ag_value_freelist_free + ag_asend_freelist_free;

    while (ag_value_freelist_free) {
        _PyAsyncGenWrappedValue *o;
        o = ag_value_freelist[--ag_value_freelist_free];
        assert(_PyAsyncGenWrappedValue_CheckExact(o));
        PyObject_GC_Del(o);
    }

    while (ag_asend_freelist_free) {
        PyAsyncGenASend *o;
        o = ag_asend_freelist[--ag_asend_freelist_free];
        assert(Py_TYPE(o) == &_PyAsyncGenASend_Type);
        PyObject_GC_Del(o);
    }

    return ret;
}

void
PyAsyncGen_Fini(void)
{
    PyAsyncGen_ClearFreeLists();
}


static PyObject *
async_gen_unwrap_value(PyAsyncGenObject *gen, PyObject *result)
{
    if (result == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetNone(PyExc_StopAsyncIteration);
        }

        if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)
            || PyErr_ExceptionMatches(PyExc_GeneratorExit)
        ) {
            gen->ag_closed = 1;
        }

        gen->ag_running_async = 0;
        return NULL;
    }

    if (_PyAsyncGenWrappedValue_CheckExact(result)) {
        /* async yield */
        _PyGen_SetStopIterationValue(((_PyAsyncGenWrappedValue*)result)->agw_val);
        Py_DECREF(result);
        gen->ag_running_async = 0;
        return NULL;
    }

    return result;
}


/* ---------- Async Generator ASend Awaitable ------------ */


static void
async_gen_asend_dealloc(PyAsyncGenASend *o)
{
    _PyObject_GC_UNTRACK((PyObject *)o);
    Py_CLEAR(o->ags_gen);
    Py_CLEAR(o->ags_sendval);
    if (ag_asend_freelist_free < _PyAsyncGen_MAXFREELIST) {
        assert(PyAsyncGenASend_CheckExact(o));
        ag_asend_freelist[ag_asend_freelist_free++] = o;
    } else {
        PyObject_GC_Del(o);
    }
}

static int
async_gen_asend_traverse(PyAsyncGenASend *o, visitproc visit, void *arg)
{
    Py_VISIT(o->ags_gen);
    Py_VISIT(o->ags_sendval);
    return 0;
}


static PyObject *
async_gen_asend_send(PyAsyncGenASend *o, PyObject *arg)
{
    PyObject *result;

    if (o->ags_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited __anext__()/asend()");
        return NULL;
    }

    if (o->ags_state == AWAITABLE_STATE_INIT) {
        if (o->ags_gen->ag_running_async) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "anext(): asynchronous generator is already running");
            return NULL;
        }

        if (arg == NULL || arg == Py_None) {
            arg = o->ags_sendval;
        }
        o->ags_state = AWAITABLE_STATE_ITER;
    }

    o->ags_gen->ag_running_async = 1;
    result = gen_send_ex((PyGenObject*)o->ags_gen, arg, 0, 0);
    result = async_gen_unwrap_value(o->ags_gen, result);

    if (result == NULL) {
        o->ags_state = AWAITABLE_STATE_CLOSED;
    }

    return result;
}


static PyObject *
async_gen_asend_iternext(PyAsyncGenASend *o)
{
    return async_gen_asend_send(o, NULL);
}


static PyObject *
async_gen_asend_throw(PyAsyncGenASend *o, PyObject *args)
{
    PyObject *result;

    if (o->ags_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited __anext__()/asend()");
        return NULL;
    }

    result = gen_throw((PyGenObject*)o->ags_gen, args);
    result = async_gen_unwrap_value(o->ags_gen, result);

    if (result == NULL) {
        o->ags_state = AWAITABLE_STATE_CLOSED;
    }

    return result;
}


static PyObject *
async_gen_asend_close(PyAsyncGenASend *o, PyObject *args)
{
    o->ags_state = AWAITABLE_STATE_CLOSED;
    Py_RETURN_NONE;
}


static PyMethodDef async_gen_asend_methods[] = {
    {"send", (PyCFunction)async_gen_asend_send, METH_O, send_doc},
    {"throw", (PyCFunction)async_gen_asend_throw, METH_VARARGS, throw_doc},
    {"close", (PyCFunction)async_gen_asend_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};


static PyAsyncMethods async_gen_asend_as_async = {
    PyObject_SelfIter,                          /* am_await */
    0,                                          /* am_aiter */
    0                                           /* am_anext */
};


PyTypeObject _PyAsyncGenASend_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "async_generator_asend",                    /* tp_name */
    sizeof(PyAsyncGenASend),                    /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)async_gen_asend_dealloc,        /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    &async_gen_asend_as_async,                  /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)async_gen_asend_traverse,     /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)async_gen_asend_iternext,     /* tp_iternext */
    async_gen_asend_methods,                    /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
};


static PyObject *
async_gen_asend_new(PyAsyncGenObject *gen, PyObject *sendval)
{
    PyAsyncGenASend *o;
    if (ag_asend_freelist_free) {
        ag_asend_freelist_free--;
        o = ag_asend_freelist[ag_asend_freelist_free];
        _Py_NewReference((PyObject *)o);
    } else {
        o = PyObject_GC_New(PyAsyncGenASend, &_PyAsyncGenASend_Type);
        if (o == NULL) {
            return NULL;
        }
    }

    Py_INCREF(gen);
    o->ags_gen = gen;

    Py_XINCREF(sendval);
    o->ags_sendval = sendval;

    o->ags_state = AWAITABLE_STATE_INIT;

    _PyObject_GC_TRACK((PyObject*)o);
    return (PyObject*)o;
}


/* ---------- Async Generator Value Wrapper ------------ */


static void
async_gen_wrapped_val_dealloc(_PyAsyncGenWrappedValue *o)
{
    _PyObject_GC_UNTRACK((PyObject *)o);
    Py_CLEAR(o->agw_val);
    if (ag_value_freelist_free < _PyAsyncGen_MAXFREELIST) {
        assert(_PyAsyncGenWrappedValue_CheckExact(o));
        ag_value_freelist[ag_value_freelist_free++] = o;
    } else {
        PyObject_GC_Del(o);
    }
}


static int
async_gen_wrapped_val_traverse(_PyAsyncGenWrappedValue *o,
                               visitproc visit, void *arg)
{
    Py_VISIT(o->agw_val);
    return 0;
}


PyTypeObject _PyAsyncGenWrappedValue_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "async_generator_wrapped_value",            /* tp_name */
    sizeof(_PyAsyncGenWrappedValue),            /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)async_gen_wrapped_val_dealloc,  /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)async_gen_wrapped_val_traverse, /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
};


PyObject *
_PyAsyncGenValueWrapperNew(PyObject *val)
{
    _PyAsyncGenWrappedValue *o;
    assert(val);

    if (ag_value_freelist_free) {
        ag_value_freelist_free--;
        o = ag_value_freelist[ag_value_freelist_free];
        assert(_PyAsyncGenWrappedValue_CheckExact(o));
        _Py_NewReference((PyObject*)o);
    } else {
        o = PyObject_GC_New(_PyAsyncGenWrappedValue,
                            &_PyAsyncGenWrappedValue_Type);
        if (o == NULL) {
            return NULL;
        }
    }
    o->agw_val = val;
    Py_INCREF(val);
    _PyObject_GC_TRACK((PyObject*)o);
    return (PyObject*)o;
}


/* ---------- Async Generator AThrow awaitable ------------ */


static void
async_gen_athrow_dealloc(PyAsyncGenAThrow *o)
{
    _PyObject_GC_UNTRACK((PyObject *)o);
    Py_CLEAR(o->agt_gen);
    Py_CLEAR(o->agt_args);
    PyObject_GC_Del(o);
}


static int
async_gen_athrow_traverse(PyAsyncGenAThrow *o, visitproc visit, void *arg)
{
    Py_VISIT(o->agt_gen);
    Py_VISIT(o->agt_args);
    return 0;
}


static PyObject *
async_gen_athrow_send(PyAsyncGenAThrow *o, PyObject *arg)
{
    PyGenObject *gen = (PyGenObject*)o->agt_gen;
    PyObject *retval;

    if (o->agt_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited aclose()/athrow()");
        return NULL;
    }

    if (gen_is_completed(gen)) {
        o->agt_state = AWAITABLE_STATE_CLOSED;
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }

    if (o->agt_state == AWAITABLE_STATE_INIT) {
        if (o->agt_gen->ag_running_async) {
            o->agt_state = AWAITABLE_STATE_CLOSED;
            if (o->agt_args == NULL) {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "aclose(): asynchronous generator is already running");
            }
            else {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "athrow(): asynchronous generator is already running");
            }
            return NULL;
        }

        if (o->agt_gen->ag_closed) {
            o->agt_state = AWAITABLE_STATE_CLOSED;
            PyErr_SetNone(PyExc_StopAsyncIteration);
            return NULL;
        }

        if (arg != Py_None) {
            PyErr_SetString(PyExc_RuntimeError, NON_INIT_CORO_MSG);
            return NULL;
        }

        o->agt_state = AWAITABLE_STATE_ITER;
        o->agt_gen->ag_running_async = 1;

        if (o->agt_args == NULL) {
            /* aclose() mode */
            o->agt_gen->ag_closed = 1;

            retval = _gen_throw((PyGenObject *)gen,
                                0,  /* Do not close generator when
                                       PyExc_GeneratorExit is passed */
                                PyExc_GeneratorExit, NULL, NULL);

            if (retval && _PyAsyncGenWrappedValue_CheckExact(retval)) {
                Py_DECREF(retval);
                goto yield_close;
            }
        } else {
            PyObject *typ;
            PyObject *tb = NULL;
            PyObject *val = NULL;

            if (!PyArg_UnpackTuple(o->agt_args, "athrow", 1, 3,
                                   &typ, &val, &tb)) {
                return NULL;
            }

            retval = _gen_throw((PyGenObject *)gen,
                                0,  /* Do not close generator when
                                       PyExc_GeneratorExit is passed */
                                typ, val, tb);
            retval = async_gen_unwrap_value(o->agt_gen, retval);
        }
        if (retval == NULL) {
            goto check_error;
        }
        return retval;
    }

    assert(o->agt_state == AWAITABLE_STATE_ITER);

    retval = gen_send_ex((PyGenObject *)gen, arg, 0, 0);
    if (o->agt_args) {
        return async_gen_unwrap_value(o->agt_gen, retval);
    } else {
        /* aclose() mode */
        if (retval) {
            if (_PyAsyncGenWrappedValue_CheckExact(retval)) {
                Py_DECREF(retval);
                goto yield_close;
            }
            else {
                return retval;
            }
        }
        else {
            goto check_error;
        }
    }

yield_close:
    o->agt_gen->ag_running_async = 0;
    o->agt_state = AWAITABLE_STATE_CLOSED;
    PyErr_SetString(
        PyExc_RuntimeError, ASYNC_GEN_IGNORED_EXIT_MSG);
    return NULL;

check_error:
    o->agt_gen->ag_running_async = 0;
    o->agt_state = AWAITABLE_STATE_CLOSED;
    if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration) ||
            PyErr_ExceptionMatches(PyExc_GeneratorExit))
    {
        if (o->agt_args == NULL) {
            /* when aclose() is called we don't want to propagate
               StopAsyncIteration or GeneratorExit; just raise
               StopIteration, signalling that this 'aclose()' await
               is done.
            */
            PyErr_Clear();
            PyErr_SetNone(PyExc_StopIteration);
        }
    }
    return NULL;
}


static PyObject *
async_gen_athrow_throw(PyAsyncGenAThrow *o, PyObject *args)
{
    PyObject *retval;

    if (o->agt_state == AWAITABLE_STATE_CLOSED) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot reuse already awaited aclose()/athrow()");
        return NULL;
    }

    retval = gen_throw((PyGenObject*)o->agt_gen, args);
    if (o->agt_args) {
        return async_gen_unwrap_value(o->agt_gen, retval);
    } else {
        /* aclose() mode */
        if (retval && _PyAsyncGenWrappedValue_CheckExact(retval)) {
            o->agt_gen->ag_running_async = 0;
            o->agt_state = AWAITABLE_STATE_CLOSED;
            Py_DECREF(retval);
            PyErr_SetString(PyExc_RuntimeError, ASYNC_GEN_IGNORED_EXIT_MSG);
            return NULL;
        }
        if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration) ||
            PyErr_ExceptionMatches(PyExc_GeneratorExit))
        {
            /* when aclose() is called we don't want to propagate
               StopAsyncIteration or GeneratorExit; just raise
               StopIteration, signalling that this 'aclose()' await
               is done.
            */
            PyErr_Clear();
            PyErr_SetNone(PyExc_StopIteration);
        }
        return retval;
    }
}


static PyObject *
async_gen_athrow_iternext(PyAsyncGenAThrow *o)
{
    return async_gen_athrow_send(o, Py_None);
}


static PyObject *
async_gen_athrow_close(PyAsyncGenAThrow *o, PyObject *args)
{
    o->agt_state = AWAITABLE_STATE_CLOSED;
    Py_RETURN_NONE;
}


static PyMethodDef async_gen_athrow_methods[] = {
    {"send", (PyCFunction)async_gen_athrow_send, METH_O, send_doc},
    {"throw", (PyCFunction)async_gen_athrow_throw, METH_VARARGS, throw_doc},
    {"close", (PyCFunction)async_gen_athrow_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};


static PyAsyncMethods async_gen_athrow_as_async = {
    PyObject_SelfIter,                          /* am_await */
    0,                                          /* am_aiter */
    0                                           /* am_anext */
};


PyTypeObject _PyAsyncGenAThrow_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "async_generator_athrow",                   /* tp_name */
    sizeof(PyAsyncGenAThrow),                   /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)async_gen_athrow_dealloc,       /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    &async_gen_athrow_as_async,                 /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)async_gen_athrow_traverse,    /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)async_gen_athrow_iternext,    /* tp_iternext */
    async_gen_athrow_methods,                   /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
};


static PyObject *
async_gen_athrow_new(PyAsyncGenObject *gen, PyObject *args)
{
    PyAsyncGenAThrow *o;
    o = PyObject_GC_New(PyAsyncGenAThrow, &_PyAsyncGenAThrow_Type);
    if (o == NULL) {
        return NULL;
    }
    o->agt_gen = gen;
    o->agt_args = args;
    o->agt_state = AWAITABLE_STATE_INIT;
    Py_INCREF(gen);
    Py_XINCREF(args);
    _PyObject_GC_TRACK((PyObject*)o);
    return (PyObject*)o;
}

int
_PyGen_ClearFreeList(void)
{
    int freelist_size = 0;

    for (int i = 0; i < FREE_LIST_MAX; i++) {
        gen_free_list *free_list = &freelists[i];

        freelist_size += free_list->numfree;
        while (free_list->free_list != NULL) {
            PyGenObject *gen = free_list->free_list;
            free_list->free_list = (PyGenObject *)gen->gi_code;
            PyObject_GC_Del(gen);
            --free_list->numfree;
        }
    }
    return freelist_size;
}
