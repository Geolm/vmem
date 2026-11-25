// vmem unit tests.
//
// Crash tests fork a child to run the faulting code and verify the expected signal (or a
// non-zero exit for an ASan report), so a faulting path never kills the runner.
//
// The corruption hook below counts calls instead of trapping; when vmem_trap_mode is set it
// calls __builtin_trap(), matching the default handler, so one file tests both behaviours.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

struct stack_allocator;   // forward declaration for the hook

static int vmem_hook_calls = 0;
static bool vmem_trap_mode = false;

static void vmem_test_hook(struct stack_allocator* s)
{
    (void)s;
    vmem_hook_calls++;
    if (vmem_trap_mode)
        __builtin_trap();
}
#define VMEM_ON_CORRUPT(s) vmem_test_hook(s)

#include "vmem.h"

#if (defined(__has_feature) && __has_feature(address_sanitizer)) || defined(__SANITIZE_ADDRESS__)
#define TEST_ASAN 1
#else
#define TEST_ASAN 0
#endif

static int failures = 0;

static void expect_true(bool cond, const char* what)
{
    if (cond)
    {
        printf("PASS %s\n", what);
    }
    else
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void expect_skip(const char* what)
{
    printf("SKIP %s (ASan build)\n", what);
}

// ---- crash harness -----------------------------------------------------------------------

typedef void (*crash_fn)(void* arg);

struct crash_expect
{
    int signals[4];   // acceptable WTERMSIG values, 0-terminated
    int nsignals;
    bool allow_nonzero_exit;
};

// Under ASan, a PROT_NONE fault is reported by ASan ("BUS on unknown address") and the
// process aborts, so SIGABRT is accepted there as well.
#if TEST_ASAN
static const struct crash_expect exp_guard = { {SIGBUS, SIGSEGV, SIGABRT}, 3, true };
#else
static const struct crash_expect exp_guard = { {SIGBUS, SIGSEGV, 0, 0}, 2, false };
#endif
static const struct crash_expect exp_trap = { {SIGTRAP, SIGILL, 0, 0}, 2, false };
static const struct crash_expect exp_asan = { {SIGABRT, SIGSEGV, SIGBUS, SIGILL}, 4, true };

// State inherited by forked children.
static struct linear_allocator g_lin;
static struct stack_allocator g_st;
static void* g_buf;
static size_t g_off;

static void run_child(crash_fn fn, const struct crash_expect* e, const char* name)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == -1)
    {
        failures++;
        printf("FAIL %s: fork failed\n", name);
        return;
    }
    if (pid == 0)
    {
        fn(NULL);
        _exit(0);   // reached without dying where a crash was expected
    }

    int status = 0;
    waitpid(pid, &status, 0);

    bool ok = false;
    if (WIFSIGNALED(status))
    {
        for (int i = 0; i < e->nsignals; i++)
        {
            if (WTERMSIG(status) == e->signals[i])
                ok = true;
        }
    }
    else if (WIFEXITED(status) && e->allow_nonzero_exit && WEXITSTATUS(status) != 0)
    {
        ok = true;
    }

    if (!ok)
    {
        failures++;
        if (WIFSIGNALED(status))
            printf("FAIL %s: child killed by signal %d\n", name, WTERMSIG(status));
        else
            printf("FAIL %s: child exited %d without crashing\n", name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return;
    }

    printf("PASS %s", name);
    if (WIFSIGNALED(status))
        printf(" (signal %d)", WTERMSIG(status));
    printf("\n");
}

static void child_guard_lead(void* arg)
{
    (void)arg;
    ((unsigned char*)g_lin.base)[-1] = 1;
}

static void child_guard_trail(void* arg)
{
    (void)arg;
    size_t page = (size_t)getpagesize();
    size_t rounded = (g_lin.size + page - 1) & ~(page - 1);
    ((unsigned char*)g_lin.base)[rounded] = 1;
}

static void child_release_guard(void* arg)
{
    (void)arg;
    vmem_release(g_lin.base, g_lin.size);
    ((unsigned char*)g_lin.base)[-1] = 1;
}

static void child_poison_write(void* arg)
{
    (void)arg;
    ((unsigned char*)g_buf)[g_off] = 1;
}

static void child_pop_then_read(void* arg)
{
    (void)arg;
    stack_pop(&g_st, g_buf);
    volatile unsigned char c = ((unsigned char*)g_buf)[g_off];
    (void)c;
}

static void child_reset_then_read(void* arg)
{
    (void)arg;
    linear_reset(&g_lin);
    volatile unsigned char c = ((unsigned char*)g_buf)[g_off];
    (void)c;
}

static void child_reserved_read(void* arg)
{
    (void)arg;
    volatile unsigned char c = g_lin.base[0];
    (void)c;
}

static void child_corrupt_pop(void* arg)
{
    (void)arg;
    stack_pop(&g_st, g_buf);
}

#if defined(__APPLE__)
static long long phys_footprint(void)
{
    struct task_vm_info info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    memset(&info, 0, sizeof(info));
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count);
    return (long long)info.phys_footprint;
}
#endif

// ---- fixes 1-7 ---------------------------------------------------------------------------

static void test_linear_roundtrip(void)
{
    struct linear_allocator a;
    expect_true(linear_init(&a, 1 << 20), "linear: init over 1 MiB");
    expect_true(a.base != NULL && ((uintptr_t)a.base & 15) == 0, "linear: base is 16-byte aligned");

    void* b1 = linear_alloc(&a, 100);
    expect_true(b1 == (void*)a.base && a.used == 112, "linear: alloc(100) rounds to 112 at base");
    expect_true(((uintptr_t)b1 & 15) == 0, "linear: block is 16-byte aligned");

    void* b2 = linear_alloc(&a, 100);
    expect_true(b2 == (void*)(a.base + 112), "linear: sequential allocation");

    linear_reset(&a);
    expect_true(a.used == 0, "linear: reset sets used = 0");
    expect_true(linear_alloc(&a, 100) == (void*)a.base, "linear: re-allocates at base after reset");

    linear_release(&a);
    expect_true(a.used == 0 && a.peak == 0, "linear: release zeroes used and peak");

    unsigned char* b4 = linear_alloc(&a, 16);
    expect_true(b4 != NULL, "linear: usable after release");
    if (b4 != NULL)
    {
        b4[0] = 5;
        expect_true(b4[0] == 5, "linear: allocation writes work after release");
    }

    linear_free(&a);
    expect_true(a.base == NULL && a.size == 0, "linear: free zeroes the struct");
}

static void test_overflow_sizes(void)
{
    size_t u;

    struct linear_allocator a;
    linear_init(&a, 1 << 20);
    u = a.used;
    expect_true(linear_alloc(&a, SIZE_MAX) == NULL && a.used == u, "linear: SIZE_MAX rejected, used unchanged");
    expect_true(linear_alloc(&a, SIZE_MAX - 15) == NULL && a.used == u, "linear: SIZE_MAX - 15 rejected, used unchanged");
    expect_true(linear_alloc(&a, SIZE_MAX - 31) == NULL && a.used == u, "linear: SIZE_MAX - 31 rejected, used unchanged");
    expect_true(linear_alloc(&a, 0) == NULL && a.used == u, "linear: size 0 rejected");
    expect_true(linear_alloc(&a, 16) == (void*)a.base, "linear: normal alloc works after rejected sizes");
    linear_free(&a);

    struct stack_allocator s;
    stack_init(&s, 1 << 20);
    u = s.used;
    expect_true(stack_alloc(&s, SIZE_MAX) == NULL && s.used == u, "stack: SIZE_MAX rejected, used unchanged");
    expect_true(stack_alloc(&s, SIZE_MAX - 15) == NULL && s.used == u, "stack: SIZE_MAX - 15 rejected, used unchanged");
    expect_true(stack_alloc(&s, SIZE_MAX - 31) == NULL && s.used == u, "stack: SIZE_MAX - 31 rejected, used unchanged");
    expect_true(stack_alloc(&s, 0) == NULL && s.used == u, "stack: size 0 rejected");
    expect_true(stack_alloc(&s, 16) == (void*)s.base, "stack: normal alloc works after rejected sizes");
    stack_free(&s);
}

