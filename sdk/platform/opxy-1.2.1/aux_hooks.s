/* Aux track hooks in the aux controller (aux_tracks.cpp). Both replace eight
 * bytes with an absolute CALL into a stub here; neither site has live
 * caller-saved registers that the C++ they call could clobber.
 */
.section .text
.align 2

/* 8c08fd5a: M1 in the aux controller's key method, `R1 = 0; CALL 8c08f74c;
 * SP += 12`, with the controller in R0. The stub calls sdk_aux_m1(controller,
 * 0), which selects tab 0 as stock does or opens the chooser, then makes the
 * replaced SP += 12 before returning to the method's epilogue at 8c08fd62.
 */
.extern sdk_aux_m1
.global sdk_aux_m1_hook
.type sdk_aux_m1_hook, STT_FUNC
sdk_aux_m1_hook:
    LINK 0;
    SP += -12;
    R1 = 0;
    CALL sdk_aux_m1;
    SP += 12;
    UNLINK;
    SP += 12;
    RTS;
.size sdk_aux_m1_hook, .-sdk_aux_m1_hook

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
