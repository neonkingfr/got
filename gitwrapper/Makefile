.PATH:${.CURDIR}/../lib
.PATH:${.CURDIR}/../gotd

.include "../got-version.mk"

.if ${GOT_RELEASE} == "Yes"
BINDIR ?=	${PREFIX}/bin
.endif

PROG=		gitwrapper

SRCS=		gitwrapper.c parse.y log.c dial.c path.c error.c \
		reference_parse.c hash.c object_qid.c

CLEANFILES = parse.h

MAN =		${PROG}.1

CPPFLAGS = -I${.CURDIR}/../include -I${.CURDIR}/../lib -I${.CURDIR}/../gotd

.if ${GOT_RELEASE} != "Yes"
NOMAN = Yes
.endif

realinstall:
	${INSTALL} ${INSTALL_COPY} -o ${BINOWN} -g ${BINGRP} \
	-m ${BINMODE} ${PROG} ${BINDIR}/${PROG}

.include <bsd.prog.mk>
