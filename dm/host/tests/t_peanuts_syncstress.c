// PEANUTS: a hunt for lost wake-ups in the host's guest-kernel synchronisation (hle_thread.c), run on a Linux PC with real threads.
// hle_thread.c is built as it is, as if for Android (as t_peanuts_stopworld.c does; the same stand-ins for what differs on a PC). The "userland" here is what the game's sdk
// does around the SVCs: a mutex word taken with a compare-and-swap and ArbitrateLock / ArbitrateUnlock only when contended, a condition variable whose Signal issues the SVC
// only when the key word is not 0, events waited for with WaitSynchronization and reset with ResetSignal (auto-clear) or ClearEvent (manual), a futex-like word on
// WaitForAddress / SignalToAddress. Every wake-up that must come is waited for with a bound: a stall is a failure, and the thread dump is printed.
// Part 1, single cases:
//   1. WaitSynchronization and WaitProcessWideKeyAtomic with the longest timeout (INT64_MAX ns) wait (now + timeout wraps and wraps back: held, not a defect);
//   2. a native wait inside a native wait keeps its depth (dm_thread_native_wait; the second entry used to undo the first, and the outer leave aborted);
//   3. a same-core handoff (a waker held until the higher-priority thread it woke blocks) holds without and across a stop of all threads (dm_guest_stop_all: the woken
//      thread counted as "not runnable" while stopped, the waker left its SVC at the resume and ran beside it);
//   4. the state dump (svc_thread_dump) says of a thread parked in WaitSynchronization which handles it waits on and whether each is signalled, and lists last SVCs;
//   5. a timed wait's timeout comes on time when the wall clock is set back during the wait (deadlines were taken on the wall clock).
// Part 2, the storm (seconds from the command line, default 3):
//   A. job pools on one mutex + one condition variable: untimed consumers with Signal(1); consumers under Broadcast beside waiters that only time out; timed consumers;
//      and pairs of one consumer and one producer that pushes the next job the moment the last was taken (built with -DDM_TEST_CV_OLD_ORDER, the host's old order of
//      "release the mutex, then write the key", these pairs stall: the check that this test sees a lost wake-up at all).
//      Producers signal inside or outside the lock, on pinned and unpinned cores, at priorities above and below the consumers (same-core handoffs);
//   B. events: signalled from a host thread and from a game thread, auto-clear and manual-clear, one handle and several, timeouts none / short / zero / endless, with
//      CancelSynchronization from a third thread. Each signal must be answered (a counter the waiter copies) before the next;
//   C. thread exit: a thread is created, exits, and the wait on its handle must end;
//   D. a counter on WaitForAddress(wait if equal) / SignalToAddress(wake one);
//   and, all the while, under the global lock: no thread parked on a condition variable whose key word is 0 (the sdk would skip its signal), none parked on a mutex whose
//   word lacks an owner or the waiters bit (the owner would unlock without the SVC), none parked on a signalled object.
//   --stops: a host thread stops and resumes every game thread throughout (the companion runtime's writes).
// Build and run: make GAME=<id> peanuts-sync (dm/host/tests/Makefile); ./t_peanuts_syncstress [seconds] [--stops]
#define _GNU_SOURCE
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
static __uint128_t g_no_v[32];
#undef PLAT_UC_X
#undef PLAT_UC_FP
#undef PLAT_UC_LR
#undef PLAT_UC_SP
#undef PLAT_UC_PC
#undef PLAT_UC_CPSR
#undef PLAT_UC_V
#undef PLAT_UC_FPSR
#undef PLAT_UC_FPCR
#define PLAT_UC_X(uc, i) ((uint64_t)((uc) != NULL) * 0 + (uint64_t)(i) * 0)
#define PLAT_UC_FP(uc) 0
#define PLAT_UC_LR(uc) 0
#define PLAT_UC_SP(uc) ((uint64_t)(uc)->uc_mcontext.gregs[REG_RSP])
#define PLAT_UC_PC(uc) ((uint64_t)(uc)->uc_mcontext.gregs[REG_RIP])
#define PLAT_UC_CPSR(uc) 0
#define PLAT_UC_V(uc) g_no_v
#define PLAT_UC_FPSR(uc) 0
#define PLAT_UC_FPCR(uc) 0
#undef dm_slot_get
#define dm_slot_get(s) ((void *)0)
// The wall clock as the host reads it: g_wall_ahead seconds ahead of the real one, which is what a deadline taken on it is after the clock was set back by that much
static _Atomic long g_wall_ahead;
static int test_clock_gettime(clockid_t id, struct timespec *ts) { int r = clock_gettime(id, ts); if (id == CLOCK_REALTIME) ts->tv_sec += atomic_load(&g_wall_ahead); return r; }
#define clock_gettime test_clock_gettime
#ifndef HLE_THREAD_SOURCE   // (another copy of the file, to show a case failing on it)
#define HLE_THREAD_SOURCE "../hle_thread.c"
#endif
#include HLE_THREAD_SOURCE
#undef clock_gettime

