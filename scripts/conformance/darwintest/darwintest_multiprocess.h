/* darwintest_multiprocess.h for the conformance harness (darwintest.h): helpers run as separate
 * processes of the same program, selected by their T_HELPER_DECL name. Project-authored, MIT. */
#ifndef CONFORMANCE_DARWINTEST_MULTIPROCESS_H
#define CONFORMANCE_DARWINTEST_MULTIPROCESS_H

#include <darwintest.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    char **args;
} dt_helper_t;

dt_helper_t dt_child_helper(const char *name);
dt_helper_t dt_child_helper_args(const char *name, char **args);
/* Runs the helpers concurrently; fails the case if one does not exit 0 within timeout seconds. */
void dt_run_helpers(dt_helper_t *helpers, size_t count, int timeout);

#ifdef __cplusplus
}
#endif

#endif
