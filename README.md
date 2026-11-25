# vmem

Single-header reserve-only `mmap` + linear + stack allocators. POSIX, no dependencies, nothing to link — every function is `static inline`.

Reserve, don't commit: `vmem_reserve` only reserves address space; the OS commits a page when it is first touched, so a large reservation costs no RAM until pages are actually used. Every arena is fenced with a `PROT_NONE` guard page at each end, so running off either end faults immediately (the guards take address space only, no RAM). Pages are handed back with `vmem_release`, which remaps the range: the pages are dropped right away, the range reads back as zeros, and the range stays usable.

## Usage

Drop `vmem.h` next to your code:

```c
#include "vmem.h"
```

There is no `.c` file to compile and nothing to link.

## Example

```c
#include <stdio.h>
#include "vmem.h"

int main(void)
{
    struct linear_allocator a;
    if (!linear_init(&a, 64 * 1024 * 1024)) // reserve 64 MB of address space, commits 0 MB
        return 1;

    char* buf = linear_alloc(&a, 1024 * 1024); // 1 MB, pages commit as you touch them
    if (buf) {
        buf[0] = 'x'; // first touch commits the first page
        printf("%c, peak %zu bytes\n", buf[0], a.peak);
    }

    linear_reset(&a);   // used = 0, same address space
    linear_release(&a); // committed pages dropped, range reads back as zeros, stays usable
    linear_free(&a);    // munmap, guards included

    return 0;
}
```

```sh
cc -O2 -o demo demo.c
./demo   # prints: x, peak 1048576 bytes
```

## API

| Function | Description |
| --- | --- |
| `void* vmem_reserve(size_t size)` | reserves `size` plus one guard page at each end (`PROT_NONE`); returns the address just past the leading guard; `NULL` on failure |
| `bool vmem_release(void* ptr, size_t size)` | remaps `[ptr, ptr + size)` as fresh zero pages (`MAP_FIXED`): pages dropped immediately, range reads back as zeros, stays usable; `false` on failure |
| `void vmem_free(void* ptr, size_t size)` | `munmap` of the whole mapping, guards included |
| `bool linear_init(struct linear_allocator* a, size_t size)` | initialize over a `vmem_reserve` of `size` |
| `void* linear_alloc(struct linear_allocator* a, size_t size)` | 16-byte aligned (malloc's); `NULL` for `size 0`, overflowing sizes, or when full |
| `void linear_reset(struct linear_allocator* a)` | `used = 0` |
| `void linear_release(struct linear_allocator* a)` | `vmem_release` of `[0, peak)`, then `used = peak = 0` |
| `void linear_free(struct linear_allocator* a)` | `vmem_free`, zeroes the struct |
| `bool stack_init(struct stack_allocator* s, size_t size)` | initialize over a `vmem_reserve` of `size` |
| `void* stack_alloc(struct stack_allocator* s, size_t size)` | 16-byte aligned; 16 B of trailer metadata per block; checks the current top's trailer first; `NULL` for `size 0`, overflowing sizes, when full or corrupt |
| `void* stack_pop(struct stack_allocator* s, void* p)` | `p` must be the pointer `stack_alloc` returned for the top block; returns `p` and rewinds past it; `NULL` when empty or corrupt |
| `void* stack_top(struct stack_allocator* s)` | payload pointer of the current top; `NULL` when empty or corrupt |
| `size_t stack_mark(struct stack_allocator* s)` | returns `used`: a mark for `stack_rewind` |
| `void stack_rewind(struct stack_allocator* s, size_t m)` | rewinds `used` to mark `m`; `m` must be `<= used` and a real block boundary |
| `bool stack_check(struct stack_allocator* s)` | O(n) walk validating every trailer; `false` on the first bad one |
| `void stack_reset(struct stack_allocator* s)` | `used = 0`, clears corrupt |
| `void stack_release(struct stack_allocator* s)` | `vmem_release` of `[0, peak)`, then `used = peak = 0`, clears corrupt |
| `void stack_free(struct stack_allocator* s)` | `vmem_free`, zeroes the struct |

Each block is the 16-byte-aligned payload plus a 16-byte trailer at its end (a `uint64_t` canary followed by `prev`, the `used` offset from before the block). Every block ends with its trailer, so the top trailer is always at `used - 16`: `stack_pop` reads it directly and rewinds to `prev`.

The canary sits **after the payload rounded up to 16 bytes**, so an overrun of up to 15 bytes past the requested size lands in the padding and is not detected by the canary. (Under ASan that padding is poisoned, so the overrun is reported there instead.) When a bad trailer is read — at pop, top, the next alloc, rewind or check — the stack is marked corrupt and `VMEM_ON_CORRUPT(s)` is called; by default that prints to stderr and traps. Define `VMEM_ON_CORRUPT(s)` before including the header to replace it, for example to log or to keep the old "return NULL" behaviour. Once corrupt, every operation re-reports via the hook and returns `NULL`/`false` rather than trusting the clobbered state.

## Notes

- Not thread-safe.
- The blocks both allocators hand out are 16-byte aligned; `vmem_release`'d ranges read back as zeros.
- Under AddressSanitizer, reserved-but-unallocated space, the padding after each requested size, the trailers and freed ranges are poisoned, so out-of-bounds and use-after-free accesses are reported at the faulting instruction.
- `linear_release`/`stack_release` return only the committed prefix up to `peak`; untouched space was never charged.
- Uses `mmap`/`mprotect` — macOS and Linux (macOS is the tested platform).

## Tests

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/unit_tests
```

The tests fork a child for every case that is expected to crash (guard pages, the corruption trap, ASan reports) and verify the expected signal, so a faulting code path never kills the runner.
