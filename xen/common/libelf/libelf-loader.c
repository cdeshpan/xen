/*
 * parse and load elf binaries
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef __XEN__
#include <asm/guest_access.h>
#endif

#include "libelf-private.h"

/* ------------------------------------------------------------------------ */

elf_errorstatus elf_init(struct elf_binary *elf, const char *image_input, size_t size)
{
    ELF_HANDLE_DECL(elf_shdr) shdr;
    uint64_t i, count, section, offset;

    if ( !elf_is_elfbinary(image_input, size) )
    {
        elf_err(elf, "%s: not an ELF binary\n", __FUNCTION__);
        return -1;
    }

    elf_memset_unchecked(elf, 0, sizeof(*elf));
    elf->image_base = image_input;
    elf->size = size;
    elf->ehdr = ELF_MAKE_HANDLE(elf_ehdr, (elf_ptrval)image_input);
    elf->class = elf_uval_3264(elf, elf->ehdr, e32.e_ident[EI_CLASS]);
    elf->data = elf_uval_3264(elf, elf->ehdr, e32.e_ident[EI_DATA]);
    elf->caller_xdest_base = NULL;
    elf->caller_xdest_size = 0;

    /* Sanity check phdr. */
    offset = elf_uval(elf, elf->ehdr, e_phoff) +
        elf_uval(elf, elf->ehdr, e_phentsize) * elf_phdr_count(elf);
    if ( offset > elf->size )
    {
        elf_err(elf, "%s: phdr overflow (off %" PRIx64 " > size %lx)\n",
                __FUNCTION__, offset, (unsigned long)elf->size);
        return -1;
    }

    /* Sanity check shdr. */
    offset = elf_uval(elf, elf->ehdr, e_shoff) +
        elf_uval(elf, elf->ehdr, e_shentsize) * elf_shdr_count(elf);
    if ( offset > elf->size )
    {
        elf_err(elf, "%s: shdr overflow (off %" PRIx64 " > size %lx)\n",
                __FUNCTION__, offset, (unsigned long)elf->size);
        return -1;
    }

    /* Find section string table. */
    section = elf_uval(elf, elf->ehdr, e_shstrndx);
    shdr = elf_shdr_by_index(elf, section);
    if ( ELF_HANDLE_VALID(shdr) )
        elf->sec_strtab = elf_section_start(elf, shdr);

    /* Find symbol table and symbol string table. */
    count = elf_shdr_count(elf);
    for ( i = 0; i < count; i++ )
    {
        shdr = elf_shdr_by_index(elf, i);
        if ( elf_uval(elf, shdr, sh_type) != SHT_SYMTAB )
            continue;
        elf->sym_tab = shdr;
        shdr = elf_shdr_by_index(elf, elf_uval(elf, shdr, sh_link));
        if ( !ELF_HANDLE_VALID(shdr) )
        {
            elf->sym_tab = ELF_INVALID_HANDLE(elf_shdr);
            continue;
        }
        elf->sym_strtab = elf_section_start(elf, shdr);
        break;
    }

    return 0;
}

#ifndef __XEN__
void elf_call_log_callback(struct elf_binary *elf, bool iserr,
                           const char *fmt,...) {
    va_list al;

    if (!elf->log_callback)
        return;
    if (!(iserr || elf->verbose))
        return;

    va_start(al,fmt);
    elf->log_callback(elf, elf->log_caller_data, iserr, fmt, al);
    va_end(al);
}
    
void elf_set_log(struct elf_binary *elf, elf_log_callback *log_callback,
                 void *log_caller_data, bool verbose)
{
    elf->log_callback = log_callback;
    elf->log_caller_data = log_caller_data;
    elf->verbose = verbose;
}

static elf_errorstatus elf_load_image(struct elf_binary *elf,
                          ELF_PTRVAL_VOID dst, ELF_PTRVAL_CONST_VOID src,
                          uint64_t filesz, uint64_t memsz)
{
    elf_memcpy_safe(elf, dst, src, filesz);
    elf_memset_safe(elf, dst + filesz, 0, memsz - filesz);
    return 0;
}
#else

void elf_set_verbose(struct elf_binary *elf)
{
    elf->verbose = 1;
}

