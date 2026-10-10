/*
 * init.c -- Resident, Manager-Interface, Init/Open/Close/Expunge
 *
 * Aufbau nach ramdev (Hyperion Entertainment, Aminet dev/src/ramdev).
 * Neu gegenueber ramdev:
 *  - SANA-II-OpenDevice: Mindestlaenge des IORequest pruefen, Tag-Liste
 *    mit den Kopierfunktionen des Stacks auswerten und protokollieren,
 *    pro Opener eine eigene Struktur anlegen
 *  - dos.library fuer die Protokolldatei
 *  - libExpunge lagert das Device nur aus, wenn der Unit-Prozess beendet ist
 */

#define __USE_INLINE__ 1
#include "mv643xx.h"

#undef IExec
#undef IDOS
#undef GetInterface
#undef DropInterface
#undef SuperState
#undef UserState
#undef RemapMemory
#undef OpenLibrary
#undef CloseLibrary
#undef DeleteLibrary
#undef AllocMem
#undef FreeMem
#undef NewList
#undef Forbid
#undef Permit
#undef AddTail
#undef Remove

/* Versions-Tag */
STATIC CONST UBYTE
#ifdef __GNUC__
__attribute__((used))
#endif
verstag[] = VERSTAG;

/* Das System erwartet ein Symbol _start. Es verhindert, dass jemand das
 * Device als Programm startet. */
int32 _start(void);

int32 _start(void)
{
    return 100;
}

/***********************************************************************/

/* Init-Funktion der ROMTag-Struktur (RTF_AUTOINIT): Das Device ist
 * angelegt, ein Rueckgabewert ungleich NULL haengt es in die Geraeteliste. */
struct Library *libInit(struct MyDev *md, APTR seglist, struct Interface *ISys)
{
    struct MyDev *result = NULL;
    struct ExecIFace *iexec;
    APTR oldStack;

    iexec = (struct ExecIFace *)ISys;
    md->IExec = iexec;

    md->IMMU = (struct MMUIFace *)iexec->GetInterface(
        ISys->Data.LibBase,
        "mmu",
        1,
        NULL);

    if (md->IMMU == NULL)
        goto out;

    oldStack = iexec->SuperState();

    md->IMMU->RemapMemory(
        (APTR)MV64361_REG_BASE,
        (APTR)MV64361_REG_BASE,
        MV64361_REG_SIZE,
        MEMATTRF_READ_WRITE |
        MEMATTRF_CACHEINHIBIT |
        MEMATTRF_GUARDED);

    iexec->UserState(oldStack);

    md->md_Device.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    md->md_Device.dd_Library.lib_Node.ln_Pri  = 0;
    md->md_Device.dd_Library.lib_Node.ln_Name = MYDEVNAME;
    md->md_Device.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    md->md_Device.dd_Library.lib_Version      = VERSION;
    md->md_Device.dd_Library.lib_Revision     = REVISION;
    md->md_Device.dd_Library.lib_IdString     = VSTRING;

    md->segList = (BPTR)seglist;

    md->DOSBase = md->IExec->OpenLibrary("dos.library", 50);
    if (md->DOSBase != NULL)
        md->IDOS = (struct DOSIFace *)md->IExec->GetInterface(md->DOSBase, "main", 1, NULL);

    if (md->IDOS == NULL)
        goto out;

    md->UtilityBase = md->IExec->OpenLibrary("utility.library", 50);
    if (md->UtilityBase != NULL)
        md->IUtility = (struct UtilityIFace *)md->IExec->GetInterface(
            md->UtilityBase, "main", 1, NULL);

    if (md->IUtility == NULL)
        goto out;

dbg_init(md);
    dbg_printf("libInit: " VERS "\n");

    result = md;

out:
    if (result == NULL)
    {
        if (md->IUtility != NULL)
            md->IExec->DropInterface((struct Interface *)md->IUtility);

        if (md->UtilityBase != NULL)
            md->IExec->CloseLibrary(md->UtilityBase);

        if (md->IDOS != NULL)
            md->IExec->DropInterface((struct Interface *)md->IDOS);

        if (md->IMMU != NULL)
            md->IExec->DropInterface((struct Interface *)md->IMMU);

        if (md->DOSBase != NULL)
            md->IExec->CloseLibrary(md->DOSBase);

        md->IExec->DeleteLibrary((struct Library *)md);
    }

