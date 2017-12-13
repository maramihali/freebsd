# $Id: meta.autodep.mk,v 1.46 2017/10/25 23:44:20 sjg Exp $

#
#	@(#) Copyright (c) 2010, Simon J. Gerraty
#
#	This file is provided in the hope that it will
#	be of use.  There is absolutely NO WARRANTY.
#	Permission to copy, redistribute or otherwise
#	use this file is hereby granted provided that 
#	the above copyright notice and this notice are
#	left intact. 
#      
#	Please send copies of changes and bug-fixes to:
#	sjg@crufty.net
#

_this ?= ${.PARSEFILE}
.if !target(__${_this}__)
__${_this}__: .NOTMAIN

.-include <local.autodep.mk>

.if defined(SRCS)
# it would be nice to be able to query .SUFFIXES
OBJ_EXTENSIONS+= .o .po .lo .So

# explicit dependencies help short-circuit .SUFFIX searches
SRCS_DEP_FILTER+= N*.[hly]
.for s in ${SRCS:${SRCS_DEP_FILTER:O:u:ts:}}
.for e in ${OBJ_EXTENSIONS:O:u}
.if !target(${s:T:R}$e)
${s:T:R}$e: $s
.endif
.endfor
.endfor
.endif

.if make(gendirdeps)
# you are supposed to know what you are doing!
UPDATE_DEPENDFILE = yes
.elif !empty(.TARGETS) && !make(all)
# do not update the *depend* files 
# unless we are building the entire directory or the default target.
# NO means don't update .depend - or Makefile.depend*
# no means update .depend but not Makefile.depend*
UPDATE_DEPENDFILE = NO
.elif ${.MAKEFLAGS:M-k} != ""
# it is a bad idea to update anything
UPDATE_DEPENDFILE = NO
.endif

_CURDIR ?= ${.CURDIR}
_OBJDIR ?= ${.OBJDIR}
_OBJTOP ?= ${OBJTOP}
_OBJROOT ?= ${OBJROOT:U${_OBJTOP}}
_DEPENDFILE := ${_CURDIR}/${.MAKE.DEPENDFILE:T}

.if ${.MAKE.LEVEL} > 0 || ${BUILD_AT_LEVEL0:Uyes:tl} == "yes"
# do not allow auto update if we ever built this dir without filemon
NO_FILEMON_COOKIE = .nofilemon
CLEANFILES += ${NO_FILEMON_COOKIE}
.if ${.MAKE.MODE:Uno:Mnofilemon} != ""
UPDATE_DEPENDFILE = NO
all: ${NO_FILEMON_COOKIE}
${NO_FILEMON_COOKIE}: .NOMETA
	@echo UPDATE_DEPENDFILE=NO > ${.TARGET}
.elif exists(${NO_FILEMON_COOKIE})
UPDATE_DEPENDFILE = NO
.warning ${RELDIR} built with nofilemon; UPDATE_DEPENDFILE=NO
.endif
.endif

.if ${.MAKE.LEVEL} == 0
.if ${BUILD_AT_LEVEL0:Uyes:tl} == "no"
UPDATE_DEPENDFILE = NO
.endif
.endif
.if !exists(${_DEPENDFILE})
_bootstrap_dirdeps = yes
.endif
_bootstrap_dirdeps ?= no
UPDATE_DEPENDFILE ?= yes

.if ${DEBUG_AUTODEP:Uno:@m@${RELDIR:M$m}@} != ""
.info ${_DEPENDFILE:S,${SRCTOP}/,,} update=${UPDATE_DEPENDFILE}
.endif

.if !empty(XMAKE_META_FILE)
.if exists(${.OBJDIR}/${XMAKE_META_FILE})
# we cannot get accurate dependencies from an update build
UPDATE_DEPENDFILE = NO
.else
META_XTRAS += ${XMAKE_META_FILE}
.endif
.endif

.if ${_bootstrap_dirdeps} == "yes" || exists(${_DEPENDFILE})
# if it isn't supposed to be touched by us the Makefile should have
# UPDATE_DEPENDFILE = no
WANT_UPDATE_DEPENDFILE ?= yes
.endif

.if ${WANT_UPDATE_DEPENDFILE:Uno:tl} != "no"
.if ${.MAKE.MODE:Uno:Mmeta*} == "" || ${.MAKE.MODE:Uno:M*read*} != ""
UPDATE_DEPENDFILE = no
.endif

.if ${DEBUG_AUTODEP:Uno:@m@${RELDIR:M$m}@} != ""
.info ${_DEPENDFILE:S,${SRCTOP}/,,} update=${UPDATE_DEPENDFILE}
.endif

.if ${UPDATE_DEPENDFILE:tl} == "yes"
# sometimes we want .meta files generated to aid debugging/error detection
# but do not want to consider them for dependencies
# for example the result of running configure
# just make sure this is not empty
META_FILE_FILTER ?= N.meta
# never consider these
META_FILE_FILTER += Ndirdeps.cache*