// ---- the host services hle_thread.c takes from other files ----
struct dm_flags g_flag;
uint64_t g_t0_ns;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void guest_thread_set_id(uint64_t id) { (void)id; }
void guest_gate_stack(int own_stack) { (void)own_stack; }
void *guest_sym(const char *name) { (void)name; return NULL; }
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
__thread _Atomic int dm_host_work;
pthread_key_t g_key18;
#include <sys/uio.h>
int plat_mem_read(uint64_t addr, void *out, size_t len) {
    struct iovec l = {out, len}, r = {(void *)(uintptr_t)addr, len}; return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)len ? 0 : -1;
}
plat_thread_t plat_thread_of(pthread_t t) { (void)t; return 0; }
plat_thread_t plat_thread_self(void) { return 0; }
void plat_thread_set_name(const char *name) { (void)name; }
int plat_thread_suspend(plat_thread_t t) { (void)t; return -1; }
int plat_thread_get_regs(plat_thread_t t, plat_regs_t *r) { (void)t; (void)r; return -1; }
void plat_thread_resume(plat_thread_t t) { (void)t; }
int plat_thread_run_state(plat_thread_t t) { (void)t; return -1; }
int64_t plat_thread_cpu_us(plat_thread_t t) { (void)t; return 0; }
int plat_threads(plat_thread_t **list, unsigned *n) { (void)list; (void)n; return -1; }
void plat_threads_release(plat_thread_t *list, unsigned n) { (void)list; (void)n; }
int plat_qos_set_self(plat_qos_t q) { (void)q; return 0; }
plat_qos_t plat_qos_of(pthread_t t) { (void)t; return PLAT_QOS_USER_INITIATED; }
void *plat_qos_override_start(pthread_t t, plat_qos_t q) { (void)t; (void)q; return NULL; }
void plat_qos_override_end(void *o) { (void)o; }
size_t plat_page_size(void) { return 4096; }
int dm_guest_stop_all(uint64_t timeout_ns); void dm_guest_resume_all(void);

// ---- game code: the section guestcode (the pause signal stops a thread only there or inside an SVC) ----
#define GUEST __attribute__((section("guestcode"), noinline))
extern const char __start_guestcode[], __stop_guestcode[];
static module_t g_fake_module;
module_t *module_of(const void *p) { return (const char *)p >= __start_guestcode && (const char *)p < __stop_guestcode ? &g_fake_module : NULL; }

static uint64_t g_stall_ns = 5000000000ull;   // a wake-up that has not come after this long is lost
static _Atomic int g_stop;                     // the storm ends: producers and signallers stop making work
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("FAIL "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout);
    svc_thread_dump();   // (takes no lock)
    _exit(1);
}
static int g_fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); g_fail += !ok; fflush(stdout); }

// One SVC of a game thread, through the gate's entry and exit as gate_dispatch (hle.c) makes them
GUEST static uint64_t svc6(int num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t *o1, uint64_t *o2) {
    uint64_t r[100]; r[0] = a0; r[1] = a1; r[2] = a2; r[3] = a3; r[4] = a4; r[5] = a5; for (int i = 6; i < 100; i++) r[i] = 0;
    thr_svc_enter(r, 0); svc_thread(num, r); thr_svc_leave();
    if (o1) *o1 = r[1];
    if (o2) *o2 = r[2];
    return r[0];
}
#define SVC(n, a0, a1, a2, a3) svc6(n, (uint64_t)(a0), (uint64_t)(a1), (uint64_t)(a2), (uint64_t)(a3), 0, 0, NULL, NULL)
// The same from a host thread (audio, vsync, a service's completion): no gate, no thread record
static uint64_t hsvc(int num, uint64_t a0, uint64_t a1) { uint64_t r[100] = {a0, a1}; svc_thread(num, r); return r[0]; }

typedef _Atomic uint32_t word_t;
GUEST static uint64_t rnd(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }
GUEST static void g_sleep(int64_t ns) { SVC(0x0B, ns, 0, 0, 0); }
// nn::os::detail::InternalCriticalSection (Enter / Leave) and InternalConditionVariable (Wait / TimedWait / Signal / Broadcast), as the sdk has them
GUEST static void mtx_lock(word_t *m) {
    uint32_t me = thr_cur_handle();
    for (;;) {
        uint32_t v = atomic_load(m);
        if (v == 0) { if (atomic_compare_exchange_strong(m, &v, me)) return; continue; }
        if (!(v & WAITERS_BIT) && !atomic_compare_exchange_strong(m, &v, v | WAITERS_BIT)) continue;
        SVC(0x1A, v & ~WAITERS_BIT, m, me, 0);
        if ((atomic_load(m) & ~WAITERS_BIT) == me) return;
    }
}
GUEST static void mtx_unlock(word_t *m) {
    uint32_t me = thr_cur_handle();
    if (atomic_compare_exchange_strong(m, &me, 0)) return;
    SVC(0x1B, m, 0, 0, 0);
}
GUEST static int cv_wait(word_t *key, word_t *m, int64_t ns) {   // 1 = timed out; the mutex is held again either way
    uint64_t r = SVC(0x1C, m, key, thr_cur_handle(), ns);
    if (r == R_TIMEOUT) { mtx_lock(m); return 1; }
    if (r) fail("WaitProcessWideKeyAtomic returned %#llx", (unsigned long long)r);
    if ((atomic_load(m) & ~WAITERS_BIT) != thr_cur_handle()) fail("a condition wait returned success without the mutex (word %#x, thread %#x)", atomic_load(m), thr_cur_handle());
    return 0;
}
GUEST static void cv_signal(word_t *key, int count) { if (atomic_load(key)) SVC(0x1D, key, count, 0, 0); }   // the sdk's test in user mode: key 0 = nobody waits, no SVC

