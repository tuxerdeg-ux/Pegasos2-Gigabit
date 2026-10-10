#ifndef DEV_WAIT_H
#define DEV_WAIT_H

#include <devices/timer.h>

#define POLL_US 500UL   /* Poll-Intervall in Mikrosekunden */

/* Wartet auf ein Signal des Unit-Ports oder bis POLL_US verstrichen ist.
 * Ohne Timer (ok == FALSE) faellt sie auf Delay(1) zurueck. */
static void unit_wait(struct MyDevUnit *mdu, struct MsgPort *tport,
                      struct TimeRequest *tr, BOOL ok)
{
    struct MyDev *md = mdu->mdu_Device;
    ULONG tmask, got;

    if (!ok)
    {
        Delay(1);
        return;
    }

    tmask = 1UL << tport->mp_SigBit;
    tr->Request.io_Command = TR_ADDREQUEST;
    tr->Time.Seconds       = 0;
    tr->Time.Microseconds  = POLL_US;
    SendIO((struct IORequest *)tr);

    got = Wait(mdu->mdu_SigMask | tmask);

    if (!(got & tmask))
        AbortIO((struct IORequest *)tr);
    WaitIO((struct IORequest *)tr);
    SetSignal(0, tmask);
}

#endif
