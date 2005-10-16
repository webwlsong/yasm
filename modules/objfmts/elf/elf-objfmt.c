/*
 * ELF object format
 *
 *  Copyright (C) 2003  Michael Urman
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <util.h>
/*@unused@*/ RCSID("$Id$");

/* Notes
 *
 * elf-objfmt uses the "linking" view of an ELF file:
 * ELF header, an optional program header table, several sections,
 * and a section header table
 *
 * The ELF header tells us some overall program information,
 *   where to find the PHT (if it exists) with phnum and phentsize, 
 *   and where to find the SHT with shnum and shentsize
 *
 * The PHT doesn't seem to be generated by NASM for elftest.asm
 *
 * The SHT
 *
 * Each Section is spatially disjoint, and has exactly one SHT entry.
 */

#define YASM_LIB_INTERNAL
#define YASM_BC_INTERNAL
#define YASM_EXPR_INTERNAL
#include <libyasm.h>

#include "elf.h"
#include "elf-machine.h"

typedef struct yasm_objfmt_elf {
    yasm_objfmt_base objfmt;		/* base structure */

    elf_symtab_head* elf_symtab;	/* symbol table of indexed syms */
    elf_strtab_head* shstrtab;		/* section name strtab */
    elf_strtab_head* strtab;		/* strtab entries */

    yasm_object *object;
    yasm_symtab *symtab;
    /*@dependent@*/ yasm_arch *arch;
    
    yasm_symrec *dotdotsym;		/* ..sym symbol */
} yasm_objfmt_elf;

typedef struct {
    yasm_objfmt_elf *objfmt_elf;
    FILE *f;
    elf_secthead *shead;
    yasm_section *sect;
    yasm_object *object;
    unsigned long sindex;
} elf_objfmt_output_info;

typedef struct {
    yasm_objfmt_elf *objfmt_elf;
    int local_names;
} append_local_sym_info;

yasm_objfmt_module yasm_elf_LTX_objfmt;
yasm_objfmt_module yasm_elf32_LTX_objfmt;
yasm_objfmt_module yasm_elf64_LTX_objfmt;


static elf_symtab_entry *
elf_objfmt_symtab_append(yasm_objfmt_elf *objfmt_elf, yasm_symrec *sym,
			 elf_section_index sectidx, elf_symbol_binding bind,
			 elf_symbol_type type, elf_symbol_vis vis,
                         yasm_expr *size, elf_address value)
{
    elf_strtab_entry *name = elf_strtab_append_str(objfmt_elf->strtab,
						   yasm_symrec_get_name(sym));
    elf_symtab_entry *entry = elf_symtab_entry_create(name, sym);
    elf_symtab_append_entry(objfmt_elf->elf_symtab, entry);

    elf_symtab_set_nonzero(entry, NULL, sectidx, bind, type, size, value);
    elf_sym_set_visibility(entry, vis);
    yasm_symrec_add_data(sym, &elf_symrec_data, entry);

    return entry;
}

static int
elf_objfmt_append_local_sym(yasm_symrec *sym, /*@null@*/ void *d)
{
    append_local_sym_info *info = (append_local_sym_info *)d;
    elf_symtab_entry *entry;
    elf_address value=0;
    yasm_section *sect=NULL;
    yasm_bytecode *precbc=NULL;

    assert(info != NULL);

    if (!yasm_symrec_get_data(sym, &elf_symrec_data)) {
	int is_sect = 0;
	if (!yasm_symrec_get_label(sym, &precbc))
	    return 1;
	sect = yasm_bc_get_section(precbc);
	if (!yasm_section_is_absolute(sect) &&
	    strcmp(yasm_symrec_get_name(sym), yasm_section_get_name(sect))==0)
	    is_sect = 1;

	/* neither sections nor locals (except when debugging) need names */
	entry = elf_symtab_insert_local_sym(info->objfmt_elf->elf_symtab,
		    info->local_names && !is_sect ?
		    info->objfmt_elf->strtab : NULL, sym);
	elf_symtab_set_nonzero(entry, sect, 0, STB_LOCAL,
			       is_sect ? STT_SECTION : STT_NOTYPE, NULL, 0);
	yasm_symrec_add_data(sym, &elf_symrec_data, entry);

	if (is_sect)
	    return 1;
    }
    else {
	if (!yasm_symrec_get_label(sym, &precbc))
	    return 1;
	sect = yasm_bc_get_section(precbc);
    }

    entry = yasm_symrec_get_data(sym, &elf_symrec_data);
    if (precbc)
	value = precbc->offset + precbc->len;
    elf_symtab_set_nonzero(entry, sect, 0, 0, 0, NULL, value);

    return 1;
}

