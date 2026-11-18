# vmem

Single-header reserve-only `mmap` + linear allocator. POSIX, no dependencies, nothing to link — every function is `static inline`.

Reserve, don't commit: `vmem_reserve` only reserves address space; the OS commits a page when it is first touched, so a large reservation costs no RAM until pages are actually used. Pages are handed back with `vmem_release` (`madvise(MADV_FREE)`) and the range stays usable.

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
    linear_release(&a); // committed pages back to the OS, range stays usable
    linear_free(&a);    // munmap

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
| `void* vmem_reserve(size_t size)` | `mmap` reservation only; `NULL` on failure |
| `void vmem_release(void* ptr, size_t size)` | `madvise(MADV_FREE)`: pages back to the OS, content lost, range still usable |
| `void vmem_free(void* ptr, size_t size)` | `munmap` |
| `bool linear_init(struct linear_allocator* a, size_t size)` | initialize over a `vmem_reserve` of `size` |
| `void* linear_alloc(struct linear_allocator* a, size_t size)` | 16-byte aligned (malloc's); `NULL` when full |
| `void linear_reset(struct linear_allocator* a)` | `used = 0` |
| `void linear_release(struct linear_allocator* a)` | `vmem_release` of `[0, peak)`, then `used = peak = 0` |
| `void linear_free(struct linear_allocator* a)` | `vmem_free`, zeroes the struct |

## Notes

- The allocator's RSS tracks pages you actually touch, not how much you reserve.
- `linear_release` only returns the committed prefix up to `peak`; untouched space was never charged.
- Uses `mmap`/`madvise` — macOS and Linux.
