;<bootloader for mario>
;
;first 512 Bytes (stage_1) of BOOT_BIN (boot.bin) should be 
;chainloaded to 0x7c00 by grub4dos, bootmgr, ...
;
;stage_1:
;   Search all fat32 partitions for BOOT_BIN and if we 
;   find it we load it to 0x7c00 + OFFSET and jmp to stage_2
;
;known bug(s):
;   0:
;   when the volume id of a fat32 partition is 'BOOT    BIN'
;   (the short name of boot.bin in a fat32 partition), the 
;   boot.bin in that partition may be missed

extpr               equ 0   ;dword

fat32               equ 4   ;dword
fat32_FAT           equ 8   ;dword
fat32_DATA          equ 12  ;dword
fat32_RootClus      equ 16  ;dword
fat32_SecPerClus    equ 20  ; byte

FAT_lba             equ 24  ;dword

file_size           equ 28  ;dword

idrt_call           equ 32  ; word

BOOT_AT             equ 36  ; word


off_MBR             equ 0x0400 + 512 
            ;MBR will be loaded to DS:off_MBR
off_EBR             equ off_MBR + 512 
            ;EBR will be loaded to DS:off_EBR

off_DBR             equ 0x0400 + 4096 
            ;DBR will be loaded to DS:off_DBR
    
org  0x0400

bits 16

stage_1:
    jmp 0x0780:init1
init1:
    mov ax, cs
    mov ds, ax
    mov es, ax
    
    xor ax, ax
    mov ss, ax
    mov sp, 0x7800

    cld
    mov cx, 512
    xor di, di
    rep stosb               ;zero variables
    
    call read_one_sector    ;read MBR
    
    mov si, off_MBR + 446
    mov cx, 4
look_for_fat32:
    push cx
    mov bx, .cont
    mov al, [si + 4]
    cmp al, 0
    je .fail
    
    inc byte [BOOT_AT]
    
    cmp al, 0x0b
    je _fat32
    cmp al, 0x0c
    je _fat32
    cmp al, 0x05
    je _extp
    cmp al, 0x0f
    je _extp
.cont:
    add si, 16
    pop cx
    loop look_for_fat32
    
.fail:
    jmp $
    
_extp:
    push look_for_fat32.cont
    push si

    mov eax, [si + 8]
    mov [extpr], eax
    mov si, off_EBR + 446
.down:
    mov bx, .next
    inc byte [BOOT_AT + 1]
    mov [DAP_lba], eax
    mov word [DAP_off], off_EBR
    call read_one_sector    ;read EBR
    mov eax, [DAP_lba]
    add [si + 8], eax
    mov al, [si + 4]
    cmp al, 0x0b
    je _fat32
    cmp al, 0x0c
    je _fat32
.next:
    cmp byte [si + 20], 0
    je .nwod
    mov eax, [si + 24]
    add eax, [extpr]
    jmp .down

.nwod:
    pop si
    ;mov [BOOT_AT + 1], byte 0
    ;Don't forget to uncomment last line if you want to tell 
    ;kernel the 'boot_device' :)
    ret
    
_fat32:
    push bx
    push si
    
    mov eax, [si + 8]
    mov [fat32], eax

    mov [DAP_lba], eax
    mov si, off_DBR
    mov word [DAP_off], si
    call read_one_sector        ;read DBR
    
    movzx  eax, word [si + 14]  ;BPB_RsvdSecCnt
    add eax, [fat32]
    mov [fat32_FAT], eax
    
    movzx  eax, byte [si + 16]  ;BPB_NumFATs
    mul dword [si + 36]         ;BPB_FATSz32

    add eax, [fat32_FAT]
    mov [fat32_DATA], eax
    
    mov bl, [si + 13]           ;BPB_SecPerClus
    mov [fat32_SecPerClus], bl

    mov eax, [si + 44]          ;BPB_RootClus
    mov [fat32_RootClus], eax
    
    ;!!!
    ;We should make sure BPB_BytsPerSec is 512, and 
    ;BPB_SecPerClus is no more than 64, but there is 
    ;no space for this

    ;Now [DAP_off] is off_DBR
    call find_it
    jnc .ret 
    call load_it
.ret:
    pop si
    ret

;Used by find_it. Search one cluster for BOOT_BIN. If 
;find is done, ie we find it or we are sure it is not 
;here, CF is set. 
;if find is done
;   if it's not found
;       eax = 0
;   else
;       eax = the first cluster number of it
;       [file_size] contains the size of that file
;else
;   eax is unchanged
for_find:
    mov cx, [fat32_SecPerClus]
    shl cx, 4
    mov si, off_DBR
