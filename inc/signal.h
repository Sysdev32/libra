#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stddef.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stdint.h>
#else
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long      uintptr_t;
#endif

/* --- Signal Definitions --- */

/* Standard POSIX Signals */
#define SIGHUP     1   /* Hangup */
#define SIGINT     2   /* Interrupt (Ctrl+C) */
#define SIGQUIT    3   /* Quit */
#define SIGILL     4   /* Illegal Instruction */
#define SIGTRAP    5   /* Trace/breakpoint trap */
#define SIGABRT    6   /* Abort signal */
#define SIGIOT     6   /* IOT trap (alias for SIGABRT) */
#define SIGBUS     7   /* Bus error */
#define SIGFPE     8   /* Floating point exception */
#define SIGKILL    9   /* Kill (cannot be caught or ignored) */
#define SIGUSR1   10   /* User-defined signal 1 */
#define SIGSEGV   11   /* Invalid memory reference */
#define SIGUSR2   12   /* User-defined signal 2 */
#define SIGPIPE   13   /* Broken pipe */
#define SIGALRM   14   /* Alarm clock */
#define SIGTERM   15   /* Termination signal */
#define SIGSTKFLT 16   /* Stack fault on coprocessor */
#define SIGCHLD   17   /* Child stopped or terminated */
#define SIGCONT   18   /* Continue if stopped */
#define SIGSTOP   19   /* Stop process (cannot be caught or ignored) */
#define SIGTSTP   20   /* Stop typed at terminal */
#define SIGTTIN   21   /* Terminal input for background process */
#define SIGTTOU   22   /* Terminal output for background process */
#define SIGURG    23   /* Urgent condition on socket */
#define SIGXCPU   24   /* CPU time limit exceeded */
#define SIGXFSZ   25   /* File size limit exceeded */
#define SIGVTALRM 26   /* Virtual timer expired */
#define SIGPROF   27   /* Profiling timer expired */
#define SIGWINCH  28   /* Window resize signal */
#define SIGIO     29   /* I/O now possible */
#define SIGPOLL   SIGIO
#define SIGPWR    30   /* Power failure */
#define SIGSYS    31   /* Bad system call */

#define _NSIG     64   /* Total number of signals supported */

/* --- Default Signal Handlers --- */

typedef void (*sighandler_t)(int);

#define SIG_ERR ((sighandler_t)-1) /* Error return value */
#define SIG_DFL ((sighandler_t)0)  /* Default signal action */
#define SIG_IGN ((sighandler_t)1)  /* Ignore signal */

/* --- Signal Sets & Mask Types --- */

typedef uint64_t sigset_t; /* Simple 64-bit mask for signals 1..64 */

/* Signal Action Flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* How flags for sigprocmask */
#define SIG_BLOCK   0  /* Block set of signals */
#define SIG_UNBLOCK 1  /* Unblock set of signals */
#define SIG_SETMASK 2  /* Set signal mask directly */

/* --- Structures --- */

typedef struct {
    int sival_int;
    void *sival_ptr;
} sigval_t;

typedef struct {
    int si_signo;
    int si_code;
    int si_errno;
    int si_pid;
    int si_uid;
    void *si_addr;
    sigval_t si_value;
} siginfo_t;

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } _u;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#define sa_handler   _u.sa_handler
#define sa_sigaction _u.sa_sigaction

/* --- Inline Helper Utilities for sigset_t --- */

static inline int sigemptyset(sigset_t *set) {
    if (!set) return -1;
    *set = 0;
    return 0;
}

static inline int sigfillset(sigset_t *set) {
    if (!set) return -1;
    *set = ~0ULL;
    return 0;
}

static inline int sigaddset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum > _NSIG) return -1;
    *set |= (1ULL << (signum - 1));
    return 0;
}

static inline int sigdelset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum > _NSIG) return -1;
    *set &= ~(1ULL << (signum - 1));
    return 0;
}

static inline int sigismember(const sigset_t *set, int signum) {
    if (!set || signum < 1 || signum > _NSIG) return -1;
    return (*set & (1ULL << (signum - 1))) ? 1 : 0;
}

/* --- Core Function Declarations --- */

sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int kill(int pid, int sig);
int raise(int sig);

#endif /* _SIGNAL_H */