    return (struct Library *)result;
}

/***********************************************************************/

/* Tag-Liste aus ios2_BufferManagement auswerten */
static void parse_buffer_tags(struct Opener *op, struct TagItem *tag, struct MyDev *md)
{
    (void)md;

    while (tag != NULL)
    {
        Tag t = tag->ti_Tag;

        if (t == TAG_DONE)
            break;
        if (t == TAG_MORE)
        {
            tag = (struct TagItem *)tag->ti_Data;
            continue;
        }
        if (t == TAG_SKIP)
        {
            tag += tag->ti_Data + 1;
            continue;
        }
        if (t == TAG_IGNORE)
        {
            tag++;
            continue;
        }

        dbg_printf("  Tag 0x%08lx = 0x%08lx\n", (ULONG)t, (ULONG)tag->ti_Data);

        switch (t)
        {
            case S2_CopyToBuff:        op->op_CopyToBuff        = (APTR)tag->ti_Data; break;
            case S2_CopyFromBuff:      op->op_CopyFromBuff      = (APTR)tag->ti_Data; break;
            case S2_PacketFilter:      op->op_PacketFilter      = (APTR)tag->ti_Data; break;
            case S2_CopyToBuff16:      op->op_CopyToBuff16      = (APTR)tag->ti_Data; break;
            case S2_CopyFromBuff16:    op->op_CopyFromBuff16    = (APTR)tag->ti_Data; break;
            case S2_CopyToBuff32:      op->op_CopyToBuff32      = (APTR)tag->ti_Data; break;
            case S2_CopyFromBuff32:    op->op_CopyFromBuff32    = (APTR)tag->ti_Data; break;
            case S2_DMACopyToBuff32:   op->op_DMACopyToBuff32   = (APTR)tag->ti_Data; break;
            case S2_DMACopyFromBuff32: op->op_DMACopyFromBuff32 = (APTR)tag->ti_Data; break;
            case S2_DMACopyToBuff64:   op->op_DMACopyToBuff64   = (APTR)tag->ti_Data; break;
            case S2_DMACopyFromBuff64: op->op_DMACopyFromBuff64 = (APTR)tag->ti_Data; break;
            case S2_Log:               op->op_Log               = (APTR)tag->ti_Data; break;
            case S2_SANA2HOOK:         op->op_Sana2Hook         = (struct Sana2Hook *)tag->ti_Data; break;
            default: break;
        }

        tag++;
    }
}

/* OpenDevice(). Bei Erfolg: io_Unit gesetzt, ln_Type = NT_REPLYMSG,
 * ios2_BufferManagement = Zeiger auf den Opener. Der Rueckgabewert muss
 * auch in io_Error stehen. */