.scan:
    cmp byte [si], 0 
    je  .ret1           ;find is done, not found

    push cx
    push si
    mov di, BOOT_BIN
    mov cx, 11
    repe cmpsb
    pop si
    pop cx
    jne .cont           ;not match

;To fix bug 0, uncomment the following 2 lines, that 
;would require few more bytes

    ;test byte [si + 11], 0x18
    ;jne .ret1          ;find is done, not found
    mov eax, [si + 28]  
    mov [file_size], eax
    push word [si + 20]
    push word [si + 26]
    pop eax
    jmp .ret2           ;find is done, we find it   
.cont:
    add si, 32
    loop .scan
.ret0:
    clc
    ret
.ret1:
    xor eax, eax
.ret2:
    stc
    ret

;Find BOOT_BIN in root directory. If it is found
;CF is set and eax will be it's first cluster number
;IN:
;   eax - first cluster number of root directory
find_it:
    mov word [idrt_call], for_find
    call go_through_it
    jnc .ret
    cmp eax, 0          ;cf is cleared here
    je .ret
    stc
.ret:
    ret

;Used by load_it
;NOTE:
;   ax is not changed
for_load:
    push ax
    mov ax, [fat32_SecPerClus]
    shl ax, 9
    add [DAP_off], ax
    pop ax                  
    
    ;BOOT_BIN won't be bigger than OFFSET bytes
    cmp word [DAP_off], off_DBR + OFFSET
    cmc
    ret

;Load BOOT_BIN to DS:off_DBR. If we succeed we jump to stage_2
load_it:
    mov word [idrt_call], for_load
    call go_through_it
    cmp word [OFFSET + magic], 0xdead ;Check the magic number
    jne .ret        
    jmp OFFSET + stage_2
.ret:
    ret

;NOTE:
;   stage_1 is chainloaded to DS:0x0400 (0x7c00), while 
;   BOOT_BIN is loaded to DS:off_DBR
OFFSET equ off_DBR - 0x0400


;Go through a file/directory cluster by cluster, and the 
;operation performed on each cluster is [idrt_call]. cf is 
;cleared if there is no more cluster, CF is set if there 
;is no need to go on, for exapmle we've found the file we 
;intended to find
;IN:
;   eax - first cluster number of file/directory
go_through_it:
.one_clus:
    cmp eax, 0x0ffffff8
    jae .ret1
    push eax
    call read_clus
    pop eax
    call word [idrt_call]
    jc  .ret2
    call next_clus
    jmp .one_clus
.ret1:
    clc
.ret2:
    ret

;Read a cluster, CF is set if error occurs
;IN:
;   eax - cluster
;NOTE:
;   si is not changed
read_clus:
    ;clus2lba
    sub eax, 2
    mul dword [fat32_SecPerClus]
    add eax, [fat32_DATA]       

    mov [DAP_lba], eax
    mov al, [fat32_SecPerClus]
    mov [DAP_cnt], al
    call read_one_sector
    ret

;Given a cluster number, find number of the next cluster in 
;the FAT chain
;IN:
;   eax - cluster
;OUT:
;   eax - next cluster
;NOTE:
;   DAP_off is not changed
next_clus:
    push word [DAP_off]
    xor ebx, ebx
    mov bx, 512
    mov di, bx
    mov [DAP_off], bx
    xor edx, edx
    shl eax, 2
    div ebx
    add di, dx
    add eax, [fat32_FAT]
    cmp eax, [FAT_lba]
    je .loaded
    mov [FAT_lba], eax
    mov [DAP_lba], eax 
    call read_one_sector
.loaded:
    mov eax, [di]
    pop word [DAP_off]
    ret

BOOT_BIN db "BOOT    BIN"   
            ;short name of boot.bin in a fat32 partition

;Disk Address Packet
;Refer to 'int 13h' document(s)
DAP:
    db 0x10
    db 0
DAP_cnt:
    db 1
    db 0
DAP_off:
    dw off_MBR
DAP_seg:
    dw 0x0780
DAP_lba:
    dq 0

;if [DAP_cnt] is not specified
;   read one sector
;else
;   read [DAP_cnt] sector(s)
read_one_sector:
    push si
    mov ah, 0x42
    mov dl, 0x80
    mov si, DAP
    int 0x13
    mov byte [DAP_cnt], 1
    pop si
    ret     
    
