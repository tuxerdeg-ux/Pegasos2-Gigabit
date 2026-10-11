/*
 * dev_unit.c -- Unit und Server-Prozess
 *
 * Jede Unit hat einen eigenen DOS-Prozess. Er bearbeitet alle Anfragen
 * (perform_io), schreibt das Protokoll in die Textdatei und laeuft, bis
 * das Device ausgelagert wird (StopUnit() aus libExpunge()).
 *
 * Ein Prozess statt eines einfachen Tasks, weil er DOS-Funktionen fuer die
 * Protokolldatei braucht.
 */

#define __USE_INLINE__ 1
#include "mv643xx.h"
#include "dev_wait.h"

static void clear_memory(APTR memory, ULONG size)
{
    volatile UBYTE *p = (volatile UBYTE *)memory;

    while (size--)
        *p++ = 0;
}

static struct MyDevUnit *start_unit;

static int32 unit_process(STRPTR args, int32 arglen, struct ExecBase *sysbase)
{
    struct MyDevUnit *mdu = start_unit;
    struct MyDev *md = mdu->mdu_Device;
    struct Task *me = FindTask(NULL);
    struct IORequest *io;
    BYTE sb;
    struct MsgPort *tport = NULL;
    struct TimeRequest *tr = NULL;
    BOOL tr_open = FALSE;
    ULONG dbg_n = 0;

    sb = AllocSignal(-1);
    if (sb < 0)
    {
        mdu->mdu_Gone = TRUE;
        return 20;
    }

    mdu->mdu_SigMask = 1UL << sb;

    tport = AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (tport)
        tr = AllocSysObjectTags(ASOT_IOREQUEST,
                                ASOIOR_Size, sizeof(struct TimeRequest),
                                ASOIOR_ReplyPort, tport, TAG_DONE);
    if (tr && OpenDevice("timer.device", UNIT_MICROHZ,
                          (struct IORequest *)tr, 0) == 0)
        tr_open = TRUE;

    /* Port "scharf" schalten: erst Signalbit und Task, dann das Flag */
    mdu->mdu_Unit.unit_MsgPort.mp_SigBit = (UBYTE)sb;
    mdu->mdu_Unit.unit_MsgPort.mp_SigTask = me;
    mdu->mdu_Unit.unit_MsgPort.mp_Flags = PA_SIGNAL;

    dbg_attach(md, me, mdu->mdu_SigMask);
    dbg_openfile(md);
    dbg_printf("Unit-Prozess %ld gestartet\n", (LONG)mdu->mdu_UnitNum);

    for (;;)
    {
        /* Anfragen, die vor dem Start eingetroffen sind, werden hier
         * ebenfalls abgeholt */
        while ((io = (struct IORequest *)GetMsg(&mdu->mdu_Unit.unit_MsgPort)) != NULL)
            perform_io(io, md);

        /*
         * RX/TX-Hardware regelmaessig bedienen.
         *
         * mv_eth_poll() raeumt TX-Deskriptoren auf und service_rx()
         * holt empfangene Ethernet-Frames aus dem RX-Ring und
         * bedient wartende SANA-II-CMD_READ-Anfragen.
         */
        service_rx(mdu, md);

        if (++dbg_n >= 4000)
        {
            dbg_n = 0;
            dbg_printf("RX: frames=%lu errors=%lu restarts=%lu dropped=%lu held=%lu\n",
                       (ULONG)mdu->mdu_Eth.stats.rx_frames,
                       (ULONG)mdu->mdu_Eth.stats.rx_errors,
                       (ULONG)mdu->mdu_Eth.stats.rx_restarts,
                       (ULONG)mdu->mdu_Dropped, (ULONG)mdu->mdu_Held);
        }

        dbg_flush(md);

        if (mdu->mdu_Quit)
            break;

        /*
         * Kurzer Poll-Intervall statt dauerhaftem Wait().
         * Der Message-Port wird trotzdem wie bisher ueber das
         * Exec-Signal bedient.
         */
        unit_wait(mdu, tport, tr, tr_open);
    }

    dbg_printf("RX final: frames=%lu errors=%lu restarts=%lu dropped=%lu held=%lu\n",
               (ULONG)mdu->mdu_Eth.stats.rx_frames,
               (ULONG)mdu->mdu_Eth.stats.rx_errors,
               (ULONG)mdu->mdu_Eth.stats.rx_restarts,
               (ULONG)mdu->mdu_Dropped, (ULONG)mdu->mdu_Held);
    abort_everything(mdu, md);
    dbg_printf("Unit-Prozess %ld endet\n", (LONG)mdu->mdu_UnitNum);
    dbg_flush(md);
    dbg_closefile(md);
    dbg_detach(md);

    mdu->mdu_Unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    if (tr_open)
        CloseDevice((struct IORequest *)tr);
    if (tr)
        FreeSysObject(ASOT_IOREQUEST, tr);
    if (tport)
        FreeSysObject(ASOT_PORT, tport);

    FreeSignal(sb);

    /* Letzte Anweisung vor der Rueckkehr: ab hier darf das Device
     * ausgelagert werden. (Ein kleines Zeitfenster bleibt: die Rueckkehr
     * selbst laeuft noch im Code des Devices.) */
    mdu->mdu_Gone = TRUE;
    return 0;
}