static yasm_objfmt *
elf_objfmt_create_common(const char *in_filename, yasm_object *object,
			 yasm_arch *a, yasm_objfmt_module *module,
			 int bits_pref,
			 const elf_machine_handler **elf_march_out)
{
    yasm_objfmt_elf *objfmt_elf = yasm_xmalloc(sizeof(yasm_objfmt_elf));
    yasm_symrec *filesym;
    elf_symtab_entry *entry;
    const elf_machine_handler *elf_march;

    objfmt_elf->objfmt.module = module;
    objfmt_elf->object = object;
    objfmt_elf->symtab = yasm_object_get_symtab(object);
    objfmt_elf->arch = a;
    elf_march = elf_set_arch(a, objfmt_elf->symtab, bits_pref);
    if (!elf_march) {
	yasm_xfree(objfmt_elf);
	return NULL;
    }
    if (elf_march_out)
	*elf_march_out = elf_march;

    objfmt_elf->shstrtab = elf_strtab_create();
    objfmt_elf->strtab = elf_strtab_create();
    objfmt_elf->elf_symtab = elf_symtab_create();

    /* FIXME: misuse of NULL bytecode here; it works, but only barely. */
    filesym = yasm_symtab_define_label(objfmt_elf->symtab, ".file", NULL, 0,
				       0);
    entry = elf_symtab_entry_create(
	elf_strtab_append_str(objfmt_elf->strtab, in_filename), filesym);
    yasm_symrec_add_data(filesym, &elf_symrec_data, entry);
    elf_symtab_set_nonzero(entry, NULL, SHN_ABS, STB_LOCAL, STT_FILE, NULL, 0);
    elf_symtab_append_entry(objfmt_elf->elf_symtab, entry);

    /* FIXME: misuse of NULL bytecode */
    objfmt_elf->dotdotsym = yasm_symtab_define_label(objfmt_elf->symtab,
						     "..sym", NULL, 1, 0);

    return (yasm_objfmt *)objfmt_elf;
}

static yasm_objfmt *
elf_objfmt_create(const char *in_filename, yasm_object *object, yasm_arch *a)
{
    const elf_machine_handler *elf_march;
    yasm_objfmt *objfmt;
    yasm_objfmt_elf *objfmt_elf;

    objfmt = elf_objfmt_create_common(in_filename, object, a,
				      &yasm_elf_LTX_objfmt, 0, &elf_march);
    if (objfmt) {
	objfmt_elf = (yasm_objfmt_elf *)objfmt;
	/* Figure out which bitness of object format to use */
	if (elf_march->bits == 32)
	    objfmt_elf->objfmt.module = &yasm_elf32_LTX_objfmt;
	else if (elf_march->bits == 64)
	    objfmt_elf->objfmt.module = &yasm_elf64_LTX_objfmt;
    }
    return objfmt;
}

static yasm_objfmt *
elf32_objfmt_create(const char *in_filename, yasm_object *object, yasm_arch *a)
{
    return elf_objfmt_create_common(in_filename, object, a,
				    &yasm_elf32_LTX_objfmt, 32, NULL);
}

static yasm_objfmt *
elf64_objfmt_create(const char *in_filename, yasm_object *object, yasm_arch *a)
{
    return elf_objfmt_create_common(in_filename, object, a,
				    &yasm_elf64_LTX_objfmt, 64, NULL);
}

static long
elf_objfmt_output_align(FILE *f, unsigned int align)
{
    long pos;
    unsigned long delta;
    if ((align & (align-1)) != 0)
	yasm_internal_error("requested alignment not a power of two");

    pos = ftell(f);
    if (pos == -1) {
	yasm__error(0, N_("could not get file position on output file"));
	return -1;
    }
    delta = align - (pos & (align-1)); 
    if (delta != align) {
	pos += delta;
	if (fseek(f, pos, SEEK_SET) < 0) {
	    yasm__error(0, N_("could not set file position on output file"));
	    return -1;
	}
    }
    return pos;
}

static int
elf_objfmt_output_reloc(yasm_symrec *sym, yasm_bytecode *bc,
			unsigned char *buf, size_t destsize, size_t valsize,
			int warn, void *d)
{
    elf_reloc_entry *reloc;
    elf_objfmt_output_info *info = d;
    yasm_intnum *zero;
    int retval;

    reloc = elf_reloc_entry_create(sym, NULL,
	yasm_intnum_create_uint(bc->offset), 0, valsize);
    if (reloc == NULL) {
	yasm__error(bc->line, N_("elf: invalid relocation size"));
	return 1;
    }
    /* allocate .rel[a] sections on a need-basis */
    elf_secthead_append_reloc(info->sect, info->shead, reloc);

    zero = yasm_intnum_create_uint(0);
    elf_handle_reloc_addend(zero, reloc);
    retval = yasm_arch_intnum_tobytes(info->objfmt_elf->arch, zero, buf,
				      destsize, valsize, 0, bc, warn,
				      bc->line);
    yasm_intnum_destroy(zero);
    return retval;
}