times 510-($-$$) \
    db 0
    db 0x55
    db 0xaa

;------------------------------------------------------
;stage_2:
;   Search the fat32 partitions where BOOT_BIN was found 
;   for KERNEL_EXE and if we find it we multiboot it
;
;NOTE:
;   Refer to Multiboot Specification version 0.6.96

read_L           equ     40 ;dword
read_H           equ     44 ;dword
loaded_L         equ     48 ;dword
loaded_H         equ     52 ;dword

ClusNumber       equ     56 ;dword
ClusNumber2      equ     60 ;dword
BytesPerClus     equ     64 ;dword

;Offset of the multiboot header in KERNEL_EXE
MB_header        equ     68 ;dowrd
;The Multiboot header
MB_magic         equ     72 ;dword
MB_flags         equ     76 ;dword
MB_checksum      equ     80 ;dword
MB_header_addr   equ     84 ;dword
MB_load_addr     equ     88 ;dword
MB_load_end_addr equ     92 ;dword
MB_bss_end_addr  equ     96 ;dword
MB_entry_addr    equ     100;dword

mv_from          equ     104;dword
mv_ecx           equ     108;dword
mv_to            equ     112;dword

;We'll load INITRD to rd_start ~ rd_end
rd_start         equ     116;dword
rd_end           equ     120;dword

;used as file pointer
fp               equ     124;dword

;Temporary variable(s)
tmp0             equ     128;dword

Buffer           equ     256

MB_MAGIC         equ     0x1badb002
MB_MAGIC_EAX     equ     0x2badb002

;----------------------------------------------------
stage_2:
    jmp ((0x7800 + OFFSET) >> 4):init2

init2:
    ;Copy variables & a sector of FAT we've read
    mov cx, 1024
    xor si, si
    mov di, OFFSET
    rep movsb

    mov ax, cs
    mov ds, ax
    mov es, ax
    mov sp, 0x7800 + OFFSET

    mov [DAP_seg], ax
    mov word [DAP_off], off_DBR ;Bytes will be read to DS:off_DBR

    ;Get BytesPerClus, which should be no more than 32k
    mov ax, [fat32_SecPerClus]
    shl ax, 9
    mov [BytesPerClus], ax

    call clear_screen

    lgdt [gdtr]

;Enable A20
A20:
    in al, 0x92  
    or al, 2
    out 0x92, al
    push ._ret      ; >>> ret

    cli     ; !!!

    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp dword 8:(0x7800 + OFFSET + check_A20)
._ret:

;find KERNEL_EXE 
    mov cx, 11
    mov si, KERNEL_EXE
    mov di, BOOT_BIN
    rep movsb
 
    mov eax, [fat32_RootClus]
    call find_it
    jnc $   ;KERNEL_EXE not found, we stop here

;Now eax is the fisrt cluster number of KERNEL_EXE
    mov [ClusNumber], eax
    mov [ClusNumber2], eax
    call load_next_clus
    jnc $   ;Is KERNEL_EXE an empty file?
    mov [loaded_L], word 0  ;[loaded_H] is already set to [BytesPerClus]

;find Multiboot header
;NOTE:
;   The Multiboot header must be contained completely within the first 
;   8192 bytes of the OS image, and must be 4 bytes aligned.
    mov ax, [BytesPerClus]
    add ax, off_DBR
    mov [tmp0], ax

    mov si, off_DBR
    mov di, Buffer

find_multiboot_header:
    call getl
    cmp eax, MB_MAGIC
    jne find_multiboot_header
    mov [MB_magic], eax

    mov eax, [fp]
    sub eax, 4
    mov [MB_header], eax

    call getl
    mov [MB_flags], eax
    call getl
    mov [MB_checksum], eax

    mov eax, [MB_magic]
    add eax, [MB_flags]
    add eax, [MB_checksum]
    jz multiboot_header_found

    mov eax, [MB_checksum]
    call ungetl
    mov eax, [MB_flags]
    call ungetl

    jmp find_multiboot_header

getl:
    cmp dword [fp], 8192
    je $    ;multiboot header not found
    add dword [fp], 4

    cmp di, Buffer
    je .next_0
    sub di, 4
    mov eax, [di]
    ret
.next_0:
    cmp si, [tmp0]
    je .next_1
    mov eax, [si]
    add si, 4
    ret
.next_1:
    push di ; !!!
    call load_next_clus
    jnc $   ;We've gone through the entire file
    pop di
    mov si, off_DBR
    jmp .next_0