static elf_errorstatus elf_load_image(struct elf_binary *elf, ELF_PTRVAL_VOID dst, ELF_PTRVAL_CONST_VOID src, uint64_t filesz, uint64_t memsz)
{
    elf_errorstatus rc;
    if ( filesz > ULONG_MAX || memsz > ULONG_MAX )
        return -1;
    /* We trust the dom0 kernel image completely, so we don't care
     * about overruns etc. here. */
    rc = raw_copy_to_guest(ELF_UNSAFE_PTR(dst), ELF_UNSAFE_PTR(src), filesz);
    if ( rc != 0 )
        return -1;
    rc = raw_clear_guest(ELF_UNSAFE_PTR(dst + filesz), memsz - filesz);
    if ( rc != 0 )
        return -1;
    return 0;
}
#endif

/* Calculate the required additional kernel space for the elf image */
void elf_parse_bsdsyms(struct elf_binary *elf, uint64_t pstart)
{
    uint64_t sz;
    ELF_HANDLE_DECL(elf_shdr) shdr;
    unsigned i, type;

    if ( !ELF_HANDLE_VALID(elf->sym_tab) )
        return;

    pstart = elf_round_up(elf, pstart);

    /* Space to store the size of the elf image */
    sz = sizeof(uint32_t);

    /* Space for the elf and elf section headers */
    sz += (elf_uval(elf, elf->ehdr, e_ehsize) +
           elf_shdr_count(elf) * elf_uval(elf, elf->ehdr, e_shentsize));
    sz = elf_round_up(elf, sz);

    /* Space for the symbol and string tables. */
    for ( i = 0; i < elf_shdr_count(elf); i++ )
    {
        shdr = elf_shdr_by_index(elf, i);
        type = elf_uval(elf, shdr, sh_type);
        if ( (type == SHT_STRTAB) || (type == SHT_SYMTAB) )
            sz = elf_round_up(elf, sz + elf_uval(elf, shdr, sh_size));
    }

    elf->bsd_symtab_pstart = pstart;
    elf->bsd_symtab_pend   = pstart + sz;
}

static void elf_load_bsdsyms(struct elf_binary *elf)
{
    ELF_HANDLE_DECL_NONCONST(elf_ehdr) sym_ehdr;
    unsigned long sz;
    ELF_PTRVAL_VOID maxva;
    ELF_PTRVAL_VOID symbase;
    ELF_PTRVAL_VOID symtab_addr;
    ELF_HANDLE_DECL_NONCONST(elf_shdr) shdr;
    unsigned i, type;

    if ( !elf->bsd_symtab_pstart )
        return;

#define elf_hdr_elm(_elf, _hdr, _elm, _val)     \
do {                                            \
    if ( elf_64bit(_elf) )                      \
        elf_store_field(_elf, _hdr, e64._elm, _val);  \
    else                                        \
        elf_store_field(_elf, _hdr, e32._elm, _val);  \
} while ( 0 )

    symbase = elf_get_ptr(elf, elf->bsd_symtab_pstart);
    symtab_addr = maxva = symbase + sizeof(uint32_t);

    /* Set up Elf header. */
    sym_ehdr = ELF_MAKE_HANDLE(elf_ehdr, symtab_addr);
    sz = elf_uval(elf, elf->ehdr, e_ehsize);
    elf_memcpy_safe(elf, ELF_HANDLE_PTRVAL(sym_ehdr), ELF_HANDLE_PTRVAL(elf->ehdr), sz);
    maxva += sz; /* no round up */

    elf_hdr_elm(elf, sym_ehdr, e_phoff, 0);
    elf_hdr_elm(elf, sym_ehdr, e_shoff, elf_uval(elf, elf->ehdr, e_ehsize));
    elf_hdr_elm(elf, sym_ehdr, e_phentsize, 0);
    elf_hdr_elm(elf, sym_ehdr, e_phnum, 0);

    /* Copy Elf section headers. */
    shdr = ELF_MAKE_HANDLE(elf_shdr, maxva);
    sz = elf_shdr_count(elf) * elf_uval(elf, elf->ehdr, e_shentsize);
    elf_memcpy_safe(elf, ELF_HANDLE_PTRVAL(shdr),
                    ELF_IMAGE_BASE(elf) + elf_uval(elf, elf->ehdr, e_shoff),
                    sz);
    maxva = ELF_OBSOLETE_VOIDP_CAST elf_round_up(elf, (unsigned long)maxva + sz);

    for ( i = 0; i < elf_shdr_count(elf); i++ )
    {
        type = elf_uval(elf, shdr, sh_type);
        if ( (type == SHT_STRTAB) || (type == SHT_SYMTAB) )
        {
             elf_msg(elf, "%s: shdr %i at 0x%"ELF_PRPTRVAL" -> 0x%"ELF_PRPTRVAL"\n", __func__, i,
                     elf_section_start(elf, shdr), maxva);
             sz = elf_uval(elf, shdr, sh_size);
             elf_memcpy_safe(elf, maxva, elf_section_start(elf, shdr), sz);
             /* Mangled to be based on ELF header location. */
             elf_hdr_elm(elf, shdr, sh_offset, maxva - symtab_addr);
             maxva = ELF_OBSOLETE_VOIDP_CAST elf_round_up(elf, (unsigned long)maxva + sz);
        }
        shdr = ELF_MAKE_HANDLE(elf_shdr, ELF_HANDLE_PTRVAL(shdr) +
                            (unsigned long)elf_uval(elf, elf->ehdr, e_shentsize));
    }

    /* Write down the actual sym size. */
    elf_store_val(elf, uint32_t, symbase, maxva - symtab_addr);

#undef elf_ehdr_elm
}