void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp; ((void (*)(uint64_t))entry)(arg);
    uint64_t r[100] = {0}; svc_thread(0x0A, r); abort();
}
// CreateThread + (a core mask) + StartThread; mask 0 = the ideal core alone, as created
GUEST static uint32_t spawn(void (*fn)(uint64_t), void *arg, int prio, int core, uint64_t mask) {
    uint64_t h = 0;
    if (svc6(0x08, 0, (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)arg, 0, (uint64_t)prio, (uint64_t)core, &h, NULL)) fail("CreateThread");
    if (mask) SVC(0x0F, h, (uint64_t)(int64_t)-3, mask, 0);
    if (SVC(0x09, h, 0, 0, 0)) fail("StartThread");
    return (uint32_t)h;
}
GUEST static void join(uint32_t h) {
    uint32_t hs[1] = {h};
    for (;;) { uint64_t r = SVC(0x18, 0, hs, 1, -1); if (!r) break; if (r != R_CANCELLED) fail("a wait on a thread handle returned %#llx", (unsigned long long)r); }
    SVC(0x16, h, 0, 0, 0);
}
// The main thread is one of the game's: while it waits on host time it stands inside an SVC, as a game thread that waits does (a stop of all threads counts it stopped)
static void main_idle(useconds_t us) { uint64_t gate[100] = {0}; thr_svc_enter(gate, 0); usleep(us); thr_svc_leave(); }

// ---- things that must make progress: have must reach want, or (want NULL) keep moving until the storm ends ----
typedef struct { const char *name; _Atomic uint64_t *want, *have; uint64_t last, since; } duty_t;
static duty_t g_duty[64]; static int g_nduty;
static void duty(const char *name, _Atomic uint64_t *want, _Atomic uint64_t *have) { g_duty[g_nduty++] = (duty_t){name, want, have, 0, 0}; }
static _Atomic int g_watch_quit; static _Atomic uint64_t g_inv_checks;
// Holding G (or with every thread stopped, which holds it): what must hold of every parked thread
static void invariants(void) {
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind == 2 && !ld32(t->wkey))
            fail("thread %#x is parked on condition key %#llx whose word is 0: the sdk will not signal it", t->handle, (unsigned long long)t->wkey);
        if (t->wkind == 1) { uint32_t w = ld32(t->waddr);
            if (!(w & WAITERS_BIT) || !(w & ~WAITERS_BIT)) fail("thread %#x is parked on mutex %#llx whose word is %#x: its owner will not hand it over", t->handle, (unsigned long long)t->waddr, w); }
        if (t->wkind == 4) for (int i = 0; i < t->whn; i++) { int bad = 0; if (sig_obj(t->whs[i], &bad)) fail("thread %#x is parked in WaitSynchronization on handle %#x, which is signalled", t->handle, t->whs[i]); }
    }
    atomic_fetch_add(&g_inv_checks, 1);
}
static void *watch_main(void *arg) {
    (void)arg; uint64_t tick = 0;
    while (!atomic_load(&g_watch_quit)) {
        usleep(200); tick++;
        pthread_mutex_lock(&G); invariants(); pthread_mutex_unlock(&G);
        if (tick % 250) continue;
        uint64_t now = now_ns();
        for (int i = 0; i < g_nduty; i++) { duty_t *d = &g_duty[i];
            uint64_t have = atomic_load(d->have), want = d->want ? atomic_load(d->want) : have + !atomic_load(&g_stop);
            if (have != d->last || have >= want) { d->last = have; d->since = now; continue; }
            if (now - d->since > g_stall_ns) fail("%s: stalled at %llu of %llu for %.1f s: a wake-up was lost", d->name, (unsigned long long)have, (unsigned long long)want, (now - d->since) / 1e9);
        }
    }
    return NULL;
}

// ---- A. job pools ----
enum { W_UNTIMED, W_TIMED, W_NOISE };
typedef struct { word_t mtx, key; _Atomic int pending, quit, broadcast; _Atomic uint64_t pushed, done; char name[32]; } pool_t;
typedef struct { pool_t *p; int kind, inside; uint64_t rng; } actor_t;
GUEST static void pool_worker(uint64_t a) {
    actor_t *me = (actor_t *)a; pool_t *p = me->p;
    for (;;) {
        mtx_lock(&p->mtx);
        if (me->kind == W_NOISE) {   // waits on the key and the mutex with a timeout and takes nothing: its timeouts cross the signals meant for the consumers
            int q = atomic_load(&p->quit);
            if (!q) cv_wait(&p->key, &p->mtx, 20000 + (int64_t)(rnd(&me->rng) % 2000000));
            mtx_unlock(&p->mtx);
            if (q) return;
            continue;
        }
        while (!atomic_load(&p->pending) && !atomic_load(&p->quit))
            cv_wait(&p->key, &p->mtx, me->kind == W_UNTIMED ? -1 : 20000 + (int64_t)(rnd(&me->rng) % 3000000));
        if (atomic_load(&p->pending)) {
            atomic_fetch_sub(&p->pending, 1); mtx_unlock(&p->mtx); atomic_fetch_add(&p->done, 1);
            for (uint64_t k = rnd(&me->rng) % 64; k; k--) rnd(&me->rng);   // the job
            continue;
        }
        mtx_unlock(&p->mtx); return;
    }
}
GUEST static void pool_producer(uint64_t a) {
    actor_t *me = (actor_t *)a; pool_t *p = me->p; int n = atomic_load(&p->broadcast) ? -1 : 1;
    while (!atomic_load(&g_stop)) {
        mtx_lock(&p->mtx);
        atomic_fetch_add(&p->pending, 1); atomic_fetch_add(&p->pushed, 1);
        if (me->inside) cv_signal(&p->key, n);
        mtx_unlock(&p->mtx);
        if (!me->inside) cv_signal(&p->key, n);
        uint64_t x = rnd(&me->rng) % 16;   // the pace: bursts, and gaps in which every consumer goes back to sleep
        if (x < 3) g_sleep(20000 + (int64_t)(rnd(&me->rng) % 400000)); else if (x < 6) g_sleep(0);
        while (atomic_load(&p->pushed) - atomic_load(&p->done) > 256 && !atomic_load(&g_stop)) g_sleep(50000);
    }
}
// One job at a time: the next is pushed only when the last was taken, as soon as it was (the consumer is then on its way back into the wait: the push, the look at the
// key word and the skipped signal fall into the wait's own steps). A job pushed while the consumer parks and never taken has no later signal to hide behind
GUEST static void pair_producer(uint64_t a) {
    actor_t *me = (actor_t *)a; pool_t *p = me->p;
    while (!atomic_load(&g_stop)) {
        mtx_lock(&p->mtx);
        atomic_fetch_add(&p->pending, 1); uint64_t q = atomic_fetch_add(&p->pushed, 1) + 1;
        if (me->inside) cv_signal(&p->key, 1);
        mtx_unlock(&p->mtx);
        if (!me->inside) cv_signal(&p->key, 1);
        uint64_t t0 = 0;
        for (unsigned k = 0; atomic_load(&p->done) < q; k++) {
            if (k < 20000) continue;
            if (!t0) t0 = now_ns();
            g_sleep(30000);
            if (now_ns() - t0 > g_stall_ns) fail("%s: job %llu was pushed and never taken in %.1f s: the consumer's wake-up was lost", p->name, (unsigned long long)q, g_stall_ns / 1e9);
        }
        for (uint64_t k = rnd(&me->rng) % 512; k; k--) rnd(&me->rng);   // sweeps the moment of the next push over the consumer's way into its wait
    }
}

