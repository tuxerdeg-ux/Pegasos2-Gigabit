/*
 * dev_cmd.c -- SANA-II-Befehle
 *
 * perform_io() wird nur vom Server-Prozess der Unit aufgerufen. Alle
 * Listen (Leseanfragen, Orphans, Events) werden unter Forbid()/Permit()
 * veraendert, weil AbortIO() sie aus fremden Kontexten durchsucht.
 *
 * Die Ethernet-Hardware wird ueber mv64361.c bedient. RX-Frames werden
 * per Polling an passende Leseanfragen verteilt; TX-Anfragen senden
 * Ethernet-Frames ueber die Hardware.
 */

#define __USE_INLINE__ 1
#include <exec/emulation.h>
#include "mv643xx.h"
#include "dev_hold.h"

static void zero_mem(APTR ptr, ULONG size)
{
    volatile UBYTE *p = (volatile UBYTE *)ptr;

    while (size--)
        *p++ = 0;
}

#define ALL_EVENTS (S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX | S2EVENT_ONLINE | \
                    S2EVENT_OFFLINE | S2EVENT_BUFF | S2EVENT_HARDWARE |        \
                    S2EVENT_SOFTWARE)

/* Befehle, die NSCMD_DEVICEQUERY meldet (Ende mit 0) */
static const UWORD cmd_list[] =
{
    CMD_READ,
    CMD_WRITE,
    CMD_FLUSH,
    NSCMD_DEVICEQUERY,
    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_MULTICAST,
    S2_BROADCAST,
    S2_TRACKTYPE,
    S2_UNTRACKTYPE,
    S2_GETGLOBALSTATS,
    S2_GETSPECIALSTATS,
    S2_ONEVENT,
    S2_READORPHAN,
    S2_ONLINE,
    S2_OFFLINE,
    S2_SANA2HOOK,
    0
};

/* ---- Antworten ---------------------------------------------------- */

/* Anfrage mit Fehlercode und Wire-Fehler zurueckgeben */

/*
 * SANA-II Buffer-Management
 *
 * Die beiden Funktionen werden vom Protocol Stack bei OpenDevice()
 * ueber S2_CopyToBuff / S2_CopyFromBuff geliefert.
 *
 * PPC-AmigaOS ABI:
 *   A0 = to
 *   A1 = from
 *   D0 = length
 *   D0 = BOOL result
 */
static void finish(struct IORequest *io, LONG err,
                      ULONG wireerr, struct MyDev *md);

#undef IExec
static BOOL emulate_sana_copy(struct MyDev *md,
                               CONST_APTR fn,
                               APTR to,
                               APTR from,
                               ULONG length)
{
    if (md == NULL || fn == NULL)
        return FALSE;

    return (BOOL)(md->IExec->EmulateTags)(
        fn,
        ET_RegisterA0, to,
        ET_RegisterA1, from,
        ET_RegisterD0, length,
        TAG_END);
}
#define IExec md->IExec

static BOOL sana_copy_from_buff(struct Opener *op, struct IOSana2Req *ios2, APTR to, APTR from,
                               ULONG length, struct MyDev *md)
{
    if (op == NULL || ios2 == NULL || md == NULL)
        return FALSE;

    if (op->op_Sana2Hook != NULL)
    {
        struct SANA2CopyHookMsg msg;

        msg.schm_Method = S2_CopyFromBuff;
        msg.schm_MsgSize = sizeof(msg);
        msg.schm_To = to;
        msg.schm_From = from;
        msg.schm_Size = length;


        {
            BOOL result = (BOOL)(md->IUtility->CallHookPkt)(
                &op->op_Sana2Hook->s2h_Hook,
                ios2, &msg);


            return result;
        }
    }

    if (op->op_CopyFromBuff == NULL)
        return FALSE;

    return emulate_sana_copy(md,
                             (CONST_APTR)op->op_CopyFromBuff,
                             to, from, length);
}

