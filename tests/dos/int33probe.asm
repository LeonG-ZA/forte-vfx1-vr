; int33probe.asm -- does head tracking actually reach a DOS program?
;
; A 16-bit COM program that initialises the INT 33h mouse, polls the motion
; counters (function 0Bh) for about six seconds, and prints what it saw. If the
; VFX1 head-tracking path works, a program that never touched VFX1 hardware
; sees motion here with no mouse plugged in and no cursor moving.
;
; It reports two numbers per axis. NET is the signed sum, which says which way
; the view ended up. ABS is the total distance travelled, which is the honest
; measure of "did anything move": a head sweeping back and forth on a sine nets
; near zero however far it actually went.
;
; This is the end-to-end test for the INT 33h mouse path. It is written from
; scratch precisely so that proving the path needs no copyrighted game binary.
;
;   nasm -f bin -o INT33.COM tests/dos/int33probe.asm
;
; Driven by tests/dos/run-dosbox-x.sh.

BITS 16
ORG 0x100

TICKS_TO_SAMPLE equ 109         ; BIOS ticks at 18.2/s, so about six seconds

start:
        mov     ax, 0x0000      ; INT 33h 00h: reset driver and detect
        int     0x33
        cmp     ax, 0xFFFF      ; FFFFh means a driver answered
        je      have_mouse

        mov     dx, msg_nomouse
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C01
        int     0x21

have_mouse:
        mov     dx, msg_running
        mov     ah, 0x09
        int     0x21

        push    0x0040          ; BIOS data area, for the tick counter
        pop     es
        mov     ax, [es:0x6C]
        add     ax, TICKS_TO_SAMPLE
        mov     [deadline], ax

poll_loop:
        mov     ax, 0x000B      ; INT 33h 0Bh: read motion counters
        int     0x33            ; CX = horizontal mickeys, DX = vertical
        mov     bx, dx          ; stash vertical; DX is needed for division

        add     [net_x], cx
        add     [net_y], bx

        mov     ax, cx
        or      ax, bx
        jz      no_motion
        inc     word [polls]
no_motion:
        mov     ax, cx          ; absolute horizontal travel
        test    ax, ax
        jns     abs_x_ok
        neg     ax
abs_x_ok:
        add     [abs_x], ax

        mov     ax, bx          ; absolute vertical travel
        test    ax, ax
        jns     abs_y_ok
        neg     ax
abs_y_ok:
        add     [abs_y], ax

        mov     ax, [es:0x6C]
        cmp     ax, [deadline]
        jb      poll_loop

        mov     dx, msg_netx
        call    print_str
        mov     ax, [net_x]
        call    print_signed

        mov     dx, msg_nety
        call    print_str
        mov     ax, [net_y]
        call    print_signed

        mov     dx, msg_absx
        call    print_str
        mov     ax, [abs_x]
        call    print_signed

        mov     dx, msg_absy
        call    print_str
        mov     ax, [abs_y]
        call    print_signed

        mov     dx, msg_polls
        call    print_str
        mov     ax, [polls]
        call    print_signed

        mov     dx, msg_crlf
        call    print_str

        ; Exit code says whether anything moved, so a script can check without
        ; parsing the text.
        mov     ax, [abs_x]
        or      ax, [abs_y]
        jz      exit_nomotion
        mov     ax, 0x4C00
        int     0x21
exit_nomotion:
        mov     ax, 0x4C02
        int     0x21

; ---------------------------------------------------------------
print_str:                      ; DX -> $-terminated string
        push    ax
        mov     ah, 0x09
        int     0x21
        pop     ax
        ret

; ---------------------------------------------------------------
print_signed:                   ; AX as signed decimal
        push    bx
        push    cx
        push    dx

        test    ax, ax
        jns     .positive
        push    ax
        mov     dl, '-'
        mov     ah, 0x02
        int     0x21
        pop     ax
        neg     ax
.positive:
        xor     cx, cx
        mov     bx, 10
.divide:
        xor     dx, dx
        div     bx              ; DX:AX / 10, remainder in DX
        push    dx
        inc     cx
        test    ax, ax
        jnz     .divide
.emit:
        pop     dx
        add     dl, '0'
        push    cx
        mov     ah, 0x02
        int     0x21
        pop     cx
        loop    .emit

        pop     dx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------
deadline        dw 0
net_x           dw 0
net_y           dw 0
abs_x           dw 0
abs_y           dw 0
polls           dw 0

msg_nomouse     db 'No INT 33h driver present.', 13, 10, '$'
msg_running     db 'Polling INT 33h motion counters for ~6s...', 13, 10, '$'
msg_netx        db 'NETX=$'
msg_nety        db ' NETY=$'
msg_absx        db ' ABSX=$'
msg_absy        db ' ABSY=$'
msg_polls       db ' POLLS=$'
msg_crlf        db 13, 10, '$'
