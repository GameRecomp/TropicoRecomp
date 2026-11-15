/* SPDX-License-Identifier: MIT */
/* Guest threads: two workers and the main thread take turns at Sleep. */
#include <stdbool.h>

#include "rc/dispatch.h"
#include "rc/mem.h"
#include "rc/thread.h"
#include "rc/vm.h"
#include "test.h"

#define STEPS 4

static uint32_t seen[2][STEPS];
static int steps[2];
static uint32_t ids[2], tibs[2];
static bool on_own_stack[2];

/* A guest function: worker(n) records STEPS steps, sleeping n ms between
 * them, and returns 100 + n. */
static void worker(rc_cpu *c)
{
    uint32_t n = rc_ld32(c->r[RC_ESP] + 4);
    uint32_t k = n - 1;
    ids[k] = rc_thread_id();
    tibs[k] = rc_thread_tib();
    uint32_t sp = c->r[RC_ESP];
    on_own_stack[k] = sp >= rc_ld32(tibs[k] + 8) && sp < rc_ld32(tibs[k] + 4);
    for (int i = 0; i < STEPS; i++) {
        seen[k][steps[k]++] = n * 10 + (uint32_t)i;
        rc_thread_sleep(n);
    }
    c->r[RC_EAX] = 100 + n;
    c->r[RC_ESP] += 8; /* ret 4 */
}

void test_thread(void)
{
    if (!rc_mem_base) {
        CHECK(rc_mem_reserve(UINT64_C(1) << 32));
    }
    static const rc_fn_entry table[] = {{0x00401000u, worker}};
    rc_set_lifted(table, 1);

    static rc_cpu main_cpu;
    uint32_t main_tib = rc_vm_alloc(0, RC_VM_GRANULE, true);
    CHECK(main_tib != 0);
    rc_thread_init_main(&main_cpu, main_tib);
    CHECK_EQ(rc_thread_id(), 1);

    uint32_t id1 = 0, id2 = 0;
    uint32_t h1 = rc_thread_create(0x00401000u, 1, 0, &id1);
    uint32_t h2 = rc_thread_create(0x00401000u, 2, 0, &id2);
#if defined(__EMSCRIPTEN__)
    CHECK(h1 == 0 && h2 == 0); /* no threads there yet */
    return;
#endif
    CHECK(h1 != 0 && h2 != 0 && h1 != h2);
    CHECK(id1 == 2 && id2 == 3);

    /* Nothing runs before the main thread gives way. */
    CHECK_EQ(steps[0] + steps[1], 0);

    bool ended1 = false, ended2 = false;
    uint32_t code1 = 0, code2 = 0;
    for (int i = 0; i < 1000 && !(ended1 && ended2); i++) {
        rc_thread_sleep(1);
        CHECK_EQ(rc_thread_id(), 1); /* the main thread is back */
        CHECK(rc_thread_handle(h1, &ended1, &code1));
        CHECK(rc_thread_handle(h2, &ended2, &code2));
    }
    CHECK(ended1 && ended2);
    CHECK_EQ(code1, 101);
    CHECK_EQ(code2, 102);
    for (int k = 0; k < 2; k++) {
        CHECK_EQ(steps[k], STEPS);
        for (int i = 0; i < STEPS; i++) {
            CHECK_EQ(seen[k][i], (uint32_t)(k + 1) * 10 + (uint32_t)i);
        }
        CHECK(on_own_stack[k]);
    }
    CHECK(ids[0] == id1 && ids[1] == id2);
    CHECK(tibs[0] != tibs[1] && tibs[0] != main_tib && tibs[1] != main_tib);
    CHECK_EQ(rc_thread_tib(), main_tib);
    CHECK(!rc_thread_handle(0x1234, NULL, NULL));
}