static int
elf_objfmt_output_expr(yasm_expr **ep, unsigned char *buf, size_t destsize,
			size_t valsize, int shift, unsigned long offset,
			yasm_bytecode *bc, int rel, int warn,
			/*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ yasm_intnum *intn;
    /*@dependent@*/ /*@null@*/ const yasm_floatnum *flt;
    /*@dependent@*/ /*@null@*/ yasm_symrec *sym;
    /*@null@*/ elf_reloc_entry *reloc = NULL;
    /*@null@*/ yasm_expr *wrt_expr;
    /*@dependent@*/ /*@null@*/ yasm_symrec *wrt = NULL;

    if (info == NULL)
	yasm_internal_error("null info struct");

    *ep = yasm_expr_simplify(*ep, yasm_common_calc_bc_dist);

    /* Handle floating point expressions */
    flt = yasm_expr_get_floatnum(ep);
    if (flt) {
	if (shift < 0)
	    yasm_internal_error(N_("attempting to negative shift a float"));
	return yasm_arch_floatnum_tobytes(info->objfmt_elf->arch, flt, buf,
					  destsize, valsize,
					  (unsigned int)shift, warn, bc->line);
    }

    /* Check for a WRT relocation */
    wrt_expr = yasm_expr_extract_wrt(ep);
    if (wrt_expr) {
	wrt = yasm_expr_extract_symrec(&wrt_expr, 0,
				       yasm_common_calc_bc_dist);
	yasm_expr_destroy(wrt_expr);
	if (!wrt) {
	    yasm__error(bc->line, N_("WRT expression too complex"));
	    return 1;
	}
    }

    /* Handle integer expressions, with relocation if necessary */
    sym = yasm_expr_extract_symrec(ep,
				   !(wrt == info->objfmt_elf->dotdotsym ||
				     (wrt && elf_is_wrt_sym_relative(wrt))),
				   yasm_common_calc_bc_dist);
    if (sym) {
	yasm_sym_vis vis;

	vis = yasm_symrec_get_visibility(sym);
	if (wrt == info->objfmt_elf->dotdotsym)
	    wrt = NULL;
	else if (wrt && elf_is_wrt_sym_relative(wrt))
	    ;
	else if (!(vis & (YASM_SYM_COMMON|YASM_SYM_EXTERN)))
	{
	    yasm_bytecode *label_precbc;
	    /* Local symbols need relocation to their section's start */
	    if (yasm_symrec_get_label(sym, &label_precbc)) {
		yasm_section *label_sect = yasm_bc_get_section(label_precbc);
		/*@null@*/ elf_secthead *sym_shead;
		sym_shead =
		    yasm_section_get_data(label_sect, &elf_section_data);
		assert(sym_shead != NULL);
		sym = elf_secthead_get_sym(sym_shead);
	    }
	}

	if (rel) {
	    /* Need to reference to start of section, so add $$ in. */
	    *ep = yasm_expr_create(YASM_EXPR_ADD, yasm_expr_expr(*ep),
		yasm_expr_sym(yasm_symtab_define_label2("$$",
		    yasm_section_bcs_first(info->sect), 0, (*ep)->line)),
		(*ep)->line);
	    /* HELP: and this seems to have the desired effect. */
	    *ep = yasm_expr_create(YASM_EXPR_ADD, yasm_expr_expr(*ep),
		yasm_expr_int(yasm_intnum_create_uint(bc->offset + offset)),
		(*ep)->line);
	    *ep = yasm_expr_simplify(*ep, yasm_common_calc_bc_dist);
	}

	reloc = elf_reloc_entry_create(sym, wrt,
	    yasm_intnum_create_uint(bc->offset + offset), rel, valsize);
	if (reloc == NULL) {
	    yasm__error(bc->line, N_("elf: invalid relocation (WRT or size)"));
	    return 1;
	}
	/* allocate .rel[a] sections on a need-basis */
	elf_secthead_append_reloc(info->sect, info->shead, reloc);
    }

    intn = yasm_expr_get_intnum(ep, NULL);
    if (intn) {
	if (rel) {
	    int retval = yasm_arch_intnum_fixup_rel(info->objfmt_elf->arch,
						    intn, valsize, bc,
						    bc->line);
	    if (retval)
		return retval;
	}
	if (reloc)
	    elf_handle_reloc_addend(intn, reloc);
	return yasm_arch_intnum_tobytes(info->objfmt_elf->arch, intn, buf,
					destsize, valsize, shift, bc, warn,
					bc->line);
    }

    /* Check for complex float expressions */
    if (yasm_expr__contains(*ep, YASM_EXPR_FLOAT)) {
	yasm__error(bc->line, N_("floating point expression too complex"));
	return 1;
    }

    yasm__error(bc->line, N_("elf: relocation too complex"));
    return 1;
}

