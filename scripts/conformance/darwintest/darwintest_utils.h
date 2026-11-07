/* darwintest_utils.h for the conformance harness (darwintest.h): the dt_* helpers the open-source
 * tests call. Project-authored, MIT. */
#ifndef CONFORMANCE_DARWINTEST_UTILS_H
#define CONFORMANCE_DARWINTEST_UTILS_H

#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A fresh directory for the case's files (under $TMPDIR, else /tmp). */
const char *dt_tmpdir(void);
int dt_ncpu(void);
/* posix_spawn argv[0] with argv; returns 0 or an errno value. */
int dt_launch_tool(pid_t *pid, char **argv, bool start_suspended, const char *stdout_path, const char *stderr_path);
/* Waits up to timeout seconds; true when the child exited with status 0. */
bool dt_waitpid(pid_t pid, int *exit_status, int *signal, int timeout);
void dt_print_hex_dump(const unsigned char *buf, size_t len);
void dt_timer_start(const char *name);
void dt_timer_stop(const char *name);

#ifdef __cplusplus
}
#endif

#endif
