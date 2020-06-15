/*-
 * Copyright (c) 2019 Domagoj Stolfa
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <dt_elf.h>
#include <dt_program.h>
#include <dt_impl.h>
#include <dt_resolver.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <sys/stat.h>

#include <libelf.h>
#include <gelf.h>

#include <err.h>
#include <errno.h>

#define DTELF_MAXOPTNAME	64

/*
 * Helper structs
 */
typedef struct dt_elf_eact_list {
	dt_list_t list;
	dtrace_actdesc_t *act;
	dt_elf_ref_t eact_ndx;
	dt_elf_actdesc_t *eact;
} dt_elf_eact_list_t;

typedef struct _dt_elf_eopt {
	char eo_name[DTELF_MAXOPTNAME];
	uint64_t eo_option;
	size_t eo_len;
	char eo_arg[];
} _dt_elf_eopt_t;

/*
 * dt_elf_state_t: A state struct used during ELF generation and is
 * context-dependent.
 *
 * s_first_act_scn:     The first action of the current stmt.
 * s_last_act_scn:      The last action of the current stmt.
 * s_first_ecbdesc_scn: The first ecbdesc of the current stmt.
 * s_actions:           List that contains all the actions inside the ELF file.
 * s_idname_table:      A table containing all of the identifier names.
 * s_idname_size:       Size of the idname table.
 * s_idname_offset:     Offset into the next-to-be-added entry in the table.
 */
typedef struct dt_elf_state {
	dt_elf_ref_t s_first_act_scn;
	dt_elf_ref_t s_last_act_scn;
	dt_elf_ref_t s_first_ecbdesc_scn;
	dt_list_t s_actions;
	dt_elf_actdesc_t *s_eadprev;
	char *s_idname_table;
	size_t s_idname_size;
	size_t s_idname_offset;
	uint32_t s_rflags;
} dt_elf_state_t;

char sec_strtab[] =
	"\0.shstrtab\0.dtrace_prog\0.dtrace_difo\0.dtrace_actdesc\0"
	".dtrace_ecbdesc\0.difo_strtab\0.difo_inttab\0"
	".difo_symtab\0.dtrace_stmtdesc\0.dtrace_predicate\0"
	".dtrace_opts\0.dtrace_vartab\0.dtrace_stmt_idname_table\0"
	".dtrace_ident";

#define	DTELF_SHSTRTAB		  1
#define	DTELF_PROG		 11
#define	DTELF_DIFO		 24
#define	DTELF_ACTDESC		 37
#define	DTELF_ECBDESC		 53
#define	DTELF_DIFOSTRTAB	 69
#define	DTELF_DIFOINTTAB	 82
#define	DTELF_DIFOSYMTAB	 95
#define	DTELF_STMTDESC		108
#define	DTELF_PREDICATE		125
#define	DTELF_OPTS		143
#define	DTELF_DIFOVARTAB	156
#define	DTELF_IDNAMETAB		171
#define	DTELF_IDENT		197

#define	DTELF_VARIABLE_SIZE	  0

#define	DTELF_PROG_SECIDX	  2

#define DTELF_BASESIZE		  0

static dt_elf_state_t *dtelf_state;

dt_elf_opt_t dtelf_ctopts[] = {
	{ "aggpercpu", 0, NULL, DTRACE_A_PERCPU },
	{ "amin", 0, NULL, 0 },
	{ "argref", 0, NULL, DTRACE_C_ARGREF },
	{ "core", 0, NULL, 0 },
	{ "cpp", 0, NULL, DTRACE_C_CPP },
	{ "cpphdrs", 0, NULL, 0 },
	{ "cpppath", 0, NULL, 0 },
	{ "ctypes", 0, NULL, 0 },
	{ "defaultargs", 0, NULL, DTRACE_C_DEFARG },
	{ "dtypes", 0, NULL, 0 },
	{ "debug", 0, NULL, 0 },
	{ "define", 0, NULL, (uintptr_t)"-D" },
	{ "droptags", 0, NULL, 0 },
	{ "empty", 0, NULL, DTRACE_C_EMPTY },
	{ "encoding", 0, NULL, 0 },
	{ "errtags", 0, NULL, DTRACE_C_ETAGS },
	{ "evaltime", 0, NULL, 0 },
	{ "incdir", 0, NULL, (uintptr_t)"-I" },
	{ "iregs", 0, NULL, 0 },
	{ "kdefs", 0, NULL, DTRACE_C_KNODEF },
	{ "knodefs", 0, NULL, DTRACE_C_KNODEF },
	{ "late", 0, NULL, 0 },
	{ "lazyload", 0, NULL, 0 },
	{ "ldpath", 0, NULL, 0 },
	{ "libdir", 0, NULL, 0 },
	{ "linkmode", 0, NULL, 0 },
	{ "linktype", 0, NULL, 0 },
	{ "nolibs", 0, NULL, DTRACE_C_NOLIBS },
#ifdef __FreeBSD__
	{ "objcopypath", 0, NULL, 0 },
#endif
	{ "pgmax", 0, NULL, 0 },
	{ "pspec", 0, NULL, DTRACE_C_PSPEC },
	{ "setenv", 0, NULL, 1 },
	{ "stdc", 0, NULL, 0 },
	{ "strip", 0, NULL, DTRACE_D_STRIP },
	{ "syslibdir", 0, NULL, 0 },
	{ "tree", 0, NULL, 0 },
	{ "tregs", 0, NULL, 0 },
	{ "udefs", 0, NULL, DTRACE_C_UNODEF },
	{ "undef", 0, NULL, (uintptr_t)"-U" },
	{ "unodefs", 0, NULL, DTRACE_C_UNODEF },
	{ "unsetenv", 0, NULL, 0 },
	{ "verbose", 0, NULL, DTRACE_C_DIFV },
	{ "version", 0, NULL, 0 },
	{ "zdefs", 0, NULL, DTRACE_C_ZDEFS },
	{ NULL, 0, NULL, 0 }
};

dt_elf_opt_t dtelf_rtopts[] = {
	{ "aggsize", 0, NULL, DTRACEOPT_AGGSIZE },
	{ "bufsize", 0, NULL, DTRACEOPT_BUFSIZE },
	{ "bufpolicy", 0, NULL, DTRACEOPT_BUFPOLICY },
	{ "bufresize", 0, NULL, DTRACEOPT_BUFRESIZE },
	{ "cleanrate", 0, NULL, DTRACEOPT_CLEANRATE },
	{ "cpu", 0, NULL, DTRACEOPT_CPU },
	{ "destructive", 0, NULL, DTRACEOPT_DESTRUCTIVE },
	{ "dynvarsize", 0, NULL, DTRACEOPT_DYNVARSIZE },
	{ "grabanon", 0, NULL, DTRACEOPT_GRABANON },
	{ "jstackframes", 0, NULL, DTRACEOPT_JSTACKFRAMES },
	{ "ddtracearg", 0, NULL, DTRACEOPT_DDTRACEARG},
	{ "jstackstrsize", 0, NULL, DTRACEOPT_JSTACKSTRSIZE },
	{ "nspec", 0, NULL, DTRACEOPT_NSPEC },
	{ "specsize", 0, NULL, DTRACEOPT_SPECSIZE },
	{ "stackframes", 0, NULL, DTRACEOPT_STACKFRAMES },
	{ "statusrate", 0, NULL, DTRACEOPT_STATUSRATE },
	{ "strsize", 0, NULL, DTRACEOPT_STRSIZE },
	{ "ustackframes", 0, NULL, DTRACEOPT_USTACKFRAMES },
	{ "temporal", 0, NULL, DTRACEOPT_TEMPORAL },
	{ NULL, 0, NULL, 0 }
};

dt_elf_opt_t dtelf_drtopts[] = {
	{ "agghist", 0, NULL, DTRACEOPT_AGGHIST },
	{ "aggpack", 0, NULL, DTRACEOPT_AGGPACK },
	{ "aggrate", 0, NULL, DTRACEOPT_AGGRATE },
	{ "aggsortkey", 0, NULL, DTRACEOPT_AGGSORTKEY },
	{ "aggsortkeypos", 0, NULL, DTRACEOPT_AGGSORTKEYPOS },
	{ "aggsortpos", 0, NULL, DTRACEOPT_AGGSORTPOS },
	{ "aggsortrev", 0, NULL, DTRACEOPT_AGGSORTREV },
	{ "aggzoom", 0, NULL, DTRACEOPT_AGGZOOM },
	{ "flowindent", 0, NULL, DTRACEOPT_FLOWINDENT },
	{ "oformat", 0, NULL, DTRACEOPT_OFORMAT },
	{ "quiet", 0, NULL, DTRACEOPT_QUIET },
	{ "rawbytes", 0, NULL, DTRACEOPT_RAWBYTES },
	{ "stackindent", 0, NULL, DTRACEOPT_STACKINDENT },
	{ "switchrate", 0, NULL, DTRACEOPT_SWITCHRATE },
	{ NULL, 0, NULL, 0 }
};


