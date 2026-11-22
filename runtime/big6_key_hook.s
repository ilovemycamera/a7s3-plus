/*
 * big6_key_hook.s — Minimal libObj.so input-node (0x0a) callback trampoline.
 *
 * Maps any physical camera button assigned to "Home" (NVRAM 0x0b70 -> intId 0x574 ->
 * RTOS key code 0x114) to the native A7S III HOME key code 0x187, opening the
 * Cinema Line BIG6 Home Screen immediately on button press.
 */
.global big6_cb_start
.global big6_cb_end
.text
big6_cb_start:
1:
    stp x29, x30, [sp, -32]!
    mov x29, sp
    stp x19, x28, [sp, 16]
    adr x28, 1b
    cbz x1, cb_forward
    ldrb w9, [x1, 8]          /* press state: 1=ON, 0=OFF */
    ldr w10, [x1]             /* RTOS key code */
    and w12, w10, 0xfff
    cmp w12, 0x114            /* Home / BIG6 custom-key RTOS code */
    b.ne cb_forward
    cmp w9, 1                 /* transform press-ON to 0x187; suppress release */
    b.ne cb_exit
    mov w10, 0x187
    str w10, [x1]
cb_forward:
    ldr x9, [x28, 0x718]      /* original callback pointer */
    blr x9
cb_exit:
    ldp x19, x28, [sp, 16]
    ldp x29, x30, [sp], 32
    ret
big6_cb_end:
