/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2010, 2011, 2012 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include <limits.h>     /* PATH_MAX, */
#include <sys/types.h>  /* pid_t, */
#include <sys/ptrace.h> /* ptrace(3), PTRACE_*, */
#include <sys/types.h>  /* waitpid(2), */
#include <sys/wait.h>   /* waitpid(2), */
#include <sys/personality.h> /* personality(2), ADDR_NO_RANDOMIZE, */
#include <sys/time.h>   /* *rlimit(2), */
#include <sys/resource.h> /* *rlimit(2), */
#include <fcntl.h>      /* AT_FDCWD, */
#include <unistd.h>     /* fork(2), chdir(2), getpid(2), */
#include <string.h>     /* strcpy(3), */
#include <errno.h>      /* errno(3), */
#include <stdbool.h>    /* bool, true, false, */
#include <assert.h>     /* assert(3), */
#include <stdlib.h>     /* atexit(3), */
#include <sys/queue.h>  /* LIST_*, */
#include <talloc.h>     /* talloc_*, */

#include "tracee/event.h"
#include "notice.h"
#include "path/path.h"
#include "path/binding.h"
#include "syscall/syscall.h"

#include "compat.h"

/**
 * Launch the first process as specified by @tracee->cmdline[].  This
 * function returns -1 if an error occurred, otherwise 0.
 */
int launch_process(Tracee *tracee)
{
	struct rlimit rlimit;
	long status;
	pid_t pid;

	pid = fork();
	switch(pid) {
	case -1:
		notice(ERROR, SYSTEM, "fork()");
		return -1;

	case 0: /* child */
		/* Declare myself as ptraceable before executing the
		 * requested program. */
		status = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		if (status < 0) {
			notice(ERROR, SYSTEM, "ptrace(TRACEME)");
			return -1;
		}

		/* Warn about open file descriptors. They won't be
		 * translated until they are closed. */
		if (verbose_level > 0)
			list_open_fd(getpid());

		/* RHEL4 uses an ASLR mechanism that creates conflicts
		 * between the layout of QEMU and the layout of the
		 * target program. */
		status = personality(0xffffffff);
		if (   status < 0
		    || personality(status | ADDR_NO_RANDOMIZE) < 0)
			notice(WARNING, INTERNAL, "can't disable ASLR");

		/* 1. The ELF interpreter is explicitly used as a
		 *    loader by PRoot, it means the Linux kernel
		 *    allocates the heap segment for this loader, not
		 *    for the application.  It isn't really a problem
		 *    since the application re-uses the loader's heap
		 *    transparently, i.e. its own brk points there.
		 *
		 * 2. When the current stack limit is set to a "high"
		 *    value, the ELF interpreter is loaded to a "low"
		 *    address, I guess it is the way the Linux kernel
		 *    deals with this constraint.
		 *
		 * This two behaviors can be observed by running the
		 * command "/usr/bin/cat /proc/self/maps", with/out
		 * the ELF interpreter and with/out a high current
		 * stack limit.
		 *
		 * When this two behaviors are combined, the dynamic
		 * ELF linker might mmap a shared libraries to the
		 * same address as the start of its heap if no heap
		 * allocation was made prior this mmap.  This problem
		 * was first observed with something similar to the
		 * example below (because GNU make sets the current
		 * limit to the maximum):
		 *
		 *     #!/usr/bin/make -f
		 *
		 *     SHELL=/lib64/ld-linux-x86-64.so.2 /usr/bin/bash
		 *     FOO:=$(shell test -e /dev/null && echo OK)
		 *
		 *     all:
		 *            @/usr/bin/test -n "$(FOO)"
		 *
		 * The code below is a workaround to this wrong
		 * combination of behaviors, however it might create
		 * conflict with tools that assume a "standard" stack
		 * layout, like libgomp and QEMU.
		 */
		status = getrlimit(RLIMIT_STACK, &rlimit);
		if (status >= 0 && rlimit.rlim_max == RLIM_INFINITY) {
			/* "No one will need more than 256MB of stack memory" (tm).  */
			rlimit.rlim_max = 256 * 1024 * 1024;
			if (rlimit.rlim_cur > rlimit.rlim_max)
				rlimit.rlim_cur = rlimit.rlim_max;
			status = setrlimit(RLIMIT_STACK, &rlimit);
		}
		if (status < 0)
			notice(WARNING, SYSTEM, "can't set the maximum stack size");

		/* Synchronize with the tracer's event loop.  Without
		 * this trick the tracer only sees the "return" from
		 * the next execve(2) so PRoot wouldn't handle the
		 * interpreter/runner.  I also verified that strace
		 * does the same thing. */
		kill(getpid(), SIGSTOP);

		/* Now process is ptraced, so the current rootfs is
		 * already the guest rootfs. */

		if (tracee->cwd != NULL) {
			status = chdir(tracee->cwd);
			if (status < 0) {
				notice(WARNING, SYSTEM, "chdir('%s/)", tracee->cwd);
				chdir("/");
			}
		}
		else {
			status = chdir(".");
			if (status < 0) {
				notice(INFO, USER,
					"the current working directory isn't "
					"accessible anymore, changing to \"/\"");
				chdir("/");
			}
		}

		notice(INFO, INTERNAL, "started");

		execvp(tracee->cmdline[0], tracee->cmdline);

		return -1;

	default: /* parent */
		/* We know the pid of the first tracee now.  */
		tracee->pid = pid;

		/* This tracee has no traced parent.  */
		inherit(tracee, NULL);
		return 0;
	}

	/* Never reached.  */
	return -1;
}

