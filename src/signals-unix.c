// This file is a part of Julia. License is MIT: http://julialang.org/license

#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#if defined(_OS_DARWIN_) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#ifdef MAC_OS_X_VERSION_10_9
#include <sys/_types/_ucontext64.h>
#else
#define __need_ucontext64_t
#include <machine/_structs.h>
#endif
#endif

// Figure out the best signals/timers to use for this platform
#ifdef __APPLE__ // Darwin's mach ports allow signal-free thread management
#define HAVE_MACH
#elif defined(__FreeBSD__) // generic bsd
#define HAVE_ITIMER
#else // generic linux
#define HAVE_TIMER
#endif

// 8M signal stack, same as default stack size and enough
// for reasonable finalizers.
// Should also be enough for parallel GC when we have it =)
#define sig_stack_size (8 * 1024 * 1024)

static bt_context_t *jl_to_bt_context(void *sigctx)
{
#ifdef __APPLE__
    return (bt_context_t*)&((ucontext64_t*)sigctx)->uc_mcontext64->__ss;
#else
    return (bt_context_t*)sigctx;
#endif
}

static int thread0_exit_count = 0;

static inline __attribute__((unused)) uintptr_t jl_get_rsp_from_ctx(const void *_ctx)
{
#if defined(_OS_LINUX_) && defined(_CPU_X86_64_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.gregs[REG_RSP];
#elif defined(_OS_LINUX_) && defined(_CPU_X86_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.gregs[REG_ESP];
#elif defined(_OS_LINUX_) && defined(_CPU_AARCH64_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.sp;
#elif defined(_OS_LINUX_) && defined(_CPU_ARM_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.arm_sp;
#elif defined(_OS_DARWIN_)
    const ucontext64_t *ctx = (const ucontext64_t*)_ctx;
    return ctx->uc_mcontext64->__ss.__rsp;
#else
    // TODO Add support for FreeBSD and PowerPC(64)?
    return 0;
#endif
}

static void jl_call_in_ctx(jl_ptls_t ptls, void (*fptr)(void), void *_ctx)
{
    // Modifying the ucontext should work but there is concern that
    // sigreturn orientated programming mitigation can work against us
    // by rejecting ucontext that is modified.
    // The current (staged) implementation in the Linux Kernel only
    // checks that the syscall is made in the signal handler and that
    // the ucontext address is valid. Hopefully the value of the ucontext
    // will not be part of the validation...
    uintptr_t rsp = (uintptr_t)ptls->signal_stack + sig_stack_size;
    assert(rsp % 16 == 0);
#if defined(_OS_LINUX_) && defined(_CPU_X86_64_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    rsp -= sizeof(void*);
    *(void**)rsp = NULL;
    ctx->uc_mcontext.gregs[REG_RSP] = rsp;
    ctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)fptr;
#elif defined(_OS_LINUX_) && defined(_CPU_X86_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    rsp -= sizeof(void*);
    *(void**)rsp = NULL;
    ctx->uc_mcontext.gregs[REG_ESP] = rsp;
    ctx->uc_mcontext.gregs[REG_EIP] = (uintptr_t)fptr;
#elif defined(_OS_LINUX_) && defined(_CPU_AARCH64_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    ctx->uc_mcontext.sp = rsp;
    ctx->uc_mcontext.regs[29] = 0; // Clear link register (x29)
    ctx->uc_mcontext.pc = (uintptr_t)fptr;
#elif defined(_OS_LINUX_) && defined(_CPU_ARM_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    ctx->uc_mcontext.arm_sp = rsp;
    ctx->uc_mcontext.arm_lr = 0; // Clear link register
    ctx->uc_mcontext.arm_pc = (uintptr_t)fptr;
#elif defined(_OS_DARWIN_)
    // Only used for SIGFPE.
    // This doesn't seems to be reliable when the SIGFPE is generated
    // from a divide-by-zero exception, which is now handled by
    // `catch_exception_raise`. It works fine when a signal is recieved
    // due to `kill`/`raise` though.
    ucontext64_t *ctx = (ucontext64_t*)_ctx;
    rsp -= sizeof(void*);
    *(void**)rsp = NULL;
    ctx->uc_mcontext64->__ss.__rsp = rsp;
    ctx->uc_mcontext64->__ss.__rip = (uintptr_t)fptr;
#else
    // TODO Add support for FreeBSD and PowerPC(64)?
    fptr();
#endif
}