LONG libOpen(struct DeviceManagerInterface *Self, struct IORequest *io,
             ULONG unit_number, ULONG flags)
{
    LONG error;
    struct MyDev *md = (struct MyDev *)Self->Data.LibBase;
    struct IOSana2Req *s2 = (struct IOSana2Req *)io;
    struct MyDevUnit *mdu;
    struct Opener *op;

    /* Beim Anlegen von Speicher kann ein Expunge dieses Devices kommen:
     * einen Opener vortaeuschen, solange wir arbeiten. Der temporare
     * Zaehler wird erst nach erfolgreichem Open in den echten OpenCount
     * umgewandelt; bei Fehlern wird er wieder zurueckgesetzt. */
    md->md_Device.dd_Library.lib_OpenCnt++;

    dbg_printf("Open: Unit %ld, Flags 0x%lx, Request %ld Byte (noetig: %ld)\n",
               (LONG)unit_number, (ULONG)flags,
               (LONG)io->io_Message.mn_Length, (LONG)sizeof(struct IOSana2Req));

    /* zu kurzer IORequest: ablehnen, sonst "scheppert es gewaltig" */
    if (io->io_Message.mn_Length < sizeof(struct IOSana2Req))
    {
        dbg_printf("  abgelehnt: IORequest zu kurz\n");
        error = IOERR_OPENFAIL;
        goto out;
    }

    if (unit_number >= MD_NUMUNITS)
    {
        dbg_printf("  abgelehnt: Unit %ld gibt es nicht\n", (LONG)unit_number);
        error = IOERR_OPENFAIL;
        goto out;
    }

    if (flags & SANA2OPF_PROM)
    {
        dbg_printf("  abgelehnt: Promiscuous-Modus nicht unterstuetzt\n");
        error = IOERR_OPENFAIL;
        goto out;
    }

    if (s2->ios2_BufferManagement == NULL)
    {
        dbg_printf("  abgelehnt: keine Tag-Liste fuer die Pufferverwaltung\n");
        error = IOERR_OPENFAIL;
        goto out;
    }

    if (md->md_Units[unit_number] == NULL)
    {
        error = InitUnit(unit_number, md);
        if (error != OK)
            goto out;
    }
    mdu = md->md_Units[unit_number];

    op = (struct Opener *)md->IExec->AllocMem(sizeof(*op), MEMF_SHARED | MEMF_CLEAR);
    if (op == NULL)
    {
        error = IOERR_OPENFAIL;
        goto out;
    }

    md->IExec->NewList(&op->op_Reads);
    op->op_Flags = flags;

    parse_buffer_tags(op, (struct TagItem *)s2->ios2_BufferManagement, md);

    if (op->op_Sana2Hook == NULL &&
        (op->op_CopyToBuff == NULL || op->op_CopyFromBuff == NULL))
    {
        dbg_printf("  abgelehnt: weder SANA2-Hook noch CopyToBuff/CopyFromBuff vorhanden\n");
        md->IExec->FreeMem(op, sizeof(*op));
        error = IOERR_OPENFAIL;
        goto out;
    }

    if (op->op_Sana2Hook != NULL)
        dbg_printf("  SANA2-Hook vorhanden: %p\n",
                   (void *)op->op_Sana2Hook);

    md->IExec->Forbid();
    md->IExec->AddTail(&mdu->mdu_Openers, &op->op_Node);
    md->IExec->Permit();

    s2->ios2_BufferManagement = (APTR)op;
    io->io_Unit = (struct Unit *)mdu;

    mdu->mdu_Unit.unit_OpenCnt++;
    md->md_Device.dd_Library.lib_OpenCnt--;
    md->md_Device.dd_Library.lib_OpenCnt++;

    md->md_Device.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    dbg_printf("  geoeffnet (Opener %p)\n", (void *)op);
    error = OK;

out:
    if (error != OK)
    {
        /* io_Device bei Fehlern ungueltig machen */
        io->io_Device = NULL;
        md->md_Device.dd_Library.lib_OpenCnt--;
    }

    io->io_Error = error;

    return error;
}

/***********************************************************************/

/* CloseDevice(). Rueckgabe: NULL oder die SegList, wenn das Device nun
 * ausgelagert werden soll. */
BPTR libClose(struct DeviceManagerInterface *Self, struct IORequest *io)
{
    struct MyDev *md = (struct MyDev *)Self->Data.LibBase;
    struct IOSana2Req *s2 = (struct IOSana2Req *)io;
    struct MyDevUnit *mdu = (struct MyDevUnit *)io->io_Unit;
    struct Opener *op = (struct Opener *)s2->ios2_BufferManagement;
    BPTR result = (BPTR)NULL;

    dbg_printf("Close (Opener %p)\n", (void *)op);

    /* noch wartende Anfragen dieses Openers zurueckgeben, dann entfernen */
    abort_opener_requests(mdu, op, md);

    md->IExec->Forbid();
    md->IExec->Remove(&op->op_Node);
    md->IExec->Permit();

    md->IExec->FreeMem(op, sizeof(*op));

    /* IORequest unbrauchbar machen: eine spaetere Benutzung stuerzt sofort
     * ab, statt unauffaellig Speicher zu zerstoeren */
    io->io_Unit = (struct Unit *)-1;
    io->io_Device = (struct Device *)-1;

    mdu->mdu_Unit.unit_OpenCnt--;
    md->md_Device.dd_Library.lib_OpenCnt--;

    if (md->md_Device.dd_Library.lib_OpenCnt == 0)
    {
        if (md->md_Device.dd_Library.lib_Flags & LIBF_DELEXP)
            result = libExpunge(Self);
    }

    return result;
}