/* Send the KILL signal to all tracees when PRoot has received a fatal
 * signal.  */
static void kill_all_tracees2(int signum, siginfo_t *siginfo, void *ucontext)
{
	notice(WARNING, INTERNAL, "signal %d received from process %d", signum, siginfo->si_pid);
	kill_all_tracees();

	/* Exit immediately for system signals (segmentation fault,
	 * illegal instruction, ...), otherwise exit cleanly through
	 * the event loop.  */
	if (signum != SIGQUIT)
		_exit(EXIT_FAILURE);
}

/* Print on stderr the complete talloc hierarchy.  */
static void print_talloc_hierarchy(int signum, siginfo_t *siginfo, void *ucontext)
{
	void print_talloc_chunk(const void *ptr, int depth, int max_depth, int is_ref, void *data)
	{
		const char *name;
		size_t size;

		name = talloc_get_name(ptr);
		size = talloc_get_size(ptr);

		if (depth == 0)
			return;

		while (depth-- > 1)
			fprintf(stderr, "\t");

		fprintf(stderr, "%-16s ", name);

		if (is_ref)
			fprintf(stderr, "-> %-8p", ptr);
		else {
			fprintf(stderr, "%-8p  %zd bytes", ptr, size);

			if (name[0] == '$') {
				fprintf(stderr, "\t(\"%s\")", (char *)ptr);
			}
			else if (strcmp(name, "Tracee") == 0) {
				fprintf(stderr, "\t(pid = %d)", ((Tracee *)ptr)->pid);
			}
			else if (strcmp(name, "Bindings") == 0) {
				 Tracee *tracee;

				 tracee = talloc_get_type_abort(talloc_parent(ptr), Tracee);

				 if (ptr == tracee->bindings_user)
					 fprintf(stderr, "\t(user)");
				 else if (ptr == tracee->bindings_guest)
					 fprintf(stderr, "\t(guest)");
				 else if (ptr == tracee->bindings_host)
					 fprintf(stderr, "\t(host)");
			}
			else if (strcmp(name, "Binding") == 0) {
				Binding *binding = (Binding *)ptr;
				fprintf(stderr, "\t(%s:%s)", binding->host.path, binding->guest.path);
			}
		}

		fprintf(stderr, "\n");
	}

	switch (signum) {
	case SIGUSR1:
		talloc_report_depth_cb(NULL, 0, 100, print_talloc_chunk, NULL);
		break;

	case SIGUSR2:
		talloc_report_depth_file(NULL, 0, 100, stderr);
		break;

	default:
		break;
	}
}