static Elf_Scn *
dt_elf_new_inttab(Elf *e, dtrace_difo_t *difo)
{
	Elf_Scn *scn;
	Elf32_Shdr *shdr;
	Elf_Data *data;
	uint64_t *inttab;

	/*
	 * If the integer table is NULL, we return a NULL section,
	 * which will return section index 0 when passed into elf_ndxscn().
	 */
	if (difo->dtdo_inttab == NULL)
		return (NULL);

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	inttab = malloc(DTELF_BASESIZE + sizeof(uint64_t) * difo->dtdo_intlen);
	if (inttab == NULL)
		errx(EXIT_FAILURE, "failed to malloc inttab");

	/*
	 * Populate the temporary buffer that will contain our integer table.
	 */
	memcpy(inttab, difo->dtdo_inttab, sizeof(uint64_t) * difo->dtdo_intlen);

	/*
	 * For the integer table, we require an alignment of 8 and specify it as
	 * a bunch of bytes (ELF_T_BYTE) because this is a 32-bit ELF file.
	 *
	 * In the case that this is parsed on a 32-bit machine, we deal with it
	 * in the same way that DTrace deals with 64-bit integers in the inttab
	 * on 32-bit machines.
	 */
	data->d_align = 8;
	data->d_buf = inttab;
	data->d_size = sizeof(uint64_t) * difo->dtdo_intlen;
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	/*
	 * The entsize is set to sizeof(uint64_t) because each entry is a 64-bit
	 * integer, which is fixed-size. According to the ELF specification, we
	 * have to specify what the size of each entry is if it is fixed-size.
	 */
	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_DIFOINTTAB;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = sizeof(uint64_t);

	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);
	return (scn);
}

static Elf_Scn *
dt_elf_new_strtab(Elf *e, dtrace_difo_t *difo)
{
	Elf_Scn *scn = NULL;
	Elf32_Shdr *shdr;
	Elf_Data *data;
	char *c;
	char *strtab;

	/*
	 * If the string table is NULL, we return a NULL section,
	 * which will return section index 0 when passed into elf_ndxscn().
	 */
	if (difo->dtdo_strtab == NULL)
		return (NULL);

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	strtab = malloc(DTELF_BASESIZE + difo->dtdo_strlen);
	if (strtab == NULL)
		errx(EXIT_FAILURE, "failed to malloc strtab");

	/*
	 * Populate the temporary buffer that will contain our string table.
	 */
	memcpy(strtab, difo->dtdo_strtab, difo->dtdo_strlen);

	/*
	 * We don't have any special alignment requirements. Treat this as an
	 * ordinary string table in ELF (apart from the specification in the
	 * section header).
	 */
	data->d_align = 1;
	data->d_buf = strtab;
	data->d_size = difo->dtdo_strlen;
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		    elf_errmsg(-1));

	/*
	 * The strings in the string table are not fixed-size, so entsize is set to 0.
	 */
	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_DIFOSTRTAB;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = 0;

	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	return (scn);
}

/*
 * A symbol table in DTrace is just a string table. This subroutine handles yet another
 * string table with minimal differences from the regular DIFO string table.
 */
static Elf_Scn *
dt_elf_new_symtab(Elf *e, dtrace_difo_t *difo)
{
	Elf_Scn *scn;
	Elf32_Shdr *shdr;
	Elf_Data *data;
	char *symtab;

	if (difo->dtdo_symtab == NULL)
		return (NULL);

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	symtab = malloc(DTELF_BASESIZE + difo->dtdo_symlen);
	if (symtab == NULL)
		errx(EXIT_FAILURE, "failed to malloc symtab");

	memcpy(symtab, difo->dtdo_symtab, difo->dtdo_symlen);

	data->d_align = 1;
	data->d_buf = symtab;
	data->d_size = difo->dtdo_symlen;
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_DIFOSYMTAB;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = 0;

	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	return (scn);
}

static Elf_Scn *
dt_elf_new_vartab(Elf *e, dtrace_difo_t *difo)
{
	Elf_Scn *scn;
	Elf32_Shdr *shdr;
	Elf_Data *data;
	dtrace_difv_t *vartab;

	/*
	 * If the variable table is NULL, we return a NULL section,
	 * which will return section index 0 when passed into elf_ndxscn().
	 */
	if (difo->dtdo_vartab == NULL)
		return (NULL);

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	vartab = malloc(DTELF_BASESIZE +
	    sizeof(dtrace_difv_t) * difo->dtdo_varlen);
	if (vartab == NULL)
		errx(EXIT_FAILURE, "failed to malloc vartab");

	/*
	 * Populate the temporary buffer that will contain our variable table.
	 */
	memcpy(vartab, difo->dtdo_vartab, sizeof(dtrace_difv_t) * difo->dtdo_varlen);

	/*
	 * On both 32 and 64-bit architectures, dtrace_difv_t only requires
	 * an alignment of 4.
	 */
	data->d_align = 4;
	data->d_buf = vartab;
	data->d_size = difo->dtdo_varlen * sizeof(dtrace_difv_t);
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	/*
	 * Each entry is of fixed size, so entsize is set to sizeof(dtrace_difv_t).
	 */
	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_DIFOVARTAB;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = sizeof(dtrace_difv_t);

	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	return (scn);
}

static Elf_Scn *
dt_elf_new_difo(Elf *e, dtrace_difo_t *difo)
{
	Elf_Scn *scn;
	Elf32_Shdr *shdr;
	Elf_Data *data;
	uint64_t i;
	dt_elf_difo_t *edifo;
	char *c;

	/*
	 * If the difo is NULL, we return a NULL section,
	 * which will return section index 0 when passed into elf_ndxscn().
	 */
	if (difo == NULL)
		return (NULL);

	/*
	 * Each dt_elf_difo_t has a flexible array member at the end of it that
	 * contains all of the instructions associated with a DIFO. In order to
	 * avoid creating a separate section that contains the instructions, we
	 * simply put them at the end of the DIFO.
	 *
	 * Here, we allocate the edifo according to how many instructions are present
	 * in the current DIFO (dtdo_len).
	 */
	edifo = malloc(sizeof(dt_elf_difo_t) +
	    (difo->dtdo_len * sizeof(dif_instr_t)));
	if (edifo == NULL)
		errx(EXIT_FAILURE, "failed to malloc edifo");

	/* Zero the edifo to achieve determinism */
	memset(edifo, 0, sizeof(dt_elf_difo_t) +
	    (difo->dtdo_len * sizeof(dif_instr_t)));

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	/*
	 * From each DIFO table (integer, string, symbol, variable), get the reference
	 * to the corresponding ELF section that contains it.
	 */
	edifo->dted_inttab = elf_ndxscn(dt_elf_new_inttab(e, difo));
	edifo->dted_strtab = elf_ndxscn(dt_elf_new_strtab(e, difo));
	edifo->dted_symtab = elf_ndxscn(dt_elf_new_symtab(e, difo));
	edifo->dted_vartab = elf_ndxscn(dt_elf_new_vartab(e, difo));

	/*
	 * Fill in the rest of the fields.
	 */
	edifo->dted_intlen = difo->dtdo_intlen;
	edifo->dted_strlen = difo->dtdo_strlen;
	edifo->dted_symlen = difo->dtdo_symlen;
	edifo->dted_varlen = difo->dtdo_varlen;
	edifo->dted_rtype = difo->dtdo_rtype;
	edifo->dted_destructive = difo->dtdo_destructive;

	edifo->dted_len = difo->dtdo_len;

	/*
	 * Fill in the DIF instructions.
	 */
	for (i = 0; i < difo->dtdo_len; i++)
		edifo->dted_buf[i] = difo->dtdo_buf[i];

	/*
	 * Because of intlen/strlen/symlen/varlen/etc, we require the section data to
	 * be 8-byte aligned.
	 */
	data->d_align = 8;
	data->d_buf = edifo;
	data->d_size = sizeof(dt_elf_difo_t) +
	    (difo->dtdo_len * sizeof(dif_instr_t));
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	/*
	 * This is a section containing just _one_ DIFO. Therefore its size is not
	 * variable and we specify entsize to be the size of the whole section.
	 */
	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_DIFO;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = sizeof(dt_elf_difo_t) +
	    (difo->dtdo_len * sizeof(dif_instr_t));

	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	return (scn);
}

