/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * SPDX-FileCopyrightText: Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2014-2017 Paul Sokolovsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <assert.h>

#include "py/runtime.h"
#include "py/bc.h"
#include "py/objgenerator.h"
#include "py/objfun.h"
#include "py/stackctrl.h"

#include "supervisor/shared/translate.h"

/******************************************************************************/
/* generator wrapper                                                          */

typedef struct _mp_obj_gen_wrap_t {
    mp_obj_base_t base;
    mp_obj_t *fun;
    bool coroutine_generator;
} mp_obj_gen_wrap_t;

typedef struct _mp_obj_gen_instance_t {
    mp_obj_base_t base;
    mp_obj_dict_t *globals;
    bool coroutine_generator;
    mp_code_state_t code_state;
} mp_obj_gen_instance_t;

/******************************************************************************/
// native generator wrapper

#if MICROPY_EMIT_NATIVE

STATIC mp_obj_t native_gen_wrap_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_obj_gen_wrap_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_fun_bc_t *self_fun = (mp_obj_fun_bc_t *)self->fun;

    // Determine start of prelude, and extract n_state from it
    uintptr_t prelude_offset = ((uintptr_t *)self_fun->bytecode)[0];
    size_t n_state = mp_decode_uint_value(self_fun->bytecode + prelude_offset);
    size_t n_exc_stack = 0;

    // Allocate the generator object, with room for local stack and exception stack
    mp_obj_gen_instance_t *o = m_new_obj_var(mp_obj_gen_instance_t, byte,
        n_state * sizeof(mp_obj_t) + n_exc_stack * sizeof(mp_exc_stack_t));
    o->base.type = &mp_type_gen_instance;

    // Parse the input arguments and set up the code state
    o->coroutine_generator = self->coroutine_generator;
    o->globals = self_fun->globals;
    o->code_state.fun_bc = self_fun;
    o->code_state.ip = (const byte *)prelude_offset;
    mp_setup_code_state(&o->code_state, n_args, n_kw, args);

    // Indicate we are a native function, which doesn't use this variable
    o->code_state.exc_sp = NULL;

    // Prepare the generator instance for execution
    uintptr_t start_offset = ((uintptr_t *)self_fun->bytecode)[1];
    o->code_state.ip = MICROPY_MAKE_POINTER_CALLABLE((void *)(self_fun->bytecode + start_offset));

    return MP_OBJ_FROM_PTR(o);
}

#endif // MICROPY_EMIT_NATIVE

STATIC mp_obj_t bc_gen_wrap_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_obj_gen_wrap_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_fun_bc_t *self_fun = (mp_obj_fun_bc_t *)self->fun;
    assert(self_fun->base.type == &mp_type_fun_bc);

    // bytecode prelude: get state size and exception stack size
    size_t n_state = mp_decode_uint_value(self_fun->bytecode);
    size_t n_exc_stack = mp_decode_uint_value(mp_decode_uint_skip(self_fun->bytecode));

    // allocate the generator object, with room for local stack and exception stack
    mp_obj_gen_instance_t *o = m_new_obj_var(mp_obj_gen_instance_t, byte,
        n_state * sizeof(mp_obj_t) + n_exc_stack * sizeof(mp_exc_stack_t));
    o->base.type = &mp_type_gen_instance;

    o->coroutine_generator = self->coroutine_generator;
    o->globals = self_fun->globals;
    o->code_state.fun_bc = self_fun;
    o->code_state.ip = 0;
    mp_setup_code_state(&o->code_state, n_args, n_kw, args);
    return MP_OBJ_FROM_PTR(o);
}

STATIC mp_obj_t gen_wrap_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_obj_gen_wrap_t *self = MP_OBJ_TO_PTR(self_in);

    #if MICROPY_EMIT_NATIVE
    mp_obj_fun_bc_t *self_fun = (mp_obj_fun_bc_t *)self->fun;
    if (self_fun->base.type == &mp_type_fun_native) {
        return native_gen_wrap_call(self, n_args, n_kw, args);
    }
    #endif
    return bc_gen_wrap_call(self, n_args, n_kw, args);
}

#if MICROPY_PY_FUNCTION_ATTRS
static void gen_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_gen_wrap_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_fun_bc_t *self_fun = (mp_obj_fun_bc_t *)self->fun;
    mp_obj_fun_bc_attr(self_fun, attr, dest);
}
#endif

const mp_obj_type_t mp_type_gen_wrap = {
    { &mp_type_type },
    .name = MP_QSTR_generator,
    .call = gen_wrap_call,
    .unary_op = mp_generic_unary_op,
    #if MICROPY_PY_FUNCTION_ATTRS
    .attr = gen_attr,
    #endif
};


