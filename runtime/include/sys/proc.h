#ifndef ALCHEMY_SYS_PROC_H
#define ALCHEMY_SYS_PROC_H
/* Darwin <sys/proc.h>: the user-visible struct extern_proc (kinfo_proc's kp_proc)
   layout, as in the APSL xnu release (bsd/sys/proc.h). */
#include "alchemy_c.h"
#include <signal.h>
struct proc;
struct vmspace;
struct sigacts;
struct vnode;
struct pgrp;
struct user;
struct rusage;
struct itimerval { struct timeval it_interval; struct timeval it_value; };
#define MAXCOMLEN 16
struct extern_proc {
    union {
        struct { struct proc* __p_forw; struct proc* __p_back; } p_st1;
        struct timeval __p_starttime;
    } p_un;
    struct vmspace* p_vmspace;
    struct sigacts* p_sigacts;
    int p_flag;
    char p_stat;
    pid_t p_pid;
    pid_t p_oppid;
    int p_dupfd;
    char* user_stack;
    void* exit_thread;
    int p_debugger;
    int sigwait;
    unsigned int p_estcpu;
    int p_cpticks;
    unsigned int p_pctcpu;
    void* p_wchan;
    char* p_wmesg;
    unsigned int p_swtime;
    unsigned int p_slptime;
    struct itimerval p_realtimer;
    struct timeval p_rtime;
    unsigned long long p_uticks;
    unsigned long long p_sticks;
    unsigned long long p_iticks;
    int p_traceflag;
    struct vnode* p_tracep;
    int p_siglist;
    struct vnode* p_textvp;
    int p_holdcnt;
    sigset_t p_sigmask;
    sigset_t p_sigignore;
    sigset_t p_sigcatch;
    unsigned char p_priority;
    unsigned char p_usrpri;
    char p_nice;
    char p_comm[MAXCOMLEN + 1];
    struct pgrp* p_pgrp;
    struct user* p_addr;
    unsigned short p_xstat;
    unsigned short p_acflag;
    struct rusage* p_ru;
};
#ifdef __cplusplus
static_assert(sizeof(struct extern_proc) == 296, "extern_proc is kinfo_proc's 296-byte kp_proc (LP64)");
#else
_Static_assert(sizeof(struct extern_proc) == 296, "extern_proc is kinfo_proc's 296-byte kp_proc (LP64)");
#endif
#endif