// ---- B. events ----
typedef struct { uint32_t wh, rh, dummy; int autoclear; _Atomic uint64_t req, ack; _Atomic int quit; uint32_t waiter[4]; int nwaiter; char name[40]; } evg_t;
typedef struct { evg_t *g; int tmo, multi; uint64_t rng; } ewait_t;   // tmo: 0 none, 1 short, 2 a poll with timeout 0 now and then, 3 endless (INT64_MAX)
GUEST static void ev_waiter(uint64_t a) {
    ewait_t *me = (ewait_t *)a; evg_t *g = me->g; uint32_t hs[2]; int n = 0;
    if (me->multi) hs[n++] = g->dummy;
    hs[n++] = g->rh;
    for (;;) {
        int64_t ns = me->tmo == 1 ? 20000 + (int64_t)(rnd(&me->rng) % 1500000) : me->tmo == 2 && rnd(&me->rng) % 4 == 0 ? 0 : me->tmo == 3 ? INT64_MAX : -1;
        uint64_t idx = 0, r = svc6(0x18, 0, (uint64_t)(uintptr_t)hs, (uint64_t)n, (uint64_t)ns, 0, 0, &idx, NULL);
        if (atomic_load(&g->quit)) return;
        if (r) { if (r != R_TIMEOUT && r != R_CANCELLED) fail("WaitSynchronization returned %#llx", (unsigned long long)r); continue; }   // only a signalled wait answers: a timeout must not hide a lost signal
        if ((int)idx != n - 1) fail("WaitSynchronization named handle %d, the signalled one is %d", (int)idx, n - 1);
        if (g->autoclear) { if (SVC(0x17, g->rh, 0, 0, 0)) continue; }   // nn::os::WaitSystemEvent on an auto-clear event: ResetSignal, wait again if another waiter took it
        else SVC(0x12, g->rh, 0, 0, 0);
        uint64_t q = atomic_load(&g->req), o = atomic_load(&g->ack);
        while (o < q && !atomic_compare_exchange_weak(&g->ack, &o, q)) {}
    }
}
static void *ev_host_signaller(void *a) {   // a host thread: file I/O completion, audio, vsync
    evg_t *g = a; uint64_t rng = (uint64_t)(uintptr_t)a | 1;
    while (!atomic_load(&g_stop)) {
        uint64_t q = atomic_fetch_add(&g->req, 1) + 1;
        if (hsvc(0x11, g->wh, 0)) fail("%s: SignalEvent failed", g->name);
        uint64_t t0 = now_ns();
        for (unsigned k = 0; atomic_load(&g->ack) < q; k++) {
            if (k > 2000) usleep(20);
            if ((k & 255) == 255 && now_ns() - t0 > g_stall_ns) fail("%s: signal %llu from a host thread was not answered in %.1f s: the wake-up was lost", g->name, (unsigned long long)q, g_stall_ns / 1e9);
        }
        uint64_t x = rnd(&rng) % 16; if (x < 4) usleep((useconds_t)(rnd(&rng) % 300));   // else at once: the waiter is on its way back into the wait
    }
    return NULL;
}
GUEST static void ev_guest_signaller(uint64_t a) {
    ewait_t *me = (ewait_t *)a; evg_t *g = me->g;
    while (!atomic_load(&g_stop)) {
        uint64_t q = atomic_fetch_add(&g->req, 1) + 1;
        if (SVC(0x11, g->wh, 0, 0, 0)) fail("%s: SignalEvent failed", g->name);
        uint64_t t0 = now_ns();
        for (unsigned k = 0; atomic_load(&g->ack) < q; k++) {
            if (k > 200) g_sleep(30000);
            if ((k & 63) == 63 && now_ns() - t0 > g_stall_ns) fail("%s: signal %llu from a game thread was not answered in %.1f s: the wake-up was lost", g->name, (unsigned long long)q, g_stall_ns / 1e9);
        }
        if (rnd(&me->rng) % 16 < 4) g_sleep(20000 + (int64_t)(rnd(&me->rng) % 300000));
    }
}
GUEST static void ev_canceller(uint64_t a) {
    ewait_t *me = (ewait_t *)a; evg_t *g = me->g;
    while (!atomic_load(&g->quit)) { SVC(0x19, g->waiter[rnd(&me->rng) % (uint64_t)g->nwaiter], 0, 0, 0); g_sleep(30000 + (int64_t)(rnd(&me->rng) % 500000)); }
}