static BOOL sana_copy_to_buff(struct Opener *op, struct IOSana2Req *ios2, APTR to, APTR from,
                             ULONG length, struct MyDev *md)
{
    if (op == NULL || ios2 == NULL || md == NULL)
        return FALSE;

    if (op->op_Sana2Hook != NULL)
    {
        struct SANA2CopyHookMsg msg;

        msg.schm_Method = S2_CopyToBuff;
        msg.schm_MsgSize = sizeof(msg);
        msg.schm_To = to;
        msg.schm_From = from;
        msg.schm_Size = length;


        {
            BOOL result = (BOOL)(md->IUtility->CallHookPkt)(
                &op->op_Sana2Hook->s2h_Hook,
                ios2, &msg);


            return result;
        }
    }

    if (op->op_CopyToBuff == NULL)
        return FALSE;

    return emulate_sana_copy(md,
                             (CONST_APTR)op->op_CopyToBuff,
                             to, from, length);
}

/*
 * Passt ein Ethernet-Frame zu einer normalen CMD_READ-Anfrage?
 *
 * RAW akzeptiert den kompletten Frame.
 * Bei normalen Ethernet-Reads wird nach PacketType selektiert.
 */
static BOOL rx_matches(struct IOSana2Req *s2, const UBYTE *frame,
                       ULONG frame_len)
{
    ULONG type;

    if (s2 == NULL || frame == NULL || frame_len < 14)
        return FALSE;

    if (((struct IORequest *)s2)->io_Flags & SANA2IOF_RAW)
        return TRUE;

    type = ((ULONG)frame[12] << 8) | (ULONG)frame[13];

    return (s2->ios2_PacketType == type);
}


/*
 * Einen empfangenen Frame in eine wartende SANA-II-READ-Anfrage kopieren.
 *
 * Rueckgabe:
 *   TRUE  = Anfrage wurde bedient
 *   FALSE = Anfrage konnte nicht bedient werden
 */
static BOOL deliver_rx(struct MyDevUnit *mdu,
                       struct Opener *op,
                       struct IOSana2Req *s2,
                       const UBYTE *frame,
                       ULONG frame_len,
                       struct MyDev *md)
{
    ULONG payload_len;
    BOOL raw;
    BOOL ok;

    if (mdu == NULL || op == NULL || s2 == NULL ||
        frame == NULL || frame_len < 14)
        return FALSE;

    raw = ((((struct IORequest *)s2)->io_Flags & SANA2IOF_RAW) != 0);

    if (raw)
        payload_len = frame_len;
    else
        payload_len = frame_len - 14;

    /*
     * Ethernet-Adressen immer aus dem Hardwareframe uebernehmen.
     */
    CopyMem((APTR)&frame[6], (APTR)s2->ios2_SrcAddr, 6);
    CopyMem((APTR)&frame[0], (APTR)s2->ios2_DstAddr, 6);

    ((struct IORequest *)s2)->io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);

    if (frame[0] & 1) {
        if (frame[0] == 0xff && frame[1] == 0xff &&
            frame[2] == 0xff && frame[3] == 0xff &&
            frame[4] == 0xff && frame[5] == 0xff)
            ((struct IORequest *)s2)->io_Flags |= SANA2IOF_BCAST;
        else
            ((struct IORequest *)s2)->io_Flags |= SANA2IOF_MCAST;
    }

    /*
     * Wie bei Olsen/Roadshow wird ios2_DataLength beim
     * Empfang auf die tatsaechliche Datenlaenge gesetzt.
     */
    s2->ios2_DataLength = payload_len;

    if (raw) {
        ok = sana_copy_to_buff(op, s2,
                               s2->ios2_Data,
                               (APTR)frame,
                               frame_len,
                               md);
    } else {
        ok = sana_copy_to_buff(op, s2,
                               s2->ios2_Data,
                               (APTR)(frame + 14),
                               payload_len,
                               md);
    }

    if (!ok) {
        finish((struct IORequest *)s2,
               S2ERR_NO_RESOURCES,
               S2WERR_GENERIC_ERROR,
               md);
        return TRUE;
    }

    s2->ios2_DataLength = payload_len;

    if (!raw)
        s2->ios2_PacketType =
            ((ULONG)frame[12] << 8) | (ULONG)frame[13];

    finish((struct IORequest *)s2, 0, 0, md);

    return TRUE;
}