/*
 * This subroutine is always called with a valid actdesc.
 */
static Elf_Scn *
dt_elf_new_action(Elf *e, dtrace_actdesc_t *ad, dt_elf_ref_t sscn)
{
	Elf_Scn *scn;
	Elf32_Shdr *shdr;
	Elf_Data *data;
	dt_elf_actdesc_t *eact;
	dt_elf_eact_list_t *el;

	eact = malloc(sizeof(dt_elf_actdesc_t));
	if (eact == NULL)
		errx(EXIT_FAILURE, "failed to malloc eact");

	/*
	 * We will keep the actions in a list in order to
	 * simplify the code when creating the ECBs.
	 */
	el = malloc(sizeof(dt_elf_eact_list_t));
	if (el == NULL)
		errx(EXIT_FAILURE, "failed to malloc el");

	memset(eact, 0, sizeof(dt_elf_actdesc_t));
	memset(el, 0, sizeof(dt_elf_eact_list_t));

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	if (ad->dtad_difo != NULL) {
		eact->dtea_difo = elf_ndxscn(dt_elf_new_difo(e, ad->dtad_difo));
	} else
		eact->dtea_difo = 0;

	/*
	 * Fill in all of the simple struct members.
	 */
	eact->dtea_next = 0; /* Filled in later */
	eact->dtea_kind = ad->dtad_kind;
	eact->dtea_ntuple = ad->dtad_ntuple;
	eact->dtea_arg = ad->dtad_arg;
	eact->dtea_uarg = sscn;

	data->d_align = 8;
	data->d_buf = eact;
	data->d_size = sizeof(dt_elf_actdesc_t);
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	/*
	 * Since actions are of fixed size (because they contain references to a DIFO)
	 * and other actions, instead of varying in size because they contain the DIFO
	 * itself, we set entsize to sizeof(dt_elf_actdesc_t). In the future, we may
	 * consider a section that contains all of the actions, rather than a separate
	 * section for each action, but this would require some re-engineering of the
	 * code around ECBs.
	 */
	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_ACTDESC;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = sizeof(dt_elf_actdesc_t);
	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	/*
	 * Fill in the information that we will keep in a list (section index, action,
	 * ELF representation of an action).
	 */
	el->eact_ndx = elf_ndxscn(scn);
	el->act = ad;
	el->eact = eact;

	dt_list_append(&dtelf_state->s_actions, el);
	return (scn);
}

static void
dt_elf_create_actions(Elf *e, dtrace_stmtdesc_t *stmt, dt_elf_ref_t sscn)
{
	Elf_Scn *scn;
	Elf32_Shdr *shdr;
	Elf_Data *data = NULL;
	dtrace_actdesc_t *ad;

	if (stmt->dtsd_action == NULL)
		return;

	/*
	 * If we have the first action, then we better have the last action as well.
	 */
	if (stmt->dtsd_action_last == NULL)
		errx(EXIT_FAILURE, "dtsd_action_last is NULL, but dtsd_action is not.");

	/*
	 * We iterate through the actions, creating a new section with its data filled
	 * with an ELF representation for each DTrace action we iterate through. We then
	 * refer to the previous action we created in our list of actions and assign the
	 * next reference in the ELF file, which constructs the "action list" as known
	 * in DTrace, but in our ELF file.
	 */
	for (ad = stmt->dtsd_action;
	    ad != stmt->dtsd_action_last->dtad_next && ad != NULL;
	    ad = ad->dtad_next) {
		scn = dt_elf_new_action(e, ad, sscn);

		if (dtelf_state->s_eadprev != NULL)
			dtelf_state->s_eadprev->dtea_next = elf_ndxscn(scn);

		if ((data = elf_getdata(scn, NULL)) == NULL)
			errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
			    elf_errmsg(-1), __func__);

		if (data->d_buf == NULL)
			errx(EXIT_FAILURE, "data->d_buf must not be NULL.");

		dtelf_state->s_eadprev = data->d_buf;

		/*
		 * If this is the first action, we will save it in order to fill in
		 * the necessary data in the ELF representation of a D program. It needs
		 * a reference to the first action.
		 */
		if (ad == stmt->dtsd_action)
			dtelf_state->s_first_act_scn = elf_ndxscn(scn);
	}
	/*
	 * We know that this is the last section that we could have
	 * created, so we simply set the state variable to it.
	 */
	dtelf_state->s_last_act_scn = elf_ndxscn(scn);
}

static Elf_Scn *
dt_elf_new_ecbdesc(Elf *e, dtrace_stmtdesc_t *stmt)
{
	Elf_Scn *scn;
	Elf32_Shdr *shdr;
	Elf_Data *data = NULL;
	dtrace_ecbdesc_t *ecb;
	dt_elf_ecbdesc_t *eecb;
	dt_elf_eact_list_t *el = NULL;

	if (stmt->dtsd_ecbdesc == NULL)
		return (NULL);

	eecb = malloc(sizeof(dt_elf_ecbdesc_t));
	if (eecb == NULL)
		errx(EXIT_FAILURE, "failed to malloc eecb");

	memset(eecb, 0, sizeof(dt_elf_ecbdesc_t));

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	ecb = stmt->dtsd_ecbdesc;

	/*
	 * Find the corresponding action's section number.
	 */
	for (el = dt_list_next(&dtelf_state->s_actions); el != NULL;
	    el = dt_list_next(el))
		if (ecb->dted_action == el->act)
			break;

	/*
	 * If the data structure is laid out correctly, we are guaranteed
	 * that during the action creation phase, we will have created the
	 * action needed for this ecbdesc. If this is not the case, bail out
	 * hard.
	 */
	assert(el != NULL);
	eecb->dtee_action = el->eact_ndx;

	/*
	 * While the DTrace struct has a number of things associated with it
	 * that are not the DIFO, this is only useful in the context of the
	 * kernel. We do not need this in userspace, and therefore simply treat
	 * dtee_pred as a DIFO.
	 */
	eecb->dtee_pred = elf_ndxscn(
	    dt_elf_new_difo(e, ecb->dted_pred.dtpdd_difo));

	eecb->dtee_probe.dtep_pdesc = ecb->dted_probe;
	eecb->dtee_uarg = ecb->dted_uarg;

	data->d_align = 8;
	data->d_buf = eecb;
	data->d_size = sizeof(dt_elf_ecbdesc_t);
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	/*
	 * Since dt_elf_ecbdesc_t is of fixed size, we set entsize to its size.
	 */
	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_ECBDESC;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = sizeof(dt_elf_ecbdesc_t);
	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	return (scn);
}

static size_t
dt_elf_new_id_name(const char *name)
{
	size_t offset, len, osize;
	int needs_realloc;
	char *otab;

	len = strlen(name);
	offset = dtelf_state->s_idname_offset;

	/*
	 * This makes no sense, so hard fail on it.
	 */
	if (offset > dtelf_state->s_idname_size)
		errx(EXIT_FAILURE, "offset (%zu) > idname_size (%zu)",
		    offset, dtelf_state->s_idname_size);

	/*
	 * Save the old size in case we need to realloc;
	 */
	osize = dtelf_state->s_idname_size;

	/*
	 * If we are at the boundary, we have to reallocate the identifier
	 * name string table in order to add a new entry. We first make sure
	 * that the size of the table is large enough to accommodate the new
	 * string we are putting in it. Thus, we increase the size of the
	 * table over and over (shifting it to the left by 1) until we satisfy
	 * the condition where the current offset (the next entry to be added)
	 * added to the length of the string we want to add is less than the
	 * size of the table.
	 */
	while ((offset + len) >= dtelf_state->s_idname_size) {
		/*
		 * Save the flag that we need to actually realloc the table.
		 */
		needs_realloc = 1;

		/*
		 * XXX: Need a better way to check this...
		 */
		if ((dtelf_state->s_idname_size << 1) <= dtelf_state->s_idname_size)
			errx(EXIT_FAILURE, "idname string table at max size");

		/*
		 * Increase the size of the identifier name string table by
		 * shifting it left by 1
		 */
		dtelf_state->s_idname_size <<= 1;
	}

	if (needs_realloc) {
		otab = dtelf_state->s_idname_table;
		dtelf_state->s_idname_table = malloc(dtelf_state->s_idname_size);
		memcpy(dtelf_state->s_idname_table, otab, osize);
		free(otab);
	}

	/*
	 * Add the new string to the table and bump the offset.
	 */
	memcpy(dtelf_state->s_idname_table + offset, name, len);
	dtelf_state->s_idname_table[offset + len - 1] = '\0';
	dtelf_state->s_idname_offset += len;

	/*
	 * Return the old offset where the new string resides.
	 */
	return (offset);
}