ungetl:
    sub dword [fp], 4

    mov [di], eax
    add di, 4
    ret

multiboot_header_found:
    ;Check whether the multiboot header contains the address fields
    mov eax, [MB_flags]
    and eax, 0x00010000
    je $
    call getl
    mov [MB_header_addr], eax
    call getl
    mov [MB_load_addr], eax
    call getl
    mov [MB_load_end_addr], eax
    call getl
    mov [MB_bss_end_addr], eax
    add eax, 0xfff
    and eax, ~0xfff
    mov [rd_start], eax     ;rd_start should be page aligned
    call getl
    mov [MB_entry_addr], eax

load_kernel:
    mov bp, Str0
    mov cx, Len0
    call print_str

    mov eax, [MB_load_addr]
    mov [mv_to], eax
    sub eax, [MB_header_addr]
    add eax, [MB_header]
    mov [read_L], eax

    ;Make sure that [read_L] >= [loaded_L]
    cmp eax, [loaded_L]
    jae .next_0
    ;Re-read KERNEL_EXE
    mov eax, [ClusNumber2]
    mov [ClusNumber], eax
    call load_next_clus
    mov [loaded_L], dword 0
    mov eax, [BytesPerClus]
    mov [loaded_H], eax
.next_0:
    mov eax, [MB_load_end_addr]
    sub eax, [MB_header_addr]
    add eax, [MB_header]
    mov [read_H], eax

    call load

    mov bp, Str1
    mov cx, Len1
    call print_str
    call new_line

load_initrd:
    mov bp, Str2
    mov cx, Len2
    call print_str

;find INITRD 
    mov cx, 11
    mov si, INITRD
    mov di, BOOT_BIN
    rep movsb
 
    mov eax, [fat32_RootClus]
    call find_it
    jnc $   ;INITRD not found, we stop here

;Now eax is the fisrt cluster number of INITRD
    mov [ClusNumber], eax

    mov [read_L], dword 0
    mov eax, [file_size]
    mov [read_H], eax

    mov eax, [rd_start]
    mov [mv_to], eax
    add eax, [file_size]
    mov [rd_end], eax

    call load_next_clus
    mov [loaded_L], dword 0
    mov eax, [BytesPerClus]
    mov [loaded_H], eax

    call load

    mov bp, Str1
    mov cx, Len1
    call print_str
    call new_line
    
;Let's make the Multiboot_Information_Structure our
;kernel needs, we'll put it at ds:off_DBR

flag               equ  off_DBR      ;dword
mem_lower          equ  off_DBR + 4  ;dword
mem_upper          equ  off_DBR + 8  ;dword
mods_count         equ  off_DBR + 20 ;dword
mods_addr          equ  off_DBR + 24 ;dword
mmap_length        equ  off_DBR + 44 ;dword
mmap_addr          equ  off_DBR + 48 ;dword
boot_loader_name   equ  off_DBR + 64 ;dword

;zero the area we'll use
    cld
    xor ax, ax
    mov cx, 2048    ;2048 bytes should be big enough
    mov di, off_DBR
    rep stosb

LowMem:
    ;int 0x12
    xor eax, eax
    int 0x12
    jc E820
    test ax, ax
    jz E820
    mov [mem_lower], eax    ;eax is the amount of continuous
                            ;memory in KB starting from 0.

HighMem:
    ;int 0x15, ax = 0xe801
    xor cx, cx
    xor dx, dx
    mov ax, 0xe801
    int 0x15
    jc .next_0
    cmp ah, 0x86    ;unsupported function
    je .next_0
    cmp ah, 0x80    ;invalid command
    je .next_0
    jcxz .e801_0

    mov ax, cx
    mov bx, dx
.e801_0:
    ;ax = number of continuous KB, 1M to 16M
    ;bx = continuous 64KB pages above 16M
    and eax, 0xffff
    and ebx, 0xffff

    cmp ax, 0x3c00  ;Is there an ISA hole ?
    jb .e801_1

    shl ebx, 6  ;2^6 = 64
    add eax, ebx
.e801_1:
    mov [mem_upper], eax
    jmp .done

.next_0:
    ;int 0x15, ah = 0x8a
    mov ah, 0x8a
    int 0x15
    jc .next_1
    ;dx:ax = the number of contiguous KB of usable RAM 
    ;starting at 0x00100000
    mov [mem_upper], ax
    mov [mem_upper + 2], dx
    jmp .done