// ---- C. thread exit ----
static _Atomic uint64_t g_joined;
GUEST static void child(uint64_t a) { if (a & 1) g_sleep(20000 + (int64_t)(a % 200000)); else if (a & 2) g_sleep(0); }
GUEST static void joiner(uint64_t a) {
    uint64_t rng = a | 1;
    while (!atomic_load(&g_stop)) { uint64_t x = rnd(&rng); join(spawn(child, (void *)(uintptr_t)x, 30 + (int)(x % 30), (int)(x % 3), 0)); atomic_fetch_add(&g_joined, 1); }
}

// ---- D. the address arbiter ----
typedef struct { _Atomic int32_t val; _Atomic uint64_t posted, got; _Atomic int quit; } sem_t;
static sem_t g_sem;
GUEST static void sem_waiter(uint64_t a) {
    uint64_t rng = a | 1;
    for (;;) {
        if (atomic_load(&g_sem.quit)) return;
        int32_t v = atomic_load(&g_sem.val);
        if (v > 0) { if (atomic_compare_exchange_strong(&g_sem.val, &v, v - 1)) atomic_fetch_add(&g_sem.got, 1); continue; }
        SVC(0x34, &g_sem.val, 2, 0, (a & 1) ? -1 : 20000 + (int64_t)(rnd(&rng) % 2000000));   // wait while the word is 0
    }
}
GUEST static void sem_poster(uint64_t a) {
    uint64_t rng = a | 1;
    while (!atomic_load(&g_stop)) {
        atomic_fetch_add(&g_sem.posted, 1); atomic_fetch_add(&g_sem.val, 1);
        SVC(0x35, &g_sem.val, 0, 0, 1);
        uint64_t x = rnd(&rng) % 16; if (x < 4) g_sleep(20000 + (int64_t)(rnd(&rng) % 300000));
        while (atomic_load(&g_sem.posted) - atomic_load(&g_sem.got) > 256 && !atomic_load(&g_stop)) g_sleep(50000);
    }
}

// ---- --stops: every game thread stopped and resumed, again and again ----
static _Atomic uint64_t g_stops_ok, g_stops_gave_up; static _Atomic int g_stopper_quit;
static void *stopper_main(void *arg) {
    (void)arg; uint64_t rng = 0x9e3779b97f4a7c15ull;
    while (!atomic_load(&g_stopper_quit)) {
        if (dm_guest_stop_all(25000000ull) == 0) { invariants(); usleep((useconds_t)(rnd(&rng) % 300)); atomic_fetch_add(&g_stops_ok, 1); dm_guest_resume_all(); }
        else atomic_fetch_add(&g_stops_gave_up, 1);
        usleep(500 + (useconds_t)(rnd(&rng) % 4000));
    }
    return NULL;
}

// ---- part 1 ----
static struct { uint32_t wh, rh; _Atomic uint64_t res, t0, t1; _Atomic int phase; } g_c1;
GUEST static void c1_waiter(uint64_t a) {
    (void)a; uint32_t hs[1] = {g_c1.rh};
    atomic_store(&g_c1.t0, now_ns()); atomic_store(&g_c1.phase, 1);
    atomic_store(&g_c1.res, SVC(0x18, 0, hs, 1, INT64_MAX)); atomic_store(&g_c1.t1, now_ns()); atomic_store(&g_c1.phase, 2);
}
static struct { word_t mtx, key; _Atomic uint64_t t0, t1; _Atomic int phase, timed_out, flag; } g_c1b;
GUEST static void c1b_waiter(uint64_t a) {
    (void)a; mtx_lock(&g_c1b.mtx); atomic_store(&g_c1b.t0, now_ns()); atomic_store(&g_c1b.phase, 1);
    int to = 0; while (!atomic_load(&g_c1b.flag) && !to) to = cv_wait(&g_c1b.key, &g_c1b.mtx, INT64_MAX);
    mtx_unlock(&g_c1b.mtx); atomic_store(&g_c1b.timed_out, to); atomic_store(&g_c1b.t1, now_ns()); atomic_store(&g_c1b.phase, 2);
}
// 3: T (priority 30, core 0) is woken by W (priority 50, core 0), works 80 ms in game code and then blocks; W's SignalEvent must not return before that
typedef struct { uint32_t wh, rh, wh2, rh2; _Atomic uint64_t woke, blocked, ret; _Atomic int phase; } c3_t;
static c3_t g_c3[2];
GUEST static void c3_T(uint64_t a) {
    c3_t *c = (c3_t *)a; uint32_t hs[1] = {c->rh};
    SVC(0x18, 0, hs, 1, -1);
    uint64_t t0 = now_ns(); atomic_store(&c->woke, t0); atomic_store(&c->phase, 1);
    while (now_ns() - t0 < 80000000ull) {}
    atomic_store(&c->blocked, now_ns()); atomic_store(&c->phase, 2);
    hs[0] = c->rh2; SVC(0x18, 0, hs, 1, -1);
}
GUEST static void c3_W(uint64_t a) { c3_t *c = (c3_t *)a; SVC(0x11, c->wh, 0, 0, 0); atomic_store(&c->ret, now_ns()); }
static void *c3_stopper(void *a) {
    c3_t *c = a; while (atomic_load(&c->phase) != 1) usleep(100);
    usleep(5000);
    int r = dm_guest_stop_all(500000000ull); if (r == 0) { usleep(2000); dm_guest_resume_all(); }
    return (void *)(intptr_t)r;
}
static void new_event(uint32_t *wh, uint32_t *rh) { uint64_t w = 0, r = 0; if (svc6(0x45, 0, 0, 0, 0, 0, 0, &w, &r)) fail("CreateEvent"); *wh = (uint32_t)w; if (rh) *rh = (uint32_t)r; }
static void on_alarm(int sig) { (void)sig; static const char m[] = "FAIL the test hung\n"; if (write(1, m, sizeof m - 1)) {} _exit(99); }

