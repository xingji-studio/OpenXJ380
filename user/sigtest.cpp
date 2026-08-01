#include "./xapi/include/x3api.h"

#define SIGUSR1 10

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1

typedef uint64_t sigset_t;
typedef void (*sighandler_t)(void);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

struct sigaction {
    sighandler_t  sa_handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

static volatile int g_seen_signal = 0;
static volatile int g_seen_count  = 0;

extern "C" void signal_restorer();
extern "C" __attribute__((naked)) void signal_restorer()
{
    __asm__ __volatile__(
        "movq $15, %rax\n\t"
        "syscall\n\t"
        "1:\n\t"
        "pause\n\t"
        "jmp 1b\n\t");
}

extern "C" void sigusr1_handler(int sig)
{
    g_seen_signal = sig;
    g_seen_count++;
}

static int64_t raw_syscall(uint64_t nr,
                           uint64_t arg1 = 0,
                           uint64_t arg2 = 0,
                           uint64_t arg3 = 0,
                           uint64_t arg4 = 0,
                           uint64_t arg5 = 0,
                           uint64_t arg6 = 0)
{
    return (int64_t)enter_syscall(nr, arg1, arg2, arg3, arg4, arg5, arg6);
}

static void log_step(const char *name, bool ok)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "sigtest：%s：%s\n", name, ok ? "通过" : "失败");
    xapi_OutputSerial(buf);
}

static bool expect_ret(const char *name, int64_t ret)
{
    bool ok = ret >= 0;
    if (!ok)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "sigtest：%s 失败 ret=%lld\n", name, (long long)ret);
        xapi_OutputSerial(buf);
    }
    log_step(name, ok);
    return ok;
}

static int sigtest_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    xapi_OutputSerial((char *)"sigtest：开始\n");

    struct sigaction act;
    struct sigaction oldact;
    memset(&act, 0, sizeof(act));
    memset(&oldact, 0, sizeof(oldact));
    act.sa_handler  = (sighandler_t)sigusr1_handler;
    act.sa_restorer = signal_restorer;
    act.sa_mask     = 0;

    bool ok = true;
    int64_t ret = raw_syscall(SYS_RT_SIGACTION,
                              SIGUSR1,
                              (uint64_t)&act,
                              (uint64_t)&oldact,
                              sizeof(sigset_t));
    ok &= expect_ret("rt_sigaction", ret);

    sigset_t block_usr1 = (1ULL << SIGUSR1);
    sigset_t oldmask    = 0;
    ret = raw_syscall(SYS_RT_SIGPROCMASK, SIG_BLOCK, (uint64_t)&block_usr1, (uint64_t)&oldmask);
    ok &= expect_ret("block SIGUSR1", ret);

    int64_t pid = raw_syscall(SYS_GETPID);
    ok &= expect_ret("getpid", pid);

    ret = raw_syscall(SYS_KILL, (uint64_t)pid, SIGUSR1);
    ok &= expect_ret("kill self while blocked", ret);

    raw_syscall(SYS_GETPID);
    bool still_blocked = (g_seen_count == 0);
    log_step("pending signal not delivered while blocked", still_blocked);
    ok &= still_blocked;

    ret = raw_syscall(SYS_RT_SIGPROCMASK, SIG_UNBLOCK, (uint64_t)&block_usr1, 0);
    ok &= expect_ret("unblock SIGUSR1", ret);

    bool delivered_after_unblock = (g_seen_signal == SIGUSR1 && g_seen_count == 1);
    log_step("deliver after unblock", delivered_after_unblock);
    ok &= delivered_after_unblock;

    ret = raw_syscall(SYS_KILL, (uint64_t)pid, SIGUSR1);
    ok &= expect_ret("kill self unblocked", ret);

    raw_syscall(SYS_GETPID);
    bool delivered_again = (g_seen_signal == SIGUSR1 && g_seen_count == 2);
    log_step("handler returns and can run again", delivered_again);
    ok &= delivered_again;

    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    ret = raw_syscall(SYS_RT_SIGACTION, SIGUSR1, (uint64_t)&ign, 0, sizeof(sigset_t));
    ok &= expect_ret("set SIG_IGN", ret);

    ret = raw_syscall(SYS_KILL, (uint64_t)pid, SIGUSR1);
    ok &= expect_ret("kill self ignored", ret);

    raw_syscall(SYS_GETPID);
    bool ignored = (g_seen_count == 2);
    log_step("SIG_IGN suppresses handler", ignored);
    ok &= ignored;

    ret = raw_syscall(SYS_RT_SIGACTION, SIGUSR1, (uint64_t)&act, 0, sizeof(sigset_t));
    ok &= expect_ret("restore SIGUSR1 handler", ret);

    ret = raw_syscall(SYS_KILL, (uint64_t)pid, SIGUSR1);
    ok &= expect_ret("kill self after restore", ret);

    raw_syscall(SYS_GETPID);
    bool restored = (g_seen_signal == SIGUSR1 && g_seen_count == 3);
    log_step("restored handler runs", restored);
    ok &= restored;

    uint64_t child = fork();
    if ((int64_t)child < 0)
    {
        ok = false;
        log_step("fork for default action", false);
    }
    else if (child == 0)
    {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        raw_syscall(SYS_RT_SIGACTION, SIGUSR1, (uint64_t)&dfl, 0, sizeof(sigset_t));
        raw_syscall(SYS_KILL, (uint64_t)raw_syscall(SYS_GETPID), SIGUSR1);
        raw_syscall(SYS_GETPID);
        exit(77);
    }
    else
    {
        int status = -1;
        int waited = waitpid((int)child, &status, 0);
        bool default_term = (waited == (int)child && WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGUSR1);
        if (!default_term)
        {
            char buf[160];
            snprintf(buf,
                     sizeof(buf),
                     "sigtest：默认动作状态 waited=%d child=%llu status=0x%x code=%d\n",
                     waited,
                     (unsigned long long)child,
                     (unsigned int)status,
                     WEXITSTATUS(status));
            xapi_OutputSerial(buf);
        }
        log_step("default SIGUSR1 terminates child as exit 138", default_term);
        ok &= default_term;
    }

    xapi_OutputSerial(ok ? (char *)"sigtest：通过\n" : (char *)"sigtest：失败\n");
    return ok ? 0 : 1;
}

extern "C" int sigtest_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int sigtest_main_cpp(int argc, char *argv[], char *envp[])
{
    return sigtest_main_impl(argc, argv, envp);
}