LONG InitUnit(ULONG unit_number, struct MyDev *md)
{
    struct MyDevUnit *mdu;

    dbg_printf("InitUnit %ld\n", (LONG)unit_number);

    mdu = (struct MyDevUnit *)AllocVecTags(
        sizeof(*mdu),
        AVT_Type, MEMF_SHARED,
        TAG_DONE);
    if (mdu == NULL)
        return IOERR_OPENFAIL;
    clear_memory(mdu, sizeof(*mdu));

    mdu->mdu_UnitNum = unit_number;
    mdu->mdu_Device = md;

    if (mv_eth_open(&mdu->mdu_Eth, IExec, md->IMMU, dbg_printf) != MV_OK)
    {
        dbg_printf("InitUnit: mv_eth_open() fehlgeschlagen\n");
        FreeVec(mdu);
        return IOERR_OPENFAIL;
    }

    mv_eth_get_mac(&mdu->mdu_Eth, mdu->mdu_Mac);
    memcpy(mdu->mdu_DefMac, mdu->mdu_Mac, 6);

    dbg_printf("InitUnit: Hardware-MAC %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
        (ULONG)mdu->mdu_Mac[0],
        (ULONG)mdu->mdu_Mac[1],
        (ULONG)mdu->mdu_Mac[2],
        (ULONG)mdu->mdu_Mac[3],
        (ULONG)mdu->mdu_Mac[4],
        (ULONG)mdu->mdu_Mac[5]);

    {
        ULONG speed = 0;
        int full = 0;
        int link = mv_eth_link(&mdu->mdu_Eth, &speed, &full);

        if (link > 0)
        {
            dbg_printf("InitUnit: Link UP, %ld Mbit/s, %s Duplex\n",
                speed,
                full ? "Full" : "Half");
        }
        else if (link == 0)
        {
            dbg_printf("InitUnit: Link DOWN\n");
        }
        else
        {
            dbg_printf("InitUnit: PHY-Linkabfrage fehlgeschlagen (%ld)\n",
                (LONG)link);
        }
    }

    NewList(&mdu->mdu_Openers);
    NewList(&mdu->mdu_Orphans);
    NewList(&mdu->mdu_Events);

    /* Der Port ignoriert Signale, bis der Prozess ihn eingerichtet hat.
     * Anfragen, die bis dahin eintreffen, werden nur eingereiht. */
    mdu->mdu_Unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    mdu->mdu_Unit.unit_MsgPort.mp_SigTask = NULL;
    NewList(&mdu->mdu_Unit.unit_MsgPort.mp_MsgList);

    start_unit = mdu;

    mdu->mdu_Process = CreateNewProcTags(
        NP_Entry,     (Tag)unit_process,
        NP_Name,      (Tag)"mv643xx.device unit",
        NP_Priority,  MYPROCPRI,
        NP_StackSize, MYPROCSTACKSIZE,
        TAG_DONE);

    if (mdu->mdu_Process == NULL)
    {
        dbg_printf("InitUnit: Prozess konnte nicht gestartet werden\n");
        mv_eth_close(&mdu->mdu_Eth);
        FreeVec(mdu);
        return IOERR_OPENFAIL;
    }

    md->md_Units[unit_number] = mdu;
    return OK;
}

/* Prozess beenden und Unit freigeben. Aus libExpunge(). FALSE = der
 * Prozess steht noch (dann bleibt alles, wie es ist). */
BOOL StopUnit(struct MyDevUnit *mdu, struct MyDev *md)
{
    ULONG i;
    ULONG unit_number = mdu->mdu_UnitNum;

    mdu->mdu_Quit = TRUE;

    for (i = 0; i < 100 && !mdu->mdu_Gone; i++)
    {
        if (mdu->mdu_SigMask)
            Signal((struct Task *)mdu->mdu_Process, mdu->mdu_SigMask);
        Delay(1);
    }

    if (!mdu->mdu_Gone)
        return FALSE;

    if (mv_eth_close(&mdu->mdu_Eth) != MV_OK)
    {
        dbg_printf("StopUnit: mv_eth_close() fehlgeschlagen\n");
        return FALSE;
    }

    md->md_Units[unit_number] = NULL;
    FreeVec(mdu);
    return TRUE;
}