// 5: three timed waits that nothing signals, begun with the wall clock an hour ahead of where it will be (set back during the wait)
static struct { word_t mtx, key; _Atomic int32_t word; uint32_t wh, rh; _Atomic int phase; _Atomic uint64_t r[3]; } g_c5;
GUEST static void c5_waiter(uint64_t a) {
    (void)a; uint32_t hs[1] = {g_c5.rh};
    mtx_lock(&g_c5.mtx); atomic_store(&g_c5.r[0], (uint64_t)cv_wait(&g_c5.key, &g_c5.mtx, 2000000)); mtx_unlock(&g_c5.mtx); atomic_store(&g_c5.phase, 1);
    atomic_store(&g_c5.r[1], SVC(0x18, 0, hs, 1, 2000000)); atomic_store(&g_c5.phase, 2);
    atomic_store(&g_c5.r[2], SVC(0x34, &g_c5.word, 2, 0, 2000000)); atomic_store(&g_c5.phase, 3);
}
static int g_dump_ok;
static void part1(void) {
    // 1
    new_event(&g_c1.wh, &g_c1.rh);
    uint32_t h = spawn(c1_waiter, NULL, 44, 1, 0);
    while (atomic_load(&g_c1.phase) != 1) main_idle(100);
    main_idle(30000); int early = atomic_load(&g_c1.phase) == 2;
    hsvc(0x11, g_c1.wh, 0); join(h);
    check(!early && atomic_load(&g_c1.res) == 0 && atomic_load(&g_c1.t1) - atomic_load(&g_c1.t0) >= 25000000ull,
          "WaitSynchronization with a timeout of INT64_MAX ns waits for the signal (it does not report a timeout at once)");
    h = spawn(c1b_waiter, NULL, 44, 1, 0);
    while (atomic_load(&g_c1b.phase) != 1) main_idle(100);
    main_idle(30000); early = atomic_load(&g_c1b.phase) == 2;
    mtx_lock(&g_c1b.mtx); atomic_store(&g_c1b.flag, 1); cv_signal(&g_c1b.key, 1); mtx_unlock(&g_c1b.mtx); join(h);
    check(!early && !atomic_load(&g_c1b.timed_out), "WaitProcessWideKeyAtomic with a timeout of INT64_MAX ns waits for the signal too");
    // 2
    thr_t *me = cur();
    dm_thread_native_wait(1); dm_thread_native_wait(1);
    int depth2 = me->native_wait == 2;
    if (depth2) { dm_thread_native_wait(0); dm_thread_native_wait(0); } else dm_thread_native_wait(0);   // (as it was: one leave is all the count has left, a second would abort)
    check(depth2 && me->native_wait == 0, "a native wait inside a native wait: depth 2 inside, 0 after both are left");
    // 3
    for (int with_stop = 0; with_stop < 2; with_stop++) {
        c3_t *c = &g_c3[with_stop]; new_event(&c->wh, &c->rh); new_event(&c->wh2, &c->rh2);
        uint32_t ht = spawn(c3_T, c, 30, 0, 0);
        for (;;) { pthread_mutex_lock(&G); thr_t *t = h_peek(ht, O_THREAD); int parked = t && t->wkind == 4; pthread_mutex_unlock(&G); if (parked) break; main_idle(100); }
        if (!with_stop) {   // 4: the state dump of this moment, read back
            fflush(stderr); int keep = dup(2), fd = memfd_create("dump", 0); dup2(fd, 2);
            svc_thread_dump();
            fflush(stderr); dup2(keep, 2); close(keep);
            static char txt[1 << 16]; ssize_t n = pread(fd, txt, sizeof txt - 1, 0); close(fd); txt[n > 0 ? n : 0] = 0;
            char want[96]; snprintf(want, sizeof want, "%#x wait: waiting", ht); char *w = strstr(txt, want), *e = w ? strchr(w, '\n') : NULL; if (e) *e = 0;
            snprintf(want, sizeof want, "handle %#x event", c->rh);
            g_dump_ok = w && strstr(w, "timeout none") && strstr(w, want) && strstr(w, "signalled 0");
            if (e) *e = '\n';
            g_dump_ok = g_dump_ok && strstr(txt, "history (age, svc(x0, x1) = result") && strstr(txt, " 0x18(");
            if (!g_dump_ok) printf("the dump:\n%s", txt);
        }
        pthread_t st; if (with_stop) pthread_create(&st, NULL, c3_stopper, c);
        uint32_t hw = spawn(c3_W, c, 50, 0, 0);
        void *sr = NULL; if (with_stop) { uint64_t gate[100] = {0}; thr_svc_enter(gate, 0); pthread_join(st, &sr); thr_svc_leave(); }
        join(hw);
        int64_t d = (int64_t)(atomic_load(&c->ret) - atomic_load(&c->woke));
        int held = atomic_load(&c->phase) == 2 && atomic_load(&c->ret) >= atomic_load(&c->blocked);
        if (with_stop) check(sr == NULL, "  (the stop during the handoff completed)");
        printf("     the waker's SignalEvent returned %.1f ms after the woken thread ran (it works 80 ms before it blocks)\n", d / 1e6);
        check(held, with_stop ? "the same handoff with every game thread stopped and resumed in the middle: the waker is still held until the woken thread blocks"
                              : "a same-core handoff: the waker of a higher-priority thread pinned to its core is held until that thread blocks");
        hsvc(0x11, c->wh2, 0); join(ht);
    }
    // 5
    new_event(&g_c5.wh, &g_c5.rh);
    atomic_store(&g_wall_ahead, 3600);
    h = spawn(c5_waiter, NULL, 44, 2, 0);
    for (int i = 0; i < 1000 && atomic_load(&g_c5.phase) != 3; i++) main_idle(1000);
    atomic_store(&g_wall_ahead, 0);
    int timely = atomic_load(&g_c5.phase) == 3;
    check(timely && atomic_load(&g_c5.r[0]) == 1 && atomic_load(&g_c5.r[1]) == R_TIMEOUT && atomic_load(&g_c5.r[2]) == R_TIMEOUT,
          "2 ms timed waits (condition variable, WaitSynchronization, WaitForAddress) time out on time though the wall clock is set back an hour meanwhile");
    if (!timely) { printf("     (the waiter is still in wait %d of 3, an hour from its timeout: stopping here)\nFAILED: %d\n", atomic_load(&g_c5.phase) + 1, g_fail); fflush(stdout); _exit(1); }
    join(h);
    check(g_dump_ok, "the state dump names a WaitSynchronization waiter's handle with its signalled state, how long it has waited, and the threads' last SVCs");
}