static void jl_throw_in_ctx(jl_ptls_t ptls, jl_value_t *e, void *sigctx)
{
    if (!ptls->safe_restore)
        ptls->bt_size = rec_backtrace_ctx(ptls->bt_data, JL_MAX_BT_SIZE,
                                          jl_to_bt_context(sigctx));
    ptls->exception_in_transit = e;
    jl_call_in_ctx(ptls, &jl_rethrow, sigctx);
}

static pthread_t signals_thread;

static int is_addr_on_stack(jl_ptls_t ptls, void *addr)
{
#ifdef COPY_STACKS
    return ((char*)addr > (char*)ptls->stack_lo-3000000 &&
            (char*)addr < (char*)ptls->stack_hi);
#else
    return ((char*)addr > (char*)ptls->current_task->stkbuf &&
            (char*)addr < (char*)ptls->current_task->stkbuf + ptls->current_task->ssize);
#endif
}

void sigdie_handler(int sig, siginfo_t *info, void *context)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    sigset_t sset;
    uv_tty_reset_mode();
    jl_critical_error(sig, jl_to_bt_context(context),
                      ptls->bt_data, &ptls->bt_size);
    sigfillset(&sset);
    sigprocmask(SIG_UNBLOCK, &sset, NULL);
    signal(sig, SIG_DFL);
    if (sig != SIGSEGV &&
        sig != SIGBUS &&
        sig != SIGILL) {
        raise(sig);
    }
    // fall-through return to re-execute faulting statement (but without the error handler)
}

static void jl_unblock_signal(int sig)
{
    // Put in a separate function to save some stack space since
    // sigset_t can be pretty big.
    sigset_t sset;
    sigemptyset(&sset);
    sigaddset(&sset, sig);
    sigprocmask(SIG_UNBLOCK, &sset, NULL);
}

#if defined(HAVE_MACH)
#include <signals-mach.c>
#else

static int is_addr_on_sigstack(jl_ptls_t ptls, void *ptr)
{
    // One guard page for signal_stack.
    return !((char*)ptr < (char*)ptls->signal_stack - jl_page_size ||
             (char*)ptr > (char*)ptls->signal_stack + sig_stack_size);
}

static int jl_is_on_sigstack(jl_ptls_t ptls, void *ptr, void *context)
{
    return (is_addr_on_sigstack(ptls, ptr) &&
            is_addr_on_sigstack(ptls, (void*)jl_get_rsp_from_ctx(context)));
}

static void segv_handler(int sig, siginfo_t *info, void *context)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    assert(sig == SIGSEGV || sig == SIGBUS);

    if (jl_addr_is_safepoint((uintptr_t)info->si_addr)) {
        jl_unblock_signal(sig);
#ifdef JULIA_ENABLE_THREADING
        jl_set_gc_and_wait();
        // Do not raise sigint on worker thread
        if (ptls->tid != 0)
            return;
#endif
        if (ptls->defer_signal) {
            jl_safepoint_defer_sigint();
        }
        else if (jl_safepoint_consume_sigint()) {
            jl_clear_force_sigint();
            jl_throw_in_ctx(ptls, jl_interrupt_exception, context);
        }
        return;
    }
    if (ptls->safe_restore || is_addr_on_stack(ptls, info->si_addr)) { // stack overflow, or restarting jl_
        jl_unblock_signal(sig);
        jl_throw_in_ctx(ptls, jl_stackovf_exception, context);
    }
    else if (jl_is_on_sigstack(ptls, info->si_addr, context)) {
        // This mainly happens when one of the finalizers during final cleanup
        // on the signal stack has a deep/infinite recursion.
        // There isn't anything more we can do
        // (we are already corrupting that stack running this function)
        // so just call `_exit` to terminate immediately.
        jl_safe_printf("ERROR: Signal stack overflow, exit\n");
        _exit(sig + 128);
    }
    else if (sig == SIGSEGV && info->si_code == SEGV_ACCERR) {  // writing to read-only memory (e.g., mmap)
        jl_unblock_signal(sig);
        jl_throw_in_ctx(ptls, jl_readonlymemory_exception, context);
    }
    else {
#ifdef SEGV_EXCEPTION
        jl_unblock_signal(sig);
        jl_throw_in_ctx(ptls, jl_segv_exception, context);
#else
        sigdie_handler(sig, info, context);
#endif
    }
}