static int
elf_objfmt_output_bytecode(yasm_bytecode *bc, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    unsigned char buf[256];
    /*@null@*/ /*@only@*/ unsigned char *bigbuf;
    unsigned long size = 256;
    unsigned long multiple;
    unsigned long i;
    int gap;

    if (info == NULL)
	yasm_internal_error("null info struct");

    bigbuf = yasm_bc_tobytes(bc, buf, &size, &multiple, &gap, info,
			     elf_objfmt_output_expr, elf_objfmt_output_reloc);

    /* Don't bother doing anything else if size ended up being 0. */
    if (size == 0) {
	if (bigbuf)
	    yasm_xfree(bigbuf);
	return 0;
    }
    else {
	yasm_intnum *bcsize = yasm_intnum_create_uint(size);
	yasm_intnum *mult = yasm_intnum_create_uint(multiple);

	yasm_intnum_calc(bcsize, YASM_EXPR_MUL, mult, 0);
	elf_secthead_add_size(info->shead, bcsize);

	yasm_intnum_destroy(bcsize);
	yasm_intnum_destroy(mult);
    }

    /* Warn that gaps are converted to 0 and write out the 0's. */
    if (gap) {
	unsigned long left;
	yasm__warning(YASM_WARN_GENERAL, bc->line,
	    N_("uninitialized space declared in code/data section: zeroing"));
	/* Write out in chunks */
	memset(buf, 0, 256);
	left = multiple*size;
	while (left > 256) {
	    fwrite(buf, 256, 1, info->f);
	    left -= 256;
	}
	fwrite(buf, left, 1, info->f);
    } else {
	/* Output multiple copies of buf (or bigbuf if non-NULL) to file */
	for (i=0; i<multiple; i++)
	    fwrite(bigbuf ? bigbuf : buf, (size_t)size, 1, info->f);
    }

    /* If bigbuf was allocated, free it */
    if (bigbuf)
	yasm_xfree(bigbuf);

    return 0;
}

static elf_secthead *
elf_objfmt_create_dbg_secthead(yasm_section *sect,
			       elf_objfmt_output_info *info)
{
    elf_secthead *shead;
    elf_section_type type=SHT_PROGBITS;
    yasm_intnum *align=NULL;
    elf_size entsize=0;
    const char *sectname = yasm_section_get_name(sect);
    elf_strtab_entry *name = elf_strtab_append_str(info->objfmt_elf->shstrtab,
						   sectname);

    if (yasm__strcasecmp(sectname, ".stab")==0) {
	align = yasm_intnum_create_uint(4);
	entsize = 12;
    } else if (yasm__strcasecmp(sectname, ".stabstr")==0) {
	type = SHT_STRTAB;
	align = yasm_intnum_create_uint(1);
    }
    else
	yasm_internal_error(N_("Unrecognized section without data"));

    shead = elf_secthead_create(name, type, 0, 0, 0);
    elf_secthead_set_align(shead, align);
    elf_secthead_set_entsize(shead, entsize);

    yasm_section_add_data(sect, &elf_section_data, shead);

    return shead;
}

static int
elf_objfmt_output_section(yasm_section *sect, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ elf_secthead *shead;
    long pos;
    char *relname;
    const char *sectname;

    /* Don't output absolute sections into the section table */
    if (yasm_section_is_absolute(sect))
	return 0;

    if (info == NULL)
	yasm_internal_error("null info struct");
    shead = yasm_section_get_data(sect, &elf_section_data);
    if (shead == NULL)
	shead = elf_objfmt_create_dbg_secthead(sect, info);

    /* don't output header-only sections */
    if ((elf_secthead_get_type(shead) & SHT_NOBITS) == SHT_NOBITS)
    {
	yasm_bytecode *last = yasm_section_bcs_last(sect);
	if (last) {
	    yasm_intnum *sectsize;
	    sectsize = yasm_intnum_create_uint(last->offset + last->len);
	    elf_secthead_add_size(shead, sectsize);
	    yasm_intnum_destroy(sectsize);
	}
	elf_secthead_set_index(shead, ++info->sindex);
	return 0;
    }

    if ((pos = ftell(info->f)) == -1)
	yasm__error(0, N_("couldn't read position on output stream"));
    pos = elf_secthead_set_file_offset(shead, pos);
    if (fseek(info->f, pos, SEEK_SET) < 0)
	yasm__error(0, N_("couldn't seek on output stream"));

    info->sect = sect;
    info->shead = shead;
    yasm_section_bcs_traverse(sect, info, elf_objfmt_output_bytecode);

    elf_secthead_set_index(shead, ++info->sindex);

    /* No relocations to output?  Go on to next section */
    if (elf_secthead_write_relocs_to_file(info->f, sect, shead) == 0)
	return 0;
    elf_secthead_set_rel_index(shead, ++info->sindex);

    /* name the relocation section .rel[a].foo */
    sectname = yasm_section_get_name(sect);
    relname = elf_secthead_name_reloc_section(sectname);
    elf_secthead_set_rel_name(shead,
        elf_strtab_append_str(info->objfmt_elf->shstrtab, relname));
    yasm_xfree(relname);

    return 0;
}

static int
elf_objfmt_output_secthead(yasm_section *sect, /*@null@*/ void *d)
{
    /*@null@*/ elf_objfmt_output_info *info = (elf_objfmt_output_info *)d;
    /*@dependent@*/ /*@null@*/ elf_secthead *shead;

    /* Don't output absolute sections into the section table */
    if (yasm_section_is_absolute(sect))
	return 0;

    if (info == NULL)
	yasm_internal_error("null info struct");
    shead = yasm_section_get_data(sect, &elf_section_data);
    if (shead == NULL)
	yasm_internal_error("no section header attached to section");

    if(elf_secthead_write_to_file(info->f, shead, info->sindex+1))
	info->sindex++;

    /* output strtab headers here? */

    /* relocation entries for .foo are stored in section .rel[a].foo */
    if(elf_secthead_write_rel_to_file(info->f, 3, sect, shead,
				      info->sindex+1))
	info->sindex++;

    return 0;
}