mp_obj_t mp_obj_new_gen_wrap(mp_obj_t fun, bool is_coroutine) {
    mp_obj_gen_wrap_t *o = m_new_obj(mp_obj_gen_wrap_t);
    o->base.type = &mp_type_gen_wrap;
    o->fun = MP_OBJ_TO_PTR(fun);
    o->coroutine_generator = is_coroutine;
    return MP_OBJ_FROM_PTR(o);
}

/******************************************************************************/
/* generator instance                                                         */

STATIC void gen_instance_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mp_obj_gen_instance_t *self = MP_OBJ_TO_PTR(self_in);
    #if MICROPY_PY_ASYNC_AWAIT
    if (self->coroutine_generator) {
        mp_printf(print, "<coroutine object '%q' at %p>", mp_obj_fun_get_name(MP_OBJ_FROM_PTR(self->code_state.fun_bc)), self);
        return;
    }
    #endif
    mp_printf(print, "<generator object '%q' at %p>", mp_obj_fun_get_name(MP_OBJ_FROM_PTR(self->code_state.fun_bc)), self);
}

mp_vm_return_kind_t mp_obj_gen_resume(mp_obj_t self_in, mp_obj_t send_value, mp_obj_t throw_value, mp_obj_t *ret_val) {
    MP_STACK_CHECK();
    mp_check_self(MP_OBJ_IS_TYPE(self_in, &mp_type_gen_instance));
    mp_obj_gen_instance_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->code_state.ip == 0) {
        // Trying to resume already stopped generator
        *ret_val = MP_OBJ_STOP_ITERATION;
        return MP_VM_RETURN_NORMAL;
    }
    if (self->code_state.sp == self->code_state.state - 1) {
        if (send_value != mp_const_none) {
            mp_raise_TypeError(translate("can't send non-None value to a just-started generator"));
        }
    } else {
        #if MICROPY_PY_GENERATOR_PEND_THROW
        // If exception is pending (set using .pend_throw()), process it now.
        if (*self->code_state.sp != mp_const_none) {
            throw_value = *self->code_state.sp;
            *self->code_state.sp = MP_OBJ_NULL;
        } else
        #endif
        {
            *self->code_state.sp = send_value;
        }
    }

    // We set self->globals=NULL while executing, for a sentinel to ensure the generator
    // cannot be reentered during execution
    if (self->globals == NULL) {
        mp_raise_ValueError(translate("generator already executing"));
    }

    // Set up the correct globals context for the generator and execute it
    self->code_state.old_globals = mp_globals_get();
    mp_globals_set(self->globals);
    self->globals = NULL;

    mp_vm_return_kind_t ret_kind;

    #if MICROPY_EMIT_NATIVE
    if (self->code_state.exc_sp == NULL) {
        // A native generator, with entry point 2 words into the "bytecode" pointer
        typedef uintptr_t (*mp_fun_native_gen_t)(void *, mp_obj_t);
        mp_fun_native_gen_t fun = MICROPY_MAKE_POINTER_CALLABLE((const void *)(self->code_state.fun_bc->bytecode + 2 * sizeof(uintptr_t)));
        ret_kind = fun((void *)&self->code_state, throw_value);
    } else
    #endif
    {
        // A bytecode generator
        ret_kind = mp_execute_bytecode(&self->code_state, throw_value);
    }

    self->globals = mp_globals_get();
    mp_globals_set(self->code_state.old_globals);

    switch (ret_kind) {
        case MP_VM_RETURN_NORMAL:
        default:
            // Explicitly mark generator as completed. If we don't do this,
            // subsequent next() may re-execute statements after last yield
            // again and again, leading to side effects.
            self->code_state.ip = 0;
            *ret_val = *self->code_state.sp;
            break;

        case MP_VM_RETURN_YIELD:
            *ret_val = *self->code_state.sp;
            #if MICROPY_PY_GENERATOR_PEND_THROW
            *self->code_state.sp = mp_const_none;
            #endif
            break;

        case MP_VM_RETURN_EXCEPTION: {
            self->code_state.ip = 0;
            *ret_val = self->code_state.state[0];
            // PEP479: if StopIteration is raised inside a generator it is replaced with RuntimeError
            if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(*ret_val)), MP_OBJ_FROM_PTR(&mp_type_StopIteration))) {
                *ret_val = mp_obj_new_exception_msg(&mp_type_RuntimeError, translate("generator raised StopIteration"));
            }
            break;
        }
    }

    return ret_kind;
}

STATIC mp_obj_t gen_resume_and_raise(mp_obj_t self_in, mp_obj_t send_value, mp_obj_t throw_value) {
    mp_obj_t ret;
    switch (mp_obj_gen_resume(self_in, send_value, throw_value, &ret)) {
        case MP_VM_RETURN_NORMAL:
        default:
            // Optimize return w/o value in case generator is used in for loop
            if (ret == mp_const_none || ret == MP_OBJ_STOP_ITERATION) {
                return MP_OBJ_STOP_ITERATION;
            } else {
                nlr_raise(mp_obj_new_exception_args(&mp_type_StopIteration, 1, &ret));
            }

        case MP_VM_RETURN_YIELD:
            return ret;

        case MP_VM_RETURN_EXCEPTION:
            nlr_raise(ret);
    }
}

