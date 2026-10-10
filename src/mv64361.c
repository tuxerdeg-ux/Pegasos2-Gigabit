/*
 * mv64361.c -- Hardwareschicht fuer Ethernetport 1 des Marvell MV64361
 *              (Pegasos II). Siehe mv64361.h.
 *
 * Registerzugriffe: Die Register sind Little Endian, die CPU Big Endian,
 * deshalb jeder Zugriff mit __builtin_bswap32().
 * Deskriptoren liegen im Big-Endian-Layout des Linux-Treibers
 * (SDMA_CONFIG ohne BLM_*_NO_SWAP), siehe mv643xx_eth.c Zeile 188 ff.
 */

#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/exectags.h>
#include <exec/execbase.h>
#include <utility/tagitem.h>
#include <interfaces/exec.h>
#include "mv64361.h"

/* ---- Adressen ---------------------------------------------------- */

#define PORT_NUM     1UL
#define SHARED_OFS   0x2000UL
#define PORT_OFS     (SHARED_OFS + 0x400UL + PORT_NUM * 0x400UL)
#define UCAST_OFS    (0x1600UL + (PORT_NUM << 10))   /* im Shared-Block */

/* Shared-Block */
#define R_PHYADDR    0x0000UL
#define R_SMI        0x0004UL
#define SMI_BUSY     0x10000000UL
#define SMI_READVAL  0x08000000UL
#define SMI_READ     0x04000000UL
#define SMI_TIMEOUT  1000000UL

/* PHY Basic Control Register (MII Reg 0) */
#define PHY_BMCR          0UL
#define PHY_BMCR_RESET    0x8000UL
#define PHY_BMCR_ANENABLE 0x1000UL
#define PHY_BMCR_ANRESTART 0x0200UL

#define PHY_ANAR          4UL   /* Autonegotiation Advertisement */
#define PHY_CTRL1000      9UL   /* 1000BASE-T Control */
#define PHY_RESET_POLLS   100UL


/* Portregister */
#define R_PCONF      0x0000UL
#define R_PCONFEXT   0x0004UL
#define R_MAC_LOW    0x0014UL
#define R_MAC_HIGH   0x0018UL
#define R_SDMA       0x001cUL
#define R_SERIAL     0x003cUL
#define R_STATUS     0x0044UL
#define R_TXQ_CMD    0x0048UL
#define R_TXQ_PRIO   0x004cUL
#define R_TX_RATE    0x0050UL
#define R_TX_MTU     0x0058UL
#define R_TX_BURST   0x005cUL
#define R_INT_CAUSE  0x0060UL
#define R_INT_EXT    0x0064UL
#define R_RXQ_PTR    0x020cUL
#define R_RXQ_CMD    0x0280UL
#define R_TXQ_PTR    0x02c0UL
#define R_TXQ_TOKENS 0x0300UL
#define R_TXQ_CONF   0x0304UL

#define ST_TXFIFO_EMPTY  0x00000400UL
#define ST_TX_INPROGRESS 0x00000080UL

/* Deskriptorstatus */
#define D_OWN        0x80000000UL
#define RX_CMD       0xA0000000UL   /* OWN | RX_ENABLE_INTERRUPT */
#define RX_OKMASK    0x0C000001UL   /* FIRST | LAST | ERROR_SUMMARY */
#define RX_OKVAL     0x0C000000UL   /* FIRST | LAST, kein Fehler */
#define TX_CMD       0x80F82800UL   /* OWN|INT|CRC|FIRST|LAST|PAD|IHL=5 */
#define TX_ERR       0x00000001UL

/* Aufbau des DMA-Speichers (4-KB-ausgerichtet, ein Stueck) */
#define OFS_RXD      0x0000UL
#define OFS_TXD      (OFS_RXD + MV_RXN * 16UL)
#define OFS_TXBUF    ((OFS_TXD + MV_TXN * 16UL + 0xFFFUL) & ~0xFFFUL)
#define OFS_RXBUF    (OFS_TXBUF + MV_TXN * MV_BUFSIZE)
#define MEM_SIZE     (OFS_RXBUF + MV_RXN * MV_BUFSIZE)

#define WAIT_LOOPS   600000UL

struct mv_desc
{
    UWORD byte_cnt;
    UWORD w1;            /* RX: buf_size, TX: l4i_chk */
    ULONG cmd_sts;
    ULONG next;
    ULONG buf;
};

static void zero_mem(APTR ptr, ULONG size)
{
    volatile UBYTE *p = (volatile UBYTE *)ptr;

    while (size--)
        *p++ = 0;
}

/* ---- Hilfsfunktionen -------------------------------------------- */

static ULONG hw_rd(volatile ULONG *base, ULONG ofs)
{
    return __builtin_bswap32(*(volatile ULONG *)((volatile UBYTE *)base + ofs));
}

static void hw_wr(volatile ULONG *base, ULONG ofs, ULONG v)
{
    *(volatile ULONG *)((volatile UBYTE *)base + ofs) = __builtin_bswap32(v);
}

static void hw_sync(void)
{
    __asm__ __volatile__("sync" : : : "memory");
}

static volatile struct mv_desc *rx_desc(struct mv_eth *e)
{
    return (volatile struct mv_desc *)(e->mem + OFS_RXD);
}

static volatile struct mv_desc *tx_desc(struct mv_eth *e)
{
    return (volatile struct mv_desc *)(e->mem + OFS_TXD);
}

/* ---- MDIO ------------------------------------------------------- */

static int smi_wait(struct mv_eth *e, ULONG *value)
{
    ULONG i, v = 0;

    for (i = 0; i < SMI_TIMEOUT; i++)
    {
        v = hw_rd(e->shared, R_SMI);
        if (!(v & SMI_BUSY))
        {
            *value = v;
            return MV_OK;
        }
    }
    return MV_ERR_TIMEOUT;
}

int mv_mdio_read(struct mv_eth *e, ULONG phy, ULONG reg, ULONG *val)
{
    ULONG v;

    if (smi_wait(e, &v))
        return MV_ERR_TIMEOUT;

    hw_wr(e->shared, R_SMI, SMI_READ | ((reg & 0x1f) << 21) | ((phy & 0x1f) << 16));

    if (smi_wait(e, &v))
        return MV_ERR_TIMEOUT;
    if (!(v & SMI_READVAL))
        return MV_ERR_PHY;

    *val = v & 0xffff;
    return MV_OK;
}

int mv_mdio_write(struct mv_eth *e, ULONG phy, ULONG reg, ULONG val)
{
    ULONG v;

    if (smi_wait(e, &v))
        return MV_ERR_TIMEOUT;

    /* Opcode WRITE = 0 */
    hw_wr(e->shared, R_SMI, ((reg & 0x1f) << 21) | ((phy & 0x1f) << 16) | (val & 0xffff));

    if (smi_wait(e, &v))
        return MV_ERR_TIMEOUT;
    return MV_OK;
}

int mv_eth_link(struct mv_eth *e, ULONG *speed, int *full)
{
    ULONG v;
    int r = mv_mdio_read(e, e->phy, 17, &v);   /* Marvell PHY-Status */

    if (r)
        return r;

    if (speed)
        *speed = ((v & 0xc000) == 0x8000) ? 1000 :
                 ((v & 0xc000) == 0x4000) ? 100 : 10;
    if (full)
        *full = (v & 0x2000) ? 1 : 0;

    return (v & 0x0400) ? 1 : 0;
}

/* ---- Adresse ---------------------------------------------------- */

void mv_eth_get_mac(struct mv_eth *e, UBYTE mac[6])
{
    memcpy(mac, e->mac, 6);
}

static void program_ucast(struct mv_eth *e)
{
    ULONG nib = e->mac[5] & 0x0f, k;

    for (k = 0; k < 4; k++)
        hw_wr(e->shared, UCAST_OFS + k * 4,
              (k == (nib >> 2)) ? (1UL << (8 * (nib & 3))) : 0);
}

void mv_eth_set_mac(struct mv_eth *e, const UBYTE mac[6])
{
    memcpy(e->mac, mac, 6);

    hw_wr(e->port, R_MAC_HIGH,
          ((ULONG)mac[0] << 24) | ((ULONG)mac[1] << 16) |
          ((ULONG)mac[2] << 8)  |  (ULONG)mac[3]);
    hw_wr(e->port, R_MAC_LOW, ((ULONG)mac[4] << 8) | (ULONG)mac[5]);

    if (e->state == MV_STARTED)
        program_ucast(e);
}

/* ---- Ringe ------------------------------------------------------ */

static void ring_reset(struct mv_eth *e)
{
    volatile struct mv_desc *r = rx_desc(e);
    volatile struct mv_desc *t = tx_desc(e);
    ULONG i;

    zero_mem(e->mem, OFS_TXBUF);   /* Deskriptorbereich loeschen */

    for (i = 0; i < MV_RXN; i++)
    {
        r[i].byte_cnt = 0;
        r[i].w1       = MV_BUFSIZE;
        r[i].cmd_sts  = 0;
        r[i].next     = e->phys + OFS_RXD + ((i + 1) % MV_RXN) * 16;
        r[i].buf      = e->phys + OFS_RXBUF + i * MV_BUFSIZE;
    }
    for (i = 0; i < MV_TXN; i++)
    {
        t[i].byte_cnt = 0;
        t[i].w1       = 0;
        t[i].cmd_sts  = 0;
        t[i].next     = e->phys + OFS_TXD + ((i + 1) % MV_TXN) * 16;
        t[i].buf      = e->phys + OFS_TXBUF + i * MV_BUFSIZE;
    }
    hw_sync();

    for (i = 0; i < MV_RXN; i++)
        r[i].cmd_sts = RX_CMD;
    hw_sync();

    e->rx_cur = 0;
    e->rx_peeked = 0;
    e->tx_cur = 0;
    e->tx_used = 0;
    e->tx_count = 0;
}

/* ---- Oeffnen / Schliessen --------------------------------------- */

int mv_eth_open(struct mv_eth *e, struct ExecIFace *iexec,
                struct MMUIFace *mmu, void (*logf)(const char *, ...))
{
    struct DMAEntry dmalist[8];
    ULONG n, phys, h, l, k;
    APTR oldstack;

    zero_mem(e, sizeof(*e));
    e->iexec  = iexec;
    e->mmu    = mmu;
    e->logf   = logf;
    e->shared = (volatile ULONG *)(MV64361_REG_BASE + SHARED_OFS);
    e->port   = (volatile ULONG *)(MV64361_REG_BASE + PORT_OFS);

    /* Vorbedingungen wie in den Tests: nichts laeuft, Port aus */
    if ((hw_rd(e->port, R_TXQ_CMD) & 0xff) || (hw_rd(e->port, R_RXQ_CMD) & 0xff))
        return MV_ERR_BUSY;
    if (hw_rd(e->port, R_SERIAL) & 0x00000001UL)
        return MV_ERR_ENABLED;

    e->mem = (UBYTE *)iexec->AllocVecTags(MEM_SIZE,
                 AVT_Type, MEMF_SHARED,
                 AVT_Contiguous, TRUE,
                 AVT_Lock, TRUE,
                 AVT_Alignment, 4096,
                 AVT_PhysicalAlignment, 4096,
                 AVT_ClearWithValue, 0,
                 TAG_DONE);
    if (!e->mem)
        return MV_ERR_MEMORY;

    zero_mem(dmalist, sizeof(dmalist));
    n = iexec->StartDMA(e->mem, MEM_SIZE, DMAF_NoModify);
    if (n >= 1 && n <= 8)
        iexec->GetDMAList(e->mem, MEM_SIZE, DMAF_NoModify, dmalist);
    iexec->EndDMA(e->mem, MEM_SIZE, DMAF_NoModify);
    phys = (ULONG)dmalist[0].PhysicalAddress;

    /* Ein Stueck, 4-KB-ausgerichtet, vollstaendig in den DMA-Fenstern
     * (die Firmware gibt dem Port die ersten 1 GB frei). */
    if (n != 1 || dmalist[0].BlockLength < MEM_SIZE ||
        (phys & 0xfff) || phys + MEM_SIZE > 0x40000000UL)
    {
        iexec->FreeVec(e->mem);
        e->mem = NULL;
        return MV_ERR_DMA;
    }
    e->phys = phys;

    e->attr_old = mmu->GetMemoryAttrs(e->mem, 0);
    oldstack = iexec->SuperState();
    mmu->SetMemoryAttrs(e->mem, MEM_SIZE,
        MEMATTRF_READ_WRITE | MEMATTRF_CACHEINHIBIT |
        MEMATTRF_COHERENT | MEMATTRF_GUARDED);
    iexec->UserState(oldstack);

    /* Ausgangszustand der Register merken */
    e->pscr_old  = hw_rd(e->port, R_SERIAL);
    e->sdma_old  = hw_rd(e->port, R_SDMA);
    e->pc_old    = hw_rd(e->port, R_PCONF);
    e->pce_old   = hw_rd(e->port, R_PCONFEXT);
    e->rxptr_old = hw_rd(e->port, R_RXQ_PTR);
    e->rate_old  = hw_rd(e->port, R_TX_RATE);
    e->mtu_old   = hw_rd(e->port, R_TX_MTU);
    e->burst_old = hw_rd(e->port, R_TX_BURST);
    e->prio_old  = hw_rd(e->port, R_TXQ_PRIO);
    e->tok_old   = hw_rd(e->port, R_TXQ_TOKENS);
    e->conf_old  = hw_rd(e->port, R_TXQ_CONF);
    for (k = 0; k < 4; k++)
        e->uc_old[k] = hw_rd(e->shared, UCAST_OFS + k * 4);

    /* MAC-Adresse aus den Registern (von der Firmware gesetzt) */
    h = hw_rd(e->port, R_MAC_HIGH);
    l = hw_rd(e->port, R_MAC_LOW);
    e->mac[0] = (UBYTE)(h >> 24);
    e->mac[1] = (UBYTE)(h >> 16);
    e->mac[2] = (UBYTE)(h >> 8);
    e->mac[3] = (UBYTE)h;
    e->mac[4] = (UBYTE)(l >> 8);
    e->mac[5] = (UBYTE)l;

    /* PHY-Adresse dieses Ports */
    e->phy = (hw_rd(e->shared, R_PHYADDR) >> (5 * PORT_NUM)) & 0x1f;

    e->state = MV_OPEN;
    return MV_OK;
}

static void log_link_diag(struct mv_eth *e, const char *where)
{
    ULONG r0 = 0, r1a = 0, r1b = 0;
    ULONG r4 = 0, r5 = 0, r9 = 0, r10 = 0, r17 = 0;
    int rc0, rc1a, rc1b, rc4, rc5, rc9, rc10, rc17;

    if (!e->logf)
        return;

    e->logf("\n[LINK-DIAG %s] SERIAL=0x%08lx STATUS=0x%08lx\n",
        where,
        (unsigned long)hw_rd(e->port, R_SERIAL),
        (unsigned long)hw_rd(e->port, R_STATUS));

    rc0  = mv_mdio_read(e, e->phy, 0,  &r0);
    rc1a = mv_mdio_read(e, e->phy, 1,  &r1a);
    rc1b = mv_mdio_read(e, e->phy, 1,  &r1b);
    rc4  = mv_mdio_read(e, e->phy, 4,  &r4);
    rc5  = mv_mdio_read(e, e->phy, 5,  &r5);
    rc9  = mv_mdio_read(e, e->phy, 9,  &r9);
    rc10 = mv_mdio_read(e, e->phy, 10, &r10);
    rc17 = mv_mdio_read(e, e->phy, 17, &r17);

    e->logf("[LINK-DIAG %s] PHY=%lu"
            " BMCR=%s%04lx BMSR1=%s%04lx BMSR2=%s%04lx\n",
        where, (unsigned long)e->phy,
        rc0  ? "ERR:" : "0x", (unsigned long)r0,
        rc1a ? "ERR:" : "0x", (unsigned long)r1a,
        rc1b ? "ERR:" : "0x", (unsigned long)r1b);

    e->logf("[LINK-DIAG %s] ANAR=%s%04lx LPA=%s%04lx"
            " CTRL1000=%s%04lx STAT1000=%s%04lx PHY_STAT=%s%04lx\n",
        where,
        rc4  ? "ERR:" : "0x", (unsigned long)r4,
        rc5  ? "ERR:" : "0x", (unsigned long)r5,
        rc9  ? "ERR:" : "0x", (unsigned long)r9,
        rc10 ? "ERR:" : "0x", (unsigned long)r10,
        rc17 ? "ERR:" : "0x", (unsigned long)r17);
}
/*
 * PHY zuruecksetzen und Autonegotiation neu starten.
 * Die Advertisement-Register werden vor dem Reset gesichert,
 * weil ein PHY-Reset ihre Werte auf Defaults setzen kann.
 */
static int phy_restart_autoneg(struct mv_eth *e)
{
    ULONG bmcr, anar, ctrl1000, v;
    ULONG i;
    int rc;

    rc = mv_mdio_read(e, e->phy, PHY_BMCR, &bmcr);
    if (rc != MV_OK)
        goto mdio_error;

    rc = mv_mdio_read(e, e->phy, PHY_ANAR, &anar);
    if (rc != MV_OK)
        goto mdio_error;

    rc = mv_mdio_read(e, e->phy, PHY_CTRL1000, &ctrl1000);
    if (rc != MV_OK)
        goto mdio_error;

    if (e->logf)
        e->logf("[PHY-RESET] vorher BMCR=0x%04lx ANAR=0x%04lx CTRL1000=0x%04lx\n",
                (unsigned long)bmcr,
                (unsigned long)anar,
                (unsigned long)ctrl1000);

    rc = mv_mdio_write(e, e->phy, PHY_BMCR, bmcr | PHY_BMCR_RESET);
    if (rc != MV_OK)
        goto mdio_error;

    /* Reset-Bit muss sich selbststaendig loeschen. */
    for (i = 0; i < PHY_RESET_POLLS; ++i)
    {
        rc = mv_mdio_read(e, e->phy, PHY_BMCR, &v);
        if (rc != MV_OK)
            goto mdio_error;

        if (!(v & PHY_BMCR_RESET))
            break;
    }

    if (i == PHY_RESET_POLLS)
    {
        if (e->logf)
            e->logf("[PHY-RESET] TIMEOUT: Reset-Bit bleibt gesetzt\n");
        return MV_ERR_TIMEOUT;
    }

    /* Vorherige Autonegotiation- und Gigabit-Ankuendigung restaurieren. */
    rc = mv_mdio_write(e, e->phy, PHY_ANAR, anar);
    if (rc != MV_OK)
        goto mdio_error;

    rc = mv_mdio_write(e, e->phy, PHY_CTRL1000, ctrl1000);
    if (rc != MV_OK)
        goto mdio_error;

    /* Autonegotiation einschalten und explizit neu starten. */
    bmcr &= ~(PHY_BMCR_RESET | PHY_BMCR_ANRESTART);
    bmcr |= PHY_BMCR_ANENABLE | PHY_BMCR_ANRESTART;

    rc = mv_mdio_write(e, e->phy, PHY_BMCR, bmcr);
    if (rc != MV_OK)
        goto mdio_error;

    if (e->logf)
        e->logf("[PHY-RESET] Reset beendet; Autonegotiation neu gestartet\n");

    return MV_OK;

mdio_error:
    if (e->logf)
        e->logf("[PHY-RESET] MDIO-Fehler %ld\n", (long)rc);
    return rc;
}

int mv_eth_start(struct mv_eth *e)
{
    if (e->state != MV_OPEN || e->hung)
        return MV_ERR_STATE;

    log_link_diag(e, "VOR-START");

    /* PHY-Reset vorlaeufig deaktiviert:
       zuerst den zuvor funktionierenden Netzwerkstart pruefen. */
    ring_reset(e);

    /* wie port_start() in Linux */
    hw_wr(e->port, R_INT_CAUSE, 0);
    hw_wr(e->port, R_INT_EXT, 0);
    hw_wr(e->port, R_SDMA, 0x00800004UL);      /* Burst 4/4, ohne NO_SWAP */
    hw_wr(e->port, R_PCONF, 0x02000000UL);
    hw_wr(e->port, R_PCONFEXT, 0);
    program_ucast(e);
    /* Linux init_pscr(): PHY controls speed and duplex. */
    hw_wr(e->port, R_SERIAL, 0x000A0200UL); /* RX 9700, reserved */
    hw_wr(e->port, R_SERIAL, 0x000A0201UL); /* Port enable */
    hw_wr(e->port, R_SERIAL, 0x000A0601UL); /* Do not force link fail */
    hw_wr(e->port, R_TX_RATE, 481);
    hw_wr(e->port, R_TX_MTU, 6);
    hw_wr(e->port, R_TX_BURST, 65535);
    hw_wr(e->port, R_TXQ_TOKENS, 481UL << 14);
    hw_wr(e->port, R_TXQ_CONF, (65535UL << 10) | 481UL);
    hw_wr(e->port, R_TXQ_PRIO, e->prio_old | 1);
    hw_wr(e->port, R_TXQ_PTR, e->phys + OFS_TXD);
    hw_wr(e->port, R_RXQ_PTR, e->phys + OFS_RXD);
    hw_sync();

    hw_wr(e->port, R_RXQ_CMD, 0x00000001UL);

    e->state = MV_STARTED;
    log_link_diag(e, "NACH-START");
    return MV_OK;
}

int mv_eth_stop(struct mv_eth *e)
{
    ULONG i, st;
    int tx_idle = 0, rx_idle = 0;
    ULONG k;

    if (e->state != MV_STARTED)
        return MV_OK;

    /* wie rxq_disable()/txq_disable()/port_reset() in Linux */
    hw_wr(e->port, R_TXQ_CMD, 0x00000100UL);
    hw_wr(e->port, R_RXQ_CMD, 0x00000100UL);

    for (i = 0; i < WAIT_LOOPS; i++)
        if (!(hw_rd(e->port, R_TXQ_CMD) & 0x01)) { tx_idle = 1; break; }
    for (i = 0; i < WAIT_LOOPS; i++)
        if (!(hw_rd(e->port, R_RXQ_CMD) & 0x01)) { rx_idle = 1; break; }
    for (i = 0; i < WAIT_LOOPS; i++)
    {
        st = hw_rd(e->port, R_STATUS);
        if ((st & (ST_TX_INPROGRESS | ST_TXFIFO_EMPTY)) == ST_TXFIFO_EMPTY)
            break;
    }

    if (!tx_idle || !rx_idle)
    {
        /* Port abschalten, Speicher aber nie wieder freigeben */
        hw_wr(e->port, R_SERIAL, e->pscr_old);
        hw_sync();
        e->hung = 1;
        e->state = MV_OPEN;
        if (e->logf)
            e->logf("mv_eth_stop: Queue steht nicht (TX %ld, RX %ld)\n",
                    (long)tx_idle, (long)rx_idle);
        return MV_ERR_TIMEOUT;
    }

    /* Ausgangszustand wiederherstellen */
    hw_wr(e->port, R_SERIAL, e->pscr_old);
    hw_wr(e->port, R_TXQ_PTR, 0);
    hw_wr(e->port, R_RXQ_PTR, e->rxptr_old);
    for (k = 0; k < 4; k++)
        hw_wr(e->shared, UCAST_OFS + k * 4, e->uc_old[k]);
    hw_wr(e->port, R_TXQ_PRIO, e->prio_old);
    hw_wr(e->port, R_TXQ_CONF, e->conf_old);
    hw_wr(e->port, R_TXQ_TOKENS, e->tok_old);
    hw_wr(e->port, R_TX_BURST, e->burst_old);
    hw_wr(e->port, R_TX_MTU, e->mtu_old);
    hw_wr(e->port, R_TX_RATE, e->rate_old);
    hw_wr(e->port, R_PCONFEXT, e->pce_old);
    hw_wr(e->port, R_PCONF, e->pc_old);
    hw_wr(e->port, R_SDMA, e->sdma_old);
    hw_wr(e->port, R_INT_CAUSE, 0);
    hw_wr(e->port, R_INT_EXT, 0);
    hw_sync();

    e->state = MV_OPEN;
    return MV_OK;
}

int mv_eth_close(struct mv_eth *e)
{
    APTR oldstack;

    if (e->state == MV_STARTED)
    {
        int r = mv_eth_stop(e);
        if (r)
            return r;
    }
    if (e->hung)
        return MV_ERR_TIMEOUT;
    if (e->state != MV_OPEN)
        return MV_OK;

    oldstack = e->iexec->SuperState();
    e->mmu->SetMemoryAttrs(e->mem, MEM_SIZE,
        e->attr_old & ~(MEMATTRF_REFERENCED | MEMATTRF_CHANGED));
    e->iexec->UserState(oldstack);
    e->iexec->FreeVec(e->mem);
    e->mem = NULL;
    e->state = MV_CLOSED;
    return MV_OK;
}

/* ---- Betrieb ---------------------------------------------------- */

static ULONG tx_reclaim(struct mv_eth *e)
{
    volatile struct mv_desc *t = tx_desc(e);
    ULONG n = 0, cs;

    while (e->tx_count > 0)
    {
        volatile struct mv_desc *d = &t[e->tx_used];

        cs = d->cmd_sts;


        if (cs & D_OWN)
            break;

        if (cs & TX_ERR)
        {
            e->stats.tx_errors++;

            if (e->logf)
                e->logf("TXRECLAIM idx=%lu TX_ERROR cs=%08lx\n",
                        (unsigned long)e->tx_used,
                        (unsigned long)cs);
        }
        else
        {
            e->stats.tx_frames++;
            e->stats.tx_bytes += d->byte_cnt;

        }

        e->tx_used = (e->tx_used + 1) % MV_TXN;
        e->tx_count--;
        n++;
    }

    return n;
}