.if !empty(DPADD)
# if we have any non-libs in DPADD, 
# they probably need to be paid attention to
.if !empty(DPLIBS)
FORCE_DPADD = ${DPADD:${DPLIBS:${M_ListToSkip}}:${DPADD_LAST:${M_ListToSkip}}}
.else
_nonlibs := ${DPADD:T:Nlib*:N*include}
.if !empty(_nonlibs)
FORCE_DPADD += ${_nonlibs:@x@${DPADD:M*/$x}@}
.endif
.endif
.endif

.if !make(gendirdeps)
.END:	gendirdeps
.endif

# if we don't have OBJS, then .depend isn't useful
.if !target(.depend) && (!empty(OBJS) || ${.ALLTARGETS:M*.o} != "")
# some makefiles and/or targets contain
# circular dependencies if you dig too deep 
# (as meta mode is apt to do) 
# so we provide a means of suppressing them.
# the input to the loop below is target: dependency
# with just one dependency per line.
# Also some targets are not really local, or use random names.
# Use local.autodep.mk to provide local additions!
SUPPRESS_DEPEND += \
	${SB:S,/,_,g}* \
	*:y.tab.c \
	*.c:*.c \
	*.h:*.h

.NOPATH:	.depend
# we use ${.MAKE.META.CREATED} to trigger an update but
# we process using ${.MAKE.META.FILES}
# the double $$ defers initial evaluation
# if necessary, we fake .po dependencies, just so the result 
# in Makefile.depend* is stable
# The current objdir may be referred to in various ways
OBJDIR_REFS += ${.OBJDIR} ${.OBJDIR:tA} ${_OBJDIR} ${RELOBJTOP}/${RELDIR}
_depend = .depend
# it would be nice to be able to get .SUFFIXES as ${.SUFFIXES}
# we actually only care about the .SUFFIXES of files that might be 
# generated by tools like yacc.
DEPEND_SUFFIXES += .c .h .cpp .hpp .cxx .hxx .cc .hh
.depend: .NOMETA $${.MAKE.META.CREATED} ${_this}
	@echo "Updating $@: ${.OODATE:T:[1..8]}"
	@egrep -i '^R .*\.(${DEPEND_SUFFIXES:tl:O:u:S,^.,,:ts|})$$' /dev/null ${.MAKE.META.FILES:T:O:u:${META_FILE_FILTER:ts:}:M*o.meta} | \
	sed -e 's, \./, ,${OBJDIR_REFS:O:u:@d@;s, $d/, ,@};/\//d' \
		-e 's,^\([^/][^/]*\).meta...[0-9]* ,\1: ,' | \
	sort -u | \
	while read t d; do \
		case "$$d:" in $$t) continue;; esac; \
		case "$$t$$d" in ${SUPPRESS_DEPEND:U.:O:u:ts|}) continue;; esac; \
		echo $$t $$d; \
	done > $@.${.MAKE.PID}
	@case "${.MAKE.META.FILES:T:M*.po.*}" in \
	*.po.*) mv $@.${.MAKE.PID} $@;; \
	*) { cat $@.${.MAKE.PID}; \
	sed 's,\.So:,.o:,;s,\.o:,.po:,' $@.${.MAKE.PID}; } | sort -u > $@; \
	rm -f $@.${.MAKE.PID};; \
	esac
.else
# make sure this exists
.depend:
# do _not_ assume that .depend is in any fit state for us to use
CAT_DEPEND = /dev/null
.if ${.MAKE.LEVEL} > 0
.export CAT_DEPEND
.endif
_depend =
.endif

.if ${DEBUG_AUTODEP:Uno:@m@${RELDIR:M$m}@} != ""
.info ${_DEPENDFILE:S,${SRCTOP}/,,} _depend=${_depend}
.endif

.if ${UPDATE_DEPENDFILE} == "yes"
gendirdeps:	${_DEPENDFILE}
.endif