static void test_stack_lifo(void)
{
    struct stack_allocator s;
    stack_init(&s, 1 << 20);

    void* a1 = stack_alloc(&s, 32);   // payload [0, 16*2), block [0, 48)
    void* a2 = stack_alloc(&s, 64);   // payload [48, 112), block [48, 128)
    expect_true(a1 == (void*)s.base && a2 == (void*)(s.base + 48), "stack: LIFO addresses");
    expect_true(s.used == 128, "stack: used tracks payload + trailer");
    expect_true(((uintptr_t)a1 & 15) == 0 && ((uintptr_t)a2 & 15) == 0, "stack: blocks are 16-byte aligned");

    expect_true(stack_top(&s) == a2, "stack: top is the most recent block");
    expect_true(stack_pop(&s, a2) == a2 && s.used == 48, "stack: pop rewinds past the top");
    expect_true(stack_top(&s) == a1, "stack: top falls back to the previous block");
    expect_true(stack_pop(&s, a1) == a1 && s.used == 0, "stack: pop the last block");
    expect_true(stack_pop(&s, a1) == NULL, "stack: pop on empty returns NULL");
    expect_true(stack_top(&s) == NULL, "stack: top on empty returns NULL");

    stack_free(&s);
}

static void test_overrun_corrupts(void)
{
#if TEST_ASAN
    // Writing the trailer from user code dies under ASan first; the poisoned trailer
    // covers this in test_asan_section instead.
    expect_skip("overrun into the trailer marks the stack corrupt");
#else
    vmem_hook_calls = 0;
    stack_reset(&g_st);
    g_buf = stack_alloc(&g_st, 16);
    ((unsigned char*)g_buf)[16] = 0x00;   // clobber canary byte 0

    expect_true(stack_top(&g_st) == NULL && vmem_hook_calls == 1 && g_st.corrupt, "overrun detected at top: hook once, corrupt");
    expect_true(stack_alloc(&g_st, 16) == NULL && vmem_hook_calls == 2, "further ops refuse and re-report");
    expect_true(stack_pop(&g_st, g_buf) == NULL && vmem_hook_calls == 3, "pop refuses and re-reports");
    stack_reset(&g_st);
    expect_true(!g_st.corrupt && stack_alloc(&g_st, 16) != NULL, "stack_reset clears the corrupt flag");
    stack_reset(&g_st);
#endif
}

static void test_footprint(void)
{
#if TEST_ASAN
    expect_skip("phys_footprint drops after linear_release");
    return;
#elif defined(__APPLE__)
    const size_t arena = 8u * 1024u * 1024u;
    const size_t chunk = 2u * 1024u * 1024u;

    struct linear_allocator a;
    linear_init(&a, arena);
    unsigned char* buf = linear_alloc(&a, chunk);
    memset(buf, 1, chunk);   // commit every page of the chunk

    long long before = phys_footprint();
    linear_release(&a);
    long long after = phys_footprint();

    expect_true(before - after >= (long long)(1024u * 1024u), "phys_footprint drops after linear_release");

    bool zeros = true;
    for (size_t i = 0; i < chunk; i += 4096)
    {
        if (buf[i] != 0)
            zeros = false;
    }
    expect_true(zeros, "released range reads back as zeros");

    unsigned char* b2 = linear_alloc(&a, 16);
    b2[0] = 7;
    expect_true(b2[0] == 7, "arena usable after release");

    linear_free(&a);
#else
    expect_skip("phys_footprint drops after linear_release (macOS only)");
#endif
}

