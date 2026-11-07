#ifndef ALCHEMY_SIGNAL_H
#define ALCHEMY_SIGNAL_H
/* Independently authored Darwin <signal.h> declarations (LP64). */
#include "alchemy_c.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int sigset_t;
typedef void (*sig_t)(int);
typedef int sig_atomic_t;
union sigval { int sival_int; void *sival_ptr; };
typedef struct __siginfo {
    int si_signo; int si_errno; int si_code; pid_t si_pid; uid_t si_uid; int si_status;
    void *si_addr; union sigval si_value; long si_band; unsigned long __pad[7];
} siginfo_t;
struct sigaction {
    union { void (*__sa_handler)(int); void (*__sa_sigaction)(int, siginfo_t *, void *); } __sigaction_u;
    sigset_t sa_mask;
    int sa_flags;
};
#define sa_handler __sigaction_u.__sa_handler
#define sa_sigaction __sigaction_u.__sa_sigaction
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGBUS 10
#define SIGSEGV 11
#define SIGSYS 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGURG 16
#define SIGSTOP 17
#define SIGTSTP 18
#define SIGCONT 19
#define SIGCHLD 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGIO 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGINFO 29
#define SIGUSR1 30
#define SIGUSR2 31
#define SA_ONSTACK 0x0001
#define SA_RESTART 0x0002
#define SA_RESETHAND 0x0004
#define SA_NOCLDSTOP 0x0008
#define SA_NODEFER 0x0010
#define SA_NOCLDWAIT 0x0020
#define SA_SIGINFO 0x0040
#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3
sig_t signal(int, sig_t);
int raise(int);
int kill(pid_t, int);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
int sigprocmask(int, const sigset_t *, sigset_t *);
int pthread_sigmask(int, const sigset_t *, sigset_t *);
int pthread_kill(struct _opaque_pthread_t *, int);
#ifdef __cplusplus
}
#endif
#endif