static Elf_Scn *
dt_elf_new_stmt(Elf *e, dtrace_stmtdesc_t *stmt, dt_elf_stmt_t *pstmt)
{
	Elf_Scn *scn;
	Elf_Data *data;
	Elf32_Shdr *shdr;
	dt_elf_stmt_t *estmt;

	if (stmt == NULL)
		return (NULL);

	estmt = malloc(sizeof(dt_elf_stmt_t));
	if (estmt == NULL)
		errx(EXIT_FAILURE, "failed to malloc estmt");

	memset(estmt, 0, sizeof(dt_elf_stmt_t));

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	dt_elf_create_actions(e, stmt, elf_ndxscn(scn));

	estmt->dtes_ecbdesc = elf_ndxscn(dt_elf_new_ecbdesc(e, stmt));

	/*
	 * Fill in the first and last action for a statement that we've previously
	 * saved when creating actions.
	 */
	estmt->dtes_action = dtelf_state->s_first_act_scn;
	estmt->dtes_action_last = dtelf_state->s_last_act_scn;
	estmt->dtes_descattr.dtea_attr = stmt->dtsd_descattr;
	estmt->dtes_stmtattr.dtea_attr = stmt->dtsd_stmtattr;
	estmt->dtes_aggdata = 0;

	if (stmt->dtsd_aggdata != NULL) {
		dt_ident_t *aid = (dt_ident_t *)stmt->dtsd_aggdata;
		Elf_Scn *aid_scn;
		Elf_Data *aid_data;
		dt_elf_ident_t *eaid;

		if ((aid_scn = elf_newscn(e)) == NULL)
			errx(EXIT_FAILURE,
			    "elf_newscn(%p) failed with %s", e, elf_errmsg(-1));

		if ((aid_data = elf_newdata(scn)) == NULL)
			errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
			    scn, elf_errmsg(-1));

		eaid = malloc(sizeof(dt_elf_ident_t));
		memset(eaid, 0, sizeof(dt_elf_ident_t));\

		eaid->edi_name = dt_elf_new_id_name(aid->di_name);
		eaid->edi_id = aid->di_id;
		eaid->edi_kind = aid->di_kind;
		eaid->edi_flags = aid->di_flags;
		eaid->edi_attr.dtea_attr = aid->di_attr;
		eaid->edi_vers = aid->di_vers;

		aid_data->d_buf = eaid;
		aid_data->d_size = sizeof(dt_elf_ident_t);
		aid_data->d_align = 8;
		aid_data->d_type = ELF_T_BYTE;
		aid_data->d_version = EV_CURRENT;

		if ((shdr = elf32_getshdr(aid_scn)) == NULL)
			errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
			    elf_errmsg(-1));

		shdr->sh_type = SHT_DTRACE_elf;
		shdr->sh_name = DTELF_IDENT;
		shdr->sh_flags = SHF_OS_NONCONFORMING;
		shdr->sh_entsize = sizeof(dt_elf_ident_t);

		(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
		(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
		(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

		estmt->dtes_aggdata = elf_ndxscn(aid_scn);
	}

	/*
	 * If this action is an aggregation, we save the aggregation ID
	 * and name.
	 */
	if (pstmt != NULL)
		pstmt->dtes_next = elf_ndxscn(scn);

	data->d_align = 4;
	data->d_buf = estmt;
	data->d_size = sizeof(dt_elf_stmt_t);
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_STMTDESC;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = sizeof(dt_elf_stmt_t);
	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	return (scn);
}

static void
dt_elf_cleanup(void)
{
	dt_elf_eact_list_t *el;
	dt_elf_eact_list_t *p_el;

	for (el = dt_list_next(&dtelf_state->s_actions);
	    el != NULL; el = dt_list_next(el)) {
		if (p_el)
			free(p_el);

		p_el = el;
	}

	/*
	 * The last iteration will free everything for this member,
	 * except itself.
	 */
	free(p_el);
}

static Elf_Scn *
dt_elf_options(Elf *e)
{
	Elf_Scn *scn = NULL;
	Elf32_Shdr *shdr;
	Elf_Data *data;

	size_t buflen = 0; /* Current buffer length */
	size_t len = 0; /* Length of the new entry */
	size_t bufmaxlen = 0; /* Maximum buffer length */
	size_t l;
	unsigned char *buf = NULL, *obuf = NULL;
	int needs_realloc = 0;

	dt_elf_opt_t *op;
	_dt_elf_eopt_t *eop;

	/*
	 * Go over the compile-time options and fill them in.
	 *
	 * XXX: This may not be necessary for ctopts.
	 */
	for (op = dtelf_ctopts; op->dteo_name != NULL; op++) {
		if (op->dteo_set == 0)
			continue;

		len = sizeof(_dt_elf_eopt_t) + strlen(op->dteo_arg) + 1;
		eop = malloc(len);
		if (eop == NULL)
			errx(EXIT_FAILURE, "failed to malloc eop");

		l = strlcpy(eop->eo_name, op->dteo_name, sizeof(eop->eo_name));
		if (l >= sizeof(eop->eo_name))
			errx(EXIT_FAILURE, "%s is too long to be copied",
			    op->dteo_name);

		eop->eo_len = strlen(op->dteo_arg) + 1;

		l = strlcpy(eop->eo_arg, op->dteo_arg, eop->eo_len);
		if (l >= eop->eo_len)
			errx(EXIT_FAILURE, "%s is too long to be copied",
			    op->dteo_arg);


		if (strcmp("define", op->dteo_name) == 0 ||
		    strcmp("incdir", op->dteo_name) == 0 ||
		    strcmp("undef",  op->dteo_name) == 0) {
			l = strlcpy((char *)eop->eo_option,
			    (char *)op->dteo_option, sizeof(eop->eo_option));
			if (l >= sizeof(eop->eo_option))
				errx(EXIT_FAILURE, "%s is too long to be copied",
				    op->dteo_option);
		} else
			eop->eo_option = op->dteo_option;

		/*
		 * Have we run out of space in our buffer?
		 */
		while (buflen + len >= bufmaxlen) {
			if (bufmaxlen == 0)
				bufmaxlen = 1;

			needs_realloc = 1;
			/*
			 * Check for overflow. Not great, but will have to do.
			 */
			if ((bufmaxlen << 1) <= bufmaxlen)
				errx(EXIT_FAILURE,
				     "buf realloc failed, bufmaxlen exceeded");
			bufmaxlen <<= 1;
		}

		if (buf == NULL || needs_realloc) {
			/*
			 * Save the old buffer.
			 */
			obuf = buf;

			buf = malloc(bufmaxlen);
			if (buf == NULL)
				errx(EXIT_FAILURE,
				     "buf realloc failed, %s", strerror(errno));

			if (obuf) {
				memcpy(buf, obuf, buflen);
				free(obuf);
			}

			needs_realloc = 0;
		}

		memcpy(buf + buflen, eop, len);
		buflen += len;
	}

	/*
	 * Go over runtime options. If they are set, we add them to our data
	 * buffer which will be in the section that contains all of the options.
	 */
	for (op = dtelf_rtopts; op->dteo_name != NULL; op++) {
		if (op->dteo_set == 0)
			continue;

		len = sizeof(_dt_elf_eopt_t) + strlen(op->dteo_arg) + 1;
		eop = malloc(len);
		if (eop == NULL)
			errx(EXIT_FAILURE, "failed to malloc eop");

		l = strlcpy(eop->eo_name, op->dteo_name, sizeof(eop->eo_name));
		if (l >= sizeof(eop->eo_name))
			errx(EXIT_FAILURE, "%s is too long to be copied",
			    op->dteo_name);

		eop->eo_len = strlen(op->dteo_arg) + 1;

		l = strlcpy(eop->eo_arg, op->dteo_arg, eop->eo_len);
		if (l >= eop->eo_len)
			errx(EXIT_FAILURE, "%s is too long to be copied",
			    op->dteo_arg);


		if (strcmp("define", op->dteo_name) == 0 ||
		    strcmp("incdir", op->dteo_name) == 0 ||
		    strcmp("undef",  op->dteo_name) == 0) {
		        l = strlcpy((char *)eop->eo_option,
			    (char *)op->dteo_option, sizeof(eop->eo_option));
			if (l >= sizeof(eop->eo_option))
				errx(EXIT_FAILURE, "%s is too long to be copied",
				    op->dteo_option);
		} else
			eop->eo_option = op->dteo_option;

		/*
		 * Have we run out of space in our buffer?
		 */
		while (buflen + len >= bufmaxlen) {
			if (bufmaxlen == 0)
				bufmaxlen = 1;

			needs_realloc = 1;
			/*
			 * Check for overflow. Not great, but will have to do.
			 */
			if ((bufmaxlen << 1) <= bufmaxlen)
				errx(EXIT_FAILURE,
				    "buf realloc failed, bufmaxlen exceeded");
			bufmaxlen <<= 1;
		}

		if (buf == NULL || needs_realloc) {
			/*
			 * Save the old buffer.
			 */
			obuf = buf;

			buf = malloc(bufmaxlen);
			if (buf == NULL)
				errx(EXIT_FAILURE,
				    "buf realloc failed, %s", strerror(errno));

			if (obuf) {
				memcpy(buf, obuf, buflen);
				free(obuf);
			}

			needs_realloc = 0;
		}

		memcpy(buf + buflen, eop, len);
		buflen += len;
	}

	/*
	 * Go over dynamic runtime options. If they are set, we add them to our data
	 * buffer which will be in the section that contains all of the options.
	 */
	for (op = dtelf_drtopts; op->dteo_name != NULL; op++) {
		if (op->dteo_set == 0)
			continue;

		len = sizeof(_dt_elf_eopt_t) + strlen(op->dteo_arg) + 1;
		eop = malloc(len);
		if (eop == NULL)
			errx(EXIT_FAILURE, "failed to malloc eop");

		l = strlcpy(eop->eo_name, op->dteo_name, sizeof(eop->eo_name));
		if (l >= sizeof(eop->eo_name))
			errx(EXIT_FAILURE, "%s is too long to be copied",
			    op->dteo_name);

		eop->eo_len = strlen(op->dteo_arg) + 1;

		l = strlcpy(eop->eo_arg, op->dteo_arg, eop->eo_len);
		if (l >= eop->eo_len)
			errx(EXIT_FAILURE, "%s is too long to be copied",
			    op->dteo_arg);

		if (strcmp("define", op->dteo_name) == 0 ||
		    strcmp("incdir", op->dteo_name) == 0 ||
		    strcmp("undef",  op->dteo_name) == 0) {
			l = strlcpy((char *)eop->eo_option,
			    (char *)op->dteo_option, sizeof(eop->eo_option));
			if (l >= sizeof(eop->eo_option))
				errx(EXIT_FAILURE, "%s is too long to be copied",
				    op->dteo_option);
		} else
			eop->eo_option = op->dteo_option;

		/*
		 * Have we run out of space in our buffer?
		 */
		while (buflen + len >= bufmaxlen) {
			if (bufmaxlen == 0)
				bufmaxlen = 1;

			needs_realloc = 1;
			/*
			 * Check for overflow. Not great, but will have to do.
			 */
			if ((bufmaxlen << 1) <= bufmaxlen)
				errx(EXIT_FAILURE,
				     "buf realloc failed, bufmaxlen exceeded");
			bufmaxlen <<= 1;
		}

		if (buf == NULL || needs_realloc) {
			/*
			 * Save the old buffer.
			 */
			obuf = buf;

			buf = malloc(bufmaxlen);
			if (buf == NULL)
				errx(EXIT_FAILURE,
				     "buf realloc failed, %s", strerror(errno));

			if (obuf) {
				memcpy(buf, obuf, buflen);
				free(obuf);
			}

			needs_realloc = 0;
		}

		memcpy(buf + buflen, eop, len);
		buflen += len;
	}

	if (buflen == 0)
		return (NULL);

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		    e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		    scn, elf_errmsg(-1));

	data->d_align = 8;
	data->d_buf = buf;
	data->d_size = buflen;
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		    elf_errmsg(-1));

	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_OPTS;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = 0;
	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	return (scn);
}

