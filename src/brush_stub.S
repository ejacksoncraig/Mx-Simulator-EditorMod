# The same stub as brush_stub.asm, for mingw-w64 (build.sh). See that file for what it does.
    .intel_syntax noprefix
    .text
    .globl mx_brush_stub
mx_brush_stub:
    cmp     r15d, ebx
    mov     esi, ebx
    cmovle  esi, r15d
    add     esi, dword ptr [rsp+0x48]
    lea     rsp, [rsp-0xD0]
    mov     [rsp+0x20], rax
    mov     [rsp+0x28], rcx
    mov     [rsp+0x30], rdx
    mov     [rsp+0x38], r8
    mov     [rsp+0x40], r9
    mov     [rsp+0x48], r10
    mov     [rsp+0x50], r11
    movups  [rsp+0x60], xmm1
    movups  [rsp+0x70], xmm2
    movups  [rsp+0x80], xmm3
    movups  [rsp+0x90], xmm4
    movups  [rsp+0xA0], xmm5
    movaps  xmm1, xmm2
    movss   xmm2, dword ptr [rsp+0xD0+0x1210]
    movss   xmm3, dword ptr [rsp+0xD0+0x1218]
    call    mx_brush_distance
    mov     rax, [rsp+0x20]
    mov     rcx, [rsp+0x28]
    mov     rdx, [rsp+0x30]
    mov     r8, [rsp+0x38]
    mov     r9, [rsp+0x40]
    mov     r10, [rsp+0x48]
    mov     r11, [rsp+0x50]
    movups  xmm1, [rsp+0x60]
    movups  xmm2, [rsp+0x70]
    movups  xmm3, [rsp+0x80]
    movups  xmm4, [rsp+0x90]
    movups  xmm5, [rsp+0xA0]
    lea     rsp, [rsp+0xD0]
    mulss   xmm2, xmm2
    jmp     qword ptr [rip+mx_brush_return]