STATIC mp_obj_t gen_instance_iternext(mp_obj_t self_in) {
    #if MICROPY_PY_ASYNC_AWAIT
    // This translate is literally too much for m0 boards
    mp_obj_gen_instance_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->coroutine_generator) {
        mp_raise_TypeError(translate("'coroutine' object is not an iterator"));
    }
    #endif
    return gen_resume_and_raise(self_in, mp_const_none, MP_OBJ_NULL);
}

STATIC mp_obj_t gen_instance_send(mp_obj_t self_in, mp_obj_t send_value) {
    mp_obj_t ret = gen_resume_and_raise(self_in, send_value, MP_OBJ_NULL);
    if (ret == MP_OBJ_STOP_ITERATION) {
        nlr_raise(mp_obj_new_exception(&mp_type_StopIteration));
    } else {
        return ret;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(gen_instance_send_obj, gen_instance_send);

#if MICROPY_PY_ASYNC_AWAIT
STATIC mp_obj_t gen_instance_await(mp_obj_t self_in) {
    mp_obj_gen_instance_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->coroutine_generator) {
        // Pretend like a generator does not have this coroutine behavior.
        // Pay no attention to the dir() behind the curtain
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_AttributeError,
            translate("type object 'generator' has no attribute '__await__'")));
    }
    // You can directly call send on a coroutine generator or you can __await__ then send on the return of that.
    return self;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(gen_instance_await_obj, gen_instance_await);
#endif

STATIC mp_obj_t gen_instance_close(mp_obj_t self_in);
STATIC mp_obj_t gen_instance_throw(size_t n_args, const mp_obj_t *args) {
    mp_obj_t exc = (n_args == 2) ? args[1] : args[2];

    mp_obj_t ret = gen_resume_and_raise(args[0], mp_const_none, exc);
    if (ret == MP_OBJ_STOP_ITERATION) {
        nlr_raise(mp_obj_new_exception(&mp_type_StopIteration));
    } else {
        return ret;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gen_instance_throw_obj, 2, 4, gen_instance_throw);

STATIC mp_obj_t gen_instance_close(mp_obj_t self_in) {
    mp_obj_t ret;
    switch (mp_obj_gen_resume(self_in, mp_const_none, MP_OBJ_FROM_PTR(&mp_const_GeneratorExit_obj), &ret)) {
        case MP_VM_RETURN_YIELD:
            mp_raise_RuntimeError(translate("generator ignored GeneratorExit"));

        // Swallow GeneratorExit (== successful close), and re-raise any other
        case MP_VM_RETURN_EXCEPTION:
            // ret should always be an instance of an exception class
            if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(ret)), MP_OBJ_FROM_PTR(&mp_type_GeneratorExit))) {
                return mp_const_none;
            }
            nlr_raise(ret);

        default:
            // The only choice left is MP_VM_RETURN_NORMAL which is successful close
            return mp_const_none;
    }
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(gen_instance_close_obj, gen_instance_close);

STATIC mp_obj_t gen_instance_pend_throw(mp_obj_t self_in, mp_obj_t exc_in) {
    mp_obj_gen_instance_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->code_state.sp == self->code_state.state - 1) {
        mp_raise_TypeError(translate("can't pend throw to just-started generator"));
    }
    mp_obj_t prev = *self->code_state.sp;
    *self->code_state.sp = exc_in;
    return prev;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(gen_instance_pend_throw_obj, gen_instance_pend_throw);

STATIC const mp_rom_map_elem_t gen_instance_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&gen_instance_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&gen_instance_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_throw), MP_ROM_PTR(&gen_instance_throw_obj) },
    #if MICROPY_PY_GENERATOR_PEND_THROW
    { MP_ROM_QSTR(MP_QSTR_pend_throw), MP_ROM_PTR(&gen_instance_pend_throw_obj) },
    #endif
    #if MICROPY_PY_ASYNC_AWAIT
    { MP_ROM_QSTR(MP_QSTR___await__), MP_ROM_PTR(&gen_instance_await_obj) },
    #endif
};

STATIC MP_DEFINE_CONST_DICT(gen_instance_locals_dict, gen_instance_locals_dict_table);

const mp_obj_type_t mp_type_gen_instance = {
    { &mp_type_type },
    .name = MP_QSTR_generator,
    .print = gen_instance_print,
    .unary_op = mp_generic_unary_op,
    .getiter = mp_identity_getiter,
    .iternext = gen_instance_iternext,
    .locals_dict = (mp_obj_dict_t *)&gen_instance_locals_dict,
};