void
dt_elf_create(dtrace_prog_t *dt_prog, int endian)
{
	int fd, err;
	Elf *e;
	Elf32_Ehdr *ehdr;
	Elf_Scn *scn, *f_scn;
	Elf_Data *data;
	Elf32_Shdr *shdr, *s0hdr;
	Elf32_Phdr *phdr;
	const char *file_name = "/var/ddtrace/tracing_spec.elf";
	dt_stmt_t *stp;

	dtrace_stmtdesc_t *stmt = NULL;

	dt_elf_prog_t prog = {0};
	dt_elf_stmt_t *p_stmt;

	dtelf_state = malloc(sizeof(dt_elf_state_t));
	if (dtelf_state == NULL)
		errx(EXIT_FAILURE, "failed to malloc dtelf_state");

	memset(dtelf_state, 0, sizeof(dt_elf_state_t));

	/*
	 * Initialise the identifier name string table.
	 */
	dtelf_state->s_idname_size = 1;
	dtelf_state->s_idname_offset = 1;
	dtelf_state->s_idname_table = malloc(dtelf_state->s_idname_size);
	memset(dtelf_state->s_idname_table, 0, dtelf_state->s_idname_size);

	/*
	 * Create the directory that contains the ELF file (if needed).
	 */
	err = mkdir("/var/ddtrace", 0755);
	if (err != 0 && errno != EEXIST)
		errx(EXIT_FAILURE,
		    "Failed to mkdir /var/ddtrace with permissions 0755 with %s",
		    strerror(errno));

	if (elf_version(EV_CURRENT) == EV_NONE)
		errx(EXIT_FAILURE, "ELF library initialization failed: %s",
		    elf_errmsg(-1));

	if ((fd = open(file_name, O_WRONLY | O_CREAT, 0777)) < 0)
		errx(EXIT_FAILURE, "Failed to open /var/ddtrace/%s", file_name);

	if ((e = elf_begin(fd, ELF_C_WRITE, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_begin() failed with %s", elf_errmsg(-1));

	if ((ehdr = elf32_newehdr(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newehdr(%p) failed with %s",
		    e, elf_errmsg(-1));

	ehdr->e_ident[EI_DATA] = endian;
	ehdr->e_machine = EM_NONE;
	ehdr->e_type = ET_EXEC;
	ehdr->e_ident[EI_CLASS] = ELFCLASS32;

	/*
	 * Enable extended section numbering.
	 */
	ehdr->e_shstrndx = SHN_XINDEX;
	ehdr->e_shnum = 0;
	ehdr->e_shoff = 0;

	if ((phdr = elf32_newphdr(e, 1)) == NULL)
		errx(EXIT_FAILURE, "elf_newphdr(%p, 1) failed with %s",
		    e, elf_errmsg(-1));

	/*
	 * The very first section is a string table of section names.
	 */

	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		     e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		     scn, elf_errmsg(-1));

	data->d_align = 1;
	data->d_buf = sec_strtab;
	data->d_size = sizeof(sec_strtab);
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		    elf_errmsg(-1));

	shdr->sh_type = SHT_STRTAB;
	shdr->sh_name = DTELF_SHSTRTAB;
	shdr->sh_flags = SHF_STRINGS;
	shdr->sh_entsize = DTELF_VARIABLE_SIZE;
	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	/*
	 * For extended numbering
	 */
	if ((s0hdr = elf32_getshdr(elf_getscn(e, 0))) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		     elf_errmsg(-1));

	s0hdr->sh_size = 0; /* Number of sections -- filled in later! */
	s0hdr->sh_link = elf_ndxscn(scn); /* .shstrtab index */
	(void) elf_flagshdr(elf_getscn(e, 0), ELF_C_SET, ELF_F_DIRTY);

	/*
	 * Second section gives us the necessary information about a DTrace
	 * program. What DOF version we need, reference to the section that
	 * contains the first statement, etc.
	 */
	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		    e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		    scn, elf_errmsg(-1));

	data->d_align = 4;
	data->d_buf = &prog;
	data->d_size = sizeof(dt_elf_prog_t);
	data->d_type = ELF_T_BYTE;
	data->d_version = EV_CURRENT;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		    elf_errmsg(-1));

	/*
	 * Currently we only have one program that put into the ELF file.
	 * However, at some point we may wish to have multiple programs. In any
	 * case, since dt_elf_prog_t is of fixed size, entsize is set to
	 * sizeof(dt_elf_prog_t).
	 */
	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_PROG;
	shdr->sh_flags = SHF_OS_NONCONFORMING; /* DTrace-specific */
	shdr->sh_entsize = sizeof(dt_elf_prog_t);
	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	/*
	 * Get the first stmt.
	 */
	stp = dt_list_next(&dt_prog->dp_stmts);
	stmt = stp->ds_desc;

	/*
	 * Create a section with the first statement.
	 */
	f_scn = dt_elf_new_stmt(e, stmt, NULL);
	if ((data = elf_getdata(f_scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
		    elf_errmsg(-1), __func__);
	p_stmt = data->d_buf;

	/*
	 * Here, we populate the DTrace program with a reference to the ELF
	 * section that contains the first statement and the DOF version
	 * required for this program.
	 */
	prog.dtep_first_stmt = elf_ndxscn(f_scn);
	prog.dtep_dofversion = dt_prog->dp_dofversion;
	prog.dtep_rflags = dt_prog->dp_rflags;

	/*
	 * Iterate over the other statements and create ELF sections with them.
	 */
	for (stp = dt_list_next(stp); stp != NULL; stp = dt_list_next(stp)) {
		scn = dt_elf_new_stmt(e, stp->ds_desc, p_stmt);
		if ((data = elf_getdata(scn, NULL)) == NULL)
			errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
			    elf_errmsg(-1), __func__);
		p_stmt = data->d_buf;
	}

	scn = dt_elf_options(e);

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		    elf_errmsg(-1));

	shdr->sh_type = SHT_DTRACE_elf;
	shdr->sh_name = DTELF_OPTS;
	shdr->sh_flags = SHF_OS_NONCONFORMING;
	shdr->sh_entsize = 0;

	/*
	 * Save the options for this program.
	 */
	prog.dtep_options = elf_ndxscn(scn);

	/*
	 * Make the string table that will hold identifier names.
	 */
	if ((scn = elf_newscn(e)) == NULL)
		errx(EXIT_FAILURE, "elf_newscn(%p) failed with %s",
		    e, elf_errmsg(-1));

	if ((data = elf_newdata(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_newdata(%p) failed with %s",
		    scn, elf_errmsg(-1));

	data->d_buf = dtelf_state->s_idname_table;
	data->d_size = dtelf_state->s_idname_offset;
	data->d_align = 1;
	data->d_version = EV_CURRENT;
	data->d_type = ELF_T_BYTE;

	if ((shdr = elf32_getshdr(scn)) == NULL)
		errx(EXIT_FAILURE, "elf_getshdr() failed with %s",
		    elf_errmsg(-1));

	shdr->sh_type = SHT_STRTAB;
	shdr->sh_name = DTELF_IDNAMETAB;
	shdr->sh_flags = SHF_STRINGS;
	shdr->sh_entsize = DTELF_VARIABLE_SIZE;

	(void) elf_flagshdr(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagscn(scn, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	/*
	 * Update everything before writing.
	 */
	if (elf_update(e, ELF_C_NULL) < 0)
		errx(EXIT_FAILURE, "elf_update(%p, ELF_C_NULL) failed with %s",
		    e, elf_errmsg(-1));

	s0hdr->sh_size = ehdr->e_shnum;
	(void) elf_flagshdr(elf_getscn(e, 0), ELF_C_SET, ELF_F_DIRTY);

	ehdr->e_shnum = 0;

	phdr->p_type = PT_PHDR;
	phdr->p_offset = ehdr->e_phoff;
	phdr->p_filesz = gelf_fsize(e, ELF_T_PHDR, 1, EV_CURRENT);

	(void) elf_flagphdr(e, ELF_C_SET, ELF_F_DIRTY);
	(void) elf_flagehdr(e, ELF_C_SET, ELF_F_DIRTY);

	if (elf_update(e, ELF_C_WRITE) < 0)
		errx(EXIT_FAILURE, "elf_update(%p, ELF_C_WRITE) failed with %s",
		    e, elf_errmsg(-1));

	free(dtelf_state);
	(void) elf_end(e);
	(void) close(fd);
}

static void *
dt_elf_get_table(Elf *e, dt_elf_ref_t tabref)
{
	Elf_Scn *scn;
	Elf_Data *data;
	uint64_t *table;

	if (tabref == 0)
		return (NULL);

	if ((scn = elf_getscn(e, tabref)) == NULL)
		errx(EXIT_FAILURE, "elf_getscn() failed with %s", elf_errmsg(-1));

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
		    elf_errmsg(-1), __func__);

	assert(data->d_buf != NULL);

	if (data->d_size == 0)
		return (NULL);

	table = malloc(data->d_size);
	if (table == NULL)
		errx(EXIT_FAILURE, "failed to malloc table");

	memcpy(table, data->d_buf, data->d_size);

	return (table);
}

static dtrace_difo_t *
dt_elf_get_difo(Elf *e, dt_elf_ref_t diforef)
{
	dtrace_difo_t *difo;
	dt_elf_difo_t *edifo;
	Elf_Scn *scn;
	Elf_Data *data;
	size_t i;
	char *c;

	if (diforef == 0)
		return (NULL);

	if ((scn = elf_getscn(e, diforef)) == NULL)
		errx(EXIT_FAILURE, "elf_getscn() failed with %s", elf_errmsg(-1));

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
		    elf_errmsg(-1), __func__);

	assert(data->d_buf != NULL);
	edifo = data->d_buf;

	difo = malloc(sizeof(dtrace_difo_t));
	if (difo == NULL)
		errx(EXIT_FAILURE, "failed to malloc difo");

	memset(difo, 0, sizeof(dtrace_difo_t));

	difo->dtdo_buf = malloc(edifo->dted_len * sizeof(dif_instr_t));
	if (difo->dtdo_buf == NULL)
		errx(EXIT_FAILURE, "failed to malloc dtdo_buf");

	memset(difo->dtdo_buf, 0, sizeof(dif_instr_t) * edifo->dted_len);

	difo->dtdo_inttab = dt_elf_get_table(e, edifo->dted_inttab);
	difo->dtdo_strtab = dt_elf_get_table(e, edifo->dted_strtab);
	difo->dtdo_vartab = dt_elf_get_table(e, edifo->dted_vartab);
	difo->dtdo_symtab = dt_elf_get_table(e, edifo->dted_symtab);

	difo->dtdo_intlen = edifo->dted_intlen;
	difo->dtdo_strlen = edifo->dted_strlen;
	difo->dtdo_varlen = edifo->dted_varlen;
	difo->dtdo_symlen = edifo->dted_symlen;

	difo->dtdo_len = edifo->dted_len;

	difo->dtdo_rtype = edifo->dted_rtype;
	difo->dtdo_destructive = edifo->dted_destructive;

	for (i = 0; i < edifo->dted_len; i++)
		difo->dtdo_buf[i] = edifo->dted_buf[i];

	return (difo);
}

static const char *
dt_elf_get_target(Elf *e, dt_elf_ref_t ecbref)
{
	Elf_Scn *scn;
	Elf_Data *data;
	dt_elf_ecbdesc_t *eecb = NULL;

	if ((scn = elf_getscn(e, ecbref)) == NULL)
		errx(EXIT_FAILURE, "elf_getscn() failed with %s",
		    elf_errmsg(-1));

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
		    elf_errmsg(-1), __func__);

	assert(data->d_buf != NULL);
	eecb = data->d_buf;
	return ((const char *)eecb->dtee_probe.dtep_pdesc.dtpd_target);
}


static dtrace_ecbdesc_t *
dt_elf_get_ecbdesc(Elf *e, dt_elf_ref_t ecbref)
{
	Elf_Scn *scn;
	Elf_Data *data;
	dt_elf_ecbdesc_t *eecb = NULL;
	dtrace_ecbdesc_t *ecb = NULL;
	dt_elf_eact_list_t *el = NULL;

	if ((scn = elf_getscn(e, ecbref)) == NULL)
		errx(EXIT_FAILURE, "elf_getscn() failed with %s",
		    elf_errmsg(-1));

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
		    elf_errmsg(-1), __func__);

	assert(data->d_buf != NULL);
	eecb = data->d_buf;

	ecb = malloc(sizeof(dtrace_ecbdesc_t));
	if (ecb == NULL)
		errx(EXIT_FAILURE, "failed to malloc ecb");

	memset(ecb, 0, sizeof(dtrace_ecbdesc_t));

	for (el = dt_list_next(&dtelf_state->s_actions);
	    el != NULL; el = dt_list_next(el)) {
		if (el->eact_ndx == eecb->dtee_action) {
			ecb->dted_action = el->act;
			break;
		}
	}

	ecb->dted_pred.dtpdd_predicate = NULL;
	ecb->dted_pred.dtpdd_difo = dt_elf_get_difo(e, eecb->dtee_pred);

	ecb->dted_probe = eecb->dtee_probe.dtep_pdesc;
	ecb->dted_probe.dtpd_target[DTRACE_TARGETNAMELEN - 1] = '\0';
	ecb->dted_probe.dtpd_provider[DTRACE_PROVNAMELEN - 1] = '\0';
	ecb->dted_probe.dtpd_mod[DTRACE_MODNAMELEN - 1] = '\0';
	ecb->dted_probe.dtpd_func[DTRACE_FUNCNAMELEN - 1] = '\0';
	ecb->dted_probe.dtpd_name[DTRACE_NAMELEN - 1] = '\0';

	ecb->dted_uarg = eecb->dtee_uarg;
	return (ecb);
}

static void
dt_elf_add_acts(dtrace_stmtdesc_t *stmt, dt_elf_ref_t fst, dt_elf_ref_t last)
{
	dt_elf_eact_list_t *el = NULL;
	dtrace_actdesc_t *act = NULL;
	dtrace_actdesc_t *p = NULL;

	assert(stmt != NULL);

	for (el = dt_list_next(&dtelf_state->s_actions);
	    el != NULL; el = dt_list_next(el)) {
		if (el->eact_ndx == fst)
			stmt->dtsd_action = el->act;

		if (el->eact_ndx == last) {
			stmt->dtsd_action_last = el->act;
			break;
		}
	}

	assert(el != NULL);
}

static void *
dt_elf_get_eaid(Elf *e, dt_elf_ref_t aidref)
{
	dt_ident_t *aid;
	Elf_Scn *scn;
	Elf_Data *data;
	dt_elf_ident_t *eaid;

	if (aidref == 0)
		return (NULL);

	if ((scn = elf_getscn(e, aidref)) == NULL)
		errx(EXIT_FAILURE, "elf_getscn() failed with %s",
		    elf_errmsg(-1));

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s",
		    elf_errmsg(-1));

	eaid = data->d_buf;
	if (eaid == NULL)
		errx(EXIT_FAILURE, "eaid is NULL");

	aid = malloc(sizeof(dt_ident_t));
	if (aid == NULL)
		errx(EXIT_FAILURE, "aid is NULL");

	aid->di_name = strdup(dtelf_state->s_idname_table + eaid->edi_name);
	aid->di_id = eaid->edi_id;
	aid->di_kind = eaid->edi_kind;
	aid->di_flags = eaid->edi_flags;
	aid->di_attr = eaid->edi_attr.dtea_attr;
	aid->di_vers = eaid->edi_vers;

	return ((void *)aid);
}

