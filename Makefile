PREFIX ?= /usr/local
MANDIR ?= ${PREFIX}/man
RCDIR ?= /etc/rc.d

CFLAGS += -std=c11 -Wall -Wextra

all: kitd rc_script

rc_script: rc_script.in
	sed 's|%%PREFIX%%|${PREFIX}|g' rc_script.in >rc_script

clean:
	rm -f kitd rc_script

install: kitd kitd.8 rc_script
	install -d ${DESTDIR}${PREFIX}/sbin
	install -d ${DESTDIR}${MANDIR}/man8
	install -d ${DESTDIR}${RCDIR}
	install kitd ${DESTDIR}${PREFIX}/sbin/kitd
	install -m 644 kitd.8 ${DESTDIR}${MANDIR}/man8/kitd.8
	install rc_script ${DESTDIR}${RCDIR}/kitd

uninstall:
	rm -f ${DESTDIR}${PREFIX}/sbin/kitd
	rm -f ${DESTDIR}${MANDIR}/man8/kitd.8
	rm -f ${DESTDIR}/etc/rc.d/kitd
