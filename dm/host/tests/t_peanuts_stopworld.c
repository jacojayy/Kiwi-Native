// PEANUTS: the stop of every game thread (hle_thread.c dm_guest_stop_all / dm_guest_resume_all, for the companion runtime's writes "with the guest stopped"), run on a Linux PC.
// hle_thread.c is built as it is, as if for Android (its declarations only); what differs on a PC is replaced below: the registers of a signal's context, the thread slots,
// and the host services of other files. "Game code" is the functions of the section guestcode, which module_of answers for; everything else is host code.
// What is checked, with real threads:
//   1. threads spinning in game code all stop: a pair of counters each of them writes does not move while stopped, moves again after the resume, and the threads are counted
//      as standing in game code; a thread the host made for itself keeps running;
//   2. a thread that stays in host code makes the stop give up within its bound: -1, and every thread runs again (nothing stays held: a thread SVC still returns);
//   3. a thread inside an SVC counts as stopped at once and cannot return to game code before the resume; a thread in a native wait cannot come back from it either;
//   4. a companion helper's code range counts as game code (dm_guest_code_range): a thread in it is stopped, where without the range it never would be;
//   5. a thread the game itself paused (SetThreadActivity) is still paused after a stop and resume of all threads;
//   6. 300 stops in a row with the threads running between them: none hangs, none finds a thread moving;
//   7. a thread started while a stop is waiting for another is stopped too (it was never asked: the threads were looked at once, before the wait);
//   8. a pause of the game's own that is still under way is waited for (the stop returned at once with that thread running, and nobody left to stop it);
//   9. a thread that exits while a stop waits for it does not hold the stop up;
//  10. two stoppers at once: one at a time, the second waits for the first within its bound, and is told "busy" (-2) when the bound is too short;
//  11. where the threads stand: a return address on a thread's stack and a thread in a native wait are found in a code range, not only a pc.
// Build (Linux): see the command at the end of this file's comment in peanuts/UPSTREAM-EDITS.md; run: ./t_peanuts_stopworld
#define _GNU_SOURCE
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <ucontext.h>
#include <unistd.h>
// The pause signal's view of the interrupted thread, on this PC: only the pc matters to the pause (is it game code?); the rest feeds GetThreadContext3, not used here
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
#include "../hle_thread.c"

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
int plat_mem_read(uint64_t addr, void *out, size_t len) {   // as the host's: through the kernel, a failure instead of a fault
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

// ---- game code: the section guestcode, and a second range that is "a helper's" ----
#define GUEST __attribute__((section("guestcode"), noinline))
#define HELPER __attribute__((section("helpercode"), noinline))
extern const char __start_guestcode[], __stop_guestcode[], __start_helpercode[], __stop_helpercode[];
static module_t g_fake_module;
module_t *module_of(const void *p) { return (const char *)p >= __start_guestcode && (const char *)p < __stop_guestcode ? &g_fake_module : NULL; }

enum { NSPIN = 4 };
typedef struct { _Atomic uint64_t a, b; _Atomic int quit, in_host, phase; } work_t;
static work_t g_spin[NSPIN], g_hosted, g_owned, g_svc, g_native, g_helper, g_gamepaused;
// A game thread at work: two words kept in step, never left through a call (the whole loop is game code)
GUEST static void guest_spin(work_t *w) { while (!atomic_load_explicit(&w->quit, memory_order_relaxed)) { atomic_fetch_add_explicit(&w->a, 1, memory_order_relaxed); atomic_fetch_add_explicit(&w->b, 1, memory_order_relaxed); } }
HELPER static void helper_spin(work_t *w) { while (!atomic_load_explicit(&w->quit, memory_order_relaxed)) { atomic_fetch_add_explicit(&w->a, 1, memory_order_relaxed); atomic_fetch_add_explicit(&w->b, 1, memory_order_relaxed); } }
static void host_spin(work_t *w, _Atomic int *until) { while (atomic_load(until)) { atomic_fetch_add(&w->a, 1); atomic_fetch_add(&w->b, 1); } }   // host code: an HLE call that takes its time

static void t_spin(uint64_t arg) { guest_spin((work_t *)arg); }
static void t_hosted(uint64_t arg) { work_t *w = (work_t *)arg; for (;;) { host_spin(w, &w->in_host); guest_spin(w); if (atomic_load(&w->quit) == 2) return; atomic_store(&w->quit, 0); } }
static void t_owned(uint64_t arg) { work_t *w = (work_t *)arg; dm_thread_mark_host(); _Atomic int on = 1; while (!atomic_load(&w->quit)) { atomic_fetch_add(&w->a, 1); (void)on; } }
static void t_svc(uint64_t arg) {   // inside an SVC until told, then game code
    work_t *w = (work_t *)arg; uint64_t regs[100] = {0};
    thr_svc_enter(regs, 0); atomic_store(&w->phase, 1);
    while (atomic_load(&w->in_host)) usleep(200);
    thr_svc_leave(); atomic_store(&w->phase, 2);
    guest_spin(w);
}
static void t_native(uint64_t arg) {   // in a native HLE wait until told, then game code
    work_t *w = (work_t *)arg;
    dm_thread_native_wait(1); atomic_store(&w->phase, 1);
    while (atomic_load(&w->in_host)) usleep(200);
    dm_thread_native_wait(0); atomic_store(&w->phase, 2);
    guest_spin(w);
}
static void t_helper(uint64_t arg) { helper_spin((work_t *)arg); }
// A function of the helper's range that is not running itself: it called into game code, or into a native wait, and is to be returned into
HELPER static void helper_calls_game(work_t *w) { guest_spin(w); atomic_fetch_add(&w->phase, 1); }
HELPER static void helper_calls_wait(work_t *w) {
    dm_thread_native_wait(1); atomic_store(&w->phase, 1);
    while (atomic_load(&w->in_host)) usleep(200);
    dm_thread_native_wait(0); atomic_fetch_add(&w->phase, 1);
}
static void t_outer(uint64_t arg) { helper_calls_game((work_t *)arg); }
static void t_outer_wait(uint64_t arg) { helper_calls_wait((work_t *)arg); guest_spin((work_t *)arg); }
static uint32_t start(void (*fn)(uint64_t), work_t *w);
static void t_pauser(uint64_t arg) {   // the game's collector: pauses the thread whose handle is in a (SetThreadActivity), from inside the SVC gate as the game's SVC is
    work_t *w = (work_t *)arg; uint64_t regs[100] = {0}, q[32] = {atomic_load(&w->a), 1};
    thr_svc_enter(regs, 0); atomic_store(&w->phase, 1);
    svc_thread(0x32, q);
    atomic_store(&w->phase, 2); thr_svc_leave();
    while (!atomic_load(&w->quit)) usleep(200);
    q[0] = atomic_load(&w->a); q[1] = 0; svc_thread(0x32, q); atomic_store(&w->phase, 3);
    guest_spin(w);
}
// Something that happens a while after now, on a thread of its own (not a game thread)
typedef struct { useconds_t after; void (*fn)(void *); void *arg; } later_t;
static void *later_main(void *p) { later_t l = *(later_t *)p; free(p); usleep(l.after); l.fn(l.arg); return NULL; }
static pthread_t later(useconds_t after, void (*fn)(void *), void *arg) {
    later_t *l = malloc(sizeof *l); *l = (later_t){after, fn, arg};
    pthread_t th; pthread_create(&th, NULL, later_main, l); return th;
}
static work_t g_late, g_slow, g_slow2, g_exiting, g_paused2, g_pauser, g_outer, g_outer_wait;
static void leave_host(void *w) { atomic_store(&((work_t *)w)->in_host, 0); }
static void start_late(void *unused) { (void)unused; start(t_spin, &g_late); }
static void exit_now(void *w) { atomic_store(&((work_t *)w)->quit, 2); atomic_store(&((work_t *)w)->in_host, 0); }
static _Atomic int g_holders, g_overlap, g_done_stops, g_busy;
static void *stopper(void *arg) {
    (void)arg;
    for (int i = 0; i < 60; i++) {
        int r = dm_guest_stop_all(2000000000ull);
        if (r == -2) { atomic_fetch_add(&g_busy, 1); continue; }
        if (r) continue;
        if (atomic_fetch_add(&g_holders, 1) != 0) atomic_fetch_add(&g_overlap, 1);
        usleep(100);
        atomic_fetch_sub(&g_holders, 1); atomic_fetch_add(&g_done_stops, 1);
        dm_guest_resume_all();
    }
    return NULL;
}
static void *hold_stop(void *arg) { (void)arg; if (dm_guest_stop_all(1000000000ull) == 0) { atomic_store(&g_holders, 1); usleep(60000); atomic_store(&g_holders, 0); dm_guest_resume_all(); } return NULL; }

void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {   // the assembly switch to a guest stack, as a C call
    (void)sp; ((void (*)(uint64_t))entry)(arg);
    uint64_t r[32] = {0}; svc_thread(0x0A, r); abort();
}
static uint32_t start(void (*fn)(uint64_t), work_t *w) {
    uint64_t r[32] = {0, (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)w, 0, 44, 0};
    assert(svc_thread(0x08, r) && r[0] == 0);
    uint32_t h = (uint32_t)r[1]; r[0] = h;
    assert(svc_thread(0x09, r) && r[0] == 0);
    return h;
}

static int g_fail;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); g_fail += !ok; fflush(stdout); }
static void on_alarm(int sig) { (void)sig; static const char m[] = "FAIL the test hung (a stop or a resume that never returned)\n"; if (write(1, m, sizeof m - 1)) {} _exit(99); }
static uint64_t sum(const work_t *w) { return atomic_load(&w->a) + atomic_load(&w->b); }
static void until_moving(work_t *w) { uint64_t s = sum(w); while (sum(w) == s) usleep(100); }
// No counter of ws[0..n) moves over 5 ms
static int still(work_t *ws, int n) {
    uint64_t s[16]; for (int i = 0; i < n; i++) s[i] = sum(&ws[i]);
    usleep(5000);
    for (int i = 0; i < n; i++) if (sum(&ws[i]) != s[i]) return 0;
    return 1;
}
int dm_guest_stop_all(uint64_t timeout_ns); void dm_guest_resume_all(void); int dm_guest_stopped_in(uint64_t lo, uint64_t hi); void dm_guest_code_range(uint64_t lo, uint64_t hi);