.if !target(${_DEPENDFILE})
.if ${_bootstrap_dirdeps} == "yes"
# We are boot-strapping a new directory
# Use DPADD to seed DIRDEPS
.if !empty(DPADD)
# anything which matches ${_OBJROOT}* but not ${_OBJTOP}*
# needs to be qualified in DIRDEPS
# The pseudo machine "host" is used for HOST_TARGET
DIRDEPS += \
	${DPADD:M${_OBJTOP}*:H:C,${_OBJTOP}[^/]*/,,:N.:O:u} \
	${DPADD:M${_OBJROOT}*:N${_OBJTOP}*:N${STAGE_ROOT:U${_OBJTOP}}/*:H:S,${_OBJROOT},,:C,^([^/]+)/(.*),\2.\1,:S,${HOST_TARGET}$,host,:N.*:O:u}

.endif
.endif

_gendirdeps_mutex =
.if defined(NEED_GENDIRDEPS_MUTEX)
# If a src dir gets built with multiple object dirs,
# we need a mutex.  Obviously, this is best avoided.
# Note if .MAKE.DEPENDFILE is common for all ${MACHINE}
# you either need to mutex, or ensure only one machine builds at a time!
# lockf is an example of a suitable tool
LOCKF ?= /usr/bin/lockf
.if exists(${LOCKF})
GENDIRDEPS_MUTEXER ?= ${LOCKF} -k
.endif
.if empty(GENDIRDEPS_MUTEXER)
.error NEED_GENDIRDEPS_MUTEX defined, but GENDIRDEPS_MUTEXER not set
.else
_gendirdeps_mutex = ${GENDIRDEPS_MUTEXER} ${GENDIRDEPS_MUTEX:U${_CURDIR}/Makefile}
.endif
.endif

# If we have META_XTRAS we most likely did not create them
# but we need to behave as if we did.
# Avoid adding glob patterns to .MAKE.META.CREATED though.
.MAKE.META.CREATED += ${META_XTRAS:N*\**:O:u}

.if make(gendirdeps)
META_FILES = *.meta
.elif ${OPTIMIZE_OBJECT_META_FILES:Uno:tl} == "no"
META_FILES = ${.MAKE.META.FILES:T:N.depend*:O:u}
.else
# if we have 1000's of .o.meta, .So.meta etc we need only look at one set
# it is left as an exercise for the reader to work out what this does
META_FILES = ${.MAKE.META.FILES:T:N.depend*:N*o.meta:O:u} \
	${.MAKE.META.FILES:T:M*.${.MAKE.META.FILES:M*o.meta:R:E:O:u:[1]}.meta:O:u}
.endif

.if ${DEBUG_AUTODEP:Uno:@m@${RELDIR:M$m}@} != ""
.info ${_DEPENDFILE:S,${SRCTOP}/,,}: ${_depend} ${.PARSEDIR}/gendirdeps.mk ${META2DEPS} xtras=${META_XTRAS}
.endif

.if ${.MAKE.LEVEL} > 0 && !empty(GENDIRDEPS_FILTER)
.export GENDIRDEPS_FILTER
.endif

# we might have .../ in MAKESYSPATH
_makesyspath:= ${_PARSEDIR}
${_DEPENDFILE}: ${_depend} ${.PARSEDIR}/gendirdeps.mk  ${META2DEPS} $${.MAKE.META.CREATED}
	@echo Checking $@: ${.OODATE:T:[1..8]}
	@(cd . && ${GENDIRDEPS_ENV} \
	SKIP_GENDIRDEPS='${SKIP_GENDIRDEPS:O:u}' \
	DPADD='${FORCE_DPADD:O:u}' ${_gendirdeps_mutex} \
	MAKESYSPATH=${_makesyspath} \
	${.MAKE} -f gendirdeps.mk RELDIR=${RELDIR} _DEPENDFILE=${_DEPENDFILE} \
	META_FILES='${META_XTRAS:O:u} ${META_FILES:T:O:u:${META_FILE_FILTER:ts:}}')
	@test -s $@ && touch $@; :
.endif

.endif
.endif

.if ${_bootstrap_dirdeps} == "yes"
.if ${BUILD_AT_LEVEL0:Uno} == "no"
DIRDEPS+= ${RELDIR}.${TARGET_SPEC:U${MACHINE}}
.endif
# make sure this is included at least once
.include <dirdeps.mk>
.else
${_DEPENDFILE}: .PRECIOUS
.endif

CLEANFILES += *.meta filemon.* *.db

# these make it easy to gather some stats
now_utc = ${%s:L:gmtime}
start_utc := ${now_utc}

meta_stats= meta=${empty(.MAKE.META.FILES):?0:${.MAKE.META.FILES:[#]}} \
	created=${empty(.MAKE.META.CREATED):?0:${.MAKE.META.CREATED:[#]}}

#.END: _reldir_finish
.if target(gendirdeps)
_reldir_finish: gendirdeps
.endif
_reldir_finish: .NOMETA
	@echo "${TIME_STAMP} Finished ${RELDIR}.${TARGET_SPEC} seconds=$$(( ${now_utc} - ${start_utc} )) ${meta_stats}"

#.ERROR: _reldir_failed
_reldir_failed: .NOMETA
	@echo "${TIME_STAMP} Failed ${RELDIR}.${TARGET_SPEC} seconds=$$(( ${now_utc} - ${start_utc} )) ${meta_stats}"

.if defined(WITH_META_STATS) && ${.MAKE.LEVEL} > 0
.END: _reldir_finish
.ERROR: _reldir_failed
.endif

.endif
