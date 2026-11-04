/* SPDX-License-Identifier: MIT
 * Start the opt-in installer from a normal root launchd job, before the original
 * daemon can install its own sandbox. Always exec the original daemon afterward.
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/file.h>
#include <mach/mach.h>

extern char **environ;

int main(int argc, char **argv)
{
    (void)argc;
    int log = open("/private/var/log/coachz-apps.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log >= 0) {
        dprintf(log, "COACHZ apps: daemon trigger entered uid %u euid %u parent %d\n", getuid(), geteuid(), getppid());
    }
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &port);
    if (log >= 0) dprintf(log, "COACHZ apps: daemon bootstrap status %d valid %u\n", kr, MACH_PORT_VALID(port));
    /* XNU keeps special and registered ports separately. A null special slot
     * alone does not establish that launchd supplied no bootstrap right.
     * Use only this task's own inherited registered slot, never another task. */
    if (kr == KERN_SUCCESS && !MACH_PORT_VALID(port)) {
        mach_port_array_t registered = NULL;
        mach_msg_type_number_t count = 0;
        kr = mach_ports_lookup(mach_task_self(), &registered, &count);
        int valid = kr == KERN_SUCCESS && count && MACH_PORT_VALID(registered[0]);
        if (log >= 0) dprintf(log, "COACHZ apps: daemon registered bootstrap status %d count %u valid %u\n", kr, count, valid);
        if (valid) {
            port = registered[0];
            kr = task_set_bootstrap_port(mach_task_self(), port);
            if (log >= 0) dprintf(log, "COACHZ apps: daemon bootstrap restore status %d\n", kr);
        }
        if (registered) {
            for (mach_msg_type_number_t i = valid ? 1 : 0; i < count; i++) {
                if (MACH_PORT_VALID(registered[i])) mach_port_deallocate(mach_task_self(), registered[i]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)registered, count * sizeof(*registered));
        }
    }
    int error = kr == KERN_SUCCESS && MACH_PORT_VALID(port) && geteuid() == 0 ? 0 : EPERM;
    if (MACH_PORT_VALID(port)) mach_port_deallocate(mach_task_self(), port);
    /* The helper inherits this open description and holds the lock until exit.
     * The original daemon must not inherit it. flock is cleared across reboot. */
    int lock = -1;
    if (!error) {
        lock = open("/private/var/run/coachz-apps.lock", O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) error = errno;
    }
    posix_spawn_file_actions_t actions;
    int initialized = 0;
    if (!error) { error = posix_spawn_file_actions_init(&actions); initialized = !error; }
    if (!error && log >= 0) {
        error = posix_spawn_file_actions_adddup2(&actions, log, STDOUT_FILENO);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, log, STDERR_FILENO);
    }
    pid_t child = -1;
    char *const args[] = {"/private/var/ngpu/owner-apps/coachz-apps", "bootstrap", NULL};
    if (!error) {
        /* Do not delay the real daemon or wait for installation to finish. */
        error = posix_spawn(&child, args[0], &actions, NULL, args, environ);
    }
    if (log >= 0) {
        dprintf(log, "COACHZ apps: daemon installer spawn status %d pid %d; original daemon next\n", error, child);
    }
    if (initialized) posix_spawn_file_actions_destroy(&actions);
    if (lock >= 0) close(lock);
    execv("/private/var/ngpu/owner-apps/daemon/mtmergeprops.real", argv);
    error = errno;
    if (log >= 0) dprintf(log, "COACHZ apps: original daemon exec failed %d\n", error);
    return error;
}
