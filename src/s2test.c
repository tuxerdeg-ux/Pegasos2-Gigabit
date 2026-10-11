/*
 * s2test.c -- kleiner SANA-II-Client zum Pruefen von mv643xx.device
 *
 * Uebersetzen: ppc-amigaos-gcc -O2 -o s2test s2test.c -lauto
 * Aufruf:      s2test [Device-Name oder Pfad]
 *              (Vorgabe: mv643xx.device)
 *
 * Nacheinander:
 *  A) OpenDevice mit zu kurzem Request          -> muss abgelehnt werden
 *  B) OpenDevice mit gueltigem Request          -> muss klappen
 *  C) S2_DEVICEQUERY, S2_GETSTATIONADDRESS
 *  D) S2_CONFIGINTERFACE, S2_ONLINE
 *  E) S2_ONEVENT und CMD_READ: absenden, nach 0,5 s abbrechen
 *  F) CMD_WRITE (Broadcast-Frame ueber die Hardware senden)
 *  G) zweites S2_CONFIGINTERFACE                -> muss S2ERR_BAD_STATE liefern
 *  H) S2_OFFLINE, CloseDevice
 *
 * Die Kopier-Callbacks sind Test-Stubs: Sie melden Erfolg, kopieren aber
 * keine Daten. Das Device ruft sie bei Datenuebertragungen auf.
 */

#include <stdio.h>
#include <string.h>
#include <exec/exec.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>

#undef AllocSysObjectTags
#undef FreeSysObject

static struct MsgPort *port;

static BOOL dummy_copy_to(APTR to, APTR from, ULONG n)
{
    (void)to; (void)from; (void)n;
    return TRUE;
}

static BOOL dummy_copy_from(APTR to, APTR from, ULONG n)
{
    (void)to; (void)from; (void)n;
    return TRUE;
}

static struct IOSana2Req *clone_req(const struct IOSana2Req *src)
{
    struct IOSana2Req *r = (struct IOSana2Req *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        ASOIOR_ReplyPort, port,
        TAG_DONE);

    if (r)
    {
        r->ios2_Req.io_Device = src->ios2_Req.io_Device;
        r->ios2_Req.io_Unit   = src->ios2_Req.io_Unit;
        r->ios2_BufferManagement = src->ios2_BufferManagement;
    }
    return r;
}

static void report(const char *what, const struct IOSana2Req *r)
{
    printf("   %-28s io_Error %ld, WireError %lu\n", what,
           (long)r->ios2_Req.io_Error, (unsigned long)r->ios2_WireError);
    fflush(stdout);
}

static void do_cmd(struct IOSana2Req *r, UWORD cmd, const char *what)
{
    r->ios2_Req.io_Command = cmd;
    r->ios2_Req.io_Flags = 0;
    IExec->DoIO((struct IORequest *)r);
    report(what, r);
}

/* absenden, nach 0,5 s abbrechen und das Ergebnis zeigen */
static void send_and_abort(struct IOSana2Req *r, UWORD cmd, const char *what)
{
    r->ios2_Req.io_Command = cmd;
    r->ios2_Req.io_Flags = 0;
    IExec->SendIO((struct IORequest *)r);
    IDOS->Delay(25);
    IExec->AbortIO((struct IORequest *)r);
    IExec->WaitIO((struct IORequest *)r);
    report(what, r);
}

static void print_mac(const char *label, const UBYTE *a)
{
    printf("   %s %02x:%02x:%02x:%02x:%02x:%02x\n", label,
           (unsigned)a[0], (unsigned)a[1], (unsigned)a[2],
           (unsigned)a[3], (unsigned)a[4], (unsigned)a[5]);
}

