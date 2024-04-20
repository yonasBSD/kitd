# KITD(8) 	System Manager's Manual 	KITD(8)

## NAME

kitd — process supervisor

## SYNOPSIS

kitd 	[-d] [-c cooloff] [-m maximum] [-n name] [-t restart] command ...

## DESCRIPTION

The kitd daemon supervises a child process, redirecting its standard output and standard error to syslog(3). When the child process exits, it is automatically restarted using exponential backoff.

The options are as follows:

-c cooloff
    The interval for which the child process must live before the restart interval is reset to its initial value.

    The interval may have a suffix of s, m, h or d for seconds, minutes, hours or days, respectively. Otherwise, the interval is in milliseconds.

    The default cooloff interval is 15m.
-d
    Do not daemonize. Log to standard error as well as syslog(3).

-m maximum
    The maximum interval between restarts.

    The interval is interpreted as with -c. The default maximum interval is 1h.

-n name
    Set the name of the process and the logging prefix. The default is the last path component of command.

-t restart
    The initial interval between restarts. This interval is doubled each time the child process is restarted.

    The interval is interpreted as with -c. The default restart interval is 1s.

kitd responds to the following signals:

SIGTERM | SIGINT
    The signal is forwarded to the child process. kitd exits.

SIGINFO
    The status of the child process is logged.

SIGHUP | SIGUSR1 | SIGUSR2
    The signal is forwarded to the child process.

## EXAMPLES

To set up supervisors for pounce(1):

# ln -s kitd /etc/rc.d/pounce_tilde
# ln -s kitd /etc/rc.d/pounce_libera
# rcctl enable pounce_tilde pounce_libera
# rcctl set pounce_tilde user _pounce
# rcctl set pounce_tilde flags pounce -h irc.tilde.chat defaults.conf
# rcctl set pounce_libera user _pounce
# rcctl set pounce_libera flags pounce -h irc.libera.chat defaults.conf
# rcctl start pounce_tilde pounce_libera

## AUTHORS

June McEnroe <june@causal.agency>
October 10, 2023 	OpenBSD 7.4