/*
 * RX-Polling:
 *
 * Ein Ethernet-Frame wird an genau einen wartenden READ pro Opener
 * geliefert. Damit erhaelt jeder Opener mit passendem PacketType
 * seine eigene Kopie.
 *
 * Wenn kein normaler READ passt, wird ein S2_READORPHAN bedient.
 * Gibt es auch dort keinen Abnehmer, wird das Paket verworfen.
 */
void service_rx(struct MyDevUnit *mdu, struct MyDev *md)
{
    const UBYTE *frame;
    ULONG frame_len;

    if (mdu == NULL || md == NULL)
        return;

    if (!(mdu->mdu_State & UF_ONLINE))
        return;

    mv_eth_poll(&mdu->mdu_Eth);

    while (mv_eth_rx_peek(&mdu->mdu_Eth, &frame, &frame_len)) {

        struct Opener *matched_op = NULL;
        struct IOSana2Req *matched_s2 = NULL;
        struct Node *n;
        struct Node *r;

        /*
         * Zuerst pro Opener den ersten passenden READ suchen.
         */
        Forbid();

        for (n = mdu->mdu_Openers.lh_Head;
             n->ln_Succ != NULL;
             n = n->ln_Succ) {

            struct Opener *op = (struct Opener *)n;

            for (r = op->op_Reads.lh_Head;
                 r->ln_Succ != NULL;
                 r = r->ln_Succ) {

                struct IOSana2Req *s2 =
                    (struct IOSana2Req *)r;

                if (rx_matches(s2, frame, frame_len)) {
                    Remove(r);
                    matched_op = op;
                    matched_s2 = s2;
                    break;
                }
            }

            if (matched_s2 != NULL)
                break;
        }

        Permit();

        if (matched_s2 != NULL) {
            /*
             * Der Request ist jetzt aus seiner Warteschlange entfernt.
             * deliver_rx() darf daher antworten.
             */
            if (matched_op != NULL) {
                deliver_rx(mdu, matched_op, matched_s2,
                           frame, frame_len, md);
            } else {
                /*
                 * READORPHAN:
                 * Der zugehoerige Opener ist nicht bekannt.
                 * Der Buffer-Callback liegt aber direkt im Request
                 * bzw. wurde beim OpenDevice bereits im Opener gespeichert.
                 *
                 * Da unsere vorhandene Struktur die Orphans nur als
                 * IOSana2Req speichert, muss hier der zugehoerige Opener
                 * ueber die Anfrage nicht mehr gesucht werden.
                 *
                 * Deshalb wird der Orphan hier zunaechst wieder
                 * zurueckgestellt. Die normale Roadshow-Nutzung arbeitet
                 * ohnehin ueber CMD_READ.
                 */
                Forbid();
                AddHead(&mdu->mdu_Orphans,
                        &matched_s2->ios2_Req.io_Message.mn_Node);
                Permit();

                mv_eth_rx_release(&mdu->mdu_Eth);
                break;
            }
        } else {
            /*
             * Niemand wartet auf diesen Frame.
             */
            if (rx_should_hold(mdu, frame, frame_len))
            {
                if (mdu->mdu_HoldPolls++ == 0)
                    mdu->mdu_Held++;
                break;      /* Frame im Ring lassen, spaeter erneut versuchen */
            }
            mdu->mdu_Dropped++;
        }

        mdu->mdu_HoldPolls = 0;
        mv_eth_rx_release(&mdu->mdu_Eth);
    }
}


/*
 * TX:
 *   normal: Ethernet Header erzeugen + Data aus ios2_Data kopieren
 *   RAW:    kompletten Frame unveraendert kopieren
 */
static BOOL transmit_request(struct MyDevUnit *mdu,
                             struct Opener *op,
                             struct IOSana2Req *s2,
                             struct MyDev *md)
{
    UBYTE *buf;
    ULONG len;
    ULONG payload_len;
    ULONG type;