// ---- 8. ASan poisoning --------------------------------------------------------------------

static void test_asan_section(void)
{
#if !TEST_ASAN
    return;   // poisoning is a no-op outside ASan; nothing to verify
#endif

    stack_free(&g_st);   // drop the reservation main took before re-initializing below

    // Writing 1 byte into the 16-byte padding of the requested size.
    linear_init(&g_lin, 1 << 20);
    g_buf = linear_alloc(&g_lin, 8);
    g_off = 8;   // first padding byte of the rounded slot
    run_child(child_poison_write, &exp_asan, "ASan: write into padding of linear_alloc(8) reports");

    // Reading a block after stack_pop.
    stack_init(&g_st, 1 << 20);
    g_buf = stack_alloc(&g_st, 8);
    ((unsigned char*)g_buf)[0] = 1;   // unpoisoned requested byte: fine in the parent
    g_off = 0;
    run_child(child_pop_then_read, &exp_asan, "ASan: read after stack_pop reports");
    stack_reset(&g_st);   // later tests reuse g_st; keep the reservation alive

    // Reading a block after linear_reset.
    g_buf = linear_alloc(&g_lin, 8);
    ((unsigned char*)g_buf)[0] = 1;
    g_off = 0;
    run_child(child_reset_then_read, &exp_asan, "ASan: read after linear_reset reports");

    // Reading reserved but never allocated space (fresh arena: everything still poisoned).
    linear_free(&g_lin);
    linear_init(&g_lin, 1 << 20);
    run_child(child_reserved_read, &exp_asan, "ASan: read of reserved-but-unallocated space reports");
    linear_free(&g_lin);

    // Ordinary use must not trip ASan: the allocator's own trailer accesses included.
    struct linear_allocator a;
    linear_init(&a, 1 << 20);
    unsigned char* p = linear_alloc(&a, 8);
    memset(p, 1, 8);
    linear_reset(&a);
    p = linear_alloc(&a, 8);   // same address again
    memset(p, 2, 8);
    linear_release(&a);
    linear_free(&a);

    struct stack_allocator s;
    stack_init(&s, 1 << 20);
    unsigned char* q1 = stack_alloc(&s, 8);
    unsigned char* q2 = stack_alloc(&s, 16);
    memset(q1, 1, 8);
    memset(q2, 1, 16);
    expect_true(stack_pop(&s, q2) == q2, "ASan: pop works without a report");
    expect_true(stack_pop(&s, q1) == q1, "ASan: second pop works without a report");
    q1 = stack_alloc(&s, 8);   // same address again
    memset(q1, 3, 8);
    expect_true(stack_pop(&s, q1) == q1, "ASan: re-alloc + write + pop without a report");
    stack_free(&s);

    expect_true(true, "ASan: ordinary alloc/write/pop/re-alloc produced no report");
}

// ---- 9. Guard pages ------------------------------------------------------------------------