/**
 * Wait then handle any event from any tracee.  This function returns
 * the exit status of the last terminated program.
 */
int event_loop()
{
	struct sigaction signal_action;
	int last_exit_status = -1;
	long status;
	int signum;
	int signal;

	/* Kill all tracees when exiting.  */
	status = atexit(kill_all_tracees);
	if (status != 0)
		notice(WARNING, INTERNAL, "atexit() failed");

	/* All signals are blocked when the signal handler is called.
	 * SIGINFO is used to know which process has signaled us and
	 * RESTART is used to restart waitpid(2) seamlessly.  */
	bzero(&signal_action, sizeof(signal_action));
	signal_action.sa_flags = SA_SIGINFO | SA_RESTART;
	status = sigfillset(&signal_action.sa_mask);
	if (status < 0)
		notice(WARNING, SYSTEM, "sigfillset()");

	/* Handle all signals.  */
	for (signum = 0; signum < SIGRTMAX; signum++) {
		switch (signum) {
		case SIGQUIT:
		case SIGILL:
		case SIGABRT:
		case SIGFPE:
		case SIGSEGV:
			/* Kill all tracees on abnormal termination
			 * signals.  This ensures no process is left
			 * untraced.  */
			signal_action.sa_sigaction = kill_all_tracees2;
			break;

		case SIGUSR1:
		case SIGUSR2:
			/* Print on stderr the complete talloc
			 * hierarchy, useful for debug purpose.  */
			signal_action.sa_sigaction = print_talloc_hierarchy;
			break;

		case SIGCHLD:
		case SIGCONT:
		case SIGSTOP:
		case SIGTSTP:
		case SIGTTIN:
		case SIGTTOU:
			/* The default action is OK for these signals,
			 * they are related to tty and job control.  */
			continue;

		default:
			/* Ignore all other signals, including
			 * terminating ones (^C for instance). */
			signal_action.sa_sigaction = (void *)SIG_IGN;
			break;
		}

		status = sigaction(signum, &signal_action, NULL);
		if (status < 0 && errno != EINVAL)
			notice(WARNING, SYSTEM, "sigaction(%d)", signum);
	}

	signal = 0;
	while (1) {
		int tracee_status;
		Tracee *tracee;
		pid_t pid;

		/* Wait for the next tracee's stop. */
		pid = waitpid(-1, &tracee_status, __WALL);
		if (pid < 0) {
			if (errno != ECHILD) {
				notice(ERROR, SYSTEM, "waitpid()");
				return EXIT_FAILURE;
			}
			break;
		}

		/* Get the information about this tracee. */
		tracee = get_tracee(pid, true);
		assert(tracee != NULL);

		if (WIFEXITED(tracee_status)) {
			VERBOSE(1, "pid %d: exited with status %d",
				pid, WEXITSTATUS(tracee_status));
			last_exit_status = WEXITSTATUS(tracee_status);
			TALLOC_FREE(tracee);
			continue; /* Skip the call to ptrace(SYSCALL). */
		}
		else if (WIFSIGNALED(tracee_status)) {
			VERBOSE(1, "pid %d: terminated with signal %d",
				pid, WTERMSIG(tracee_status));
			TALLOC_FREE(tracee);
			continue; /* Skip the call to ptrace(SYSCALL). */
		}
		else if (WIFCONTINUED(tracee_status)) {
			VERBOSE(1, "pid %d: continued", pid);
			signal = SIGCONT;
		}
		else if (WIFSTOPPED(tracee_status)) {

			/* Don't use WSTOPSIG() to extract the signal
			 * since it clears the PTRACE_EVENT_* bits. */
			signal = (tracee_status & 0xfff00) >> 8;

			switch (signal) {
				static bool skip_bare_sigtrap = false;

			case SIGTRAP:
				/* Distinguish some events from others and
				 * automatically trace each new process with
				 * the same options.
				 *
				 * Note that only the first bare SIGTRAP is
				 * related to the tracing loop, others SIGTRAP
				 * carry tracing information because of
				 * TRACE*FORK/CLONE/EXEC.
				 */
				if (skip_bare_sigtrap)
					break;
				skip_bare_sigtrap = true;

				status = ptrace(PTRACE_SETOPTIONS, tracee->pid, NULL,
						PTRACE_O_TRACESYSGOOD |
						PTRACE_O_TRACEFORK    |
						PTRACE_O_TRACEVFORK   |
						PTRACE_O_TRACEEXEC    |
						PTRACE_O_TRACECLONE);
				if (status < 0) {
					notice(ERROR, SYSTEM, "ptrace(PTRACE_SETOPTIONS)");
					return EXIT_FAILURE;
				}
				/* Fall through. */
			case SIGTRAP | 0x80:
				assert(tracee->exe != NULL);
				status = translate_syscall(tracee);
				if (status < 0) {
					/* The process died in a syscall. */
					TALLOC_FREE(tracee);
					continue; /* Skip the call to ptrace(SYSCALL). */
				}
				signal = 0;
				break;

			case SIGTRAP | PTRACE_EVENT_FORK  << 8:
			case SIGTRAP | PTRACE_EVENT_VFORK << 8:
			case SIGTRAP | PTRACE_EVENT_CLONE << 8: {
				Tracee *child_tracee;
				pid_t child_pid;

				signal = 0;

				/* Get the pid of the tracee's new child.  */
				status = ptrace(PTRACE_GETEVENTMSG, tracee->pid, NULL, &child_pid);
				if (status < 0) {
					notice(WARNING, SYSTEM, "ptrace(GETEVENTMSG)");
					break;
				}

				/* Declare the parent of this new tracee.  */
				child_tracee = get_tracee(child_pid, true);
				inherit(child_tracee, tracee);

				/* Restart the child tracee if it was started
				 * before this notification event.  */
				if (child_tracee->sigstop == SIGSTOP_PENDING) {
					tracee->sigstop = SIGSTOP_ALLOWED;
					status = ptrace(PTRACE_SYSCALL, child_pid, NULL, 0);
					if (status < 0) {
						notice(WARNING, SYSTEM,
							"ptrace(SYSCALL, %d) [1]", child_pid);
						TALLOC_FREE(tracee);
					}
				}
			}
				break;

			case SIGTRAP | PTRACE_EVENT_EXEC  << 8:
				signal = 0;
				break;

			case SIGSTOP:
				/* For each tracee, the first SIGSTOP
				 * is only used to notify the tracer.  */

				/* Stop this tracee until PRoot has received
				 * the EVENT_*FORK|CLONE notification.  */
				if (tracee->exe == NULL) {
					tracee->sigstop = SIGSTOP_PENDING;
					continue;
				}

				if (tracee->sigstop == SIGSTOP_IGNORED) {
					tracee->sigstop = SIGSTOP_ALLOWED;
					signal = 0;
				}
				break;

			default:
				break;
			}
		}
		else {
			notice(WARNING, INTERNAL, "unknown trace event");
			signal = 0;
		}

		/* Restart the tracee and stop it at the next entry or
		 * exit of a system call. */
		status = ptrace(PTRACE_SYSCALL, tracee->pid, NULL, signal);
		if (status < 0) {
			 /* The process died in a syscall.  */
			notice(WARNING, SYSTEM, "ptrace(SYSCALL, %d) [2]", tracee->pid);
			TALLOC_FREE(tracee);
		}
	}

	return last_exit_status;
}