    if (mdu == NULL || op == NULL || s2 == NULL)
        return FALSE;

    buf = mv_eth_tx_buf(&mdu->mdu_Eth);

    if (buf == NULL) {
        finish((struct IORequest *)s2,
               S2ERR_NO_RESOURCES,
               S2WERR_GENERIC_ERROR,
               md);
        return FALSE;
    }

    /*
     * RAW:
     * ios2_Data enthaelt bereits den kompletten Ethernetframe.
     */
    if (((struct IORequest *)s2)->io_Flags & SANA2IOF_RAW) {
        len = s2->ios2_DataLength;

        if (len < 14 || len > 1514) {
            finish((struct IORequest *)s2,
                   S2ERR_MTU_EXCEEDED,
                   S2WERR_GENERIC_ERROR,
                   md);
            return FALSE;
        }

        if (!sana_copy_from_buff(op, s2, buf,
                                 s2->ios2_Data, len, md)) {
            finish((struct IORequest *)s2,
                   S2ERR_NO_RESOURCES,
                   S2WERR_GENERIC_ERROR,
                   md);
            return FALSE;
        }
    } else {
        /*
         * Normaler Ethernet-Write:
         * Header + Payload.
         */
        payload_len = s2->ios2_DataLength;

        if (payload_len > 1500) {
            finish((struct IORequest *)s2,
                   S2ERR_MTU_EXCEEDED,
                   S2WERR_GENERIC_ERROR,
                   md);
            return FALSE;
        }

        buf[0] = s2->ios2_DstAddr[0];
        buf[1] = s2->ios2_DstAddr[1];
        buf[2] = s2->ios2_DstAddr[2];
        buf[3] = s2->ios2_DstAddr[3];
        buf[4] = s2->ios2_DstAddr[4];
        buf[5] = s2->ios2_DstAddr[5];

        buf[6] = mdu->mdu_Mac[0];
        buf[7] = mdu->mdu_Mac[1];
        buf[8] = mdu->mdu_Mac[2];
        buf[9] = mdu->mdu_Mac[3];
        buf[10] = mdu->mdu_Mac[4];
        buf[11] = mdu->mdu_Mac[5];

        type = s2->ios2_PacketType;

        buf[12] = (UBYTE)(type >> 8);
        buf[13] = (UBYTE)(type & 0xff);

        if (payload_len != 0 &&
            !sana_copy_from_buff(op, s2,
                                 buf + 14,
                                 s2->ios2_Data,
                                 payload_len,
                                 md)) {
            finish((struct IORequest *)s2,
                   S2ERR_NO_RESOURCES,
                   S2WERR_GENERIC_ERROR,
                   md);
            return FALSE;
        }

        len = payload_len + 14;
    }

    if (mv_eth_tx_submit(&mdu->mdu_Eth, len) != 0) {
        finish((struct IORequest *)s2,
               S2ERR_NO_RESOURCES,
               S2WERR_GENERIC_ERROR,
               md);
        return FALSE;
    }

    finish((struct IORequest *)s2, 0, 0, md);

    return TRUE;
}