static void
dt_elf_free_ecb(dtrace_ecbdesc_t *ecb)
{
	if (ecb == NULL)
		return;

	if (ecb->dted_pred.dtpdd_difo != NULL)
		free(ecb->dted_pred.dtpdd_difo);

	free(ecb);
}

static dt_elf_eact_list_t *
dt_elf_in_actlist(dtrace_actdesc_t *find)
{
	dt_elf_eact_list_t *e;

	e = NULL;
	
	for (e = dt_list_next(&dtelf_state->s_actions);
	    e; e = dt_list_next(e))
		if (e->act == find)
			return (e);

	return (NULL);
}


static void
dt_elf_add_stmt(Elf *e, dtrace_prog_t *prog,
    dt_elf_stmt_t *estmt, dt_elf_ref_t sscn)
{
	dtrace_stmtdesc_t *stmt;
	dt_stmt_t *stp;
	dtrace_actdesc_t *ap, *nextap, *prevap;
	dt_elf_eact_list_t *el, *rm_el;
	const char *target;


	stmt = NULL;
	stp = NULL;
	ap = NULL;
	nextap = NULL;
	prevap = NULL;
	el = NULL;
	rm_el = NULL;
	target = NULL;

	assert(estmt != NULL);

	stmt = malloc(sizeof(dtrace_stmtdesc_t));
	if (stmt == NULL)
		errx(EXIT_FAILURE, "failed to malloc stmt");

	memset(stmt, 0, sizeof(dtrace_stmtdesc_t));

	stmt->dtsd_ecbdesc = dt_elf_get_ecbdesc(e, estmt->dtes_ecbdesc);

	/*
	 * Get the target name and check if we match it.
	 */
	target = stmt->dtsd_ecbdesc->dted_probe.dtpd_target;
	if (dt_resolve(target, dtelf_state->s_rflags) != 0) {
		/*
		 * We won't be needing the ECB nor the statement.
		 */
		dt_elf_free_ecb(stmt->dtsd_ecbdesc);
		free(stmt);

		/*
		 * Go through the action list to find actions which have
		 * this statement as their "dtad_uarg" in order to properly
		 * free them and remove them from the action list. This
		 * ensures that we don't end up in a situation where the action
		 * is packed into the DOF later on as a stray action, not really
		 * belonging to any statement, as DOF generates an "actions"
		 * section with all the actions. If actions that do not belong
		 * to this target machine are left in the action list (i.e.
		 * they have an action in a statement where one of the next
		 * pointers is that action), they will get put into DOF.
		 */
		for (el = dt_list_next(&dtelf_state->s_actions);
		    el != NULL; el = dt_list_next(el)) {
			ap = el->act;

			if (ap->dtad_uarg == sscn) {
				/*
				 * While we have 'sscn' as the uarg in the
				 * current action, we will keep removing them
				 * from the list and freeing them, as they are
				 * not meant for this target machine.
				 */
				while (ap && ap->dtad_uarg == sscn) {
					nextap = ap->dtad_next;

					rm_el = dt_elf_in_actlist(ap);
					assert(rm_el != NULL);

					dt_list_delete(&dtelf_state->s_actions, rm_el);
					free(ap);
					free(rm_el);

					ap = nextap;
				}

				/*
				 * If this is not the first action, we simply
				 * set the previous action's next pointer (the
				 * one that actually belongs to this target) to
				 * the current action (even if NULL).
				 */
				if (prevap != NULL)
					prevap->dtad_next = ap;

				/*
				 * Under the assumption that the actions were
				 * parsed correctly, (ap == NULL) ==> empty list
				 * so we simply exit out, we don't need to check
				 * anything further.
				 */
				if (ap == NULL)
					return;
			}

			prevap = ap;
		}
		return;
	}

	dt_elf_add_acts(stmt, estmt->dtes_action, estmt->dtes_action_last);
	stmt->dtsd_descattr = estmt->dtes_descattr.dtea_attr;
	stmt->dtsd_stmtattr = estmt->dtes_stmtattr.dtea_attr;
	stmt->dtsd_aggdata = dt_elf_get_eaid(e, estmt->dtes_aggdata);

	stp = malloc(sizeof(dt_stmt_t));
	if (stp == NULL)
		errx(EXIT_FAILURE, "failed to malloc stp");

	memset(stp, 0, sizeof(dt_stmt_t));

	stp->ds_desc = stmt;
	dt_list_append(&prog->dp_stmts, stp);
}

