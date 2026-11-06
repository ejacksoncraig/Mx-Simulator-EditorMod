; Brush shapes (docs/editor-mod-ingame-tools.md, "Brush shapes"). While a shape other than Circle is chosen, the game's
; brush loop jumps here from 0x140060035, with the offset from a cell to the nearest point of the stroke in xmm2 (x) and
; xmm0 (z). This does the integer work the patch covered, asks mx_brush_distance for the distance, and goes back to
; 0x14006005a with it in xmm0, every other register as the game left it (xmm2 squared, as the original leaves it).
EXTERN mx_brush_distance:PROC
EXTERN mx_brush_return:QWORD

.code
mx_brush_stub PROC
    ; what the patched bytes did besides the distance
    cmp     r15d, ebx
    mov     esi, ebx
    cmovle  esi, r15d
    add     esi, dword ptr [rsp+48h]
    ; the game's rsp is 16-aligned here: 0D0h keeps it so, with 20h of shadow space for the call
    lea     rsp, [rsp-0D0h]
    mov     [rsp+20h], rax
    mov     [rsp+28h], rcx
    mov     [rsp+30h], rdx
    mov     [rsp+38h], r8
    mov     [rsp+40h], r9
    mov     [rsp+48h], r10
    mov     [rsp+50h], r11
    movups  [rsp+60h], xmm1
    movups  [rsp+70h], xmm2
    movups  [rsp+80h], xmm3
    movups  [rsp+90h], xmm4
    movups  [rsp+0A0h], xmm5
    ; mx_brush_distance(dz, dx, outer, inner): outer and inner are the brush function's 5th and 6th arguments
    movaps  xmm1, xmm2
    movss   xmm2, dword ptr [rsp+0D0h+1210h]
    movss   xmm3, dword ptr [rsp+0D0h+1218h]
    call    mx_brush_distance
    mov     rax, [rsp+20h]
    mov     rcx, [rsp+28h]
    mov     rdx, [rsp+30h]
    mov     r8, [rsp+38h]
    mov     r9, [rsp+40h]
    mov     r10, [rsp+48h]
    mov     r11, [rsp+50h]
    movups  xmm1, [rsp+60h]
    movups  xmm2, [rsp+70h]
    movups  xmm3, [rsp+80h]
    movups  xmm4, [rsp+90h]
    movups  xmm5, [rsp+0A0h]
    lea     rsp, [rsp+0D0h]
    mulss   xmm2, xmm2
    jmp     qword ptr [mx_brush_return]
mx_brush_stub ENDP
END