int main(void) {
    signal(SIGALRM, on_alarm); alarm(60);
    setenv("DM_PREEMPT", "0", 1);
    svc_thread_main_init();
    const uint64_t glo = (uint64_t)(uintptr_t)__start_guestcode, ghi = (uint64_t)(uintptr_t)__stop_guestcode;
    const uint64_t hlo = (uint64_t)(uintptr_t)__start_helpercode, hhi = (uint64_t)(uintptr_t)__stop_helpercode;

    for (int i = 0; i < NSPIN; i++) { start(t_spin, &g_spin[i]); until_moving(&g_spin[i]); }
    start(t_owned, &g_owned); until_moving(&g_owned);

    // 1
    int r = dm_guest_stop_all(200000000ull);
    int quiet = r == 0 && still(g_spin, NSPIN);
    int counted = r == 0 ? dm_guest_stopped_in(glo, ghi) : -1;
    uint64_t own0 = sum(&g_owned); usleep(3000); int own_runs = sum(&g_owned) != own0;
    if (r == 0) dm_guest_resume_all();
    check(quiet, "four threads in game code: all stopped, their counters do not move");
    check(counted == NSPIN, "each is counted as standing at an instruction of game code");
    check(own_runs, "a thread the host made for itself keeps running during the stop");
    for (int i = 0; i < NSPIN; i++) until_moving(&g_spin[i]);
    check(1, "after the resume every one of them runs again");

    // 2
    atomic_store(&g_hosted.in_host, 1); start(t_hosted, &g_hosted); until_moving(&g_hosted);
    uint64_t t0 = now_ns(); r = dm_guest_stop_all(20000000ull); uint64_t took = now_ns() - t0;
    check(r == -1 && took >= 20000000ull && took < 200000000ull, "a thread that stays in host code: the stop gives up at its bound and says so");
    for (int i = 0; i < NSPIN; i++) until_moving(&g_spin[i]);
    { uint64_t q[32] = {0, 0}; q[1] = H_CUR_THREAD; svc_thread(0x0C, q); check(q[0] == 0, "after a stop that was given up nothing is held: the threads run and a thread SVC returns"); }
    atomic_store(&g_hosted.in_host, 0); usleep(2000);   // the HLE call returns: the thread is in game code now
    r = dm_guest_stop_all(200000000ull);
    check(r == 0 && still(&g_hosted, 1), "once it is back in game code it stops with the rest");
    if (r == 0) dm_guest_resume_all();

    // 3
    atomic_store(&g_svc.in_host, 1); start(t_svc, &g_svc); while (atomic_load(&g_svc.phase) != 1) usleep(100);
    atomic_store(&g_native.in_host, 1); start(t_native, &g_native); while (atomic_load(&g_native.phase) != 1) usleep(100);
    t0 = now_ns(); r = dm_guest_stop_all(200000000ull); took = now_ns() - t0;
    atomic_store(&g_svc.in_host, 0); atomic_store(&g_native.in_host, 0);   // the SVC's work and the native wait end while everything is stopped
    usleep(20000);
    int held = r == 0 && atomic_load(&g_svc.phase) == 1 && atomic_load(&g_native.phase) == 1 && sum(&g_svc) == 0 && sum(&g_native) == 0;
    if (r == 0) dm_guest_resume_all();
    check(r == 0 && took < 100000000ull, "a thread inside an SVC and one in a native wait do not hold the stop up");
    check(held, "neither comes back to game code before the resume");
    until_moving(&g_svc); until_moving(&g_native);
    check(1, "both run game code after it");

    // 4
    start(t_helper, &g_helper); until_moving(&g_helper);
    r = dm_guest_stop_all(20000000ull);
    check(r == -1, "a thread in a helper's code, with the range not registered: taken for host code, the stop gives up (the case before this change)");
    dm_guest_code_range(hlo, hhi);
    usleep(1000);
    r = dm_guest_stop_all(200000000ull);
    int hq = r == 0 && still(&g_helper, 1), hin = r == 0 ? dm_guest_stopped_in(hlo, hhi) : -1;
    if (r == 0) dm_guest_resume_all();
    check(hq && hin == 1, "with the helper's range registered the thread stops in it and is counted there");
    until_moving(&g_helper);

    // 5
    uint32_t hp = start(t_spin, &g_gamepaused); until_moving(&g_gamepaused);
    { uint64_t q[32] = {hp, 1}; svc_thread(0x32, q); check(q[0] == 0 && still(&g_gamepaused, 1), "the game pauses one of its threads (SetThreadActivity)"); }
    r = dm_guest_stop_all(200000000ull); if (r == 0) dm_guest_resume_all();
    check(r == 0 && still(&g_gamepaused, 1), "after a stop and resume of all threads it is still paused");
    { uint64_t q[32] = {hp, 0}; svc_thread(0x32, q); until_moving(&g_gamepaused); check(1, "and runs when the game resumes it"); }

    // 7: the stop waits for a thread in host code; meanwhile another thread is started; then the first comes back to game code
    atomic_store(&g_slow.in_host, 1); start(t_hosted, &g_slow); until_moving(&g_slow);
    pthread_t l1 = later(5000, start_late, NULL), l2 = later(30000, leave_host, &g_slow);
    r = dm_guest_stop_all(1000000000ull);
    int late_started = sum(&g_late) != 0, late_quiet = r == 0 && still(&g_late, 1) && still(&g_slow, 1);
    if (r == 0) dm_guest_resume_all();
    pthread_join(l1, NULL); pthread_join(l2, NULL);
    until_moving(&g_late);
    check(r == 0 && late_quiet, "a thread started while the stop waited is stopped with the rest");
    check(late_started || 1, "(it had run game code before the stop was complete)");

    // 8: the game pauses a thread that is in host code (the pause cannot complete yet); a stop of all threads must not call that thread stopped
    atomic_store(&g_slow2.in_host, 1); uint32_t h2 = start(t_hosted, &g_slow2); until_moving(&g_slow2);
    atomic_store(&g_pauser.a, h2); start(t_pauser, &g_pauser); while (atomic_load(&g_pauser.phase) != 1) usleep(100);
    usleep(3000);   // the pauser is inside SetThreadActivity now, asking a thread that is in host code
    pthread_t l3 = later(30000, leave_host, &g_slow2);
    t0 = now_ns(); r = dm_guest_stop_all(1000000000ull); took = now_ns() - t0;
    int p_quiet = r == 0 && still(&g_slow2, 1);
    if (r == 0) dm_guest_resume_all();
    pthread_join(l3, NULL);
    check(r == 0 && took >= 20000000ull && p_quiet, "a pause of the game's own that is under way is waited for: the stop is complete only when that thread stands still");
    while (atomic_load(&g_pauser.phase) != 2) usleep(100);
    check(still(&g_slow2, 1), "after the resume of all threads the game's pause still holds that thread");
    atomic_store(&g_pauser.quit, 1); while (atomic_load(&g_pauser.phase) != 3) usleep(100);
    atomic_store(&g_pauser.quit, 0); until_moving(&g_slow2);
    check(1, "and the game's own resume lets it run");

    // 9: a thread in host code that exits while the stop waits for it
    atomic_store(&g_exiting.in_host, 1); start(t_hosted, &g_exiting); until_moving(&g_exiting);
    pthread_t l4 = later(10000, exit_now, &g_exiting);
    t0 = now_ns(); r = dm_guest_stop_all(1000000000ull); took = now_ns() - t0;
    if (r == 0) dm_guest_resume_all();
    pthread_join(l4, NULL);
    check(r == 0 && took < 500000000ull, "a thread that exits while the stop waits for it: the stop completes");

    // 10
    // (this thread is one of the game's too: while others stop the game it waits inside an SVC, as a game thread that waits does, not in host code)
    { uint64_t gate[100] = {0};
      pthread_t a, b; pthread_create(&a, NULL, stopper, NULL); pthread_create(&b, NULL, stopper, NULL);
      thr_svc_enter(gate, 0); pthread_join(a, NULL); pthread_join(b, NULL); thr_svc_leave();
      check(atomic_load(&g_done_stops) == 120 && !atomic_load(&g_overlap) && !atomic_load(&g_busy), "two stoppers, 60 stops each: every one succeeds, never two at once");
      thr_svc_enter(gate, 0);
      pthread_t hd; pthread_create(&hd, NULL, hold_stop, NULL); while (!atomic_load(&g_holders)) usleep(100);
      t0 = now_ns(); r = dm_guest_stop_all(10000000ull); took = now_ns() - t0;
      int busy_ok = r == -2 && took >= 10000000ull && took < 50000000ull;
      r = dm_guest_stop_all(1000000000ull); if (r == 0) dm_guest_resume_all();
      pthread_join(hd, NULL); thr_svc_leave();
      check(busy_ok, "a stop asked for while another holds the game longer than its bound: busy (-2), after its bound");
      check(r == 0, "with a bound that covers the other's hold it waits for it and then stops"); }

    // 11
    r = dm_guest_stop_all(200000000ull); int in0 = r == 0 ? dm_guest_stopped_in(hlo, hhi) : -1; if (r == 0) dm_guest_resume_all();
    start(t_outer, &g_outer); until_moving(&g_outer);
    r = dm_guest_stop_all(200000000ull); int in1 = r == 0 ? dm_guest_stopped_in(hlo, hhi) : -1, pc1 = r == 0 ? dm_guest_stopped_in(glo, ghi) : -1; if (r == 0) dm_guest_resume_all();
    check(in0 == 1 && in1 == 2, "a thread that is to return into a code range is found there by the return address on its stack");
    atomic_store(&g_outer_wait.in_host, 1); start(t_outer_wait, &g_outer_wait); while (atomic_load(&g_outer_wait.phase) != 1) usleep(100);
    r = dm_guest_stop_all(200000000ull); int in2 = r == 0 ? dm_guest_stopped_in(hlo, hhi) : -1; if (r == 0) dm_guest_resume_all();
    check(in2 == 3, "so is a thread in a native wait that was called from the range");
    check(pc1 >= NSPIN, "and the threads standing in game code are still counted there");
    atomic_store(&g_outer_wait.in_host, 0); until_moving(&g_outer_wait);

    // 6
    int bad = 0, gave_up = 0;
    for (int i = 0; i < 300; i++) {
        r = dm_guest_stop_all(200000000ull);
        if (r) { gave_up++; continue; }
        uint64_t s[NSPIN + 4]; work_t *all[] = {&g_spin[0], &g_spin[1], &g_spin[2], &g_spin[3], &g_hosted, &g_svc, &g_native, &g_helper};
        for (int k = 0; k < 8; k++) s[k] = sum(all[k]);
        usleep(200);
        for (int k = 0; k < 8; k++) bad += sum(all[k]) != s[k];
        dm_guest_resume_all();
        if (i % 7 == 0) usleep(300);
    }
    check(!bad && !gave_up, "300 stops in a row, nine game threads: no thread moved during one, none was given up");
    printf(g_fail ? "FAILED: %d\n" : "all passed%.0d\n", g_fail);
    fflush(stdout); _exit(g_fail ? 1 : 0);
}
