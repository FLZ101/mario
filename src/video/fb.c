#include <multiboot.h>
#include <misc.h>
#include <mm/bootmem.h>
#include <mm/mm.h>
#include <mm/pagetable.h>
#include <printk.h>
#include <lib/string.h>
#include <video/fb.h>

static struct fb_info fb;

static int parse_number(const char **text, unsigned int base, uint32_t *result)
{
	const char *p = *text;
	uint32_t value = 0;
	int digits = 0;

	while (1) {
		unsigned int digit;
		if ('0' <= *p && *p <= '9')
			digit = *p - '0';
		else if ('a' <= *p && *p <= 'f')
			digit = *p - 'a' + 10;
		else if ('A' <= *p && *p <= 'F')
			digit = *p - 'A' + 10;
		else
			break;
		if (digit >= base)
			break;
		value = value * base + digit;
		p++;
		digits++;
	}
	if (!digits)
		return -1;
	*text = p;
	*result = value;
	return 0;
}

static int fb_set_info(uint64_t phys_addr, uint32_t pitch, uint32_t width,
			   uint32_t height, uint32_t bpp,
			   uint32_t red_position, uint32_t red_size,
			   uint32_t green_position, uint32_t green_size,
			   uint32_t blue_position, uint32_t blue_size)
{
	uint64_t size = (uint64_t)pitch * height;

	if (!phys_addr || !pitch || !width || !height ||
	    (bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) ||
	    !red_size || !green_size || !blue_size ||
	    size > 64 * 1024 * 1024)
		return -1;

	fb.phys_addr = phys_addr;
	fb.pitch = pitch;
	fb.width = width;
	fb.height = height;
	fb.bpp = bpp;
	fb.red.offset = red_position;
	fb.red.length = red_size;
	fb.green.offset = green_position;
	fb.green.length = green_size;
	fb.blue.offset = blue_position;
	fb.blue.length = blue_size;
	fb.size = size;
	return 0;
}

static int fb_set_grub4dos_video(const char *cmdline)
{
	const char *p;
	uint32_t width, height, bpp, phys_addr, pitch;

	for (p = cmdline; *p; p++) {
		if (strncmp(p, "video=", 6))
			continue;
		p += 6;
		if (parse_number(&p, 10, &width) || *p++ != 'x' ||
		    parse_number(&p, 10, &height) || *p++ != 'x' ||
		    parse_number(&p, 10, &bpp) || *p++ != '@' ||
		    *p++ != '0' || (*p != 'x' && *p != 'X'))
			return -1;
		p++;
		if (parse_number(&p, 16, &phys_addr) || *p++ != ',' ||
		    parse_number(&p, 10, &pitch))
			return -1;
		/* GRUB4DOS setvbe selects QEMU stdvga's xrgb8888 mode. */
		if (bpp != 32)
			return -1;
		return fb_set_info(phys_addr, pitch, width, height, bpp,
				   16, 8, 8, 8, 0, 8);
	}
	return -1;
}

const struct fb_info *fb_get_info(void)
{
	return &fb;
}

int fb_is_enabled(void)
{
	return fb.enabled;
}

void fb_early_init(struct multiboot_info *m)
{
	if ((m->flags & MB_FLAG_FRAMEBUFFER) &&
	    m->framebuffer_type == MB_FRAMEBUFFER_TYPE_RGB &&
	    !fb_set_info(m->framebuffer_addr, m->framebuffer_pitch,
			 m->framebuffer_width, m->framebuffer_height,
			 m->framebuffer_bpp, m->framebuffer_red_field_position,
			 m->framebuffer_red_mask_size,
			 m->framebuffer_green_field_position,
			 m->framebuffer_green_mask_size,
			 m->framebuffer_blue_field_position,
			 m->framebuffer_blue_mask_size))
		return;

	if (m->flags & MB_FLAG_VBE) {
		struct multiboot_vbe_mode_info *mode;
		if (!m->vbe_mode_info)
			goto missing;
		mode = (struct multiboot_vbe_mode_info *)__vir(m->vbe_mode_info);
		if ((mode->mode_attributes & 0x0091) == 0x0091 &&
		    !fb_set_info(mode->phys_base_ptr, mode->bytes_per_scanline,
				 mode->x_resolution, mode->y_resolution,
				 mode->bits_per_pixel, mode->red_field_position,
				 mode->red_mask_size, mode->green_field_position,
				 mode->green_mask_size, mode->blue_field_position,
				 mode->blue_mask_size))
			return;
	}
	if ((m->flags & MB_FLAG_CMDLINE) && m->cmdline &&
	    !fb_set_grub4dos_video((const char *)__vir(m->cmdline)))
		return;

missing:
	pr_warn("fb: no usable Multiboot framebuffer\n");
}

void fb_map(void)
{
	unsigned long phys, virt, end;

	if (!fb.size)
		return;
	phys = (unsigned long)fb.phys_addr & PAGE_MASK;
	virt = FB_VIRT_BASE;
	end = PAGE_ALIGN((unsigned long)fb.phys_addr + fb.size);
	while (phys < end) {
		pde_t *pde = pde_offset(swapper_pg_dir, virt);
		pte_t *pte;
		if (!*pde) {
			pte_t *pt = alloc_page_bootmem();
			set_pde(pde, mk_pde(pt, _PDE));
		}
		pte = pte_offset(__vir(*pde), virt);
		set_pte(pte, __mk_pte(phys, _PTE_KERNEL | PG_RW));
		phys += PAGE_SIZE;
		virt += PAGE_SIZE;
	}
	flush_tlb();
	fb.virt_addr = (void *)(FB_VIRT_BASE + ((unsigned long)fb.phys_addr & ~PAGE_MASK));
	fb.enabled = 1;
	pr_info("fb: %ux%u pitch %u bpp %u phys %llx\n", fb.width,
		fb.height, fb.pitch, fb.bpp, fb.phys_addr);
}