/***********************************************************************/

/* Expunge: Das Device wird ausgelagert, wenn niemand es mehr offen hat
 * und der Unit-Prozess beendet ist. Sonst nur vormerken. */
BPTR libExpunge(struct DeviceManagerInterface *Self)
{
    struct MyDev *md = (struct MyDev *)Self->Data.LibBase;
    BPTR result = (BPTR)NULL;
    ULONG i;

    dbg_printf("Expunge: OpenCnt %ld\n", (LONG)md->md_Device.dd_Library.lib_OpenCnt);

    if (md->md_Device.dd_Library.lib_OpenCnt > 0)
    {
        md->md_Device.dd_Library.lib_Flags |= LIBF_DELEXP;
        return result;
    }

    for (i = 0; i < MD_NUMUNITS; i++)
    {
        if (md->md_Units[i] != NULL && !StopUnit(md->md_Units[i], md))
        {
            md->md_Device.dd_Library.lib_Flags |= LIBF_DELEXP;
            return result;
        }
    }

    result = md->segList;

    md->IExec->Remove((struct Node *)md);

    if (md->IMMU != NULL)
        md->IExec->DropInterface((struct Interface *)md->IMMU);

    if (md->IDOS != NULL)
        md->IExec->DropInterface((struct Interface *)md->IDOS);
    if (md->DOSBase != NULL)
        md->IExec->CloseLibrary(md->DOSBase);

    if (md->IUtility != NULL)
        md->IExec->DropInterface((struct Interface *)md->IUtility);
    if (md->UtilityBase != NULL)
        md->IExec->CloseLibrary(md->UtilityBase);

    md->IExec->DeleteLibrary((struct Library *)md);

    return result;
}

/* ------------------- Manager-Interface ------------------------ */

STATIC LONG _manager_Obtain(struct DeviceManagerInterface *Self)
{
    return Self->Data.RefCount++;
}

STATIC ULONG _manager_Release(struct DeviceManagerInterface *Self)
{
    return Self->Data.RefCount--;
}

STATIC CONST APTR lib_manager_vectors[] =
{
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    NULL,
    NULL,
    (APTR)libOpen,
    (APTR)libClose,
    (APTR)libExpunge,
    NULL,
    (APTR)libBeginIO,
    (APTR)libAbortIO,
    (APTR)-1
};

STATIC CONST struct TagItem lib_managerTags[] =
{
    { MIT_Name,        (Tag)"__device"          },
    { MIT_VectorTable, (Tag)lib_manager_vectors },
    { MIT_Version,     1                        },
    { TAG_DONE,        0                        }
};

STATIC CONST CONST_APTR libInterfaces[] =
{
    lib_managerTags,
    NULL
};

STATIC CONST struct TagItem libCreateTags[] =
{
    { CLT_DataSize,    sizeof(struct MyDev) },
    { CLT_InitFunc,    (Tag)libInit         },
    { CLT_Interfaces,  (Tag)libInterfaces   },
    { TAG_DONE,        0                    }
};

/* ------------------- ROM-Tag ------------------------ */

STATIC CONST struct Resident lib_res
#ifdef __GNUC__
__attribute__((used))
#endif
=
{
    RTC_MATCHWORD,
    (struct Resident *)&lib_res,
    (APTR)(&lib_res + 1),
    RTF_NATIVE | RTF_AUTOINIT,
    VERSION,
    NT_DEVICE,
    0,
    MYDEVNAME,
    VSTRING,
    (APTR)libCreateTags
};
