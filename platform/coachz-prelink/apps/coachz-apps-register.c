/* SPDX-License-Identifier: MIT
 * Register exactly one local root job through launchd. Request fields follow
 * pinned Procursus launchctl; the pipe comes from Mach, never libxpc globals.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <mach/mach.h>
#include <xpc/xpc.h>
#include "xpc_private.h"

/* Opaque private API, also used by Chromium's bootstrap IPC. No struct layout.
 * https://chromium.googlesource.com/experimental/chromium/src/+/refs/wip/bajones/webvr/sandbox/mac/pre_exec_delegate.cc
 */
extern xpc_pipe_t xpc_pipe_create_from_port(mach_port_t port, uint32_t flags);

static int send_request(xpc_object_t request, xpc_object_t *reply)
{
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &port);
    fprintf(stderr, "COACHZ apps: bootstrap port query status %d valid %u\n", kr, MACH_PORT_VALID(port));
    if (kr != KERN_SUCCESS) return EIO;
    if (!MACH_PORT_VALID(port)) return ESRCH;
    xpc_pipe_t pipe = xpc_pipe_create_from_port(port, 0);
    fprintf(stderr, "COACHZ apps: bootstrap pipe created %u\n", pipe != NULL);
    int status = ENOMEM;
    if (pipe) {
        fprintf(stderr, "COACHZ apps: launchd send begin routine %u\n", XPC_ROUTINE_LOAD);
        status = _xpc_pipe_interface_routine(pipe, 0, request, reply, 0);
        fprintf(stderr, "COACHZ apps: launchd send status %d reply %u\n", status, *reply != NULL);
        xpc_release(pipe);
    }
    mach_port_deallocate(mach_task_self(), port);
    return status;
}

static int register_job(void)
{
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
    if (!request) return ENOMEM;
    /* system domain, same values as Procursus setup("system") and LOAD. */
    xpc_dictionary_set_uint64(request, "type", 1);
    xpc_dictionary_set_uint64(request, "handle", 0);
    xpc_dictionary_set_uint64(request, "subsystem", XPC_ROUTINE_LOAD >> 8);
    xpc_dictionary_set_uint64(request, "routine", XPC_ROUTINE_LOAD);
    const char *path = "/private/var/ngpu/owner-apps/LaunchDaemons/com.coachz.apps.plist";
    xpc_object_t paths = xpc_array_create(NULL, 0);
    if (!paths) { xpc_release(request); return ENOMEM; }
    xpc_array_set_string(paths, XPC_ARRAY_APPEND, path);
    xpc_dictionary_set_value(request, "paths", paths);
    xpc_release(paths);
    int status = send_request(request, &reply);
    xpc_release(request);
    if (!status && (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY)) status = EPROTO;
    if (!status) {
        xpc_object_t error = xpc_dictionary_get_value(reply, "error");
        if (error) status = xpc_get_type(error) == XPC_TYPE_INT64 ? (int)xpc_int64_get_value(error) : EPROTO;
        fprintf(stderr, "COACHZ apps: launchd response error %d\n", status);
    }
    if (!status) {
        __block int path_error = 0;
        xpc_object_t errors = xpc_dictionary_get_value(reply, "errors");
        if (errors && xpc_get_type(errors) != XPC_TYPE_DICTIONARY) {
            path_error = EPROTO;
        } else if (errors) {
            xpc_dictionary_apply(errors, ^bool(const char *key, xpc_object_t value) {
                (void)key;
                int error = xpc_get_type(value) == XPC_TYPE_INT64 ? (int)xpc_int64_get_value(value) : EPROTO;
                fprintf(stderr, "COACHZ apps: launchd job response %d\n", error);
                if (error && error != EEXIST && error != EALREADY) path_error = error;
                return true;
            });
        }
        status = path_error ? path_error : (int)xpc_dictionary_get_int64(reply, "bootstrap-error");
    }
    if (reply) xpc_release(reply);
    return status;
}

int main(void)
{
    if (geteuid() != 0) return EPERM;
    fprintf(stderr, "COACHZ apps: launchd registration entered uid %u\n", getuid());
    int status = 0;
    for (unsigned attempt = 0; attempt < 12; attempt++) {
        status = register_job();
        fprintf(stderr, "COACHZ apps: launchd registration attempt %u status %d\n", attempt + 1, status);
        if (status != ENODOMAIN && status != EAGAIN && status != EBUSY && status != ESRCH) break;
        sleep(3);
    }
    return status;
}
