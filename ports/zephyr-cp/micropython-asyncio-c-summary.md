# MicroPython/CircuitPython async internals summary (for C awaitables)

## Where async support lives

- **Language/runtime support** is in `py/`:
  - `../../py/compile.c`
  - `../../py/emitglue.c`
  - `../../py/objgenerator.c`
  - `../../py/vm.c`
  - `../../py/runtime.c`
- **Scheduler/task layer** is `_asyncio` plus frozen Python `asyncio`:
  - `../../extmod/modasyncio.c`
  - `../../frozen/Adafruit_CircuitPython_asyncio/asyncio/core.py`

## How `async def` and `await` work in this tree

1. `async def` functions are compiled with both generator + async flags (`MP_SCOPE_FLAG_GENERATOR | MP_SCOPE_FLAG_ASYNC`) in `compile.c`.
2. Function objects become coroutine wrappers (`mp_type_coro_wrap` / `mp_type_native_coro_wrap`) in `emitglue.c`.
3. Calling an async function returns a coroutine instance (`mp_type_coro_instance`) implemented in `objgenerator.c`.
4. **CircuitPython behavior**: `await x` calls `x.__await__()` first (CPython-style), then drives the result via `YIELD_FROM`.
5. VM opcode `YIELD_FROM` uses `mp_resume(...)`, which resumes awaitables via:
   - generator/coroutine fast path,
   - iterator path (`iternext`) when sending `None`,
   - or `send`/`throw` methods.

## What a C-returned awaitable must provide

For a native C method that returns an object usable in `await`:

- Must expose `__await__` (required by this fork’s `await` lowering).
- `__await__` should return an object resumable by `mp_resume` (commonly `self`).
- Implement iterator/coroutine stepping behavior:
  - `iternext` (or `__next__`) at minimum,
  - `send` and `throw` recommended for compatibility/cancellation.

## Practical implementation pattern

- Define a custom type with `MP_TYPE_FLAG_ITER_IS_ITERNEXT` (or custom iter/getiter form).
- Set `iter` slot to your step function.
- Add methods in locals dict: `__await__`, `send`, `throw` (and optionally `close`).
- Have your C API method allocate + return this object.

## Important note

You generally should **not** manually fabricate `mp_type_coro_instance` objects directly from C for user APIs. Instead, return a dedicated awaitable object that follows the protocol above.

## Asyncio integration tip

If your awaitable must suspend until an external event (IRQ/IO/etc.), model wake/sleep behavior after `_asyncio.Task` in `../../extmod/modasyncio.c` instead of busy-yielding.
