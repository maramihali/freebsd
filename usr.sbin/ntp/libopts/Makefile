# $FreeBSD$

.PATH: ${SRCTOP}/contrib/ntp/sntp/libopts

LIB=	opts
INTERNALLIB=

MK_BITCODE_EVERYWHERE=no


SRCS=	libopts.c

CFLAGS+= -I${SRCTOP}/contrib/ntp/include \
	-I${SRCTOP}/contrib/ntp/sntp/libopts \
	-I${.CURDIR:H}

.include <bsd.lib.mk>
