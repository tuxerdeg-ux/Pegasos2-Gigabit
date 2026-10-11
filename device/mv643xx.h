#ifndef MV643XX_H
#define MV643XX_H
/*
 * mv643xx.h -- SANA-II-Treiber fuer den Gigabit-Ethernetport des
 *              Marvell MV64361 (Pegasos II, AmigaOS 4.1)
 *
 * Das Device verarbeitet SANA-II-Verwaltungs- und Datenbefehle und
 * kommuniziert ueber die Hardwareschicht mit dem MV64361. Im Dev-Build
 * werden Diagnosemeldungen in eine Textdatei geschrieben.
 *
 * Aufbau nach ramdev (Hyperion/Aminet): jede .c-Datei definiert
 * __USE_INLINE__ vor dem Einbinden, und IExec/IDOS sind Makros auf
 * md->IExec bzw. md->IDOS. Jede Funktion, die Exec- oder DOS-Funktionen
 * ruft, braucht deshalb eine Variable "md".
 */

#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <devices/sana2.h>
#include <devices/sana2specialstats.h>
#include <devices/newstyle.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <string.h>
#include <stdarg.h>

#define MYDEVNAME "mv643xx.device"
#include "mv643xx.device_rev.h"
#include "../src/mv64361.h"

/***********************************************************************/

#define OK                0
#define MD_NUMUNITS       1          /* Unit 0 = Port 1 */
#define ETH_MTU           1500
#define MYPROCSTACKSIZE   32768
#define MYPROCPRI         0
#define DEFAULT_LOGFILE   "RAM:mv643xx.log"

/* Zustandsbits einer Unit */
#define UF_CONFIGURED     (1UL << 0)
#define UF_ONLINE         (1UL << 1)

/***********************************************************************/

/* Ein Opener = ein erfolgreiches OpenDevice(). Der Zeiger darauf steht
 * als "magic cookie" in ios2_BufferManagement aller spaeteren Anfragen. */
struct Opener
{
    struct Node op_Node;        /* in mdu_Openers */
    struct List op_Reads;       /* wartende CMD_READ-Anfragen */
    ULONG       op_Flags;       /* OpenDevice-Flags (SANA2OPF_*) */

    /* Kopierfunktionen des Stacks, wie in der Tag-Liste uebergeben */
    APTR op_CopyToBuff;
    APTR op_CopyFromBuff;
    APTR op_CopyToBuff16;
    APTR op_CopyFromBuff16;
    APTR op_CopyToBuff32;
    APTR op_CopyFromBuff32;
    APTR op_DMACopyToBuff32;
    APTR op_DMACopyFromBuff32;
    APTR op_DMACopyToBuff64;
    APTR op_DMACopyFromBuff64;
    APTR op_PacketFilter;
    APTR op_Log;

    /* SANA-II Buffer-Management-Hook */
    struct Sana2Hook *op_Sana2Hook;
};

struct MyTypeStat
{
    struct Node mts_Node;
    UWORD mts_Type;
    UWORD mts_Pad;
    struct Sana2PacketTypeStats mts_Stats;
};

struct MyMulticast
{
    struct Node mm_Node;
    UBYTE mm_Address[6];
    ULONG mm_Users;
};

struct MyDev;

struct MyDevUnit
{
    struct Unit     mdu_Unit;       /* enthaelt den MsgPort fuer Anfragen */
    ULONG           mdu_UnitNum;
    struct MyDev   *mdu_Device;
    struct Process *mdu_Process;    /* Server-Prozess dieser Unit */
    ULONG           mdu_SigMask;    /* Signal des Ports, wird vom Prozess gesetzt */
    volatile ULONG  mdu_Quit;       /* Prozess soll enden */
    volatile ULONG  mdu_Gone;       /* Prozess ist fertig */

    ULONG           mdu_State;      /* UF_* */
    struct mv_eth   mdu_Eth;
    UBYTE           mdu_Mac[6];     /* aktuelle Adresse */
    UBYTE           mdu_DefMac[6];  /* Hardware-Adresse */

    struct List     mdu_Openers;
    struct List     mdu_Orphans;    /* wartende S2_READORPHAN */
    struct List     mdu_Events;     /* wartende S2_ONEVENT */

    ULONG           mdu_Dropped;    /* RX-Frames ohne passenden Empfaenger */
    ULONG mdu_HoldPolls;        /* Aufrufe, seit ein Frame im Ring wartet */
    ULONG mdu_Held;             /* wie oft ein Frame gehalten wurde */
    ULONG mdu_NTypes;           /* Anzahl gelernter PacketTypes */
    UWORD mdu_Types[8];         /* PacketTypes, die Opener angefordert haben */
    ULONG           mdu_PacketsReceived;
    ULONG           mdu_PacketsSent;
    ULONG           mdu_BadData;
    ULONG           mdu_Overruns;
    ULONG           mdu_UnknownTypesReceived;
    ULONG           mdu_Reconfigurations;
    struct TimeVal  mdu_LastStart;

    struct List      mdu_TypeStats;
    struct List      mdu_Multicast;
};

struct MyDev
{
    struct Device       md_Device;
    UWORD               pad;

    struct ExecIFace   *IExec;
    struct MMUIFace   *IMMU;
    struct DOSIFace    *IDOS;
    struct UtilityIFace *IUtility;
    struct Library     *DOSBase;
    struct Library   *UtilityBase;
    BPTR                segList;

    struct MyDevUnit   *md_Units[MD_NUMUNITS];
};

#define IExec md->IExec
#define IDOS  md->IDOS

/***********************************************************************/

/* init.c */
struct Library *libInit(struct MyDev *md, APTR seglist, struct Interface *ISys);
LONG libOpen(struct DeviceManagerInterface *Self, struct IORequest *io,
             ULONG unit_number, ULONG flags);
BPTR libClose(struct DeviceManagerInterface *Self, struct IORequest *io);
BPTR libExpunge(struct DeviceManagerInterface *Self);

/* dev_io.c */
LONG libAbortIO(struct DeviceManagerInterface *Self, struct IORequest *which_io);
void libBeginIO(struct DeviceManagerInterface *Self, struct IORequest *io);

/* dev_unit.c */
LONG InitUnit(ULONG unit_number, struct MyDev *md);
BOOL StopUnit(struct MyDevUnit *mdu, struct MyDev *md);

/* dev_cmd.c */
void service_rx(struct MyDevUnit *mdu, struct MyDev *md);

void perform_io(struct IORequest *io, struct MyDev *md);
void abort_opener_requests(struct MyDevUnit *mdu, struct Opener *op, struct MyDev *md);
void abort_everything(struct MyDevUnit *mdu, struct MyDev *md);
BOOL abort_one(struct MyDevUnit *mdu, struct IORequest *io, struct MyDev *md);

/* dev_log.c */
void dbg_init(struct MyDev *md);
void dbg_attach(struct MyDev *md, struct Task *task, ULONG sigmask);
void dbg_detach(struct MyDev *md);
void dbg_openfile(struct MyDev *md);
void dbg_closefile(struct MyDev *md);
void dbg_flush(struct MyDev *md);
void dbg_printf(const char *fmt, ...);

#endif /* MV643XX_H */