static void
elf_objfmt_output(yasm_objfmt *objfmt, FILE *f, const char *obj_filename,
		  int all_syms, yasm_dbgfmt *df)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    elf_objfmt_output_info info;
    append_local_sym_info localsym_info;
    long pos;
    unsigned long elf_shead_addr;
    elf_secthead *esdn;
    unsigned long elf_strtab_offset, elf_shstrtab_offset, elf_symtab_offset;
    unsigned long elf_strtab_size, elf_shstrtab_size, elf_symtab_size;
    elf_strtab_entry *elf_strtab_name, *elf_shstrtab_name, *elf_symtab_name;
    unsigned long elf_symtab_nlocal;

    info.objfmt_elf = objfmt_elf;
    info.f = f;

    /* Allocate space for Ehdr by seeking forward */
    if (fseek(f, (long)(elf_proghead_get_size()), SEEK_SET) < 0) {
	yasm__error(0, N_("could not seek on output file"));
	return;
    }

    /* add all (local) syms to symtab because relocation needs a symtab index
     * if all_syms, register them by name.  if not, use strtab entry 0 */
    localsym_info.local_names = all_syms;
    localsym_info.objfmt_elf = objfmt_elf;
    yasm_symtab_traverse(yasm_object_get_symtab(objfmt_elf->object),
			 &localsym_info, elf_objfmt_append_local_sym);
    elf_symtab_nlocal = elf_symtab_assign_indices(objfmt_elf->elf_symtab);

    /* output known sections - includes reloc sections which aren't in yasm's
     * list.  Assign indices as we go. */
    info.sindex = 3;
    if (yasm_object_sections_traverse(objfmt_elf->object, &info,
				      elf_objfmt_output_section))
	return;

    /* add final sections to the shstrtab */
    elf_strtab_name = elf_strtab_append_str(objfmt_elf->shstrtab, ".strtab");
    elf_symtab_name = elf_strtab_append_str(objfmt_elf->shstrtab, ".symtab");
    elf_shstrtab_name = elf_strtab_append_str(objfmt_elf->shstrtab,
					      ".shstrtab");

    /* output .shstrtab */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_shstrtab_offset = (unsigned long) pos;
    elf_shstrtab_size = elf_strtab_output_to_file(f, objfmt_elf->shstrtab);

    /* output .strtab */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_strtab_offset = (unsigned long) pos;
    elf_strtab_size = elf_strtab_output_to_file(f, objfmt_elf->strtab);

    /* output .symtab - last section so all others have indexes */
    if ((pos = elf_objfmt_output_align(f, 4)) == -1)
	return;
    elf_symtab_offset = (unsigned long) pos;
    elf_symtab_size = elf_symtab_write_to_file(f, objfmt_elf->elf_symtab);

    /* output section header table */
    if ((pos = elf_objfmt_output_align(f, 16)) == -1)
	return;
    elf_shead_addr = (unsigned long) pos;

    /* stabs debugging support */
    if (strcmp(yasm_dbgfmt_keyword(df), "stabs")==0) {
	yasm_section *stabsect = yasm_object_find_general(objfmt_elf->object,
							  ".stab");
	yasm_section *stabstrsect =
	    yasm_object_find_general(objfmt_elf->object, ".stabstr");
	if (stabsect && stabstrsect) {
	    elf_secthead *stab =
		yasm_section_get_data(stabsect, &elf_section_data);
	    elf_secthead *stabstr =
		yasm_section_get_data(stabstrsect, &elf_section_data);
	    if (stab && stabstr) {
		elf_secthead_set_link(stab, elf_secthead_get_index(stabstr));
	    }
	    else
		yasm_internal_error(N_("missing .stab or .stabstr section/data"));
	}
    }
    
    /* output dummy section header - 0 */
    info.sindex = 0;

    esdn = elf_secthead_create(NULL, SHT_NULL, 0, 0, 0);
    elf_secthead_set_index(esdn, 0);
    elf_secthead_write_to_file(f, esdn, 0);
    elf_secthead_destroy(esdn);

    esdn = elf_secthead_create(elf_shstrtab_name, SHT_STRTAB, 0,
			       elf_shstrtab_offset, elf_shstrtab_size);
    elf_secthead_set_index(esdn, 1);
    elf_secthead_write_to_file(f, esdn, 1);
    elf_secthead_destroy(esdn);

    esdn = elf_secthead_create(elf_strtab_name, SHT_STRTAB, 0,
			       elf_strtab_offset, elf_strtab_size);
    elf_secthead_set_index(esdn, 2);
    elf_secthead_write_to_file(f, esdn, 2);
    elf_secthead_destroy(esdn);

    esdn = elf_secthead_create(elf_symtab_name, SHT_SYMTAB, 0,
			       elf_symtab_offset, elf_symtab_size);
    elf_secthead_set_index(esdn, 3);
    elf_secthead_set_info(esdn, elf_symtab_nlocal);
    elf_secthead_set_link(esdn, 2);	/* for .strtab, which is index 2 */
    elf_secthead_write_to_file(f, esdn, 3);
    elf_secthead_destroy(esdn);

    info.sindex = 3;
    /* output remaining section headers */
    yasm_object_sections_traverse(objfmt_elf->object, &info,
				  elf_objfmt_output_secthead);

    /* output Ehdr */
    if (fseek(f, 0, SEEK_SET) < 0) {
	yasm__error(0, N_("could not seek on output file"));
	return;
    }

    elf_proghead_write_to_file(f, elf_shead_addr, info.sindex+1, 1);
}

