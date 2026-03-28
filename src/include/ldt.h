#ifndef _LDT_H
#define _LDT_H

#include <lib/string.h>

struct user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int seg_32bit : 1;
    unsigned int contents : 2;
    unsigned int read_exec_only : 1;
    unsigned int limit_in_pages : 1;
    unsigned int seg_not_present : 1;
    unsigned int useable : 1;
#ifdef __x86_64__
    /*
     * Because this bit is not present in 32-bit user code, user
     * programs can pass uninitialized values here.  Therefore, in
     * any context in which a user_desc comes from a 32-bit program,
     * the kernel must act as though lm == 0, regardless of the
     * actual value.
     */
    unsigned int lm : 1;
#endif
};

static inline void zero_user_desc(struct user_desc *ud) {
    memset(ud, 0, sizeof(*ud));
}

// A  user_desc is considered "empty" if read_exec_only and seg_not_present are
// set to 1 and all of the other fields are 0. If an "empty" descriptor is
// passed to set_thread_area(), the corresponding TLS entry will be cleared.
#define user_desc_empty(info)                                                  \
    ((info)->base_addr == 0 && (info)->limit == 0 && (info)->contents == 0 &&  \
     (info)->read_exec_only == 1 && (info)->seg_32bit == 0 &&                  \
     (info)->limit_in_pages == 0 && (info)->seg_not_present == 1 &&            \
     (info)->useable == 0)

#define user_desc_zero(info)                                                   \
    ((info)->base_addr == 0 && (info)->limit == 0 && (info)->contents == 0 &&  \
     (info)->read_exec_only == 0 && (info)->seg_32bit == 0 &&                  \
     (info)->limit_in_pages == 0 && (info)->seg_not_present == 0 &&            \
     (info)->useable == 0 && (info)->entry_number == 0)

/* 8 byte segment descriptor */
struct desc_struct {
    uint16_t limit0;
    uint16_t base0;
    uint16_t base1 : 8, type : 4, s : 1, dpl : 2, p : 1;
    uint16_t limit1 : 4, avl : 1, l : 1, d : 1, g : 1, base2 : 8;
} __attribute__((packed));

extern struct desc_struct gdt[];

static inline void fill_desc(struct desc_struct *desc,
                             const struct user_desc *info) {
    desc->limit0 = info->limit & 0x0ffff;

    desc->base0 = (info->base_addr & 0x0000ffff);
    desc->base1 = (info->base_addr & 0x00ff0000) >> 16;

    desc->type = (info->read_exec_only ^ 1) << 1;
    desc->type |= info->contents << 2;
    /* Set the ACCESS bit so it can be mapped RO */
    desc->type |= 1;

    desc->s = 1;
    desc->dpl = 0x3;
    desc->p = info->seg_not_present ^ 1;
    desc->limit1 = (info->limit & 0xf0000) >> 16;
    desc->avl = info->useable;
    desc->d = info->seg_32bit;
    desc->g = info->limit_in_pages;

    desc->base2 = (info->base_addr & 0xff000000) >> 24;
    /*
     * Don't allow setting of the lm bit. It would confuse
     * user_64bit_mode and would get overridden by sysret anyway.
     */
    desc->l = 0;
}

static inline void zero_desc(struct desc_struct *desc) {
    memset(desc, 0, sizeof(*desc));
}

#endif