int main(int argc, char **argv) {
    int secs = 3, stops = 0;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--stops")) stops = 1; else secs = atoi(argv[i]); }
    { const char *e = getenv("SYNC_STALL_S"); if (e && atoi(e) > 0) g_stall_ns = (uint64_t)atoi(e) * 1000000000ull; }
    signal(SIGALRM, on_alarm); alarm((unsigned)secs + 120);
    setenv("DM_PREEMPT", "0", 1);
    svc_thread_main_init();
    part1();
    if (secs <= 0) { printf(g_fail ? "FAILED: %d\n" : "all passed%.0d\n", g_fail); fflush(stdout); _exit(g_fail ? 1 : 0); }

    static pool_t pools[3]; static actor_t actors[96]; int na = 0; static uint32_t th[160]; int nth = 0; uint64_t seed = 0x1234567;
    static evg_t evs[5]; static ewait_t ew[40]; int new_ = 0; pthread_t hostsig[5]; int nhost = 0;
    // A
    for (int k = 0; k < 3; k++) {
        pool_t *p = &pools[k]; snprintf(p->name, sizeof p->name, "pool %c", 'A' + k); atomic_store(&p->broadcast, k == 1);
        for (int i = 0; i < 9; i++) {
            actor_t *a = &actors[na++]; *a = (actor_t){p, k == 0 ? W_UNTIMED : k == 1 ? (i < 5 ? W_UNTIMED : W_NOISE) : (i < 3 ? W_UNTIMED : W_TIMED), 0, seed += 0x9e3779b97f4a7c15ull};
            th[nth++] = spawn(pool_worker, a, 40 + 4 * (i % 4), i % 3, i == 7 ? 7 : 0);   // priorities 40-52 over the three cores, one free to run on any
        }
        for (int i = 0; i < 4; i++) {
            actor_t *a = &actors[na++]; *a = (actor_t){p, 0, i & 1, seed += 0x9e3779b97f4a7c15ull};
            th[nth++] = spawn(pool_producer, a, i < 2 ? 54 : 38, i % 3, i == 3 ? 6 : 0);   // two below every consumer (held by the ones they wake on their core), two above most
        }
        duty(p->name, &p->pushed, &p->done);
    }
    static pool_t pairs[6];
    for (int k = 0; k < 6; k++) {   // one consumer, one producer; on two cores, or on one with the producer above the consumer (below it, the handoff would run them in turn)
        pool_t *p = &pairs[k]; snprintf(p->name, sizeof p->name, "pair %d", k);
        actor_t *a = &actors[na++]; *a = (actor_t){p, W_UNTIMED, 0, seed += 0x9e3779b97f4a7c15ull}; th[nth++] = spawn(pool_worker, a, 44, k % 3, 0);
        a = &actors[na++]; *a = (actor_t){p, 0, k & 1, seed += 0x9e3779b97f4a7c15ull}; th[nth++] = spawn(pair_producer, a, 40, k < 4 ? (k + 1) % 3 : k % 3, 0);
        duty(p->name, &p->pushed, &p->done);
    }
    // B
    struct { int autoclear, nw, host, cancel, multi; int tmo[3]; } cfg[5] = {
        {0, 1, 1, 0, 0, {0}},          // manual-clear, one waiter with no timeout, signalled from a host thread
        {1, 3, 1, 0, 0, {0, 1, 1}},    // auto-clear, three waiters, two with short timeouts, host thread
        {1, 2, 0, 1, 1, {0, 0}},       // auto-clear, two handles each, signalled from a game thread of lower priority on the waiters' core, with cancels
        {0, 2, 1, 1, 0, {2, 3}},       // manual-clear, zero-timeout polls and an endless timeout, host thread, with cancels
        {1, 1, 0, 0, 1, {1}},          // auto-clear, one waiter with short timeouts, game thread
    };
    for (int k = 0; k < 5; k++) {
        evg_t *g = &evs[k]; g->autoclear = cfg[k].autoclear; new_event(&g->wh, &g->rh); { uint32_t dw; new_event(&dw, &g->dummy); }
        snprintf(g->name, sizeof g->name, "event group %d", k);
        for (int i = 0; i < cfg[k].nw; i++) { ewait_t *w = &ew[new_++]; *w = (ewait_t){g, cfg[k].tmo[i], cfg[k].multi, seed += 0x9e3779b97f4a7c15ull};
            th[nth++] = g->waiter[g->nwaiter++] = spawn(ev_waiter, w, 36 + 4 * i, k % 3, 0); }
        if (cfg[k].cancel) { ewait_t *w = &ew[new_++]; *w = (ewait_t){g, 0, 0, seed += 0x9e3779b97f4a7c15ull}; th[nth++] = spawn(ev_canceller, w, 44, (k + 1) % 3, 0); }
        if (cfg[k].host) pthread_create(&hostsig[nhost++], NULL, ev_host_signaller, g);
        else { ewait_t *w = &ew[new_++]; *w = (ewait_t){g, 0, 0, seed += 0x9e3779b97f4a7c15ull}; th[nth++] = spawn(ev_guest_signaller, w, 56, k % 3, 0); }
        duty(g->name, &g->req, &g->ack);
    }
    // C
    for (int i = 0; i < 2; i++) th[nth++] = spawn(joiner, (void *)(uintptr_t)(0x77 + i * 2), 44 + 8 * i, i, 0);
    duty("thread exit", NULL, &g_joined);
    // D
    for (int i = 0; i < 4; i++) th[nth++] = spawn(sem_waiter, (void *)(uintptr_t)(0x100 + i), 40 + 4 * i, i % 3, 0);
    static uint32_t posters[2]; for (int i = 0; i < 2; i++) th[nth++] = posters[i] = spawn(sem_poster, (void *)(uintptr_t)(0x200 + i), 52, i, 0);
    duty("address arbiter", &g_sem.posted, &g_sem.got);

    pthread_t watch, stopper; pthread_create(&watch, NULL, watch_main, NULL);
    if (stops) pthread_create(&stopper, NULL, stopper_main, NULL);
    for (int s = 0; s < secs; s++) main_idle(1000000);
    atomic_store(&g_stop, 1);
    { uint64_t gate[100] = {0}; thr_svc_enter(gate, 0); for (int i = 0; i < nhost; i++) pthread_join(hostsig[i], NULL); thr_svc_leave(); }
    // everything asked for must be done, within the bound (the watcher fails a duty that stalls)
    for (int waiting = 1; waiting; ) {
        waiting = 0; main_idle(2000);
        for (int k = 0; k < 3; k++) waiting |= atomic_load(&pools[k].done) < atomic_load(&pools[k].pushed);
        for (int k = 0; k < 6; k++) waiting |= atomic_load(&pairs[k].done) < atomic_load(&pairs[k].pushed);
        for (int k = 0; k < 5; k++) waiting |= atomic_load(&evs[k].ack) < atomic_load(&evs[k].req);
        waiting |= atomic_load(&g_sem.got) < atomic_load(&g_sem.posted);
    }
    // end every thread and wait for it: the last wake-ups of all
    for (int k = 0; k < 9; k++) { pool_t *p = k < 3 ? &pools[k] : &pairs[k - 3]; mtx_lock(&p->mtx); atomic_store(&p->quit, 1); cv_signal(&p->key, -1); mtx_unlock(&p->mtx); }
    for (int k = 0; k < 5; k++) { atomic_store(&evs[k].quit, 1); SVC(0x11, evs[k].wh, 0, 0, 0); }
    atomic_store(&g_sem.quit, 1); atomic_store(&g_sem.val, 1 << 30); SVC(0x35, &g_sem.val, 0, 0, -1);
    for (int i = 0; i < nth; i++) join(th[i]);
    if (stops) { atomic_store(&g_stopper_quit, 1); uint64_t gate[100] = {0}; thr_svc_enter(gate, 0); pthread_join(stopper, NULL); thr_svc_leave(); }
    atomic_store(&g_watch_quit, 1); pthread_join(watch, NULL);

    uint64_t jobs = 0, sigs = 0; for (int k = 0; k < 3; k++) jobs += atomic_load(&pools[k].done); for (int k = 0; k < 6; k++) jobs += atomic_load(&pairs[k].done); for (int k = 0; k < 5; k++) sigs += atomic_load(&evs[k].ack);
    printf("     %d s: %llu jobs through the pools, %llu event signals answered, %llu threads joined, %llu address wake-ups, %llu checks of the parked threads, same-core handoffs %llu",
           secs, (unsigned long long)jobs, (unsigned long long)sigs, (unsigned long long)atomic_load(&g_joined), (unsigned long long)atomic_load(&g_sem.got),
           (unsigned long long)atomic_load(&g_inv_checks), (unsigned long long)atomic_load(&g_preempt_n));
    if (stops) printf(", stops of all threads %llu (given up %llu)", (unsigned long long)atomic_load(&g_stops_ok), (unsigned long long)atomic_load(&g_stops_gave_up));
    printf("\n");
    check(jobs > 0 && sigs > 0 && atomic_load(&g_joined) > 0 && atomic_load(&g_sem.got) > 0, "the storm: every job taken, every signal answered, every exit seen, no parked thread left behind a signal");
    if (stops) check(atomic_load(&g_stops_ok) > 0, "with stops of all threads throughout");
    printf(g_fail ? "FAILED: %d\n" : "all passed%.0d\n", g_fail);
    fflush(stdout); _exit(g_fail ? 1 : 0);
}