static void test_guard_pages(void)
{
    // Leading guard: write one byte before base.
    linear_init(&g_lin, 1 << 20);
    run_child(child_guard_lead, &exp_guard, "guards: write 1 byte before base faults");

    // Trailing guard with a non-page-multiple size.
    linear_free(&g_lin);
    linear_init(&g_lin, (1u << 20) + 1);   // rounds up to 1 MiB + page
    run_child(child_guard_trail, &exp_guard, "guards: write 1 byte past the rounded end faults");

    // vmem_release keeps the guards.
    run_child(child_release_guard, &exp_guard, "guards: leading guard survives vmem_release");
    linear_free(&g_lin);

    // vmem_free unmaps the whole mapping, guards included: a fresh MAP_FIXED mapping at the
    // same address succeeds only if the old mapping is gone.
    struct linear_allocator a;
    linear_init(&a, (1u << 20) + 1);
    size_t page = (size_t)getpagesize();
    size_t rounded = (a.size + page - 1) & ~(page - 1);
    void* start = (void*)((char*)a.base - page);
    linear_free(&a);
    void* m = mmap(start, rounded + page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    expect_true(m == start, "guards: vmem_free unmaps the guards too (re-mmap at same address succeeds)");
    if (m == start)
        munmap(start, rounded + page * 2);
}

// ---- 10. Fail loud --------------------------------------------------------------------------

static void test_fail_loud(void)
{
    // With the counting hook: each bad case calls the hook once, sets corrupt, and a second
    // operation re-reports via the hook without touching the bad trailer.
    vmem_hook_calls = 0;
    stack_reset(&g_st);
    g_buf = stack_alloc(&g_st, 16);

#if TEST_ASAN
    // Clobbering the trailer from user code reports in ASan first; only the out-of-order
    // and rewind paths (no user write) are exercised in an ASan build.
    expect_skip("clobbered canary/prev calls the hook");
#else
    ((unsigned char*)g_buf)[16] = 0x00;   // canary byte 0 (was 0x4C)
    expect_true(stack_top(&g_st) == NULL && vmem_hook_calls == 1 && g_st.corrupt, "clobbered canary: hook called once, corrupt set");
    expect_true(stack_pop(&g_st, g_buf) == NULL && vmem_hook_calls == 2 && g_st.used == 32, "second op re-reports, used untouched");
    stack_reset(&g_st);
#endif

    // Out-of-order pop: no memory writes involved, safe in every build.
    vmem_hook_calls = 0;
    stack_reset(&g_st);
    void* p1 = stack_alloc(&g_st, 16);
    void* p2 = stack_alloc(&g_st, 16);
    expect_true(stack_pop(&g_st, p1) == NULL, "out-of-order pop returns NULL");
    expect_true(vmem_hook_calls == 1 && g_st.corrupt, "out-of-order pop calls the hook and sets corrupt");
    expect_true(g_st.used == 64, "out-of-order pop leaves used unchanged");
    vmem_hook_calls = 0;
    stack_pop(&g_st, p1);
    expect_true(vmem_hook_calls == 1, "second op on corrupt stack re-reports");
    stack_reset(&g_st);

    // trap_mode: the default behaviour kills the process on the next check.
#if !TEST_ASAN
    stack_reset(&g_st);
    g_buf = stack_alloc(&g_st, 16);
    ((unsigned char*)g_buf)[16] = 0x00;   // clobber the canary
    vmem_trap_mode = true;
    run_child(child_corrupt_pop, &exp_trap, "fail-loud: clobbered canary traps on the next stack_pop");
    vmem_trap_mode = false;
    stack_reset(&g_st);

    stack_reset(&g_st);
    p1 = stack_alloc(&g_st, 16);
    p2 = stack_alloc(&g_st, 16);
    ((uint64_t*)((unsigned char*)p2 + 24))[0] = 33;   // prev not 16-aligned
    g_buf = p2;
    vmem_trap_mode = true;
    run_child(child_corrupt_pop, &exp_trap, "fail-loud: clobbered prev (unaligned) traps on the next stack_pop");
    vmem_trap_mode = false;
    stack_reset(&g_st);

    stack_reset(&g_st);
    p1 = stack_alloc(&g_st, 16);
    p2 = stack_alloc(&g_st, 16);
    ((uint64_t*)((unsigned char*)p2 + 24))[0] = 0x7FFFFFFFFFFFFFFFULL;   // prev out of range
    g_buf = p2;
    vmem_trap_mode = true;
    run_child(child_corrupt_pop, &exp_trap, "fail-loud: clobbered prev (out of range) traps on the next stack_pop");
    vmem_trap_mode = false;
    stack_reset(&g_st);
#endif

    // Out-of-order pop in trap_mode: safe in every build.
    stack_reset(&g_st);
    p1 = stack_alloc(&g_st, 16);
    p2 = stack_alloc(&g_st, 16);
    g_buf = p1;   // not the top block
    vmem_trap_mode = true;
    run_child(child_corrupt_pop, &exp_trap, "fail-loud: out-of-order pop traps in trap_mode");
    vmem_trap_mode = false;
    stack_reset(&g_st);
}

// ---- 11. Out-of-order frees ------------------------------------------------------------------

static void test_out_of_order(void)
{
    vmem_hook_calls = 0;
    stack_reset(&g_st);
    void* p1 = stack_alloc(&g_st, 16);
    void* p2 = stack_alloc(&g_st, 16);
    expect_true(stack_pop(&g_st, p1) == NULL && vmem_hook_calls == 1 && g_st.used == 64, "pop of a non-top block: hook, used unchanged");
    stack_reset(&g_st);

    // mark / rewind
    vmem_hook_calls = 0;
    stack_reset(&g_st);
    stack_alloc(&g_st, 32);   // block [0, 48)
    size_t m1 = stack_mark(&g_st);
    stack_alloc(&g_st, 32);   // block [48, 96)
    stack_alloc(&g_st, 32);   // block [96, 144)
    expect_true(m1 == 48, "mark returns used");
    stack_rewind(&g_st, m1);
    expect_true(g_st.used == 48 && vmem_hook_calls == 0 && !g_st.corrupt, "rewind to an earlier mark succeeds");

    vmem_hook_calls = 0;
    stack_alloc(&g_st, 32);   // block [48, 96), used = 96
    stack_rewind(&g_st, m1 + 16);   // 64 is not a block boundary
    expect_true(g_st.used == 96 && vmem_hook_calls == 1 && g_st.corrupt, "rewind to a non-boundary calls the hook");
    expect_true(g_st.used == 96, "failed rewind leaves used unchanged");
    stack_reset(&g_st);

    vmem_hook_calls = 0;
    stack_alloc(&g_st, 32);
    stack_rewind(&g_st, g_st.used + 32);   // above used
    expect_true(vmem_hook_calls == 1 && g_st.corrupt, "rewind above used calls the hook");
    stack_reset(&g_st);

    // stack_alloc checks the current top's trailer before pushing.
    vmem_hook_calls = 0;
    stack_reset(&g_st);
    g_buf = stack_alloc(&g_st, 16);
#if TEST_ASAN
    expect_skip("stack_alloc catches an overrun of the current top");
#else
    ((unsigned char*)g_buf)[16] = 0x00;   // clobber the top canary
    expect_true(stack_alloc(&g_st, 16) == NULL && vmem_hook_calls == 1, "alloc after an overrun calls the hook during the alloc");
    stack_reset(&g_st);
#endif

    // stack_check
    vmem_hook_calls = 0;
    stack_reset(&g_st);
    for (int i = 0; i < 5; i++)
        stack_alloc(&g_st, 16u + (size_t)i * 16u);
    expect_true(stack_check(&g_st) && vmem_hook_calls == 0, "stack_check true for a healthy stack of 5");

#if TEST_ASAN
    expect_skip("stack_check catches a clobbered middle canary");
#else
    ((unsigned char*)g_st.base)[16] = 0x00;   // canary of the bottom block's trailer
    expect_true(stack_top(&g_st) != NULL, "top does not see the clobbered middle block yet");
    vmem_hook_calls = 0;
    expect_true(stack_check(&g_st) == false && vmem_hook_calls == 1, "stack_check catches the clobbered middle canary");
    stack_reset(&g_st);
#endif
}

int main(void)
{
    printf("vmem tests (ASan=%d)\n", (int)TEST_ASAN);

    if (!stack_init(&g_st, 1 << 20))   // shared stack arena for the in-process tests
        return 1;

    test_linear_roundtrip();
    test_overflow_sizes();
    test_stack_lifo();
    test_overrun_corrupts();
    test_footprint();
    test_asan_section();
    test_guard_pages();
    test_fail_loud();
    test_out_of_order();

    if (failures > 0)
    {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
