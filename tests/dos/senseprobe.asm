; senseprobe.asm -- does an emulated VFX1 answer like Forte's driver?
;
; A 16-bit COM program that makes the calls native VFX1 titles make, in the
; way Forte's SDK examples make them, and prints what came back:
;
;   MASK=  the function prefix found by VFX1REGS.C's SenseStatus scan
;   CFG=   bytes returned by GET_CONFIGURATION, then TYPES= the packet types
;   REP=   bytes returned by REPORT
;   YAW=, PITCH=, ROLL=  min and max head tracker angles from GET_DEVICE_DATA
;          over about twenty seconds, so a moving head shows a range
;   PUCKP=, PUCKR=  min and max CyberPuck pitch and roll over the same time
;   BTN=   every CyberPuck button bit seen, ORed together
;   PSR=   the VIP page select register read back after writing 23h
;
; Written from scratch so testing the device needs no Forte or game binary.
;
;   nasm -f bin -o SENSE.COM tests/dos/senseprobe.asm

BITS 16
ORG 0x100

VIP_BASE        equ 0x260
TICKS_TO_SAMPLE equ 364         ; about twenty seconds, time to enter VR and try the puck

start:
        ; ---- SenseStatus: AX = (i << 8) | 7Fh answers (7Fh << 8) | i ----
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
        mov     dx, msg_none
        call    print_str
        mov     ax, 0x4C01
        int     0x21
.found:
        mov     ax, si
        mov     [mask], al

        mov     dx, msg_mask
        call    print_str
        xor     ah, ah
        mov     al, [mask]
        call    print_hex8

        ; ---- GET_CONFIGURATION into cfgbuf, walk the packet types ----
        mov     ah, [mask]
        mov     al, 0x03
        xor     cx, cx
        push    ds
        pop     es
        mov     dx, cfgbuf
        int     0x33
        mov     [cfgcount], cx
        mov     dx, msg_cfg
        call    print_str
        mov     ax, [cfgcount]
        call    print_signed

        mov     dx, msg_types
        call    print_str
        xor     bx, bx
.walk:
        cmp     bx, [cfgcount]
        jae     .walked
        mov     al, [cfgbuf + bx + 1]
        push    ax
        add     al, '0'
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        pop     ax
        test    al, al          ; NULL packet ends the list
        jz      .walked
        xor     ah, ah
        mov     al, [cfgbuf + bx]
        test    al, al
        jz      .walked
        add     bx, ax
        jmp     .walk
.walked:

        ; ---- REPORT ----
        mov     ah, [mask]
        mov     al, 0x05
        xor     cx, cx
        push    ds
        pop     es
        mov     dx, repbuf
        int     0x33
        mov     [repcount], cx
        mov     dx, msg_rep
        call    print_str
        mov     ax, [repcount]
        call    print_signed

        ; ---- sample GET_DEVICE_DATA for the head tracker ----
        push    0x0040
        pop     es
        mov     ax, [es:0x6C]
        add     ax, TICKS_TO_SAMPLE
        mov     [deadline], ax
.sample:
        mov     ah, [mask]
        mov     al, 0x70
        mov     bx, 0x0301      ; 1 | (DCHTD << 8)
        int     0x33
        test    ah, ah
        jz      .ok
        inc     word [errors]
        jmp     .next
.ok:
        mov     di, yaw_min
        mov     ax, bx
        call    minmax
        mov     di, pitch_min
        mov     ax, cx
        call    minmax
        mov     di, roll_min
        mov     ax, dx
        call    minmax
.next:
        mov     ah, [mask]
        mov     al, 0x70
        mov     bx, 0x0401      ; 1 | (DCHPD << 8)
        int     0x33
        test    ah, ah
        jnz     .puck_done
        or      [buttons], al
        mov     di, puckp_min
        mov     ax, cx
        call    minmax
        mov     di, puckr_min
        mov     ax, dx
        call    minmax
.puck_done:
        mov     ax, [es:0x6C]
        cmp     ax, [deadline]
        jb      .sample

        mov     dx, msg_yaw
        mov     si, yaw_min
        call    print_range
        mov     dx, msg_pitch
        mov     si, pitch_min
        call    print_range
        mov     dx, msg_roll
        mov     si, roll_min
        call    print_range
        mov     dx, msg_puckp
        mov     si, puckp_min
        call    print_range
        mov     dx, msg_puckr
        mov     si, puckr_min
        call    print_range
        mov     dx, msg_btn
        call    print_str
        xor     ah, ah
        mov     al, [buttons]
        call    print_signed
        mov     dx, msg_err
        call    print_str
        mov     ax, [errors]
        call    print_signed

        ; ---- VIP page select register ----
        mov     dx, VIP_BASE
        mov     al, 4           ; PSR
        out     dx, al
        inc     dx
        mov     al, 0x23
        out     dx, al
        in      al, dx
        push    ax
        mov     dx, msg_psr
        call    print_str
        pop     ax
        call    print_hex8

        mov     dx, msg_crlf
        call    print_str
        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------
minmax:                         ; AX signed sample; DI -> min, max words
        cmp     ax, [di]
        jge     .notmin
        mov     [di], ax
.notmin:
        cmp     ax, [di + 2]
        jle     .notmax
        mov     [di + 2], ax
.notmax:
        ret

print_range:                    ; DX -> label, SI -> min, max
        call    print_str
        mov     ax, [si]
        call    print_signed
        mov     dl, '.'
        mov     ah, 0x02
        int     0x21
        int     0x21
        mov     ax, [si + 2]
        call    print_signed
        ret

print_str:                      ; DX -> $-terminated string
        push    ax
        mov     ah, 0x09
        int     0x21
        pop     ax
        ret

print_hex8:                     ; AL as two hex digits
        push    ax
        shr     al, 4
        call    .digit
        pop     ax
        and     al, 0x0F
.digit:
        push    ax
        add     al, '0'
        cmp     al, '9'
        jbe     .emit
        add     al, 7
.emit:
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        pop     ax
        ret

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
        div     bx
        push    dx
        inc     cx
        test    ax, ax
        jnz     .divide
.out:
        pop     dx
        add     dl, '0'
        push    cx
        mov     ah, 0x02
        int     0x21
        pop     cx
        loop    .out
        pop     dx
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------
mask            db 0
cfgcount        dw 0
repcount        dw 0
deadline        dw 0
errors          dw 0
yaw_min         dw 32767
yaw_max         dw -32768
pitch_min       dw 32767
pitch_max       dw -32768
roll_min        dw 32767
roll_max        dw -32768
puckp_min       dw 32767
puckp_max       dw -32768
puckr_min       dw 32767
puckr_max       dw -32768
buttons         db 0
cfgbuf          times 256 db 0
repbuf          times 64 db 0

msg_none        db 'No VFX1 Sense driver found.', 13, 10, '$'
msg_mask        db 'MASK=$'
msg_cfg         db ' CFG=$'
msg_types       db ' TYPES=$'
msg_rep         db ' REP=$'
msg_yaw         db ' YAW=$'
msg_pitch       db ' PITCH=$'
msg_roll        db ' ROLL=$'
msg_puckp       db ' PUCKP=$'
msg_puckr       db ' PUCKR=$'
msg_btn         db ' BTN=$'
msg_err         db ' ERR=$'
msg_psr         db ' PSR=$'
msg_crlf        db 13, 10, '$'