static dtrace_actdesc_t *
dt_elf_alloc_action(Elf *e, Elf_Scn *scn, dtrace_actdesc_t *prev)
{
	dtrace_actdesc_t *ad;
	dt_elf_eact_list_t *el;
	Elf_Data *data;
	dt_elf_actdesc_t *ead;

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
		    elf_errmsg(-1), __func__);

	assert(data->d_buf != NULL);
	ead = data->d_buf;

	ad = malloc(sizeof(dtrace_actdesc_t));
	if (ad == NULL)
		errx(EXIT_FAILURE, "failed to malloc ad");

	memset(ad, 0, sizeof(dtrace_actdesc_t));

	ad->dtad_difo = dt_elf_get_difo(e, ead->dtea_difo);
	ad->dtad_next = NULL; /* Filled in later */
	ad->dtad_kind = ead->dtea_kind;
	ad->dtad_ntuple = ead->dtea_ntuple;
	ad->dtad_arg = ead->dtea_arg;
	ad->dtad_uarg = ead->dtea_uarg;

	el = malloc(sizeof(dt_elf_eact_list_t));
	if (el == NULL)
		errx(EXIT_FAILURE, "failed to malloc el");

	memset(el, 0, sizeof(dt_elf_eact_list_t));

	el->eact_ndx = elf_ndxscn(scn);
	el->act = ad;
	el->eact = ead;

	dt_list_append(&dtelf_state->s_actions, el);

	if (prev)
		prev->dtad_next = ad;

	return (ad);
}