void elf_parse_binary(struct elf_binary *elf)
{
    ELF_HANDLE_DECL(elf_phdr) phdr;
    uint64_t low = -1;
    uint64_t high = 0;
    uint64_t i, count, paddr, memsz;

    count = elf_uval(elf, elf->ehdr, e_phnum);
    for ( i = 0; i < count; i++ )
    {
        phdr = elf_phdr_by_index(elf, i);
        if ( !elf_phdr_is_loadable(elf, phdr) )
            continue;
        paddr = elf_uval(elf, phdr, p_paddr);
        memsz = elf_uval(elf, phdr, p_memsz);
        elf_msg(elf, "%s: phdr: paddr=0x%" PRIx64
                " memsz=0x%" PRIx64 "\n", __FUNCTION__, paddr, memsz);
        if ( low > paddr )
            low = paddr;
        if ( high < paddr + memsz )
            high = paddr + memsz;
    }
    elf->pstart = low;
    elf->pend = high;
    elf_msg(elf, "%s: memory: 0x%" PRIx64 " -> 0x%" PRIx64 "\n",
            __FUNCTION__, elf->pstart, elf->pend);
}

elf_errorstatus elf_load_binary(struct elf_binary *elf)
{
    ELF_HANDLE_DECL(elf_phdr) phdr;
    uint64_t i, count, paddr, offset, filesz, memsz;
    ELF_PTRVAL_VOID dest;

    count = elf_uval(elf, elf->ehdr, e_phnum);
    for ( i = 0; i < count; i++ )
    {
        phdr = elf_phdr_by_index(elf, i);
        if ( !elf_phdr_is_loadable(elf, phdr) )
            continue;
        paddr = elf_uval(elf, phdr, p_paddr);
        offset = elf_uval(elf, phdr, p_offset);
        filesz = elf_uval(elf, phdr, p_filesz);
        memsz = elf_uval(elf, phdr, p_memsz);
        dest = elf_get_ptr(elf, paddr);
        elf_msg(elf, "%s: phdr %" PRIu64 " at 0x%"ELF_PRPTRVAL" -> 0x%"ELF_PRPTRVAL"\n",
                __func__, i, dest, (ELF_PTRVAL_VOID)(dest + filesz));
        if ( elf_load_image(elf, dest, ELF_IMAGE_BASE(elf) + offset, filesz, memsz) != 0 )
            return -1;
    }

    elf_load_bsdsyms(elf);
    return 0;
}

ELF_PTRVAL_VOID elf_get_ptr(struct elf_binary *elf, unsigned long addr)
{
    return ELF_REALPTR2PTRVAL(elf->dest_base) + addr - elf->pstart;
}

uint64_t elf_lookup_addr(struct elf_binary * elf, const char *symbol)
{
    ELF_HANDLE_DECL(elf_sym) sym;
    uint64_t value;

    sym = elf_sym_by_name(elf, symbol);
    if ( !ELF_HANDLE_VALID(sym) )
    {
        elf_err(elf, "%s: not found: %s\n", __FUNCTION__, symbol);
        return -1;
    }

    value = elf_uval(elf, sym, st_value);
    elf_msg(elf, "%s: symbol \"%s\" at 0x%" PRIx64 "\n", __FUNCTION__,
            symbol, value);
    return value;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
