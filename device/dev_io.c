/*
 * dev_io.c -- BeginIO / AbortIO
 *
 * Stufe 1: Jede Anfrage wird an den Server-Prozess der Unit geschickt,
 * der sie in perform_io() (dev_cmd.c) bearbeitet. BeginIO selbst wartet
 * nie (Regel aus ramdev: nie auf den Zeitplan des Aufrufers warten).
 */

#define __USE_INLINE__ 1
#include "mv643xx.h"

/* AbortIO() ist eine BITTE, die Anfrage zu beenden. Ist sie schon fertig,
 * passiert nichts. Die Anfrage muss wie ueblich per ReplyMsg() zurueck. */
LONG libAbortIO(struct DeviceManagerInterface *Self, struct IORequest *which_io)
{
    struct MyDev *md = (struct MyDev *)Self->Data.LibBase;
    struct MyDevUnit *mdu = (struct MyDevUnit *)which_io->io_Unit;
    LONG error = OK;

    /* NT_MESSAGE heisst: die Anfrage ist noch in Bearbeitung */
    if (which_io->io_Message.mn_Node.ln_Type == NT_MESSAGE)
    {
        if (abort_one(mdu, which_io, md))
            error = IOERR_ABORTED;
    }

    return error;
}

void libBeginIO(struct DeviceManagerInterface *Self, struct IORequest *io)
{
    struct MyDev *md = (struct MyDev *)Self->Data.LibBase;
    struct MyDevUnit *mdu = (struct MyDevUnit *)io->io_Unit;

    /* damit WaitIO() sicher funktioniert */
    io->io_Message.mn_Node.ln_Type = NT_MESSAGE;

    /* Wir erledigen nichts sofort. */
    io->io_Flags &= ~IOF_QUICK;

    PutMsg(&mdu->mdu_Unit.unit_MsgPort, &io->io_Message);
}