.next_1:
    ;int 0x15, ax = 0xda88
    mov ax, 0xda88
    int 0x15
    jc .next_2
    test ax, ax
    jnz .next_2
    ;cl:bx = the number of contiguous KB of usable RAM 
    ;starting at 0x00100000
    mov [mem_upper], bx
    mov [mem_upper + 2], cl
    jmp .done

.next_2:
    ;int 0x15, ah = 0x88
    mov ah, 0x88
    int 0x15
    jc E820
    test ax, ax
    je E820
    cmp ah, 0x80
    je E820  ;invalid command
    cmp ah, 0x86
    je E820  ;unsupported function
    ;ax = the number of contiguous KB of usable RAM 
    ;starting at 0x00100000
    mov [mem_upper], ax
.done:
    or [flag], byte 1

modules            equ  flag + 256

    mov eax, [rd_start]
    mov [modules], eax
    mov eax, [rd_end]
    mov [modules + 4], eax
    mov [modules + 8], dword 0x7800 + OFFSET + INITRD_STR

    mov [mods_count], byte 1
    mov [mods_addr], dword 0x7800 + OFFSET + modules
    or [flag], byte 8

;the e820 memory map will lie at ds:(flag + 512)
mmap               equ  flag + 512

E820:
    ;int 0x15, eax = 0xe820
    mov di, mmap + 4
    xor ebx, ebx
    xor bp, bp          ;keep an entry count in bp  
.do:
    mov [di - 4], word 24
    mov [di + 20], byte 1
    mov edx, 0x534d4150
    mov eax, 0xe820
    mov ecx, 24
    int 0x15
    jc .done    ;error
    cmp eax, 0x534d4150
    jne .done   ;error

    jcxz .next_1    ;skip 0 length entry
    cmp cl, 20
    jna  .next_2
    ;got a ACPI 3.X entry
    test [di + 20], byte 1
    je .next_1  ;ignore this entry
.next_2:
    inc bp
    add di, 24 + 4
.next_1:
    test ebx, ebx
    je .done    ;There is no more entry
    jmp .do
.done:
    test bp, bp
    je BootloaderName
    or [flag], byte 64
    mov ax, 4 + 24  ; !!!
    mul bp
    mov [mmap_length], ax
    mov [mmap_addr], dword 0x7800 + OFFSET + mmap

BootloaderName:
    call get_cur_pos
    mov [CurPos], dx
    mov [boot_loader_name], dword 0x7800 + OFFSET + Name
    or [flag], word 256

jump_to_kernel:
    cli     ; !!!

    mov ecx, cr0
    or  ecx, 1
    mov cr0, ecx
    jmp 8:(0x7800 + OFFSET + for_jump_to_kernel)

;Load offset read_L ~ read_H of KERNEL_EXE to physical address mv_to
;IN:
;   mv_to
;   read_L
;   read_H
;NOTE:
;   make sure read_L >= loaded_L before the call 
load:
    call shift_window
.mv:
    mov eax, [read_L]
    sub eax, [loaded_L]
    add eax, 0x7800 + OFFSET + off_DBR
    mov [mv_from], eax

    mov eax, [loaded_H]
    cmp eax, [read_H]
    jae ._1
    mov ecx, [loaded_H]
    sub ecx, [read_L]
    mov [mv_ecx], ecx
    call move_bytes
    mov ecx, [loaded_H]
    mov [read_L], ecx
    call load_next_clus
    jnc ._ret
    jmp .mv
._1:
    mov ecx, [read_H]
    sub ecx, [read_L]
    mov [mv_ecx], ecx
    call move_bytes
._ret: 
    ret

;Load cluster(s) until [loaded_H] > [read_L]
;NOTE:
;   eax = [loaded_H] when shift_window returns
shift_window:
    mov eax, [loaded_L]
    cmp eax, [read_L]
    ja $        ; error we can not handle

.shift:
    mov eax, [loaded_H]
    cmp eax, [read_L]
    ja  .done
    call load_next_clus
    jmp .shift
.done:
    ret

;Load next cluster; If there is no more cluster to load, 
;cf is cleared
;NOTE:
;   1. make sure [DAP_off] is set properly before the call
;   2. [ClusNumber] [loaded_L] [loaded_H] will be updated

load_next_clus:
    mov eax, [ClusNumber]
    cmp eax, 0x0ffffff8
    jae .bad
    push eax        
    call read_clus
    pop eax
    call next_clus
    mov [ClusNumber], eax

    mov eax, [BytesPerClus]
    add [loaded_L], eax
    add [loaded_H], eax
    stc
.bad:
    ret