static void
elf_objfmt_destroy(yasm_objfmt *objfmt)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    elf_symtab_destroy(objfmt_elf->elf_symtab);
    elf_strtab_destroy(objfmt_elf->shstrtab);
    elf_strtab_destroy(objfmt_elf->strtab);
    yasm_xfree(objfmt);
}

static /*@observer@*/ /*@null@*/ yasm_section *
elf_objfmt_section_switch(yasm_objfmt *objfmt, yasm_valparamhead *valparams,
			  /*@unused@*/ /*@null@*/
			  yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_valparam *vp = yasm_vps_first(valparams);
    yasm_section *retval;
    int isnew;
    unsigned long type = SHT_PROGBITS;
    unsigned long flags = SHF_ALLOC;
    unsigned long align = 4;
    yasm_intnum *align_intn = NULL;
    int flags_override = 0;
    char *sectname;
    int resonly = 0;
    static const struct {
	const char *name;
	unsigned long flags;
    } flagquals[] = {
	{ "alloc",	SHF_ALLOC },
	{ "exec",	SHF_EXECINSTR },
	{ "write",	SHF_WRITE },
	/*{ "progbits",	SHT_PROGBITS },*/
	/*{ "align",	0 } */
    };

    if (!vp || vp->param || !vp->val)
	return NULL;

    sectname = vp->val;

    if (strcmp(sectname, ".bss") == 0) {
	type = SHT_NOBITS;
	flags = SHF_ALLOC + SHF_WRITE;
	resonly = 1;
    } else if (strcmp(sectname, ".data") == 0) {
	type = SHT_PROGBITS;
	flags = SHF_ALLOC + SHF_WRITE;
    } else if (strcmp(sectname, ".rodata") == 0) {
	type = SHT_PROGBITS;
	flags = SHF_ALLOC;
    } else if (strcmp(sectname, ".text") == 0) {
	align = 16;
	type = SHT_PROGBITS;
	flags = SHF_ALLOC + SHF_EXECINSTR;
    } else {
	/* Default to code */
	align = 1;
    }

    while ((vp = yasm_vps_next(vp))) {
	size_t i;
	int match;

	match = 0;
	for (i=0; i<NELEMS(flagquals) && !match; i++) {
	    if (yasm__strcasecmp(vp->val, flagquals[i].name) == 0) {
		flags_override = 1;
		match = 1;
		flags |= flagquals[i].flags;
	    }
	    else if (yasm__strcasecmp(vp->val+2, flagquals[i].name) == 0
		  && yasm__strncasecmp(vp->val, "no", 2) == 0) {
		flags &= ~flagquals[i].flags;
		flags_override = 1;
		match = 1;
	    }
	}

	if (match)
	    ;
	else if (yasm__strncasecmp(vp->val, "gas_", 4) == 0) {
	    /* GAS-style flags */
	    flags = 0;
	    for (i=4; i<strlen(vp->val); i++) {
		switch (vp->val[i]) {
		    case 'a':
			flags |= SHF_ALLOC;
			break;
		    case 'w':
			flags |= SHF_WRITE;
			break;
		    case 'x':
			flags |= SHF_EXECINSTR;
			break;
		}
	    }
	} else if (yasm__strcasecmp(vp->val, "progbits") == 0) {
	    type |= SHT_PROGBITS;
	}
	else if (yasm__strcasecmp(vp->val, "noprogbits") == 0 ||
		 yasm__strcasecmp(vp->val, "nobits") == 0) {
	    type &= ~SHT_PROGBITS;
	    type |= SHT_NOBITS;
	}
	else if (yasm__strcasecmp(vp->val, "align") == 0 && vp->param) {
            /*@dependent@*/ /*@null@*/ const yasm_intnum *align_expr;
            unsigned long addralign;

            align_expr = yasm_expr_get_intnum(&vp->param, NULL);
            if (!align_expr) {
                yasm__error(line,
                            N_("argument to `%s' is not a power of two"),
                            vp->val);
                return NULL;
            }
            addralign = yasm_intnum_get_uint(align_expr);

            /* Alignments must be a power of two. */
            if ((addralign & (addralign - 1)) != 0) {
                yasm__error(line,
                            N_("argument to `%s' is not a power of two"),
                            vp->val);
                return NULL;
            }

            align_intn = yasm_intnum_copy(align_expr);
	} else
	    yasm__warning(YASM_WARN_GENERAL, line,
			  N_("Unrecognized qualifier `%s'"), vp->val);
    }

    retval = yasm_object_get_general(objfmt_elf->object, sectname, 0, resonly,
				     &isnew, line);

    if (isnew) {
	elf_secthead *esd;
	yasm_symrec *sym;
	elf_strtab_entry *name = elf_strtab_append_str(objfmt_elf->shstrtab,
						       sectname);

	esd = elf_secthead_create(name, type, flags, 0, 0);
	if (!align_intn)
	    align_intn = yasm_intnum_create_uint(align);
	if (align_intn)
	    elf_secthead_set_align(esd, align_intn);
	yasm_section_add_data(retval, &elf_section_data, esd);
	sym = yasm_symtab_define_label(
	    yasm_object_get_symtab(objfmt_elf->object), sectname,
	    yasm_section_bcs_first(retval), 1, line);

	elf_secthead_set_sym(esd, sym);
    } else if (flags_override)
	yasm__warning(YASM_WARN_GENERAL, line,
		      N_("section flags ignored on section redeclaration"));
    return retval;
}