static void allocate_segv_handler(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = segv_handler;
    act.sa_flags = SA_ONSTACK | SA_SIGINFO;
    if (sigaction(SIGSEGV, &act, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    // On AArch64, stack overflow triggers a SIGBUS
    if (sigaction(SIGBUS, &act, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
}

static unw_context_t *volatile signal_context;
static pthread_mutex_t in_signal_lock;
static pthread_cond_t exit_signal_cond;
static pthread_cond_t signal_caught_cond;

static void jl_thread_suspend_and_get_state(int tid, unw_context_t **ctx)
{
    pthread_mutex_lock(&in_signal_lock);
    jl_ptls_t ptls2 = jl_all_tls_states[tid];
    jl_atomic_store_release(&ptls2->signal_request, 1);
    pthread_kill(ptls2->system_id, SIGUSR2);
    pthread_cond_wait(&signal_caught_cond, &in_signal_lock);  // wait for thread to acknowledge
    assert(jl_atomic_load_acquire(&ptls2->signal_request) == 0);
    *ctx = signal_context;
}

static void jl_thread_resume(int tid, int sig)
{
    (void)sig;
    jl_ptls_t ptls2 = jl_all_tls_states[tid];
    jl_atomic_store_release(&ptls2->signal_request, 1);
    pthread_cond_broadcast(&exit_signal_cond);
    pthread_cond_wait(&signal_caught_cond, &in_signal_lock); // wait for thread to acknowledge
    assert(jl_atomic_load_acquire(&ptls2->signal_request) == 0);
    pthread_mutex_unlock(&in_signal_lock);
}

// Throw jl_interrupt_exception if the master thread is in a signal async region
// or if SIGINT happens too often.
static void jl_try_deliver_sigint(void)
{
    jl_ptls_t ptls2 = jl_all_tls_states[0];
    jl_safepoint_enable_sigint();
    jl_wake_libuv();
    jl_atomic_store_release(&ptls2->signal_request, 2);
    // This also makes sure `sleep` is aborted.
    pthread_kill(ptls2->system_id, SIGUSR2);
}

// Write only by signal handling thread, read only by main thread
// no sync necessary.
static int thread0_exit_state = 0;
static void jl_exit_thread0_cb(void)
{
    // This can get stuck if it happens at an unfortunate spot
    // (unavoidable due to its async nature).
    // Try harder to exit each time if we get multiple exit requests.
    if (thread0_exit_count <= 1) {
        jl_exit(thread0_exit_state);
    }
    else if (thread0_exit_count == 2) {
        exit(thread0_exit_state);
    }
    else {
        _exit(thread0_exit_state);
    }
}

static void jl_exit_thread0(int state)
{
    jl_ptls_t ptls2 = jl_all_tls_states[0];
    thread0_exit_state = state;
    jl_atomic_store_release(&ptls2->signal_request, 3);
    pthread_kill(ptls2->system_id, SIGUSR2);
}

// request:
// 0: nothing
// 1: get state
// 2: throw sigint if `!defer_signal && io_wait` or if force throw threshold
//    is reached
// 3: exit with `thread0_exit_state`
void usr2_handler(int sig, siginfo_t *info, void *ctx)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    sig_atomic_t request = jl_atomic_exchange(&ptls->signal_request, 0);
    if (request == 1) {
        signal_context = jl_to_bt_context(ctx);

        pthread_mutex_lock(&in_signal_lock);
        pthread_cond_broadcast(&signal_caught_cond);
        pthread_cond_wait(&exit_signal_cond, &in_signal_lock);
        request = jl_atomic_exchange(&ptls->signal_request, 0);
        assert(request == 1);
        (void)request;
        pthread_cond_broadcast(&signal_caught_cond);
        pthread_mutex_unlock(&in_signal_lock);
    }
    else if (request == 2) {
        jl_unblock_signal(sig);
        int force = jl_check_force_sigint();
        if (force || (!ptls->defer_signal && ptls->io_wait)) {
            jl_safepoint_consume_sigint();
            if (force)
                jl_safe_printf("WARNING: Force throwing a SIGINT\n");
            // Force a throw
            jl_clear_force_sigint();
            jl_throw_in_ctx(ptls, jl_interrupt_exception, ctx);
        }
    }
    else if (request == 3) {
        jl_unblock_signal(sig);
        jl_call_in_ctx(ptls, jl_exit_thread0_cb, ctx);
    }
}

#if defined(HAVE_TIMER)
// Linux-style
#include <time.h>
#include <string.h>  // for memset

static timer_t timerprof;
static struct itimerspec itsprof;

JL_DLLEXPORT int jl_profile_start_timer(void)
{
    struct sigevent sigprof;

    // Establish the signal event
    memset(&sigprof, 0, sizeof(struct sigevent));
    sigprof.sigev_notify = SIGEV_SIGNAL;
    sigprof.sigev_signo = SIGUSR1;
    sigprof.sigev_value.sival_ptr = &timerprof;
    if (timer_create(CLOCK_REALTIME, &sigprof, &timerprof) == -1)
        return -2;

    // Start the timer
    itsprof.it_interval.tv_sec = nsecprof/GIGA;
    itsprof.it_interval.tv_nsec = nsecprof%GIGA;
    itsprof.it_value.tv_sec = nsecprof/GIGA;
    itsprof.it_value.tv_nsec = nsecprof%GIGA;
    if (timer_settime(timerprof, 0, &itsprof, NULL) == -1)
        return -3;

    running = 1;
    return 0;
}

JL_DLLEXPORT void jl_profile_stop_timer(void)
{
    if (running)
        timer_delete(timerprof);
    running = 0;
}

#elif defined(HAVE_ITIMER)
// BSD-style timers
#include <string.h>
#include <sys/time.h>
struct itimerval timerprof;

JL_DLLEXPORT int jl_profile_start_timer(void)
{
    timerprof.it_interval.tv_sec = nsecprof/GIGA;
    timerprof.it_interval.tv_usec = (nsecprof%GIGA)/1000;
    timerprof.it_value.tv_sec = nsecprof/GIGA;
    timerprof.it_value.tv_usec = (nsecprof%GIGA)/1000;
    if (setitimer(ITIMER_PROF, &timerprof, 0) == -1)
        return -3;

    running = 1;

    return 0;
}

JL_DLLEXPORT void jl_profile_stop_timer(void)
{
    if (running) {
        memset(&timerprof, 0, sizeof(timerprof));
        setitimer(ITIMER_PROF, &timerprof, 0);
    }
    running = 0;
}

#else

#error no profile tools available

#endif
#endif // HAVE_MACH

static void *alloc_sigstack(size_t size)
{
    size_t pagesz = jl_getpagesize();
    // Add one guard page to catch stack overflow in the signal handler
    size = LLT_ALIGN(size, pagesz) + pagesz;
    void *stackbuff = mmap(0, size, PROT_READ | PROT_WRITE,
                           MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stackbuff == MAP_FAILED)
        jl_errorf("fatal error allocating signal stack: mmap: %s",
                  strerror(errno));
    mprotect(stackbuff, pagesz, PROT_NONE);
    return (void*)((char*)stackbuff + pagesz);
}

void jl_install_thread_signal_handler(jl_ptls_t ptls)
{
    void *signal_stack = alloc_sigstack(sig_stack_size);
    stack_t ss;
    ss.ss_flags = 0;
    ss.ss_size = sig_stack_size;
    ss.ss_sp = signal_stack;
    if (sigaltstack(&ss, NULL) < 0) {
        jl_errorf("fatal error: sigaltstack: %s", strerror(errno));
    }

#if !defined(HAVE_MACH)
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = usr2_handler;
    act.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_RESTART;
    if (sigaction(SIGUSR2, &act, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#endif

    ptls->signal_stack = signal_stack;
}

void jl_sigsetset(sigset_t *sset)
{
    sigemptyset(sset);
    sigaddset(sset, SIGINT);
    sigaddset(sset, SIGTERM);
    sigaddset(sset, SIGABRT);
    sigaddset(sset, SIGQUIT);
#ifdef SIGINFO
    sigaddset(sset, SIGINFO);
#else
    sigaddset(sset, SIGUSR1);
#endif
#ifdef HAVE_ITIMER
    sigaddset(sset, SIGPROF);
#endif
}

static void *signal_listener(void *arg)
{
    static uintptr_t bt_data[JL_MAX_BT_SIZE + 1];
    static size_t bt_size = 0;
    sigset_t sset;
    unw_context_t *signal_context;
    int sig, critical, profile;
    int i;
    jl_sigsetset(&sset);
    while (1) {
        profile = 0;
        sigwait(&sset, &sig);
#ifndef HAVE_MACH
#  ifdef HAVE_ITIMER
        profile = (sig == SIGPROF);
#  else
        profile = (sig == SIGUSR1);
#  endif
#endif
        if (sig == SIGINT) {
            if (exit_on_sigint) {
                critical = 1;
            }
            else {
                if (!jl_ignore_sigint())
                    jl_try_deliver_sigint();
                continue;
            }
        }
        else {
            critical = 0;
        }

        critical |= (sig == SIGTERM);
        critical |= (sig == SIGABRT);
        critical |= (sig == SIGQUIT);
#ifdef SIGINFO
        critical |= (sig == SIGINFO);
#else
        critical |= (sig == SIGUSR1 && !profile);
#endif

        int doexit = critical;
#ifdef SIGINFO
        if (sig == SIGINFO)
            doexit = 0;
#else
        if (sig == SIGUSR1)
            doexit = 0;
#endif

        bt_size = 0;
        // sample each thread, round-robin style in reverse order
        // (so that thread zero gets notified last)
        for (i = jl_n_threads; i-- > 0; ) {
            // notify thread to stop
            jl_thread_suspend_and_get_state(i, &signal_context);

            // do backtrace on thread contexts for critical signals
            // this part must be signal-handler safe
            if (critical) {
                bt_size += rec_backtrace_ctx(bt_data + bt_size,
                        JL_MAX_BT_SIZE / jl_n_threads - 1,
                        signal_context);
                bt_data[bt_size++] = 0;
            }

            // do backtrace for profiler
            if (profile && running) {
                if (bt_size_cur < bt_size_max - 1) {
                    // Get backtrace data
                    bt_size_cur += rec_backtrace_ctx((uintptr_t*)bt_data_prof + bt_size_cur,
                            bt_size_max - bt_size_cur - 1, signal_context);
                    // Mark the end of this block with 0
                    bt_data_prof[bt_size_cur++] = 0;
                }
                if (bt_size_cur >= bt_size_max - 1) {
                    // Buffer full: Delete the timer
                    jl_profile_stop_timer();
                }
            }

            // notify thread to resume
            jl_thread_resume(i, sig);
        }

        // this part is async with the running of the rest of the program
        // and must be thread-safe, but not necessarily signal-handler safe
        if (critical) {
            jl_critical_error(sig, NULL, bt_data, &bt_size);
            if (doexit) {
                thread0_exit_count++;
                jl_exit_thread0(128 + sig);
            }
        }
    }
    return NULL;
}

void restore_signals(void)
{
    sigset_t sset;
    jl_sigsetset(&sset);
    sigprocmask(SIG_SETMASK, &sset, 0);

#if !defined(HAVE_MACH)
    if (pthread_mutex_init(&in_signal_lock, NULL) != 0 ||
        pthread_cond_init(&exit_signal_cond, NULL) != 0 ||
        pthread_cond_init(&signal_caught_cond, NULL) != 0) {
        jl_error("SIGUSR pthread init failed");
    }
#endif

    if (pthread_create(&signals_thread, NULL, signal_listener, NULL) != 0) {
        jl_error("pthread_create(signal_listener) failed");
    }
}

void fpe_handler(int sig, siginfo_t *info, void *context)
{
    (void)info;
    jl_ptls_t ptls = jl_get_ptls_states();
    jl_unblock_signal(sig);
    jl_throw_in_ctx(ptls, jl_diverror_exception, context);
}

void jl_install_default_signal_handlers(void)
{
    struct sigaction actf;
    memset(&actf, 0, sizeof(struct sigaction));
    sigemptyset(&actf.sa_mask);
    actf.sa_sigaction = fpe_handler;
    actf.sa_flags = SA_SIGINFO;
    if (sigaction(SIGFPE, &actf, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGPIPE");
    }
    if (signal(SIGTRAP, SIG_IGN) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGTRAP");
    }

    allocate_segv_handler();

    struct sigaction act_die;
    memset(&act_die, 0, sizeof(struct sigaction));
    sigemptyset(&act_die.sa_mask);
    act_die.sa_sigaction = sigdie_handler;
    act_die.sa_flags = SA_SIGINFO;
    if (sigaction(SIGILL, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    if (sigaction(SIGABRT, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    if (sigaction(SIGSYS, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    // need to ensure the following signals are not SIG_IGN, even though they will be blocked
    act_die.sa_flags = SA_SIGINFO | SA_RESTART;
#if defined(HAVE_ITIMER)
    if (sigaction(SIGPROF, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#endif
#ifdef SIGINFO
    if (sigaction(SIGINFO, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#else
    if (sigaction(SIGUSR1, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#endif
}

JL_DLLEXPORT void jl_install_sigint_handler(void)
{
    // TODO: ?
}