int main(int argc, char **argv)
{
    const char *name = (argc > 1) ? argv[1] : "mv643xx.device";
    struct TagItem tags[3];
    struct IOSana2Req *shortreq, *s2, *ev, *rd, *wr;
    struct Sana2DeviceQuery q;
    UBYTE mac[6];
    UBYTE frame[32];
    LONG err;

    port = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!port)
    {
        printf("Kein MsgPort\n");
        return 20;
    }

    printf("Device: %s\n", name);

    /* A) zu kurzer Request */
    shortreq = (struct IOSana2Req *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST,
        ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, port,
        TAG_DONE);
    if (shortreq)
    {
        err = IExec->OpenDevice(name, 0, (struct IORequest *)shortreq, 0);
        printf("A) Open mit kurzem Request: %s (Fehler %ld)\n",
               err ? "abgelehnt (richtig)" : "ANGENOMMEN (falsch!)", (long)err);
        if (!err)
            IExec->CloseDevice((struct IORequest *)shortreq);
        IExec->FreeSysObject(ASOT_IOREQUEST, shortreq);
    }

    /* B) gueltiger Request */
    memset(tags, 0, sizeof(tags));
    tags[0].ti_Tag = S2_CopyToBuff;   tags[0].ti_Data = (ULONG)dummy_copy_to;
    tags[1].ti_Tag = S2_CopyFromBuff; tags[1].ti_Data = (ULONG)dummy_copy_from;
    tags[2].ti_Tag = TAG_DONE;

    s2 = (struct IOSana2Req *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        ASOIOR_ReplyPort, port,
        TAG_DONE);
    if (!s2)
    {
        printf("Kein IORequest\n");
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }
    s2->ios2_BufferManagement = tags;

    err = IExec->OpenDevice(name, 0, (struct IORequest *)s2, 0);
    printf("B) Open mit gueltigem Request: %s (Fehler %ld)\n",
           err ? "FEHLGESCHLAGEN" : "ok", (long)err);
    if (err)
    {
        IExec->FreeSysObject(ASOT_IOREQUEST, s2);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 10;
    }

    /* C) Abfragen */
    printf("C) Abfragen\n");
    memset(&q, 0, sizeof(q));
    q.SizeAvailable = sizeof(q);
    s2->ios2_StatData = &q;
    do_cmd(s2, S2_DEVICEQUERY, "S2_DEVICEQUERY");
    printf("   MTU %lu, BPS %lu, Hardwaretyp %lu, Adressgroesse %lu Bit\n",
           (unsigned long)q.MTU, (unsigned long)q.BPS,
           (unsigned long)q.HardwareType, (unsigned long)q.AddrFieldSize);

    do_cmd(s2, S2_GETSTATIONADDRESS, "S2_GETSTATIONADDRESS");
    print_mac("aktuell: ", s2->ios2_SrcAddr);
    print_mac("Hardware:", s2->ios2_DstAddr);
    memcpy(mac, s2->ios2_SrcAddr, 6);

    /* D) einrichten */
    printf("D) Konfigurieren\n");
    memset(s2->ios2_SrcAddr, 0, sizeof(s2->ios2_SrcAddr));
    memcpy(s2->ios2_SrcAddr, mac, 6);
    do_cmd(s2, S2_CONFIGINTERFACE, "S2_CONFIGINTERFACE");
    do_cmd(s2, S2_ONLINE, "S2_ONLINE");

    /* E) wartende Anfragen */
    printf("E) Wartende Anfragen (werden nach 0,5 s abgebrochen)\n");
    ev = clone_req(s2);
    rd = clone_req(s2);
    if (ev)
    {
        ev->ios2_WireError = S2EVENT_ERROR;
        send_and_abort(ev, S2_ONEVENT, "S2_ONEVENT (erwartet -2)");
        IExec->FreeSysObject(ASOT_IOREQUEST, ev);
    }
    if (rd)
    {
        rd->ios2_PacketType = 0x0806;
        rd->ios2_Data = frame;
        send_and_abort(rd, CMD_READ, "CMD_READ (erwartet -2)");
        IExec->FreeSysObject(ASOT_IOREQUEST, rd);
    }

    /* F) Schreiben */
    printf("F) Schreiben\n");
    wr = clone_req(s2);
    if (wr)
    {
        memset(frame, 0xAA, sizeof(frame));
        memset(wr->ios2_DstAddr, 0xFF, 6);
        wr->ios2_PacketType = 0x88B5;
        wr->ios2_DataLength = sizeof(frame);
        wr->ios2_Data = frame;
        do_cmd(wr, CMD_WRITE, "CMD_WRITE");
        IExec->FreeSysObject(ASOT_IOREQUEST, wr);
    }

    /* G) doppelte Konfiguration */
    printf("G) Zweites S2_CONFIGINTERFACE (erwartet io_Error 4)\n");
    do_cmd(s2, S2_CONFIGINTERFACE, "S2_CONFIGINTERFACE");

    /* H) aufraeumen */
    printf("H) Abschluss\n");
    do_cmd(s2, S2_OFFLINE, "S2_OFFLINE");
    IExec->CloseDevice((struct IORequest *)s2);
    IExec->FreeSysObject(ASOT_IOREQUEST, s2);
    IExec->FreeSysObject(ASOT_PORT, port);

    printf("Fertig. Protokoll des Devices: RAM:mv643xx.log\n");
    return 0;
}
