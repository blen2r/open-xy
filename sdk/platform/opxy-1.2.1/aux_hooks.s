/* Aux track hooks in the aux controller (aux_tracks.cpp). Both replace eight
 * bytes with an absolute CALL into a stub here; neither site has live
 * caller-saved registers that the C++ they call could clobber, and no stock
 * branch lands inside either site's eight bytes.
 */
.section .text
.align 2

/* 8c08f75a: the aux tab selector 8c08f74c(controller, tab) stores the tab and
 * then makes this CALL.A 8d560ac8 with the C ABI. The stub passes the
 * argument on and the selector's own return address, which its LINK saved at
 * [FP + 4], to sdk_aux_tab(argument, from); a return to 8c08fd60 means the key
 * method's M1. (The key method's own call site is no hook site: its branches
 * share the epilogue at 8c08fd60, inside those bytes.)
 */
.extern sdk_aux_tab
.global sdk_aux_tab_hook
.type sdk_aux_tab_hook, STT_FUNC
sdk_aux_tab_hook:
    LINK 0;
    SP += -12;
    P0 = [FP];
    R1 = [P0 + 4];
    CALL sdk_aux_tab;
    SP += 12;
    UNLINK;
    RTS;
.size sdk_aux_tab_hook, .-sdk_aux_tab_hook

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
