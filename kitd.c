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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

struct Line {
	size_t len;
	char buf[1024];
};

static void lineFill(struct Line *line, int fd) {
	size_t cap = sizeof(line->buf)-1 - line->len;
	ssize_t len = read(fd, &line->buf[line->len], cap);
	if (len < 0 && errno != EAGAIN) {
		syslog(LOG_ERR, "read: %m");
		return;
	}
	line->len += len;
}

static void lineFlush(struct Line *line, int priority) {
	assert(line->len < sizeof(line->buf));
	line->buf[line->len] = '\0';

	if (line->len == sizeof(line->buf)-1) {
		syslog(priority, "%s", line->buf);
		line->len = 0;
		return;
	}

	char *ptr = line->buf;
	for (char *nl; NULL != (nl = strchr(ptr, '\n')); ptr = &nl[1]) {
		*nl = '\0';
		syslog(priority, "%s", ptr);
	}
	line->len -= ptr - line->buf;
	memmove(line->buf, ptr, line->len);
}

static const char *humanize(const struct timespec *interval) {
	static char buf[256];
	if (!interval->tv_sec) {
		snprintf(buf, sizeof(buf), "%dms", (int)(interval->tv_nsec / 1000000));
		return buf;
	}
	int s = interval->tv_sec;
	int d = s / (24*60*60);
	s %= 24*60*60;
	int h = s / (60*60);
	s %= 60*60;
	int m = s / 60;
	s %= 60;
	if (d) {
		snprintf(buf, sizeof(buf), "%dd %dh %dm %ds", d, h, m, s);
	} else if (h) {
		snprintf(buf, sizeof(buf), "%dh %dm %ds", h, m, s);
	} else if (m) {
		snprintf(buf, sizeof(buf), "%dm %ds", m, s);
	} else {
		snprintf(buf, sizeof(buf), "%ds", s);
	}
	return buf;
}

static volatile sig_atomic_t signals[NSIG];
static void signalHandler(int signal) {
	signals[signal] = 1;
}

static void parseInterval(struct timespec *interval, const char *millis) {
	unsigned long ms = strtoul(millis, NULL, 10);
	interval->tv_sec = ms / 1000;
	interval->tv_nsec = 1000000 * (ms % 1000);
}

int main(int argc, char *argv[]) {
	int error;

	bool daemonize = true;
	const char *name = NULL;
	struct timespec cooloff = { .tv_sec = 15 * 60 };
	struct timespec restart = { .tv_sec = 1 };
	for (int opt; 0 < (opt = getopt(argc, argv, "c:dn:t:"));) {
		switch (opt) {
			break; case 'c': parseInterval(&cooloff, optarg);
			break; case 'd': daemonize = false;
			break; case 'n': name = optarg;
			break; case 't': parseInterval(&restart, optarg);
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

	error = pledge("stdio rpath wpath proc exec", NULL);
	if (error) err(1, "pledge");

	int stdoutRW[2];
	error = pipe2(stdoutRW, O_CLOEXEC);
	if (error) err(1, "pipe2");

	int stderrRW[2];
	error = pipe2(stderrRW, O_CLOEXEC);
	if (error) err(1, "pipe2");

	fcntl(stdoutRW[0], F_SETFL, O_NONBLOCK);
	fcntl(stderrRW[0], F_SETFL, O_NONBLOCK);

	openlog(name, LOG_NDELAY | LOG_PID | LOG_PERROR, LOG_DAEMON);
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
	signal(SIGTERM, signalHandler);
	signal(SIGCHLD, signalHandler);
	signal(SIGINFO, signalHandler);
	signal(SIGUSR1, signalHandler);
	signal(SIGUSR2, signalHandler);

	pid_t child = 0;
	bool stop = false;
	struct timespec now = {0};
	struct timespec uptime = {0};
	struct timespec timeout = {0};
	struct timespec deadline = {0};
	struct timespec interval = restart;

	sigset_t mask;
	sigemptyset(&mask);
	struct pollfd fds[2] = {
		{ .fd = stdoutRW[0], .events = POLLIN },
		{ .fd = stderrRW[0], .events = POLLIN },
	};
	struct Line stdoutBuffer = {0};
	struct Line stderrBuffer = {0};

	for (;;) {
		if (signals[SIGHUP]) {
			interval = restart;
			if (child) killpg(child, SIGTERM);
			signals[SIGHUP] = 0;
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

		if (signals[SIGINFO]) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (child) {
				timespecsub(&now, &uptime, &timeout);
				syslog(LOG_INFO, "child %d up %s", child, humanize(&timeout));
			} else {
				timespecsub(&deadline, &now, &timeout);
				syslog(LOG_INFO, "restarting in %s", humanize(&timeout));
			}
			signals[SIGINFO] = 0;
		}

		if (signals[SIGUSR1] || signals[SIGUSR2]) {
			int sig = (signals[SIGUSR1] ? SIGUSR1 : SIGUSR2);
			if (child) killpg(child, sig);
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
			setproctitle("%s", name);

			if (WIFEXITED(status)) {
				int exit = WEXITSTATUS(status);
				if (exit == 127) stop = true;
				if (exit) syslog(LOG_NOTICE, "child exited %d", exit);
			} else if (WIFSIGNALED(status)) {
				int sig = WTERMSIG(status);
				if (sig != SIGTERM) {
					syslog(LOG_NOTICE, "child got signal %s", sys_signame[sig]);
				}
			}

			if (stop) break;
			clock_gettime(CLOCK_MONOTONIC, &now);
			timespecsub(&now, &uptime, &uptime);
			if (timespeccmp(&uptime, &cooloff, >=)) {
				interval = restart;
			}
			syslog(LOG_INFO, "restarting in %s", humanize(&interval));
			timespecadd(&now, &interval, &deadline);
			timespecadd(&interval, &interval, &interval);
		}

		if (!child) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (timespeccmp(&deadline, &now, <)) {
				timespecclear(&timeout);
			} else {
				timespecsub(&deadline, &now, &timeout);
			}
		}
		int nfds = ppoll(fds, 2, (child ? NULL : &timeout), &mask);
		if (nfds < 0 && errno != EINTR) {
			syslog(LOG_ERR, "ppoll: %m");
			continue;
		}

		if (nfds > 0 && fds[0].revents) {
			lineFill(&stdoutBuffer, fds[0].fd);
			lineFlush(&stdoutBuffer, LOG_INFO);
		}
		if (nfds > 0 && fds[1].revents) {
			lineFill(&stderrBuffer, fds[1].fd);
			lineFlush(&stderrBuffer, LOG_NOTICE);
		}

		if (!child) {
			child = fork();
			if (child < 0) {
				syslog(LOG_ERR, "fork: %m");
				return 1;
			}
			if (child) {
				clock_gettime(CLOCK_MONOTONIC, &uptime);
				setproctitle("%s [%d]", name, child);
			} else {
				setpgid(0, 0);
				dup2(stdoutRW[1], STDOUT_FILENO);
				dup2(stderrRW[1], STDERR_FILENO);
				execvp(argv[0], (char *const *)argv);
				err(127, "%s", argv[0]);
			}
		}
	}
}