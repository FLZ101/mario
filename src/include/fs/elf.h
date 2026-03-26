#ifndef _ELF_H
#define _ELF_H

#include <types.h>

typedef __u32 Elf32_Addr;
typedef __u16 Elf32_Half;
typedef __u32 Elf32_Off;
typedef __s32 Elf32_Sword;
typedef __u32 Elf32_Word;

#define EI_NIDENT 16

struct Elf32_Ehdr {
	unsigned char e_ident[EI_NIDENT];
	Elf32_Half e_type;
	Elf32_Half e_machine;
	Elf32_Word e_version;
	Elf32_Addr e_entry;
	Elf32_Off e_phoff;
	Elf32_Off e_shoff;
	Elf32_Word e_flags;
	Elf32_Half e_ehsize;
	Elf32_Half e_phentsize;
	Elf32_Half e_phnum;
	Elf32_Half e_shentsize;
	Elf32_Half e_shnum;
	Elf32_Half e_shstrndx;
};

#define EI_CLASS 4
#define EI_DATA 5

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define CHECK_EHDR_MAGIC(e_ident) \
	((e_ident)[0] == ELFMAG0 && \
     (e_ident)[1] == ELFMAG1 && \
	 (e_ident)[2] == ELFMAG2 && \
	 (e_ident)[3] == ELFMAG3)

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define ET_NONE 0
#define ET_EXEC 2

#define EM_NONE 0
#define EM_386 3

struct Elf32_Phdr {
	Elf32_Word p_type;
	Elf32_Off p_offset;
	Elf32_Addr p_vaddr;
	Elf32_Addr p_paddr;
	Elf32_Word p_filesz;
	Elf32_Word p_memsz;
	Elf32_Word p_flags;
	Elf32_Word p_align;
};

#define PT_NULL 0
#define PT_LOAD 1
#define PT_PHDR 6

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
#define PF_MASK (PF_X | PF_W | PF_R)

#define PF_CODE (PF_X | PF_R)
#define PF_DATA (PF_W | PF_R)
#define PF_RODATA PF_R

typedef struct
{
	int a_type;
	union {
		long a_val;
		void *a_ptr;
		void (*a_fnc)();
	};
} auxv_t;

#define AT_NULL				0 	// ignored
#define AT_IGNORE			1 	// ignored
#define AT_EXECFD			2 	// a_val
#define AT_PHDR				3 	// a_ptr
#define AT_PHENT			4 	// a_val
#define AT_PHNUM			5 	// a_val
#define AT_PAGESZ			6 	// a_val
#define AT_BASE				7 	// a_ptr
#define AT_FLAGS			8 	// a_val
#define AT_ENTRY			9 	// a_ptr
#define AT_NOTELF			10 	// a_val
#define AT_UID				11 	// a_val
#define AT_EUID				12 	// a_val
#define AT_GID				13 	// a_val
#define AT_EGID				14 	// a_val
#define AT_PLATFORM			15 	// a_ptr
#define AT_HWCAP			16 	// a_val
#define AT_CLKTCK			17 	// a_val
#define AT_SECURE			23 	// a_val
#define AT_BASE_PLATFORM	24 	// a_ptr
#define AT_RANDOM			25 	// a_ptr
#define AT_HWCAP2			26 	// a_val
#define AT_EXECFN			31 	// a_ptr

#define AT_VECTOR_SIZE		32

#endif /* _ELF_H */