KERNEL_EXE db "KERNEL  EXE"
            ;short name of 'kernel.exe' in a FAT32 partition

INITRD     db "INITRD     "
            ;short name of 'initrd' in a FAT32 partition

INITRD_STR db "MARIO", 0

Str0    db  "Loading kernel ......... "
Len0    equ $ - Str0

Str1    db  "Done"
Len1    equ $ - Str1

Str2    db  "Loading initrd ......... "
Len2    equ $ - Str2

Name    dw  0x3f00  ;This magic number indicates that the kernel 
                    ;is loaded by our bootloader.
CurPos  dw  0       ;We want to tell kernel the cursor position :)
        db  "Good to see you :)", 0

;Print a string
;IN:
;   es:bp   string to print
;   cx      length of string
print_str:
    push cx
    push bp
    
    call get_cur_pos

    pop bp
    pop cx

    mov ax, 0x1301
    mov bx, 0x0007
    int 0x10
    ret
    
new_line:
    call get_cur_pos
    inc dh
    xor dl, dl
    mov ah, 0x02
    int 0x10        ;set cursor position
    ret

clear_screen:
    mov ax, 0x0600
    mov bh, 0x0f
    mov cx, 0
    mov dx, 0x184f
    int 0x10

    mov bh, 0
    xor dx, dx
    mov ah, 0x02
    int 0x10        ;set cursor position to (0, 0)
    ret

;get cursor position
;OUT:
;   dh - Row
;   dl - Column
get_cur_pos:
    mov ah, 0x03
    xor bh, bh
    int 0x10
    ret

;move [mv_ecx] bytes from linear address [mv_from] to 
;linear address [mv_to]
;NOTE:
;   first we switch to Protected-Mode and transfer 
;   some bytes, then we switch back to Real-Mode.

move_bytes:
    push ._ret      ; >>> ret

    cli     ; !!!

    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp dword 8:(0x7800 + OFFSET + for_move_bytes)

._ret:
    ret

;descriptor base, limit, attrs
;---------------------------------
%macro descriptor 3
	dw %2 & 0xffff
	dw %1 & 0xffff
	db (%1 >> 16) & 0xff 
	dw ((%2 >> 8) & 0x0f00) | (%3 & 0xf0ff)
	db (%1 >> 24) & 0xff
%endmacro

gdt:
    descriptor 0, 0, 0
    descriptor 0, 0xfffff, 0xc09a
        ;32-bit code segment
    descriptor 0, 0xfffff, 0xc092
        ;32-bit data segment
    descriptor 0x7800 + OFFSET, 0xfffff, 0x009a
        ;16-bit code segment
gdtr:
    dw $ - gdt - 1
    dd gdt + 0x7800 + OFFSET

;Switch back to Real-Mode
back:
    mov eax, cr0
    and eax, ~1
    mov cr0, eax
    jmp ((0x7800 + OFFSET) >> 4) : ._1
._1:
    mov ax, cs
    mov ds, ax
    mov es, ax
    ret         ; >>> push ._ret
;-------------------------------------------------

bits 32

;Test A20, loop forever if it is not enabled
;NOTE:
;   ss, sp is unchanged
check_A20:
    mov ax, 16
    mov ds, ax

    xor eax, eax
._1:
    inc eax
    mov [0], eax
    cmp eax, [0x100000]
    je ._1

    jmp 24 : back

;NOTE:
;   1. ss, sp is unchanged
;   2. [mv_to] will be updated
for_move_bytes:
    mov ax, 16
    mov ds, ax
    mov es, ax

    mov esi, [0x7800 + OFFSET + mv_from]
    mov ecx, [0x7800 + OFFSET + mv_ecx]
    mov edi, [0x7800 + OFFSET + mv_to]
    rep movsb

    mov eax, [0x7800 + OFFSET + mv_ecx]
    add [0x7800 + OFFSET + mv_to], eax 
    jmp 24 : back

for_jump_to_kernel:
    mov cx, 16
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    mov ss, cx
    mov esp, 0x7800 + OFFSET

    mov edi, [0x7800 + OFFSET + MB_load_end_addr]
    mov ecx, [0x7800 + OFFSET + MB_bss_end_addr]
    sub ecx, edi
    xor al, al
    rep stosb

    mov eax, MB_MAGIC_EAX    
    ;physical address of the Multiboot_Information_Structure
    mov ebx, 0x7800 + OFFSET + flag 

    mov ecx, [0x7800 + OFFSET + MB_entry_addr]
    jmp ecx

magic dw 0xdead