static void finish(struct IORequest *io, LONG err, ULONG werr, struct MyDev *md)
{
    struct IOSana2Req *s2 = (struct IOSana2Req *)io;

    io->io_Error = (BYTE)err;
    s2->ios2_WireError = werr;

    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

/* wie finish(), fasst aber ios2_WireError nicht an (fuer Requests, die
 * kein IOSana2Req sind, z.B. NSCMD_DEVICEQUERY mit IOStdReq) */
static void finish_plain(struct IORequest *io, LONG err, struct MyDev *md)
{
    io->io_Error = (BYTE)err;

    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

/* ---- Abbrechen ---------------------------------------------------- */

static void abort_list(struct List *list, struct MyDev *md)
{
    struct Node *n;

    while ((n = RemHead(list)) != NULL)
        finish((struct IORequest *)n, IOERR_ABORTED, 0, md);
}

static void abort_matching(struct List *list, struct Opener *op, struct MyDev *md)
{
    struct Node *n, *next;

    for (n = list->lh_Head; (next = n->ln_Succ) != NULL; n = next)
    {
        struct IOSana2Req *s2 = (struct IOSana2Req *)n;

        if (s2->ios2_BufferManagement == (APTR)op)
        {
            Remove(n);
            finish((struct IORequest *)n, IOERR_ABORTED, 0, md);
        }
    }
}

void abort_opener_requests(struct MyDevUnit *mdu, struct Opener *op, struct MyDev *md)
{
    Forbid();
    abort_list(&op->op_Reads, md);
    abort_matching(&mdu->mdu_Orphans, op, md);
    abort_matching(&mdu->mdu_Events, op, md);
    Permit();
}

void abort_everything(struct MyDevUnit *mdu, struct MyDev *md)
{
    struct Node *on;
    struct IORequest *io;

    Forbid();

    for (on = mdu->mdu_Openers.lh_Head; on->ln_Succ != NULL; on = on->ln_Succ)
        abort_list(&((struct Opener *)on)->op_Reads, md);

    abort_list(&mdu->mdu_Orphans, md);
    abort_list(&mdu->mdu_Events, md);

    while ((io = (struct IORequest *)GetMsg(&mdu->mdu_Unit.unit_MsgPort)) != NULL)
        finish(io, IOERR_ABORTED, 0, md);

    Permit();
}

static BOOL list_contains(struct List *list, struct Node *target)
{
    struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        if (n == target)
            return TRUE;

    return FALSE;
}

/* Eine einzelne wartende Anfrage suchen und mit IOERR_ABORTED zurueckgeben.
 * Aus fremden Kontexten aufrufbar (AbortIO). */
BOOL abort_one(struct MyDevUnit *mdu, struct IORequest *io, struct MyDev *md)
{
    struct Node *target = &io->io_Message.mn_Node;
    struct Node *on;
    BOOL found = FALSE;

    Forbid();

    if (list_contains(&mdu->mdu_Unit.unit_MsgPort.mp_MsgList, target))
        found = TRUE;
    else if (list_contains(&mdu->mdu_Orphans, target))
        found = TRUE;
    else if (list_contains(&mdu->mdu_Events, target))
        found = TRUE;
    else
    {
        for (on = mdu->mdu_Openers.lh_Head; on->ln_Succ != NULL; on = on->ln_Succ)
        {
            if (list_contains(&((struct Opener *)on)->op_Reads, target))
            {
                found = TRUE;
                break;
            }
        }
    }

    if (found)
    {
        Remove(target);
        finish(io, IOERR_ABORTED, 0, md);
    }

    Permit();
    return found;
}

/* ---- Ereignisse --------------------------------------------------- */

static void fire_event(struct MyDevUnit *mdu, ULONG events, struct MyDev *md)
{
    struct Node *n, *next;

    Forbid();

    for (n = mdu->mdu_Events.lh_Head; (next = n->ln_Succ) != NULL; n = next)
    {
        struct IOSana2Req *s2 = (struct IOSana2Req *)n;

        if (s2->ios2_WireError & events)
        {
            Remove(n);
            finish((struct IORequest *)n, OK, events, md);
        }
    }

    Permit();
}

static void go_offline(struct MyDevUnit *mdu, struct MyDev *md)
{
    struct Node *on, *n;

    dbg_printf("S2_OFFLINE: go_offline() aufgerufen\n");


    if (mv_eth_stop(&mdu->mdu_Eth) != MV_OK)
        dbg_printf("go_offline: mv_eth_stop() fehlgeschlagen\n");

    mdu->mdu_State &= ~(UF_ONLINE | UF_CONFIGURED);

    Forbid();

    for (on = mdu->mdu_Openers.lh_Head; on->ln_Succ != NULL; on = on->ln_Succ)
    {
        struct Opener *op = (struct Opener *)on;

        while ((n = RemHead(&op->op_Reads)) != NULL)
            finish((struct IORequest *)n, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE, md);
    }
    while ((n = RemHead(&mdu->mdu_Orphans)) != NULL)
        finish((struct IORequest *)n, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE, md);

    Permit();

    fire_event(mdu, S2EVENT_OFFLINE, md);

    /*
     * Das Logfile gehoert zur aktuellen ONLINE-Periode.
     * Nach S2_OFFLINE wird es geflusht und geschlossen, damit
     * RAM:mv643xx.log wieder frei fuer Loeschen/Neuanlegen ist.
     */
    dbg_flush(md);
    dbg_closefile(md);
}

/* ---- Dispatcher --------------------------------------------------- */

void perform_io(struct IORequest *io, struct MyDev *md)
{
    struct IOSana2Req *s2 = (struct IOSana2Req *)io;
    struct MyDevUnit *mdu = (struct MyDevUnit *)io->io_Unit;
    struct Opener *op = (struct Opener *)s2->ios2_BufferManagement;
    BOOL online = (mdu->mdu_State & UF_ONLINE) ? TRUE : FALSE;


    switch (io->io_Command)
    {
        case NSCMD_DEVICEQUERY:
        {
            struct IOStdReq *std = (struct IOStdReq *)io;
            struct NSDeviceQueryResult *dq = (struct NSDeviceQueryResult *)std->io_Data;

            if (dq == NULL || std->io_Length < sizeof(struct NSDeviceQueryResult))
            {
                finish_plain(io, IOERR_BADLENGTH, md);
                break;
            }

            dq->DevQueryFormat     = 0;
            dq->SizeAvailable      = sizeof(struct NSDeviceQueryResult);
            dq->DeviceType         = NSDEVTYPE_SANA2;
            dq->DeviceSubType      = 0;
            dq->SupportedCommands  = (UWORD *)cmd_list;
            std->io_Actual         = sizeof(struct NSDeviceQueryResult);

            finish_plain(io, OK, md);
            break;
        }

        case S2_DEVICEQUERY:
        {
            struct Sana2DeviceQuery *q = (struct Sana2DeviceQuery *)s2->ios2_StatData;

            if (q == NULL)
            {
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER, md);
                break;
            }
            if (q->SizeAvailable < sizeof(struct Sana2DeviceQuery))
            {
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_BAD_STATDATA, md);
                break;
            }

            q->SizeSupplied   = sizeof(struct Sana2DeviceQuery);
            q->DevQueryFormat = 0;
            q->DeviceLevel    = 0;
            q->AddrFieldSize  = 48;               /* Bits */
            q->MTU            = ETH_MTU;
            {
    ULONG speed = 0;
    int link = mv_eth_link(&mdu->mdu_Eth, &speed, NULL);

    if (link > 0)
        q->BPS = speed * 1000000UL;
    else
        q->BPS = 0;
}
            q->HardwareType   = S2WireType_Ethernet;

            finish(io, OK, 0, md);
            break;
        }

        case S2_GETSTATIONADDRESS:
            dbg_printf("S2_GETSTATIONADDRESS\n");
            zero_mem(s2->ios2_SrcAddr, SANA2_MAX_ADDR_BYTES);
            zero_mem(s2->ios2_DstAddr, SANA2_MAX_ADDR_BYTES);
            memcpy(s2->ios2_SrcAddr, mdu->mdu_Mac, 6);
            memcpy(s2->ios2_DstAddr, mdu->mdu_DefMac, 6);
            finish(io, OK, 0, md);
            break;

        case S2_CONFIGINTERFACE:
            dbg_printf("S2_CONFIGINTERFACE\n");
        {
            static const UBYTE zero[6] = { 0, 0, 0, 0, 0, 0 };

            if (mdu->mdu_State & UF_CONFIGURED)
            {
                finish(io, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED, md);
                break;
            }

            if (memcmp(s2->ios2_SrcAddr, zero, 6) != 0)
                memcpy(mdu->mdu_Mac, s2->ios2_SrcAddr, 6);

            mdu->mdu_State |= UF_CONFIGURED;

            dbg_printf("  konfiguriert, offline, MAC %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                       (ULONG)mdu->mdu_Mac[0], (ULONG)mdu->mdu_Mac[1],
                       (ULONG)mdu->mdu_Mac[2], (ULONG)mdu->mdu_Mac[3],
                       (ULONG)mdu->mdu_Mac[4], (ULONG)mdu->mdu_Mac[5]);

            finish(io, OK, 0, md);
            break;
        }

        case S2_ONLINE:
            dbg_printf("S2_ONLINE\n");
            if (!(mdu->mdu_State & UF_CONFIGURED))
            {
                finish(io, S2ERR_BAD_STATE, S2WERR_NOT_CONFIGURED, md);
                break;
            }
            if (!online)
            {
                dbg_openfile(md);

                int r = mv_eth_start(&mdu->mdu_Eth);

                if (r != MV_OK)
                {
                    dbg_printf("  mv_eth_start() fehlgeschlagen (%ld)\n",
                        (LONG)r);
                    finish(io, S2ERR_OUTOFSERVICE,
                        S2WERR_GENERIC_ERROR, md);
                    break;
                }

                mdu->mdu_State |= UF_ONLINE;
                dbg_printf("  online, Hardware gestartet\n");
                finish(io, OK, 0, md);
                fire_event(mdu, S2EVENT_ONLINE, md);
            }
            else
            {
                dbg_printf("S2_ONLINE: bereits online, kein Neustart\n");
                finish(io, OK, 0, md);
            }
            break;

        case S2_OFFLINE:
            if (online)
                go_offline(mdu, md);
            finish(io, OK, 0, md);
            break;

        case S2_SANA2HOOK:
        {
            struct Sana2Hook *s2h;

            dbg_printf("S2_SANA2HOOK\n");

            if (op == NULL)
            {
                dbg_printf("  kein Opener/BufferManagement vorhanden\n");
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER, md);
                break;
            }

            s2h = (struct Sana2Hook *)s2->ios2_Data;

            dbg_printf("  Hook=%p DataLength=%lu\n",
                       (void *)s2h,
                       (ULONG)s2->ios2_DataLength);



            if (s2h == NULL)
            {
                dbg_printf("  Fehler: Hook-Pointer ist NULL\n");
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER, md);
                break;
            }

            dbg_printf("  h_Entry=%p\n",
                       (void *)s2h->s2h_Hook.h_Entry);

            dbg_printf("  h_SubEntry=%p\n",
                       (void *)s2h->s2h_Hook.h_SubEntry);

            dbg_printf("  h_Data=%p\n",
                       (void *)s2h->s2h_Hook.h_Data);

            dbg_printf("  Methods=%p\n",
                       (void *)s2h->s2h_Methods);

            if (s2->ios2_DataLength < 20)
            {
                dbg_printf("  Fehler: Hook-Struktur zu kurz\n");
                finish(io, IOERR_BADLENGTH, S2WERR_GENERIC_ERROR, md);
                break;
            }

            if (s2h->s2h_Methods == NULL ||
                !(md->IUtility->TagInArray)(S2_CopyToBuff,
                                             s2h->s2h_Methods) ||
                !(md->IUtility->TagInArray)(S2_CopyFromBuff,
                                             s2h->s2h_Methods))
            {
                dbg_printf("  Fehler: CopyToBuff/CopyFromBuff fehlt\n");
                finish(io, S2WERR_FUNCTIONS_MISSING, S2WERR_GENERIC_ERROR, md);
                break;
            }

            if (op->op_Sana2Hook != NULL)
            {
                dbg_printf("  Fehler: SANA2-Hook bereits installiert\n");
                finish(io, IOERR_UNITBUSY, S2WERR_GENERIC_ERROR, md);
                break;
            }

            op->op_Sana2Hook = s2h;

            dbg_printf("  SANA2-Hook installiert: %p\n",
                       (void *)op->op_Sana2Hook);

            finish(io, OK, 0, md);
            break;
        }

        case CMD_READ:
            if (op == NULL)
            {
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER, md);
                break;
            }
            if (!online)
            {
                finish(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE, md);
                break;
            }
            Forbid();
            rx_note_type(mdu, (struct IOSana2Req *)io);
            AddTail(&op->op_Reads, &io->io_Message.mn_Node);
            Permit();

            /*
             * Falls bereits ein Frame im RX-Ring wartet, versuchen
             * wir die neue READ-Anfrage sofort zu bedienen.
             */
            service_rx(mdu, md);

            break;                      /* bleibt pending, falls kein Frame da ist */

        case S2_READORPHAN:
            if (!online)
            {
                finish(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE, md);
                break;
            }
            Forbid();
            AddTail(&mdu->mdu_Orphans, &io->io_Message.mn_Node);
            Permit();
            break;

        case CMD_WRITE:
        case S2_BROADCAST:
        case S2_MULTICAST:
            if (!online)
            {
                finish(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE, md);
                break;
            }
            if (io->io_Command == S2_MULTICAST && !(s2->ios2_DstAddr[0] & 1))
            {
                finish(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST, md);
                break;
            }
            if (!(io->io_Flags & SANA2IOF_RAW) && s2->ios2_DataLength > ETH_MTU)
            {
                finish(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR, md);
                fire_event(mdu, S2EVENT_TX, md);
                break;
            }
            /*
             * Ethernet-Frame ueber den MV64361-TX-Ring senden.
             */
            if (op == NULL) {
                finish(io, S2ERR_BAD_ARGUMENT,
                       S2WERR_NULL_POINTER, md);
                break;
            }

            transmit_request(mdu, op, s2, md);
            break;

        case S2_ONEVENT:
        {
            ULONG mask = s2->ios2_WireError;

            if (mask & ~ALL_EVENTS)
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_BAD_EVENT, md);
            else if ((mask & S2EVENT_ONLINE) && online)
                finish(io, OK, S2EVENT_ONLINE, md);
            else if ((mask & S2EVENT_OFFLINE) && !online)
                finish(io, OK, S2EVENT_OFFLINE, md);
            else
            {
                Forbid();
                AddTail(&mdu->mdu_Events, &io->io_Message.mn_Node);
                Permit();
            }
            break;
        }

        case S2_ADDMULTICASTADDRESS:
        case S2_DELMULTICASTADDRESS:
            dbg_printf("%sMULTICASTADDRESS: "
                       "%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\\n",
                       io->io_Command == S2_ADDMULTICASTADDRESS ? "ADD " : "DEL ",
                       (ULONG)s2->ios2_SrcAddr[0],
                       (ULONG)s2->ios2_SrcAddr[1],
                       (ULONG)s2->ios2_SrcAddr[2],
                       (ULONG)s2->ios2_SrcAddr[3],
                       (ULONG)s2->ios2_SrcAddr[4],
                       (ULONG)s2->ios2_SrcAddr[5]);

            if (!(s2->ios2_SrcAddr[0] & 1))
            {
                dbg_printf("  BAD_MULTICAST\\n");
                finish(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST, md);
            }
            else
            {
                dbg_printf("  Multicast-Adresse akzeptiert\\n");
                finish(io, OK, 0, md);
            }
            break;

        case S2_TRACKTYPE:
        case S2_UNTRACKTYPE:
            finish(io, OK, 0, md);
            break;

        case S2_GETTYPESTATS:
            finish(io, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR, md);
            break;

        case S2_GETGLOBALSTATS:
            if (s2->ios2_StatData == NULL)
            {
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER, md);
                break;
            }
            zero_mem(s2->ios2_StatData, sizeof(struct Sana2DeviceStats));
            finish(io, OK, 0, md);
            break;

        case S2_GETSPECIALSTATS:
        {
            struct Sana2SpecialStatHeader *h = (struct Sana2SpecialStatHeader *)s2->ios2_StatData;

            if (h == NULL)
            {
                finish(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER, md);
                break;
            }
            h->RecordCountSupplied = 0;
            finish(io, OK, 0, md);
            break;
        }

        case CMD_FLUSH:
            abort_everything(mdu, md);
            finish(io, OK, 0, md);
            break;

        default:
            dbg_printf("  unbekannter Befehl 0x%04lx\n", (ULONG)io->io_Command);
            finish_plain(io, IOERR_NOCMD, md);
            break;
    }
}