static yasm_symrec *
elf_objfmt_extern_declare(yasm_objfmt *objfmt, const char *name, /*@unused@*/
			  /*@null@*/ yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;

    sym = yasm_symtab_declare(objfmt_elf->symtab, name, YASM_SYM_EXTERN, line);
    elf_objfmt_symtab_append(objfmt_elf, sym, SHN_UNDEF, STB_GLOBAL,
                             STT_NOTYPE, STV_DEFAULT, NULL, 0);

    if (objext_valparams) {
	yasm_valparam *vp = yasm_vps_first(objext_valparams);
	for (; vp; vp = yasm_vps_next(vp))
        {
            if (vp->val)
                yasm__error(line, N_("unrecognized symbol type `%s'"), vp->val);
        }
    }
    return sym;
}

static yasm_symrec *
elf_objfmt_global_declare(yasm_objfmt *objfmt, const char *name,
			  /*@null@*/ yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;
    elf_symbol_type type = STT_NOTYPE;
    yasm_expr *size = NULL;
    elf_symbol_vis vis = STV_DEFAULT;
    unsigned int vis_overrides = 0;

    sym = yasm_symtab_declare(objfmt_elf->symtab, name, YASM_SYM_GLOBAL, line);

    if (objext_valparams) {
	yasm_valparam *vp = yasm_vps_first(objext_valparams);
	for (; vp; vp = yasm_vps_next(vp))
        {
            if (vp->val) {
                if (yasm__strcasecmp(vp->val, "function") == 0)
                    type = STT_FUNC;
                else if (yasm__strcasecmp(vp->val, "data") == 0 ||
                         yasm__strcasecmp(vp->val, "object") == 0)
                    type = STT_OBJECT;
                else if (yasm__strcasecmp(vp->val, "internal") == 0) {
                    vis = STV_INTERNAL;
                    vis_overrides++;
                }
                else if (yasm__strcasecmp(vp->val, "hidden") == 0) {
                    vis = STV_HIDDEN;
                    vis_overrides++;
                }
                else if (yasm__strcasecmp(vp->val, "protected") == 0) {
                    vis = STV_PROTECTED;
                    vis_overrides++;
                }
                else
                    yasm__error(line, N_("unrecognized symbol type `%s'"),
                                vp->val);
            }
            else if (vp->param && !size) {
                size = vp->param;
                vp->param = NULL;	/* to avoid deleting the expr */
            }
	}
        if (vis_overrides > 1) {
            yasm__warning(YASM_WARN_GENERAL, line,
                N_("More than one symbol visibility provided; using last"));
        }
    }

    elf_objfmt_symtab_append(objfmt_elf, sym, SHN_UNDEF, STB_GLOBAL,
                             type, vis, size, 0);

    return sym;
}

static yasm_symrec *
elf_objfmt_common_declare(yasm_objfmt *objfmt, const char *name,
			  /*@only@*/ yasm_expr *size, /*@null@*/
			  yasm_valparamhead *objext_valparams,
			  unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;
    unsigned long addralign = 0;

    sym = yasm_symtab_declare(objfmt_elf->symtab, name, YASM_SYM_COMMON, line);

    if (objext_valparams) {
	yasm_valparam *vp = yasm_vps_first(objext_valparams);
        for (; vp; vp = yasm_vps_next(vp)) {
            if (!vp->val && vp->param) {
                /*@dependent@*/ /*@null@*/ const yasm_intnum *align_expr;

                align_expr = yasm_expr_get_intnum(&vp->param, NULL);
                if (!align_expr) {
                    yasm__error(line,
                                N_("alignment constraint is not a power of two"));
                    return sym;
                }
                addralign = yasm_intnum_get_uint(align_expr);

                /* Alignments must be a power of two. */
                if ((addralign & (addralign - 1)) != 0) {
                    yasm__error(line,
                                N_("alignment constraint is not a power of two"));
                    return sym;
                }
            } else if (vp->val)
                yasm__warning(YASM_WARN_GENERAL, line,
                              N_("Unrecognized qualifier `%s'"), vp->val);
        }
    }

    elf_objfmt_symtab_append(objfmt_elf, sym, SHN_COMMON, STB_GLOBAL,
                             STT_NOTYPE, STV_DEFAULT, size, addralign);

    return sym;
}

