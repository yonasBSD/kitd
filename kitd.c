/* Copyright (C) 2023  June McEnroe <june@causal.agency>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

struct LineBuffer {
	size_t len;
	char buf[1024];
};

static void lbFill(struct LineBuffer *lb, int fd) {
	size_t cap = sizeof(lb->buf)-1 - lb->len;
	ssize_t len = read(fd, &lb->buf[lb->len], cap);
	if (len < 0 && errno != EAGAIN) {
		syslog(LOG_ERR, "read: %m");
	}
	if (len > 0) lb->len += len;
}

static void lbFlush(struct LineBuffer *lb, int priority) {
	assert(lb->len < sizeof(lb->buf));
	lb->buf[lb->len] = '\0';

	if (lb->len == sizeof(lb->buf)-1) {
		syslog(priority, "%s", lb->buf);
		lb->len = 0;
		return;
	}

	char *ptr = lb->buf;
	for (char *nl; NULL != (nl = strchr(ptr, '\n')); ptr = &nl[1]) {
		*nl = '\0';
		syslog(priority, "%s", ptr);
	}
	lb->len -= ptr - lb->buf;
	memmove(lb->buf, ptr, lb->len);
}

enum { M = 60, H = 60*M, D = 24*H };

static const char *humanize(const struct timeval *interval) {
	static char buf[256];
	if (!interval->tv_sec) {
		snprintf(buf, sizeof(buf), "%dms", (int)(interval->tv_usec / 1000));
		return buf;
	}
	int s = interval->tv_sec;
	int d = s / D; s %= D;
	int h = s / H; s %= H;
	int m = s / M; s %= M;
	if (d) snprintf(buf, sizeof(buf), "%dd %dh %dm %ds", d, h, m, s);
	else if (h) snprintf(buf, sizeof(buf), "%dh %dm %ds", h, m, s);
	else if (m) snprintf(buf, sizeof(buf), "%dm %ds", m, s);
	else snprintf(buf, sizeof(buf), "%ds", s);
	return buf;
}

static void parse(struct timeval *interval, const char *str) {
	char *endptr;
	unsigned long n = strtoul(str, &endptr, 10);
	timerclear(interval);
	switch (*endptr) {
		break; case 's': interval->tv_sec = n;
		break; case 'm': interval->tv_sec = n*M;
		break; case 'h': interval->tv_sec = n*H;
		break; case 'd': interval->tv_sec = n*D;
		break; case '\0': interval->tv_usec = n * 1000;
		break; default: errx(1, "invalid suffix '%c'", *endptr);
	}
}

static volatile sig_atomic_t signals[NSIG];
static void signalHandler(int signal) {
	signals[signal] = 1;
}

int main(int argc, char *argv[]) {
	int error;

	bool daemonize = true;
	const char *name = NULL;
	struct timeval restart = { .tv_sec = 1 };
	struct timeval cooloff = { .tv_sec = 15*M };
	struct timeval maximum = { .tv_sec = 1*H };
	for (int opt; 0 < (opt = getopt(argc, argv, "c:dm:n:t:"));) {
		switch (opt) {
			break; case 'c': parse(&cooloff, optarg);
			break; case 'd': daemonize = false;
			break; case 'm': parse(&maximum, optarg);
			break; case 'n': name = optarg;
			break; case 't': parse(&restart, optarg);
			break; default: return 1;
		}
	}
	argc -= optind;
	argv += optind;
	if (!argc) errx(1, "no command");
	if (!name) {
		name = strrchr(argv[0], '/');
		name = (name ? &name[1] : argv[0]);
	}

#ifdef __OpenBSD__
	error = pledge("stdio rpath proc exec", NULL);
	if (error) err(1, "pledge");
#endif

	int stdoutRW[2];
	error = pipe2(stdoutRW, O_CLOEXEC);
	if (error) err(1, "pipe2");

	int stderrRW[2];
	error = pipe2(stderrRW, O_CLOEXEC);
	if (error) err(1, "pipe2");

	fcntl(stdoutRW[0], F_SETFL, O_NONBLOCK);
	fcntl(stderrRW[0], F_SETFL, O_NONBLOCK);

	struct LineBuffer stdoutBuffer = {0};
	struct LineBuffer stderrBuffer = {0};

	openlog(name, LOG_NDELAY | LOG_PERROR, LOG_DAEMON);
	if (daemonize) {
		error = daemon(0, 0);
		if (error) {
			syslog(LOG_ERR, "daemon: %m");
			return 1;
		}
	}
	setproctitle("%s", name);

	signal(SIGHUP, signalHandler);
	signal(SIGINT, signalHandler);
	signal(SIGALRM, signalHandler);
	signal(SIGTERM, signalHandler);
	signal(SIGCHLD, signalHandler);
	signal(SIGINFO, signalHandler);
	signal(SIGUSR1, signalHandler);
	signal(SIGUSR2, signalHandler);

	pid_t child = 0;
	bool stop = false;
	struct timeval uptime = {0};
	struct timeval interval = restart;
	signals[SIGALRM] = 1;

	sigset_t mask, unmask;
	sigfillset(&mask);
	sigemptyset(&unmask);
	sigprocmask(SIG_SETMASK, &mask, NULL);
	struct pollfd fds[2] = {
		{ .fd = stdoutRW[0], .events = POLLIN },
		{ .fd = stderrRW[0], .events = POLLIN },
	};
	for (;;) {
		struct timeval now;
		struct timespec nowspec;
		clock_gettime(CLOCK_MONOTONIC, &nowspec);
		TIMESPEC_TO_TIMEVAL(&now, &nowspec);

		if (signals[SIGALRM]) {
			assert(!child);
			child = fork();
			if (child < 0) {
				syslog(LOG_ERR, "fork: %m");
				return 1;
			}
			if (child) {
				uptime = now;
				signals[SIGALRM] = 0;
			} else {
				setpgid(0, 0);
				dup2(stdoutRW[1], STDOUT_FILENO);
				dup2(stderrRW[1], STDERR_FILENO);
				sigprocmask(SIG_SETMASK, &unmask, NULL);
				execvp(argv[0], (char *const *)argv);
				err(127, "%s", argv[0]);
			}
		}

		if (signals[SIGHUP]) {
			if (child) killpg(child, SIGHUP);
			signals[SIGHUP] = 0;
		}
		if (signals[SIGUSR1]) {
			if (child) killpg(child, SIGUSR1);
			signals[SIGUSR1] = 0;
		}
		if (signals[SIGUSR2]) {
			if (child) killpg(child, SIGUSR2);
			signals[SIGUSR2] = 0;
		}

		if (signals[SIGINT] || signals[SIGTERM]) {
			stop = true;
			int sig = (signals[SIGINT] ? SIGINT : SIGTERM);
			if (child) {
				killpg(child, sig);
			} else {
				break;
			}
			signals[sig] = 0;
		}

		if (signals[SIGCHLD]) {
			int status;
			pid_t pid = wait(&status);
			signals[SIGCHLD] = 0;
			if (pid < 0) {
				syslog(LOG_ERR, "wait: %m");
				continue;
			}
			if (pid != child) {
				syslog(LOG_NOTICE, "unknown child %d", pid);
				continue;
			}
			child = 0;

			if (WIFEXITED(status)) {
				int exit = WEXITSTATUS(status);
				if (exit == 127) stop = true;
				if (exit) syslog(LOG_NOTICE, "child exited %d", exit);
			} else if (WIFSIGNALED(status)) {
				int sig = WTERMSIG(status);
				if (sig != SIGTERM) {
					syslog(LOG_NOTICE, "child got %s", strsignal(sig));
				}
			}

			if (stop) break;
			timersub(&now, &uptime, &uptime);
			if (timercmp(&uptime, &cooloff, >=)) {
				interval = restart;
			}
			syslog(LOG_INFO, "restarting in %s", humanize(&interval));
			struct itimerval timer = { .it_value = interval };
			setitimer(ITIMER_REAL, &timer, NULL);

			timeradd(&interval, &interval, &interval);
			if (timercmp(&interval, &maximum, >)) {
				interval = maximum;
			}
		}

		if (signals[SIGINFO]) {
			if (child) {
				struct timeval time;
				timersub(&now, &uptime, &time);
				syslog(LOG_INFO, "child %d up %s", child, humanize(&time));
			} else {
				struct itimerval timer;
				getitimer(ITIMER_REAL, &timer);
				syslog(LOG_INFO, "restarting in %s", humanize(&timer.it_value));
			}
			signals[SIGINFO] = 0;
		}

		int nfds = ppoll(fds, 2, NULL, &unmask);
		if (nfds < 0 && errno != EINTR) {
			syslog(LOG_ERR, "poll: %m");
			continue;
		}
		if (nfds > 0 && fds[0].revents) {
			lbFill(&stdoutBuffer, fds[0].fd);
			lbFlush(&stdoutBuffer, LOG_INFO);
		}
		if (nfds > 0 && fds[1].revents) {
			lbFill(&stderrBuffer, fds[1].fd);
			lbFlush(&stderrBuffer, LOG_NOTICE);
		}
	}

	lbFill(&stdoutBuffer, fds[0].fd);
	lbFill(&stderrBuffer, fds[1].fd);
	lbFlush(&stdoutBuffer, LOG_INFO);
	lbFlush(&stderrBuffer, LOG_NOTICE);
}
