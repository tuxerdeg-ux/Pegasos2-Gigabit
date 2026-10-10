#ifndef MV64361_H
#define MV64361_REG_BASE 0xF1000000UL
#define MV64361_REG_SIZE 0x4000UL

#define MV64361_H
/*
 * mv64361.h -- Hardwareschicht fuer Ethernetport 1 des Marvell MV64361
 *              auf dem Pegasos II (AmigaOS 4.1, PowerPC, Big Endian)
 *
 * Alle Abfolgen stammen aus den getesteten Teilen von pegasos2-gbit-info.c
 * (TX-Test, RX-Test, Echotest) und aus dem Linux-Treiber mv643xx_eth.c
 * (Kernel 2.6.39).
 *
 * Die Schicht arbeitet im Polling-Betrieb, ohne Interrupts.
 *
 * WICHTIG:
 *  - Nicht mit __USE_INLINE__ uebersetzen. Die Schicht ruft Exec-Funktionen
 *    ausdruecklich ueber das uebergebene Interface auf (iexec->Funktion).
 *  - Die Registerseite 0xF1000000 (16 KB) muss vom Aufrufer bereits als
 *    cache-inhibit/guarded eingeblendet sein (RemapMemory), und der
 *    Aufrufer muss geprueft haben, dass ein MV64360/61 vorhanden ist.
 *  - Nicht getestet sind: Neustart einer angehaltenen RX-Queue
 *    (mv_eth_poll), mehrfaches Start/Stop, Linkwechsel im Betrieb.
 */

#include <exec/types.h>
#include <interfaces/exec.h>

#define MV_RXN       256      /* Empfangsdeskriptoren */
#define MV_TXN       16      /* Sendedeskriptoren (nutzbar: MV_TXN - 1) */
#define MV_BUFSIZE   2048    /* Puffergroesse je Deskriptor */
#define MV_MAXFRAME  1514    /* groesster Frame ohne CRC */

/* Rueckgabewerte */
#define MV_OK             0
#define MV_ERR_BUSY     (-1) /* TX- oder RX-Queue laeuft bereits */
#define MV_ERR_ENABLED  (-2) /* Port ist schon aktiviert */
#define MV_ERR_MEMORY   (-3) /* kein DMA-Speicher */
#define MV_ERR_DMA      (-4) /* Speicher nicht fuer den Port erreichbar */
#define MV_ERR_TIMEOUT  (-5) /* Hardware antwortet nicht */
#define MV_ERR_STATE    (-6) /* Aufruf im falschen Zustand */
#define MV_ERR_PARAM    (-7)
#define MV_ERR_FULL     (-8) /* kein freier Sendedeskriptor */
#define MV_ERR_PHY      (-9) /* PHY-Zugriff fehlgeschlagen */

/* Zustaende */
#define MV_CLOSED   0
#define MV_OPEN     1        /* Speicher da, Port aus */
#define MV_STARTED  2        /* Port und Queues laufen */

struct mv_stats
{
    ULONG rx_frames;
    ULONG rx_bytes;
    ULONG rx_errors;
    ULONG rx_restarts;       /* RX-Queue musste neu gestartet werden */
    ULONG tx_frames;
    ULONG tx_bytes;
    ULONG tx_errors;
    ULONG tx_kicks;          /* TX-Queue musste neu gestartet werden */
};

struct mv_eth
{
    struct ExecIFace *iexec;
    struct MMUIFace  *mmu;
    void (*logf)(const char *fmt, ...);   /* darf NULL sein */

    volatile ULONG *shared;  /* Shared-Registerblock (0xF1002000) */
    volatile ULONG *port;    /* Portregister (Port 1, 0xF1002800) */

    UBYTE *mem;              /* DMA-Speicher (virtuell) */
    ULONG  phys;             /* DMA-Speicher (physisch) */
    ULONG  attr_old;

    ULONG state;
    ULONG hung;              /* 1 = Queue stand nicht, Speicher nie freigeben */
    ULONG phy;               /* PHY-Adresse */
    UBYTE mac[6];

    ULONG rx_cur;
    ULONG rx_peeked;
    ULONG tx_cur;            /* naechster freier Sendedeskriptor */
    ULONG tx_used;           /* aeltester noch nicht abgeraeumter */
    ULONG tx_count;          /* Deskriptoren in Bearbeitung */

    ULONG pscr_old, sdma_old, pc_old, pce_old, rxptr_old;
    ULONG rate_old, mtu_old, burst_old, prio_old, tok_old, conf_old;
    ULONG uc_old[4];

    struct mv_stats stats;
};

/* Zustand und Speicher */
int  mv_eth_open (struct mv_eth *e, struct ExecIFace *iexec,
                  struct MMUIFace *mmu, void (*logf)(const char *, ...));
int  mv_eth_start(struct mv_eth *e);   /* Port, Ringe und Queues starten */
int  mv_eth_stop (struct mv_eth *e);   /* Queues stoppen, Port abschalten */
int  mv_eth_close(struct mv_eth *e);   /* gibt den Speicher nur bei Stillstand frei */

/* Adresse */
void mv_eth_get_mac(struct mv_eth *e, UBYTE mac[6]);
void mv_eth_set_mac(struct mv_eth *e, const UBYTE mac[6]);

/* PHY */
int  mv_mdio_read (struct mv_eth *e, ULONG phy, ULONG reg, ULONG *val);
int  mv_mdio_write(struct mv_eth *e, ULONG phy, ULONG reg, ULONG val);
/* 1 = Link, 0 = kein Link, <0 = Fehler; speed in Mbit/s, full = Vollduplex */
int  mv_eth_link  (struct mv_eth *e, ULONG *speed, int *full);

/* Betrieb */
void mv_eth_poll(struct mv_eth *e);    /* Sendungen abraeumen, Queues bei Bedarf anstossen */

/* Empfang: 1 = Frame bereit (Ethernet-Header beginnt bei *frame, *len ohne CRC) */
int  mv_eth_rx_peek   (struct mv_eth *e, const UBYTE **frame, ULONG *len);
void mv_eth_rx_release(struct mv_eth *e);   /* Deskriptor zurueck an die Hardware */

/* Senden: Puffer holen, Frame (mit Ethernet-Header) eintragen, abschicken */
UBYTE *mv_eth_tx_buf   (struct mv_eth *e);  /* NULL = voll */
int    mv_eth_tx_submit(struct mv_eth *e, ULONG len);

/* Selbsttest (mv64361_test.c): 20 s Echotest ueber diese API */
void mv_selftest(struct ExecIFace *iexec, struct MMUIFace *mmu,
                 void (*logf)(const char *, ...));

#endif /* MV64361_H */
