; Application-processor bring-up trampoline.
;
; This file is linked with the kernel but copied verbatim to physical
; TRAMPOLINE_BASE.  The real-mode part uses CS-relative addresses because the
; SIPI starts the AP with CS:IP = TRAMPOLINE_BASE:0.  The protected-mode part
; runs with a flat data segment and therefore uses full physical addresses.

%define TRAMPOLINE_BASE 0x70000

%define GDT_OFF         0x100
%define GDT_PTR_OFF     0x140
%define PROT_OFF        0x200
%define ENTRY_PTR_OFF   0x240
%define CR3_OFF         0x248
%define STACK_OFF       0x250

GLOBAL smp_trampoline_start
GLOBAL smp_trampoline_end

[BITS 16]
smp_trampoline_start:
	cli
	mov ax, cs
	mov ds, ax
	lgdt [GDT_PTR_OFF]		; physical TRAMPOLINE_BASE + GDT_PTR_OFF
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	jmp dword 0x08:(TRAMPOLINE_BASE + PROT_OFF)	; enter 32-bit protected mode via the code selector

; Pad so the embedded GDT and GDT pointer stay at fixed offsets.
times GDT_OFF - ($ - smp_trampoline_start) db 0

; Flat protected-mode GDT: null, kernel code, kernel data.
	dq 0x0000000000000000
	dq 0x00cf9a000000ffff
	dq 0x00cf92000000ffff

times GDT_PTR_OFF - ($ - smp_trampoline_start) db 0
gdt_ptr:
	dw (3 * 8) - 1
	dd TRAMPOLINE_BASE + GDT_OFF

times PROT_OFF - ($ - smp_trampoline_start) db 0
[BITS 32]
smp_trampoline_protected:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov esp, [TRAMPOLINE_BASE + STACK_OFF]
	mov eax, [TRAMPOLINE_BASE + CR3_OFF]
	mov cr3, eax
	mov eax, cr0
	or eax, 0x80000000
	mov cr0, eax
	jmp far [TRAMPOLINE_BASE + ENTRY_PTR_OFF]
smp_trampoline_end:

; Keep the linked object from triggering an executable-stack warning.  The
; trampoline itself is never called with a stack from the kernel and does not
; need an executable stack.
section .note.GNU-stack noalloc noexec nowrite progbits
