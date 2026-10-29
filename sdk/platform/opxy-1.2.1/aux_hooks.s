/* Aux track hooks in the aux controller (aux_tracks.cpp). Both replace eight
 * bytes with an absolute CALL into a stub here; neither site has live
 * caller-saved registers that the C++ they call could clobber, and no stock
 * branch lands inside either site's eight bytes.
 */
.section .text
.align 2

/* 8c08f6ec: Shift + M1 in the aux controller's shifted-key method (method
 * table entry +0x10, 8c08f6b0), reached through its jump table for key code 6:
 * `UNLINK; R0 = 1; RTS`, which ignores the key and reports it handled. The
 * stub calls sdk_aux_shift_m1(), then leaves the method as those eight bytes
 * did.
 */
.extern sdk_aux_shift_m1
.global sdk_aux_shift_m1_hook
.type sdk_aux_shift_m1_hook, STT_FUNC
sdk_aux_shift_m1_hook:
    LINK 0;
    SP += -12;
    CALL sdk_aux_shift_m1;
    SP += 12;
    UNLINK;
    UNLINK;
    R0 = 1;
    RTS;
.size sdk_aux_shift_m1_hook, .-sdk_aux_shift_m1_hook

/* 8c08f540: the aux page method 8c08f53c, just after its LINK 0xc, loads
 * P1 = 840a0534 (the selected track) over eight bytes. sdk_aux_page() answers
 * the page a machine shows there, or -1: then the stub makes the replaced load
 * and returns into stock; otherwise it leaves the method with that page, as
 * the method's own UNLINK; RTS would. R0-R3 and P0-P2 are dead at the site.
 */
.extern sdk_aux_page
.global sdk_aux_page_hook
.type sdk_aux_page_hook, STT_FUNC
sdk_aux_page_hook:
    LINK 0;
    SP += -12;
    CALL sdk_aux_page;
    SP += 12;
    UNLINK;
    CC = R0 < 0;
    IF CC JUMP 1f;
    UNLINK;
    RTS;
1:
    P1.L = 0x0534;
    P1.H = 0x840a;
    RTS;
.size sdk_aux_page_hook, .-sdk_aux_page_hook