void mv_eth_poll(struct mv_eth *e)
{
    if (e->state != MV_STARTED)
        return;

    tx_reclaim(e);

    /* wie txq_kick() in Linux */
    if (e->tx_count && !(hw_rd(e->port, R_TXQ_CMD) & 1) &&
        hw_rd(e->port, R_TXQ_PTR) != e->phys + OFS_TXD + e->tx_cur * 16)
    {
        hw_wr(e->port, R_TXQ_CMD, 0x00000001UL);
        e->stats.tx_kicks++;
    }

    /* Annahme (nicht getestet): Eine angehaltene RX-Queue hat Bit 0 geloescht */
    if (!(hw_rd(e->port, R_RXQ_CMD) & 1))
    {
        hw_wr(e->port, R_RXQ_CMD, 0x00000001UL);
        e->stats.rx_restarts++;
    }
}

int mv_eth_rx_peek(struct mv_eth *e, const UBYTE **frame, ULONG *len)
{
    volatile struct mv_desc *r = rx_desc(e);

    if (e->state != MV_STARTED)
        return 0;

    for (;;)
    {
        volatile struct mv_desc *d = &r[e->rx_cur];
        ULONG cs = d->cmd_sts;
        ULONG cnt;


        if (cs & D_OWN)
            return 0;

        cnt = d->byte_cnt;   /* 2 Fuellbytes + Frame + 4 Byte CRC */

        if ((cs & RX_OKMASK) != RX_OKVAL || cnt < 6 + 14 || cnt > MV_BUFSIZE)
        {
            e->stats.rx_errors++;
            e->rx_peeked = 0;
            mv_eth_rx_release(e);
            continue;
        }

        if (!e->rx_peeked)
        {
            e->stats.rx_frames++;
            e->stats.rx_bytes += cnt - 6;
            e->rx_peeked = 1;
        }

        *frame = e->mem + OFS_RXBUF + e->rx_cur * MV_BUFSIZE + 2;
        *len = cnt - 6;
        return 1;
    }
}

void mv_eth_rx_release(struct mv_eth *e)
{
    volatile struct mv_desc *d = &rx_desc(e)[e->rx_cur];

    d->byte_cnt = 0;
    hw_sync();
    d->cmd_sts = RX_CMD;
    hw_sync();

    e->rx_cur = (e->rx_cur + 1) % MV_RXN;
    e->rx_peeked = 0;
}

UBYTE *mv_eth_tx_buf(struct mv_eth *e)
{
    if (e->state != MV_STARTED)
        return NULL;

    tx_reclaim(e);

    /* Hinter dem neuesten Deskriptor muss immer ein nicht besessener
     * liegen, sonst sendet die Hardware weiter (siehe Ein-Paket-Test). */
    if (e->tx_count >= MV_TXN - 1)
        return NULL;

    return e->mem + OFS_TXBUF + e->tx_cur * MV_BUFSIZE;
}

int mv_eth_tx_submit(struct mv_eth *e, ULONG len)
{
    volatile struct mv_desc *d;
    UBYTE *buf;
    ULONG idx;

    if (e->state != MV_STARTED)
        return MV_ERR_STATE;
    if (e->tx_count >= MV_TXN - 1)
        return MV_ERR_FULL;
    if (len < 14 || len > MV_MAXFRAME)
        return MV_ERR_PARAM;

    idx = e->tx_cur;
    buf = e->mem + OFS_TXBUF + idx * MV_BUFSIZE;

    if (len < 60)
    {
        zero_mem(buf + len, 60 - len);
        len = 60;
    }

    d = &tx_desc(e)[idx];

    d->byte_cnt = (UWORD)len;
    d->w1       = 0;
    hw_sync();



    d->cmd_sts = TX_CMD;
    hw_sync();


    e->tx_cur = (e->tx_cur + 1) % MV_TXN;
    e->tx_count++;

    hw_wr(e->port, R_TXQ_CMD, 0x00000001UL);


    return MV_OK;
}
