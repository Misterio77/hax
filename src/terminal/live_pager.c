/* SPDX-License-Identifier: MIT */
#include "terminal/live_pager.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include "xalloc.h"
#include "render/spinner.h"
#include "system/spawn.h"
#include "terminal/interrupt.h"

struct live_pager {
    pid_t pid;
    int stdout_fd;
    int stderr_fd;
    FILE *output;
    struct spinner *spinner; /* borrowed until close */
    struct spawn_signal_state signals;
};

static void restore_output(struct live_pager *pager)
{
    spinner_lock_output(pager->spinner);
    fflush(stdout);
    fflush(stderr);
    dup2(pager->stdout_fd, STDOUT_FILENO);
    dup2(pager->stderr_fd, STDERR_FILENO);
    close(pager->stdout_fd);
    close(pager->stderr_fd);

    rewind(pager->output);
    char bytes[8192];
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), pager->output)) > 0)
        fwrite(bytes, 1, count, stdout);
    fflush(stdout);
    spinner_unlock_output(pager->spinner);
    fclose(pager->output);
    pager->output = NULL;
    spawn_parent_restore_signals(&pager->signals);
    interrupt_resume();
}

struct live_pager *live_pager_open(const char *command, FILE *snapshot, struct spinner *spinner)
{
    if (fflush(snapshot) != 0 || fseek(snapshot, 0, SEEK_SET) != 0)
        return NULL;
    struct live_pager *pager = xcalloc(1, sizeof(*pager));
    pager->spinner = spinner;
    pager->stdout_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    pager->stderr_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    pager->output = tmpfile();
    if (pager->stdout_fd < 0 || pager->stderr_fd < 0 || !pager->output)
        goto fail;
    if (fcntl(fileno(pager->output), F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fileno(snapshot), F_SETFD, FD_CLOEXEC) < 0)
        goto fail;

    interrupt_suspend();
    spinner_lock_output(spinner);
    fflush(stdout);
    fflush(stderr);
    spawn_parent_ignore_signals(&pager->signals);
    pid_t parent_pid = getpid();
    int snapshot_fd = fileno(snapshot);
    pager->pid = spawn_fork();
    if (pager->pid == 0) {
        spawn_child_reset_signals();
        spawn_child_die_with_parent(parent_pid, SIGKILL);
        if (dup2(snapshot_fd, STDIN_FILENO) < 0)
            _exit(127);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    if (pager->pid < 0) {
        spawn_parent_restore_signals(&pager->signals);
        spinner_unlock_output(spinner);
        interrupt_resume();
        goto fail;
    }
    if (dup2(fileno(pager->output), STDOUT_FILENO) < 0 ||
        dup2(fileno(pager->output), STDERR_FILENO) < 0) {
        kill(pager->pid, SIGKILL);
        spawn_wait_child(pager->pid);
        spinner_unlock_output(spinner);
        restore_output(pager);
        free(pager);
        return NULL;
    }
    spinner_unlock_output(spinner);
    return pager;

fail:
    if (pager->stdout_fd >= 0)
        close(pager->stdout_fd);
    if (pager->stderr_fd >= 0)
        close(pager->stderr_fd);
    if (pager->output)
        fclose(pager->output);
    free(pager);
    return NULL;
}

int live_pager_poll(struct live_pager *pager)
{
    if (!pager || !pager->output)
        return 1;
    if (!spawn_reap_if_exited(pager->pid))
        return 0;
    restore_output(pager);
    return 1;
}

void live_pager_close(struct live_pager *pager)
{
    if (!pager)
        return;
    if (pager->output) {
        spawn_wait_child(pager->pid);
        restore_output(pager);
    }
    free(pager);
}
