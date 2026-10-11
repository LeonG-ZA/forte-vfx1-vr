; vfx1qk.asm -- VFX1 head tracking for DOS Quake, through Quake's external
; control interface.
;
; Quake 1.08 for DOS takes "-control <address>": the real-mode linear address
; of a block a resident driver fills in. Every frame Quake writes its view
; angles and movement there, calls the interrupt the block names, and reads
; them back (WinQuake/in_dos.c, IN_ExternalMove, in id's released source).
; Forte shipped such a driver for the VFX1 (VFX1QKE.EXE); this is a
; replacement written from that source and Forte's VFX1 SDK alone.
;
;   VFX1QK [program [arguments]]        default program QUAKE.EXE
;
; It finds the VFX1 driver (VFX1REGS.C's SenseStatus scan), sets the headset
; to line-sequential video, the mode the VIP card takes interlaced stereo in
; (Quake's lcd_x), hooks a free interrupt, and runs the program with
; "-control <address>" added. On each call it reads the head tracker (GET_
; DEVICE_DATA, class 3) and adds the change in head angle since the last
; call to Quake's view, so mouse and keyboard turning still work. When the
; program ends it puts everything back.
;
; The control block, from in_dos.c:
;
;   +0   long   interruptVector
;   +4   char   deviceName[16]
;   +20  long   numAxes
;   +24  long   numButtons
;   +28  long   flags                AUX_FLAG_FREELOOK = 1
;   +32  float  viewangles[3]        pitch (down positive), yaw (left positive), roll
;   +44  float  forwardmove, sidemove, upmove
;   +56  long   buttons
;
;   nasm -f bin -o VFX1QK.COM dos/vfx1qk.asm

BITS 16
CPU 386
ORG 0x100

STACK_BYTES     equ 512

start:
        mov     sp, stack_top

        ; ---- the VFX1 driver: AX = (i << 8) | 7Fh answers (7Fh << 8) | i ----
        mov     si, 0x60
.scan:
        mov     ax, si
        mov     ah, al
        mov     al, 0x7F
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        int     0x33
        mov     bx, si
        mov     bh, 0x7F
        cmp     ax, bx
        je      .found
        inc     si
        cmp     si, 0x70
        jbe     .scan
        mov     dx, msg_nodriver
        call    print
        mov     ax, 0x4C01
        int     0x21
.found:
        mov     ax, si
        mov     [prefix], al

        ; ---- a free interrupt: the first of 60h-66h with no handler ----
        mov     byte [vector], 0x65
        mov     cl, 0x60
.free:
        mov     ah, 0x35
        mov     al, cl
        int     0x21                        ; ES:BX = handler
        mov     ax, es
        or      ax, bx
        jz      .chosen
        inc     cl
        cmp     cl, 0x66
        jbe     .free
        jmp     .hook
.chosen:
        mov     [vector], cl
.hook:
        mov     ah, 0x35
        mov     al, [vector]
        int     0x21
        mov     [old_vector], bx
        mov     [old_vector + 2], es
        push    ds
        pop     es
        mov     ah, 0x25
        mov     al, [vector]
        mov     dx, handler
        int     0x21

        ; ---- the control block ----
        movzx   eax, byte [vector]
        mov     [ctrl + 0], eax
        mov     dword [ctrl + 20], 3        ; yaw, pitch, roll
        mov     dword [ctrl + 24], 0
        mov     dword [ctrl + 28], 1        ; AUX_FLAG_FREELOOK: no pitch drift

        ; ---- line-sequential video: SET_DEVICE_DATA, the headset, mode 2 ----
        mov     ah, [prefix]
        mov     al, 0x71
        mov     bx, 0x0201
        mov     cx, 2
        int     0x33

        ; ---- the command: program name and arguments from ours ----
        mov     si, 0x81
        movzx   cx, byte [0x80]
        add     cx, si                      ; end of our command tail
.skip:
        cmp     si, cx
        jae     .default
        cmp     byte [si], ' '
        jne     .name
        inc     si
        jmp     .skip
.default:
        mov     si, default_program
        mov     di, program
.copy_default:
        lodsb
        stosb
        test    al, al
        jnz     .copy_default
        mov     si, cx                      ; no arguments
        jmp     .tail
.name:
        mov     di, program
.copy_name:
        cmp     si, cx
        jae     .name_done
        mov     al, [si]
        cmp     al, ' '
        je      .name_done
        cmp     al, 0x0D
        je      .name_done
        stosb
        inc     si
        jmp     .copy_name
.name_done:
        mov     byte [di], 0
.tail:
        ; arguments as given, then " -control <address>"
        mov     di, tail + 1
.copy_args:
        cmp     si, cx
        jae     .args_done
        mov     al, [si]
        cmp     al, 0x0D
        je      .args_done
        stosb
        inc     si
        jmp     .copy_args
.args_done:
        push    di
        mov     si, control_arg
.copy_control:
        lodsb
        test    al, al
        jz      .control_done
        stosb
        jmp     .copy_control
.control_done:
        ; the block's linear address in decimal
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        add     eax, ctrl
        call    put_decimal
        mov     byte [di], 0x0D
        mov     ax, di
        sub     ax, tail + 1
        mov     [tail], al
        pop     di

        mov     dx, msg_hello
        call    print
        mov     al, [vector]
        call    print_hex8
        mov     dx, msg_running
        call    print
        mov     dx, program
        call    print_z
        mov     dx, msg_crlf
        call    print

        ; ---- run it: give back the memory we do not need first ----
        mov     ah, 0x4A
        mov     bx, (program_end - $$ + 0x100 + 15) / 16
        push    cs
        pop     es
        int     0x21
        mov     word [exec_block + 0], 0    ; our environment
        mov     word [exec_block + 2], tail
        mov     [exec_block + 4], cs
        mov     word [exec_block + 6], 0x5C
        mov     [exec_block + 8], cs
        mov     word [exec_block + 10], 0x6C
        mov     [exec_block + 12], cs
        mov     [save_sp], sp
        mov     ax, 0x4B00
        mov     dx, program
        mov     bx, exec_block
        push    cs
        pop     es
        int     0x21
        cli
        mov     ax, cs
        mov     ss, ax
        mov     sp, [save_sp]
        sti
        push    cs
        pop     ds
        mov     byte [exit_code], 1
        jnc     .ran
        mov     dx, msg_noexec
        call    print
        jmp     .restore
.ran:
        mov     ah, 0x4D
        int     0x21
        mov     [exit_code], al

.restore:
        mov     ah, [prefix]                ; the headset back to mono
        mov     al, 0x71
        mov     bx, 0x0201
        xor     cx, cx
        int     0x33
        push    ds
        lds     dx, [old_vector]
        mov     ah, 0x25
        mov     al, [cs:vector]
        int     0x21
        pop     ds
        mov     ah, 0x4C
        mov     al, [exit_code]
        int     0x21

; ---- Quake calls this once a frame, in real mode ----
handler:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        mov     ah, [prefix]
        mov     al, 0x70                    ; GET_DEVICE_DATA
        mov     bx, 0x0301                  ; the head tracker
        int     0x33                        ; BX yaw (left +), CX pitch (up +), DX roll (right +)
        cmp     byte [primed], 0
        jne     .move
        mov     [last_yaw], bx              ; the first call: nothing to add
        mov     [last_pitch], cx
        mov     [last_roll], dx
        mov     byte [primed], 1
        jmp     .done
.move:
        mov     ax, bx
        sub     ax, [last_yaw]              ; 16-bit, so a turn past 180 degrees wraps
        mov     [last_yaw], bx
        mov     [delta + 0], ax
        mov     ax, [last_pitch]
        sub     ax, cx                      ; Quake's pitch is down positive
        mov     [last_pitch], cx
        mov     [delta + 2], ax
        mov     ax, dx
        sub     ax, [last_roll]
        mov     [last_roll], dx
        mov     [delta + 4], ax
        fild    word [delta + 2]            ; pitch
        fmul    dword [units_to_degrees]
        fadd    dword [ctrl + 32]
        fstp    dword [ctrl + 32]
        fild    word [delta + 0]            ; yaw
        fmul    dword [units_to_degrees]
        fadd    dword [ctrl + 36]
        fstp    dword [ctrl + 36]
        fild    word [delta + 4]            ; roll
        fmul    dword [units_to_degrees]
        fadd    dword [ctrl + 40]
        fstp    dword [ctrl + 40]
.done:
        pop     es
        pop     ds
        popa
        iret

; ---- helpers ----

; EAX as decimal at DI
put_decimal:
        push    bx
        push    cx
        push    edx
        mov     ebx, 10
        xor     cx, cx
.div:
        xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        test    eax, eax
        jnz     .div
.out:
        pop     ax
        add     al, '0'
        stosb
        loop    .out
        pop     edx
        pop     cx
        pop     bx
        ret

; '$'-terminated string at DX
print:
        mov     ah, 0x09
        int     0x21
        ret

; zero-terminated string at DX
print_z:
        push    si
        mov     si, dx
.next:
        lodsb
        test    al, al
        jz      .end
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     .next
.end:
        pop     si
        ret

; AL as two hex digits
print_hex8:
        push    ax
        shr     al, 4
        call    .digit
        pop     ax
        and     al, 0x0F
.digit:
        add     al, '0'
        cmp     al, '9'
        jbe     .put
        add     al, 7
.put:
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        ret

; ---- data ----
msg_nodriver    db 'VFX1QK: no VFX1 driver found (load VFX1.COM first).', 13, 10, '$'
msg_hello       db 'VFX1QK: VFX1 head tracking for Quake on INT $'
msg_running     db 'h, running $'
msg_noexec      db 'VFX1QK: could not run the program.', 13, 10, '$'
msg_crlf        db 13, 10, '$'
default_program db 'QUAKE.EXE', 0
control_arg     db ' -control ', 0
units_to_degrees dd 0.0054931640625         ; 360 / 65536: the VFX1's angle units

align 4
ctrl:
        dd      0                           ; interruptVector
        db      'VFX1 head', 0, 0, 0, 0, 0, 0, 0
        dd      0, 0, 0                     ; numAxes, numButtons, flags
        dd      0, 0, 0                     ; viewangles
        dd      0, 0, 0                     ; forwardmove, sidemove, upmove
        dd      0                           ; buttons

prefix          db 0
vector          db 0
primed          db 0
exit_code       db 0
old_vector      dd 0
last_yaw        dw 0
last_pitch      dw 0
last_roll       dw 0
delta           dw 0, 0, 0
save_sp         dw 0
exec_block      times 14 db 0
program         times 80 db 0
tail            times 130 db 0

align 2
                times STACK_BYTES db 0
stack_top:
program_end:
