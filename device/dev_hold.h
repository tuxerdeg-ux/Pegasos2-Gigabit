#ifndef DEV_HOLD_H
#define DEV_HOLD_H

#define RX_HOLD_MAX 8   /* so viele Versuche, dann wird der Frame verworfen */

/* PacketType merken, den ein Opener per CMD_READ angefordert hat */
static void rx_note_type(struct MyDevUnit *mdu, struct IOSana2Req *s2)
{
    ULONG i, type = s2->ios2_PacketType;

    if (((struct IORequest *)s2)->io_Flags & SANA2IOF_RAW)
        return;
    if (type > 0xFFFF)
        return;
    for (i = 0; i < mdu->mdu_NTypes; i++)
        if (mdu->mdu_Types[i] == type)
            return;
    if (mdu->mdu_NTypes < 8)
        mdu->mdu_Types[mdu->mdu_NTypes++] = (UWORD)type;
}

/* TRUE = Frame im Ring lassen, weil ein Opener den Typ kennt */
static BOOL rx_should_hold(struct MyDevUnit *mdu, const UBYTE *frame,
                           ULONG len)
{
    ULONG i, type;

    if (len < 14 || mdu->mdu_HoldPolls >= RX_HOLD_MAX)
        return FALSE;
    type = ((ULONG)frame[12] << 8) | (ULONG)frame[13];
    for (i = 0; i < mdu->mdu_NTypes; i++)
        if (mdu->mdu_Types[i] == type)
            return TRUE;
    return FALSE;
}

#endif