static int
elf_objfmt_directive(yasm_objfmt *objfmt, const char *name,
		     yasm_valparamhead *valparams,
		     /*@unused@*/ /*@null@*/
		     yasm_valparamhead *objext_valparams,
		     unsigned long line)
{
    yasm_objfmt_elf *objfmt_elf = (yasm_objfmt_elf *)objfmt;
    yasm_symrec *sym;
    yasm_valparam *vp = yasm_vps_first(valparams);
    char *symname = vp->val;
    elf_symtab_entry *entry;

    if (!symname) {
	yasm__error(line, N_("Symbol name not specified"));
	return 0;
    }

    if (yasm__strcasecmp(name, "type") == 0) {
	/* Get symbol elf data */
	sym = yasm_symtab_use(objfmt_elf->symtab, symname, line);
	entry = yasm_symrec_get_data(sym, &elf_symrec_data);

	/* Create entry if necessary */
	if (!entry) {
	    entry = elf_symtab_entry_create(
		elf_strtab_append_str(objfmt_elf->strtab, symname), sym);
	    elf_symtab_append_entry(objfmt_elf->elf_symtab, entry);
	    yasm_symrec_add_data(sym, &elf_symrec_data, entry);
	}

	/* Pull new type from val */
	vp = yasm_vps_next(vp);
	if (vp->val) {
	    if (yasm__strcasecmp(vp->val, "function") == 0)
		elf_sym_set_type(entry, STT_FUNC);
	    else if (yasm__strcasecmp(vp->val, "object") == 0)
		elf_sym_set_type(entry, STT_OBJECT);
	    else
		yasm__warning(YASM_WARN_GENERAL, line,
			      N_("unrecognized symbol type `%s'"), vp->val);
	} else
	    yasm__error(line, N_("no type specified"));
    } else if (yasm__strcasecmp(name, "size") == 0) {
	/* Get symbol elf data */
	sym = yasm_symtab_use(objfmt_elf->symtab, symname, line);
	entry = yasm_symrec_get_data(sym, &elf_symrec_data);

	/* Create entry if necessary */
	if (!entry) {
	    entry = elf_symtab_entry_create(
		elf_strtab_append_str(objfmt_elf->strtab, symname), sym);
	    elf_symtab_append_entry(objfmt_elf->elf_symtab, entry);
	    yasm_symrec_add_data(sym, &elf_symrec_data, entry);
	}

	/* Pull new size from either param (expr) or val */
	vp = yasm_vps_next(vp);
	if (vp->param) {
	    elf_sym_set_size(entry, vp->param);
	    vp->param = NULL;
	} else if (vp->val)
	    elf_sym_set_size(entry, yasm_expr_create_ident(yasm_expr_sym(
		yasm_symtab_use(objfmt_elf->symtab, vp->val, line)), line));
	else
	    yasm__error(line, N_("no size specified"));
    } else if (yasm__strcasecmp(name, "weak") == 0) {
	sym = yasm_symtab_declare(objfmt_elf->symtab, symname, YASM_SYM_GLOBAL,
				  line);
	elf_objfmt_symtab_append(objfmt_elf, sym, SHN_UNDEF, STB_WEAK,
				 STT_NOTYPE, STV_DEFAULT, NULL, 0);
    } else
	return 1;	/* unrecognized */

    return 0;
}


/* Define valid debug formats to use with this object format */
static const char *elf_objfmt_dbgfmt_keywords[] = {
    "null",
    "stabs",
    NULL
};

/* Define objfmt structure -- see objfmt.h for details */
yasm_objfmt_module yasm_elf_LTX_objfmt = {
    "ELF",
    "elf",
    "o",
    ".text",
    32,
    elf_objfmt_dbgfmt_keywords,
    "null",
    elf_objfmt_create,
    elf_objfmt_output,
    elf_objfmt_destroy,
    elf_objfmt_section_switch,
    elf_objfmt_extern_declare,
    elf_objfmt_global_declare,
    elf_objfmt_common_declare,
    elf_objfmt_directive
};

yasm_objfmt_module yasm_elf32_LTX_objfmt = {
    "ELF (32-bit)",
    "elf32",
    "o",
    ".text",
    32,
    elf_objfmt_dbgfmt_keywords,
    "null",
    elf32_objfmt_create,
    elf_objfmt_output,
    elf_objfmt_destroy,
    elf_objfmt_section_switch,
    elf_objfmt_extern_declare,
    elf_objfmt_global_declare,
    elf_objfmt_common_declare,
    elf_objfmt_directive
};

yasm_objfmt_module yasm_elf64_LTX_objfmt = {
    "ELF (64-bit)",
    "elf64",
    "o",
    ".text",
    64,
    elf_objfmt_dbgfmt_keywords,
    "null",
    elf64_objfmt_create,
    elf_objfmt_output,
    elf_objfmt_destroy,
    elf_objfmt_section_switch,
    elf_objfmt_extern_declare,
    elf_objfmt_global_declare,
    elf_objfmt_common_declare,
    elf_objfmt_directive
};
