PREFIX ?= /usr/local
MANDIR ?= ${PREFIX}/man

CFLAGS += -std=c11 -Wall -Wextra

all: kitd rc

rc: rc.in
	sed 's|%%PREFIX%%|${PREFIX}|g' rc.in >rc

clean:
	rm -f kitd rc

install: kitd kitd.8 rc
	install -d ${DESTDIR}${PREFIX}/sbin ${DESTDIR}${MANDIR}/man8
	install -d ${DESTDIR}/etc/rc.d
	install kitd ${DESTDIR}${PREFIX}/sbin
	install -m 644 kitd.8 ${DESTDIR}${MANDIR}/man8
	install rc ${DESTDIR}/etc/rc.d/kitd

uninstall:
	rm -f ${DESTDIR}${PREFIX}/sbin/kitd ${DESTDIR}${MANDIR}/man8/kitd.8
	rm -f ${DESTDIR}/etc/rc.d/kitd