static void
dt_elf_alloc_actions(Elf *e, dt_elf_stmt_t *estmt)
{
	Elf_Scn *scn;
	Elf_Data *data;
	dt_elf_actdesc_t *ead;
	dt_elf_ref_t fst, actref;
	dtrace_actdesc_t *prev = NULL;

	fst = estmt->dtes_action;

	for (actref = fst; actref != 0; actref = ead->dtea_next) {
		if ((scn = elf_getscn(e, actref)) == NULL)
			errx(EXIT_FAILURE, "elf_getscn() failed with %s", elf_errmsg(-1));

		if ((data = elf_getdata(scn, NULL)) == NULL)
			errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
			    elf_errmsg(-1), __func__);

		assert(data->d_buf != NULL);

		ead = data->d_buf;
		prev = dt_elf_alloc_action(e, scn, prev);
	}
}

static void
dt_elf_get_stmts(Elf *e, dtrace_prog_t *prog, dt_elf_ref_t first_stmt_scn)
{
	Elf_Scn *scn;
	Elf_Data *data;
	dt_elf_stmt_t *estmt;
	dt_elf_ref_t scnref;

	for (scnref = first_stmt_scn; scnref != 0; scnref = estmt->dtes_next) {
		if ((scn = elf_getscn(e, scnref)) == NULL)
			errx(EXIT_FAILURE, "elf_getscn() failed with %s", elf_errmsg(-1));

		if ((data = elf_getdata(scn, NULL)) == NULL)
			errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
			    elf_errmsg(-1), __func__);

		assert(data->d_buf != NULL);
		estmt = data->d_buf;

		if (first_stmt_scn == scnref)
			dt_elf_alloc_actions(e, estmt);

		dt_elf_add_stmt(e, prog, estmt, scnref);
	}
}

static void
dt_elf_get_options(dtrace_hdl_t *dtp, Elf *e, dt_elf_ref_t eopts)
{
	Elf_Scn *scn;
	Elf_Data *data;
        uintptr_t eop;
	_dt_elf_eopt_t *dteop;

	if ((scn = elf_getscn(e, eopts)) == NULL)
		errx(EXIT_FAILURE, "elf_getscn() failed with %s",
		    elf_errmsg(-1));

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s",
		    elf_errmsg(-1));

	/*
	 * If you remove the uintptr_ts, you will suffer at the hands of
	 * a compiler developer.
	 */
	for (eop = (uintptr_t)data->d_buf;
	    eop != ((uintptr_t)data->d_buf) + data->d_size;
	    eop = eop + dteop->eo_len + sizeof(_dt_elf_eopt_t)) {
		dteop = (_dt_elf_eopt_t *)eop;
		dtrace_setopt(dtp, dteop->eo_name, dteop->eo_arg);
	}
}

dtrace_prog_t *
dt_elf_to_prog(dtrace_hdl_t *dtp, int fd)
{
	Elf *e;
	Elf_Scn *scn = NULL;
	Elf_Data *data;
	GElf_Shdr shdr;
	size_t shstrndx, shnum;
	char *name;
	int class;
	GElf_Ehdr ehdr;

	dtrace_prog_t *prog;

	dt_elf_prog_t *eprog;

	dtelf_state = malloc(sizeof(dt_elf_state_t));
	if (dtelf_state == NULL)
		errx(EXIT_FAILURE, "failed to malloc dtelf_state");

	memset(dtelf_state, 0, sizeof(dt_elf_state_t));

	if (elf_version(EV_CURRENT) == EV_NONE)
		errx(EXIT_FAILURE, "ELF library initialization failed: %s",
		    elf_errmsg(-1));

	if ((e = elf_begin(fd, ELF_C_READ, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_begin() failed with %s", elf_errmsg(-1));

	if (elf_kind(e) != ELF_K_ELF)
		errx(EXIT_FAILURE, "tracing specification is not an ELF file");

	if (gelf_getehdr(e, &ehdr) == NULL)
		errx(EXIT_FAILURE, "gelf_getehdr() failed with %s",
		    elf_errmsg(-1));

	class = gelf_getclass(e);
	if (class != ELFCLASS32 && class != ELFCLASS64)
		errx(EXIT_FAILURE, "gelf_getclass() failed with %s",
		    elf_errmsg(-1));

	if (elf_getshdrstrndx(e, &shstrndx) != 0)
		errx(EXIT_FAILURE, "elf_getshstrndx() failed with %s",
		    elf_errmsg(-1));

	if (elf_getshdrnum(e, &shnum) != 0)
		errx(EXIT_FAILURE, "elf_getshdrnum() failed with %s",
		    elf_errmsg(-1));

	/*
	 * Parse in the identifier name string table.
	 */
	while ((scn = elf_nextscn(e, scn)) != NULL) {
		static const char idtab_name[] = ".dtrace_stmt_idname_table";
		if (gelf_getshdr(scn, &shdr) != &shdr)
			errx(EXIT_FAILURE, "gelf_getshdr() failed with %s",
			    elf_errmsg(-1));

		if ((name = elf_strptr(e, shstrndx, shdr.sh_name)) == NULL)
			errx(EXIT_FAILURE, "elf_strptr() failed with %s",
			    elf_errmsg(-1));

		if (strncmp(name, idtab_name, sizeof(idtab_name)) == 0) {
			if ((data = elf_getdata(scn, NULL)) == NULL)
				errx(EXIT_FAILURE, "elf_getdata() failed with %s",
				    elf_errmsg(-1));

			/*
			 * We fill in the global state. We don't actually need
			 * to copy it over as we're only going to use it while
			 * parsing ELF, not afterwards.
			 */
			dtelf_state->s_idname_table = data->d_buf;
			dtelf_state->s_idname_size = data->d_size;
			break;
		}
	}

	/*
	 * Get the program description.
	 */
	if ((scn = elf_getscn(e, DTELF_PROG_SECIDX)) == NULL)
		errx(EXIT_FAILURE, "elf_getscn() failed with %s",
		    elf_errmsg(-1));

	if (gelf_getshdr(scn, &shdr) != &shdr)
		errx(EXIT_FAILURE, "gelf_getshdr() failed with %s",
		     elf_errmsg(-1));

	if ((name = elf_strptr(e, shstrndx, shdr.sh_name)) == NULL)
		errx(EXIT_FAILURE, "elf_strptr() failed with %s",
		     elf_errmsg(-1));

	if (strcmp(name, ".dtrace_prog") != 0)
		errx(EXIT_FAILURE, "section name is not .dtrace_prog (%s)",
		    name);

	if ((data = elf_getdata(scn, NULL)) == NULL)
		errx(EXIT_FAILURE, "elf_getdata() failed with %s in %s",
		    elf_errmsg(-1), __func__);

	assert(data->d_buf != NULL);
	eprog = data->d_buf;

	dtelf_state->s_rflags = eprog->dtep_rflags;

	prog = malloc(sizeof(dtrace_prog_t));
	if (prog == NULL)
		errx(EXIT_FAILURE, "failed to malloc prog");

	memset(prog, 0, sizeof(dtrace_prog_t));
	prog->dp_dofversion = eprog->dtep_dofversion;

	dt_elf_get_stmts(e, prog, eprog->dtep_first_stmt);

	dt_elf_get_options(dtp, e, eprog->dtep_options);

	free(dtelf_state);
	(void) elf_end(e);
	return (prog);
}


void
dtrace_use_elf(dtrace_hdl_t *dtp)
{

	dtp->dt_use_elf = 1;
}

