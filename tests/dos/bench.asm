; bench.asm -- how fast is this emulator, in terms a DOS game would feel?
;
; Two measurements, each over about five seconds of BIOS ticks:
;
;   CPU   iterations of an integer, multiply and memory-write loop, which is
;         the shape of the fixed-point inner loops in games like Descent.
;   FILL  full-screen mode 13h frames written to VGA memory, which is what a
;         software renderer does every frame, and where the emulator's video
;         path costs the most.
;
; Output is one line, raw counts plus the tick span, so the caller computes
; rates and nothing depends on this program doing arithmetic it cannot check:
;
;   CPU=<iterations> CPUTICKS=<ticks> FILL=<frames> FILLTICKS=<ticks>
;
; Written from scratch, like int33probe.asm, so benchmarking needs no
; copyrighted software. The results are only meaningful relative to each other:
; the same program under native DOSBox-X at known cycle settings is what gives
; the WebAssembly number a scale. See tests/dos/run-bench.sh.
;
;   nasm -f bin -o BENCH.COM tests/dos/bench.asm

BITS 16
ORG 0x100

SAMPLE_TICKS equ 55             ; 18.2 ticks/s, so three emulated seconds
; The VGA test's length in ticks. An emulator whose clock falls behind real
; time under VGA load (v86) makes 55 ticks last minutes, so its build uses
; fewer:  nasm -f bin -DFILL_TICKS=5 -o BENCHV.COM tests/dos/bench.asm
%ifndef FILL_TICKS
FILL_TICKS equ SAMPLE_TICKS
%endif

; Marker files. Each is created at a boundary of a measurement, so a host that
; can watch the drive (the browser page does) can time the tests against the
; real clock. Emulated time alone cannot reveal an emulator that runs slower
; than real time, and that is the failure a browser build is most prone to.

start:
        ; ------------------------------------------------ CPU
        call    wait_tick_edge          ; start on a tick boundary
        mov     dx, mrk_t0
        call    mark
        call    read_ticks
        mov     [t0], ax
        add     ax, SAMPLE_TICKS
        mov     [deadline], ax

cpu_loop:
        mov     cx, 64
        mov     bx, [seed]
        mov     di, buffer
.inner:
        mov     ax, bx
        mov     dx, 0x6255
        mul     dx                      ; 16x16 multiply
        add     ax, 0x3619
        mov     bx, ax
        xor     al, ah
        mov     [di], al                ; memory write
        inc     di
        loop    .inner
        mov     [seed], bx

        add     word [cpu_lo], 1
        adc     word [cpu_hi], 0

        call    read_ticks
        cmp     ax, [deadline]
        jb      cpu_loop
        sub     ax, [t0]
        mov     [cpu_ticks], ax
        mov     dx, mrk_t1
        call    mark

        ; ------------------------------------------------ FILL
        mov     ax, 0x0013              ; 320x200, 256 colours
        int     0x10

        call    wait_tick_edge
        mov     dx, mrk_t2
        call    mark
        call    read_ticks
        mov     [t0], ax
        add     ax, FILL_TICKS
        mov     [deadline], ax

        mov     ax, 0xA000
        mov     es, ax
        xor     dl, dl                  ; colour
fill_loop:
        xor     di, di
        mov     al, dl
        mov     cx, 64000
        rep     stosb
        inc     dl

        add     word [fill_lo], 1
        adc     word [fill_hi], 0

        call    read_ticks
        cmp     ax, [deadline]
        jb      fill_loop
        sub     ax, [t0]
        mov     [fill_ticks], ax
        push    ds
        pop     es                      ; ES was the VGA segment
        mov     dx, mrk_t3
        call    mark

        mov     ax, 0x0003              ; back to text mode before printing
        int     0x10

        ; ------------------------------------------------ report
        mov     dx, msg_cpu
        call    print_str
        mov     ax, [cpu_lo]
        mov     dx, [cpu_hi]
        call    print_u32

        mov     dx, msg_cputicks
        call    print_str
        mov     ax, [cpu_ticks]
        xor     dx, dx
        call    print_u32

        mov     dx, msg_fill
        call    print_str
        mov     ax, [fill_lo]
        mov     dx, [fill_hi]
        call    print_u32

        mov     dx, msg_filltick
        call    print_str
        mov     ax, [fill_ticks]
        xor     dx, dx
        call    print_u32

        mov     dx, msg_crlf
        call    print_str

        mov     ax, 0x4C00
        int     0x21

; ---------------------------------------------------------------
mark:                           ; DS:DX -> ASCIIZ name; create it, then close it
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     si, dx
        mov     ah, 0x3C
        xor     cx, cx
        int     0x21
        jc      .done                   ; a failed marker must not stop the test
        mov     bx, ax
        mov     ah, 0x3E
        int     0x21
.done:
        ; Also "T0".."T3" and a newline straight to the COM1 UART, for hosts that
        ; watch a serial port rather than the drive (web/bench-v86.html). The
        ; writes sit outside both timed loops.
        mov     dx, 0x3F8
        mov     al, 'T'
        out     dx, al
        mov     al, [si + 1]
        out     dx, al
        mov     al, 10
        out     dx, al
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

read_ticks:                     ; AX = low word of the BIOS tick count
        push    ds
        push    bx
        mov     bx, 0x0040
        mov     ds, bx
        mov     ax, [0x006C]
        pop     bx
        pop     ds
        ret

wait_tick_edge:
        call    read_ticks
        mov     [edge], ax
.wait:
        call    read_ticks
        cmp     ax, [edge]
        je      .wait
        ret

print_str:                      ; DX -> $-terminated string
        push    ax
        mov     ah, 0x09
        int     0x21
        pop     ax
        ret

; print_u32 -- DX:AX as an unsigned decimal number.
print_u32:
        push    bx
        push    cx
        push    si
        push    di
        mov     si, ax                  ; SI = low word
        mov     di, dx                  ; DI = high word
        xor     cx, cx                  ; digit count
        mov     bx, 10
.divide:
        ; (DI:SI) / 10, remainder out, one word at a time
        mov     ax, di
        xor     dx, dx
        div     bx
        mov     di, ax                  ; new high word
        mov     ax, si
        div     bx                      ; DX holds the high remainder going in
        mov     si, ax                  ; new low word
        push    dx                      ; the digit
        inc     cx
        mov     ax, si
        or      ax, di
        jnz     .divide
.emit:
        pop     dx
        add     dl, '0'
        push    cx
        mov     ah, 0x02
        int     0x21
        pop     cx
        loop    .emit
        pop     di
        pop     si
        pop     cx
        pop     bx
        ret

; ---------------------------------------------------------------
t0          dw 0
deadline    dw 0
edge        dw 0
seed        dw 0x1234
cpu_lo      dw 0
cpu_hi      dw 0
cpu_ticks   dw 0
fill_lo     dw 0
fill_hi     dw 0
fill_ticks  dw 0

mrk_t0       db 'T0.MRK', 0
mrk_t1       db 'T1.MRK', 0
mrk_t2       db 'T2.MRK', 0
mrk_t3       db 'T3.MRK', 0

msg_cpu      db 'CPU=$'
msg_cputicks db ' CPUTICKS=$'
msg_fill     db ' FILL=$'
msg_filltick db ' FILLTICKS=$'
msg_crlf     db 13, 10, '$'

buffer      times 64 db 0
