/* libpad / libmtap: one plain digital controller on port 0, driven by the
 * keyboard (see plat_pad_buttons). Reporting a digital pad skips the DualShock
 * actuator setup in the menu's pad state machine (uyafn_39E4B0). */
#include "uya_host.h"
unsigned plat_pad_buttons(void);   /* keyboard (Windows) or UYA_KEYS script (Linux), 1 = held */

/* 0x0012F470: scePadGetState(port, slot); 6 = stable (ready). */
s32 uyafn_12F470(s32 port, s32 slot) { return (port == 0 && slot == 0) ? 6 : 0; }

/* 0x0012F728: scePadInfoMode(port, slot, term, offs); term 1 = current ID (4 = digital), 2 = extended ID (0 = none). */
s32 uyafn_12F728(s32 port, s32 slot, s32 term, s32 offs)
{
    (void)port; (void)slot; (void)offs;
    return term == 1 ? 4 : 0;
}

/* 0x0012EBB8: sceMtapGetConnection(port); 0 = no multitap. */
s32 uyafn_12EBB8(s32 port) { (void)port; return 0; }

/* 0x0012F3F8: scePadRead(port, slot, buf): 32 bytes; [0] 0 = ok, [1] 0x41 = digital, [2..3] buttons (1 = released). */
s32 uyafn_12F3F8(s32 port, s32 slot, u8 *buf)
{
    for (int i = 0; i < 32; i++) buf[i] = 0;
    if (port || slot) return 0;
    buf[1] = 0x41;
    {
        unsigned held = plat_pad_buttons();
        buf[2] = (u8)~(held & 0xFF);          /* Select L3 R3 Start Up Right Down Left */
        buf[3] = (u8)~((held >> 8) & 0xFF);   /* L2 R2 L1 R1 Triangle Circle Cross Square */
    }
    for (int i = 4; i < 8; i++) buf[i] = 0x80;  /* sticks centred, if anyone looks */
    return 32;
}

/* 0x0012F5A8: scePadGetReqState(port, slot); 0 = complete. */
s32 uyafn_12F5A8(s32 port, s32 slot) { (void)port; (void)slot; return 0; }

/* 0x0012F918: scePadSetActAlign(port, slot, data): maps the pad's vibration
 * motors. There is no vibration on the keyboard, so it reports success. */
s32 uyafn_12F918(s32 port, s32 slot, u8 *data) { (void)port; (void)slot; (void)data; return 1; }
