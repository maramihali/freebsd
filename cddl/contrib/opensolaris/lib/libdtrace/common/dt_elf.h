/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 Domagoj Stolfa.
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _DT_ELF_H_
#define _DT_ELF_H_

#include <dt_program.h>

/*
 * dt_elf_ref_t: A context-dependent reference to another ELF section.
 */
typedef uint32_t dt_elf_ref_t;

/*
 * dt_elf_attribute_t: Same as dtrace_attribute_t.
 */
typedef struct dt_elf_attribute {
	dtrace_attribute_t dtea_attr;
} dt_elf_attribute_t;

/*
 * dt_elf_probedesc_t: 5-tuple and probe ID. Same as dtrace_probedesc_t.
 */
typedef struct dt_elf_probedesc {
	dtrace_probedesc_t dtep_pdesc;
} dt_elf_probedesc_t;

/*
 * dt_elf_difo_t: A serialised version of a DIFO.
 *
 * dted_buf: Contains all of the DIF instructions.
 * dted_inttab_le: A reference to a section containing the integer table
 *                 in little-endian format.
 * dted_inttab_be: A reference to a section containing the integer table
 *                 in big-endian format.
 * dted_strtab: A reference to a section containing the string table.
 * dted_vartab: A reference to a section containing the variable table.
 * dted_len: Number of instructions in dted_buf.
 * dted_intlen: Length of the integer table.
 * dted_strlen: Length of the string table.
 * dted_varlen: Length of the variable table.
 * dted_rtype: DIFO return type.
 * dted_destructive: Invoke destructive subroutines?
 */
typedef struct dt_elf_difo {
	dt_elf_ref_t dted_inttab;
        dt_elf_ref_t dted_strtab;
	dt_elf_ref_t dted_symtab;
        dt_elf_ref_t dted_vartab;
	uint64_t dted_intlen;
	uint64_t dted_strlen;
	uint64_t dted_symlen;
	uint64_t dted_varlen;
	dtrace_diftype_t dted_rtype;
	uint64_t dted_destructive;
	/*
	 * XXX(dstolfa): kreltab, ureltab and xlmtab are also members of
	 *               the userland version of dtrace_difo_t, however
	 *               it seems that they are pretty much discarded in the
	 *               assembly stage and there doesn't seem to be any code
	 *               path that gets them used inside dt_dof.c For now, we
	 *               won't pass those over to the target machine and will
	 *               strongly assume that all of the DTrace "relocations"
	 *               and translation happens on the source machine.
	 */
	uint64_t dted_len;
	dif_instr_t dted_buf[];
} dt_elf_difo_t;

/*
 * dt_elf_actdesc_t: A serialised version of an action description.
 *
 * dtea_difo: A reference to an ELF section containing the DIFO for this action.
 * dtea_next: A reference to an ELF section with the next action.
 * dtea_kind: The kind of an action.
 * dtea_ntuple: Number in the tuple.
 * dtea_arg: Action argument.
 * dtea_uarg: User argument.
 */
typedef struct dt_elf_actdesc {
	dt_elf_ref_t dtea_difo;
	dt_elf_ref_t dtea_next;
	dtrace_actkind_t dtea_kind;
	uint64_t dtea_ntuple;
	uint64_t dtea_arg;
	uint64_t dtea_uarg;
} dt_elf_actdesc_t;

/*
 * dt_elf_ecbdesc_t: A serialised version of an ecbdesc.
 *
 * dtee_action: A reference to an ELF section containing the first action.
 * dtee_pred: A reference to an ELF section containing a predicate.
 * dtee_probe: A probe description.
 * dtee_uarg: Library argument.
 */
typedef struct dt_elf_ecbdesc {
	dt_elf_ref_t dtee_action;
	dt_elf_ref_t dtee_pred;
	dt_elf_probedesc_t dtee_probe;
	uint64_t dtee_uarg;
} dt_elf_ecbdesc_t;

/*
 * dt_elf_stmt_t: A serialised version of a D statement.
 *
 * dtes_ecbdesc: A reference to an ELF section containing the ecbdesc.
 * dtes_action: A reference to an ELF section containing the first action.
 * dtes_descattr: probedesc attributes.
 * dtes_stmtattr: Statement attributes.
 */
typedef struct dt_elf_stmt {
	dt_elf_ref_t dtes_ecbdesc;
	dt_elf_ref_t dtes_action;
	dt_elf_ref_t dtes_action_last;
	dt_elf_attribute_t dtes_descattr;
	dt_elf_attribute_t dtes_stmtattr;
} dt_elf_stmt_t;

/*
 * dt_elf_prog_t: Information about the DTrace program.
 *
 * dtep_first_stmt: A reference to an ELF section containing the first stmt.
 * dtep_dofversion: The DOF version required for this DTrace program.
 */
typedef struct dt_elf_prog {
	dt_elf_ref_t dtep_first_stmt;
	uint8_t dtep_dofversion;
} dt_elf_prog_t;

typedef struct dt_elf_opt {
	const char *dteo_name;
	int dteo_set;
	const char *dteo_arg;
	uintptr_t dteo_option;
} dt_elf_opt_t;

extern dt_elf_opt_t dtelf_ctopts[];
extern dt_elf_opt_t dtelf_rtopts[];
extern dt_elf_opt_t dtelf_drtopts[];

extern void dt_elf_create(dtrace_prog_t *, int);
extern dtrace_prog_t *dt_elf_to_prog(int);
extern void dtrace_use_elf(dtrace_hdl_t *);


#endif /* _DT_ELF_H_ */
