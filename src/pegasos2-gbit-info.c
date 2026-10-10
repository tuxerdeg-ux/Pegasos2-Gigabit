#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <exec/types.h>
#include "mv64361.h"
#include <exec/interfaces.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/exectags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

#include <interfaces/exec.h>
#include <interfaces/expansion.h>
#include <expansion/pci.h>

#define MARVELL_VENDOR_ID  0x11AB
#define MV64360_DEVICE_ID  0x6460

#define ETH_PORT_BASE      0x2400
#define ETH_PORT_STRIDE    0x0400

/* ------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------ */

static FILE *LogFile = NULL;

static int log_open(const char *filename)
{
    LogFile = fopen(filename, "w");

    if (!LogFile)
    {
        printf("FEHLER: Logdatei konnte nicht geoeffnet werden: %s\n",
               filename);
        return 0;
    }

    return 1;
}

static void log_close(void)
{
    if (LogFile)
    {
        fflush(LogFile);
        fclose(LogFile);
        LogFile = NULL;
    }
}


static void log_printf(const char *fmt, ...)
{
    char buffer[2048];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    /* Immer auf der Konsole ausgeben. */
    printf("%s", buffer);

    /* Und sofort ins RAM-Log schreiben. */
    if (LogFile)
    {
        fputs(buffer, LogFile);
        fflush(LogFile);
    }
}



static ULONG log_read_reg(volatile ULONG *base,
                            ULONG offset,
                            const char *name)
{
    ULONG value;

    (void)name;

    value = __builtin_bswap32(*(volatile ULONG *)((UBYTE *)base + offset));

    return value;
}

static int mdio_read(volatile ULONG *smi,
                      ULONG phy_addr,
                      ULONG phy_reg,
                      ULONG *result)
{
    ULONG value;
    ULONG i;

    log_printf("[MDIO] READ PHY=%lu REG=%lu\n",
               phy_addr, phy_reg);

    /*
     * Warten, bis der SMI-Bus frei ist.
     */
    log_printf("[MDIO] Warte auf SMI-BUS...\n");

    for (i = 0; i < 1000000; i++)
    {
        value = __builtin_bswap32(*(volatile ULONG *)((UBYTE *)smi + 0x0004));

        if (!(value & 0x10000000))
            break;
    }

    log_printf("[MDIO] SMI-BUS Status = 0x%08lx\n", value);

    if (i == 1000000)
    {
        log_printf("[MDIO] ERROR: SMI busy timeout\n");
        return 0;
    }

    /*
     * MDIO READ:
     *
     *   bit 26..27 = READ opcode
     *   bit 21..25 = PHY register
     *   bit 16..20 = PHY address
     */
    value = 0x04000000 |
            ((phy_reg & 0x1f) << 21) |
            ((phy_addr & 0x1f) << 16);

    log_printf("[MDIO] WRITE SMI_REG = 0x%08lx\n", value);

    *(volatile ULONG *)((UBYTE *)smi + 0x0004) = __builtin_bswap32(value);

    log_printf("[MDIO] WRITE abgeschlossen\n");

    /*
     * Auf Abschluss warten.
     */
    log_printf("[MDIO] Warte auf SMI-Abschluss...\n");

    for (i = 0; i < 1000000; i++)
    {
        value = __builtin_bswap32(*(volatile ULONG *)((UBYTE *)smi + 0x0004));

        if (!(value & 0x10000000))
            break;
    }

    log_printf("[MDIO] SMI-Abschlusswert = 0x%08lx\n", value);

    if (i == 1000000)
    {
        log_printf("[MDIO] ERROR: SMI completion timeout\n");
        return 0;
    }

    /*
     * READ_VALID pruefen.
     */
    if (!(value & 0x08000000))
    {
        log_printf("[MDIO] ERROR: READ_VALID nicht gesetzt\n");
        return 0;
    }

    *result = value & 0x0000ffff;

    log_printf("[MDIO] RESULT PHY=%lu REG=%lu DATA=0x%04lx\n",
               phy_addr, phy_reg, *result);

    return 1;
}



static int mdio_write(volatile ULONG *smi,
                       ULONG phy_addr,
                       ULONG phy_reg,
                       ULONG data)
{
    ULONG value;
    ULONG i;

    for (i = 0; i < 1000000; i++)
    {
        value = __builtin_bswap32(*(volatile ULONG *)((UBYTE *)smi + 0x0004));
        if (!(value & 0x10000000))
            break;
    }

    if (i == 1000000)
    {
        log_printf("[MDIO] ERROR: SMI busy timeout (write)\n");
        return 0;
    }

    /* SMI_OPCODE_WRITE = 0 */
    value = ((phy_reg & 0x1f) << 21) |
            ((phy_addr & 0x1f) << 16) |
            (data & 0xffff);

    log_printf("[MDIO] WRITE PHY=%lu REG=%lu DATA=0x%04lx\n",
               phy_addr, phy_reg, data & 0xffff);

    *(volatile ULONG *)((UBYTE *)smi + 0x0004) = __builtin_bswap32(value);

    for (i = 0; i < 1000000; i++)
    {
        value = __builtin_bswap32(*(volatile ULONG *)((UBYTE *)smi + 0x0004));
        if (!(value & 0x10000000))
            break;
    }

    if (i == 1000000)
    {
        log_printf("[MDIO] ERROR: SMI write completion timeout\n");
        return 0;
    }

    return 1;
}

static void decode_port_status(ULONG pn, ULONG st)
{
    log_printf("  Port %lu PORT_STATUS = 0x%08lx\n", pn, st);
    log_printf("    Link:     %s\n", (st & 0x02) ? "UP" : "DOWN (MAC)");
    log_printf("    Speed:    %s\n",
               ((st & 0x30) == 0x10) ? "1000" :
               ((st & 0x30) == 0x20) ? "100" :
               ((st & 0x30) == 0x00) ? "10" : "?");
    log_printf("    Duplex:   %s\n", (st & 0x04) ? "voll" : "halb");
    log_printf("    FlowCtrl: %s\n", (st & 0x08) ? "an" : "aus");
    log_printf("    TX-FIFO leer: %s, TX aktiv: %s\n",
               (st & 0x400) ? "ja" : "nein", (st & 0x80) ? "ja" : "nein");
}

#define TX_TIMEOUT 600000UL

struct txd_be
{
    UWORD byte_cnt;
    UWORD l4i_chk;
    ULONG cmd_sts;
    ULONG next_desc_ptr;
    ULONG buf_ptr;
};

static ULONG hw_rd(volatile ULONG *base, ULONG offset)
{
    return __builtin_bswap32(*(volatile ULONG *)((UBYTE *)base + offset));
}

static void hw_wr(volatile ULONG *base, ULONG offset, ULONG value)
{
    *(volatile ULONG *)((UBYTE *)base + offset) = __builtin_bswap32(value);
}

static void hw_sync(void)
{
    __asm__ __volatile__("sync" : : : "memory");
}

static void tx_test(struct MMUIFace *mmu, volatile ULONG *smi)
{
    static const UBYTE mac[6] = { 0x00, 0x0b, 0x2f, 0x6e, 0xf1, 0x3f };
    volatile ULONG *pp = (volatile ULONG *)(MV64361_REG_BASE +
                          ETH_PORT_BASE + ETH_PORT_STRIDE);
    struct DMAEntry dmalist[8];
    volatile struct txd_be *desc;
    UBYTE *mem, *frame;
    APTR oldstack;
    ULONG size = 4096, n, i, phys, attr_old, phy17, st;
    ULONG pscr_old, sdma_old, rate_old, mtu_old, burst_old;
    ULONG prio_old, tok_old, conf_old;
    int queue_idle = 0;

    log_printf("\nTX-TEST Port 1 (sendet EIN ARP-Paket)\n");
    log_printf("-------------------------------------\n");

    if ((hw_rd(pp, 0x0048) & 0xff) || (hw_rd(pp, 0x0280) & 0xff))
    {
        log_printf("  ABBRUCH: TX- oder RX-Queue laeuft bereits\n");
        return;
    }
    if (hw_rd(pp, 0x003c) & 0x00000001)
    {
        log_printf("  ABBRUCH: Port ist bereits aktiviert\n");
        return;
    }
    if (!mdio_read(smi, 7, 17, &phy17) || !(phy17 & 0x0400))
    {
        log_printf("  ABBRUCH: PHY meldet keinen Link (Kabel stecken?)\n");
        return;
    }

    mem = (UBYTE *)IExec->AllocVecTags(size,
              AVT_Type, MEMF_SHARED,
              AVT_Contiguous, TRUE,
              AVT_Lock, TRUE,
              AVT_Alignment, 4096,
              AVT_PhysicalAlignment, 4096,
              AVT_ClearWithValue, 0,
              TAG_DONE);
    if (!mem)
    {
        log_printf("  ABBRUCH: AllocVecTags fehlgeschlagen\n");
        return;
    }

    memset(dmalist, 0, sizeof(dmalist));
    n = IExec->StartDMA(mem, size, DMAF_NoModify | DMAF_ReadFromRAM);
    if (n >= 1 && n <= 8)
        IExec->GetDMAList(mem, size, DMAF_NoModify | DMAF_ReadFromRAM, dmalist);
    IExec->EndDMA(mem, size, DMAF_NoModify | DMAF_ReadFromRAM);
    phys = (ULONG)dmalist[0].PhysicalAddress;

    log_printf("  Puffer virtuell 0x%08lx, physisch 0x%08lx, %lu Eintraege\n",
               (ULONG)mem, phys, n);

    if (n != 1 || dmalist[0].BlockLength < size ||
        (phys & 0xfff) || phys + size > 0x40000000UL)
    {
        log_printf("  ABBRUCH: physische Adresse ungeeignet\n");
        IExec->FreeVec(mem);
        return;
    }

    attr_old = mmu->GetMemoryAttrs(mem, 0);
    oldstack = IExec->SuperState();
    mmu->SetMemoryAttrs(mem, size,
        MEMATTRF_READ_WRITE | MEMATTRF_CACHEINHIBIT |
        MEMATTRF_COHERENT | MEMATTRF_GUARDED);
    IExec->UserState(oldstack);
    log_printf("  Attribute 0x%08lx -> 0x%08lx\n", attr_old,
               mmu->GetMemoryAttrs(mem, 0));

    memset(mem, 0, size);
    desc  = (volatile struct txd_be *)mem;
    frame = mem + 256;

    memset(frame, 0xff, 6);
    memcpy(frame + 6, mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;
    frame[14] = 0x00; frame[15] = 0x01;
    frame[16] = 0x08; frame[17] = 0x00;
    frame[18] = 6;    frame[19] = 4;
    frame[20] = 0x00; frame[21] = 0x01;
    memcpy(frame + 22, mac, 6);
    frame[38] = 192;  frame[39] = 168; frame[40] = 1; frame[41] = 1;

    desc->byte_cnt      = 60;
    desc->l4i_chk       = 0;
    desc->next_desc_ptr = phys + 16;
    desc->buf_ptr       = phys + 256;
    desc->cmd_sts       = 0;
    desc[1].byte_cnt = 0; desc[1].l4i_chk = 0; desc[1].cmd_sts = 0;
    desc[1].next_desc_ptr = phys; desc[1].buf_ptr = 0;
    hw_sync();

    pscr_old  = hw_rd(pp, 0x003c);
    sdma_old  = hw_rd(pp, 0x001c);
    rate_old  = hw_rd(pp, 0x0050);
    mtu_old   = hw_rd(pp, 0x0058);
    burst_old = hw_rd(pp, 0x005c);
    prio_old  = hw_rd(pp, 0x004c);
    tok_old   = hw_rd(pp, 0x0300);
    conf_old  = hw_rd(pp, 0x0304);

    log_printf("  alt: SERIAL_CTRL=0x%08lx SDMA=0x%08lx PRIO=0x%08lx\n",
               pscr_old, sdma_old, prio_old);

    /* wie port_start() in Linux */
    hw_wr(pp, 0x001c, 0x00800004UL);
    hw_wr(pp, 0x003c, pscr_old | 0x00000001UL);
    hw_wr(pp, 0x003c, pscr_old | 0x00000401UL);
    hw_wr(pp, 0x0050, 481);
    hw_wr(pp, 0x0058, 6);
    hw_wr(pp, 0x005c, 65535);
    hw_wr(pp, 0x0300, 481UL << 14);
    hw_wr(pp, 0x0304, (65535UL << 10) | 481UL);
    hw_wr(pp, 0x004c, prio_old | 1);
    hw_wr(pp, 0x02c0, phys);
    hw_sync();

    st = 0;
    for (i = 0; i < TX_TIMEOUT; i++)
    {
        st = hw_rd(pp, 0x0044);
        if (st & 0x02)
            break;
    }
    log_printf("  PORT_STATUS = 0x%08lx (MAC-Link %s)\n", st,
               (st & 0x02) ? "UP" : "DOWN");

    if (st & 0x02)
    {
        desc->cmd_sts = 0x80F82800UL;
        hw_sync();
        hw_wr(pp, 0x0048, 0x00000001UL);

        for (i = 0; i < TX_TIMEOUT; i++)
        {
            st = desc->cmd_sts;
            if (!(st & 0x80000000UL))
                break;
        }
        log_printf("  cmd_sts danach = 0x%08lx (%s)\n", st,
                   (st & 0x80000000UL) ? "NICHT abgeholt" : "von DMA abgeholt");
        log_printf("  Fehlerbit: %s\n", (st & 0x1) ? "GESETZT" : "nein");
        log_printf("  TXQ_COMMAND=0x%08lx INT_CAUSE=0x%08lx INT_CAUSE_EXT=0x%08lx\n",
                   hw_rd(pp, 0x0048), hw_rd(pp, 0x0060), hw_rd(pp, 0x0064));
    }

    /* wie port_reset() in Linux */
    hw_wr(pp, 0x0048, 0x00000100UL);
    for (i = 0; i < TX_TIMEOUT; i++)
    {
        if (!(hw_rd(pp, 0x0048) & 0x01))
        {
            queue_idle = 1;
            break;
        }
    }
    for (i = 0; i < TX_TIMEOUT; i++)
    {
        st = hw_rd(pp, 0x0044);
        if ((st & 0x480) == 0x400)
            break;
    }

    hw_wr(pp, 0x003c, pscr_old);
    hw_wr(pp, 0x02c0, 0);
    hw_wr(pp, 0x004c, prio_old);
    hw_wr(pp, 0x0304, conf_old);
    hw_wr(pp, 0x0300, tok_old);
    hw_wr(pp, 0x005c, burst_old);
    hw_wr(pp, 0x0058, mtu_old);
    hw_wr(pp, 0x0050, rate_old);
    hw_wr(pp, 0x001c, sdma_old);
    hw_wr(pp, 0x0060, 0);
    hw_wr(pp, 0x0064, 0);
    hw_sync();

    log_printf("  Zustand danach: SERIAL_CTRL=0x%08lx TXQ=0x%08lx RXQ=0x%08lx\n",
               hw_rd(pp, 0x003c), hw_rd(pp, 0x0048), hw_rd(pp, 0x0280));

    if (!queue_idle)
    {
        log_printf("  WARNUNG: TX-Queue steht nicht. Speicher wird NICHT freigegeben.\n");
        log_printf("  Bitte Neustart durchfuehren.\n");
        return;
    }

    oldstack = IExec->SuperState();
    mmu->SetMemoryAttrs(mem, size,
        attr_old & ~(MEMATTRF_REFERENCED | MEMATTRF_CHANGED));
    IExec->UserState(oldstack);
    IExec->FreeVec(mem);
    log_printf("  Speicher freigegeben, TX-Test beendet\n");
}

struct rxd_be
{
    UWORD byte_cnt;
    UWORD buf_size;
    ULONG cmd_sts;
    ULONG next_desc_ptr;
    ULONG buf_ptr;
};

static void rx_test(struct MMUIFace *mmu, volatile ULONG *smi)
{
    static const UBYTE mac[6] = { 0x00, 0x0b, 0x2f, 0x6e, 0xf1, 0x3f };
    volatile ULONG *pp = (volatile ULONG *)(MV64361_REG_BASE +
                          ETH_PORT_BASE + ETH_PORT_STRIDE);
    volatile ULONG *sh = (volatile ULONG *)(MV64361_REG_BASE + 0x2000);
    struct DMAEntry dmalist[8];
    volatile struct rxd_be *desc;
    UBYTE *mem, *buf;
    APTR oldstack;
    ULONG size = 12288, n, i, j, k, nib, phys, attr_old, phy17, st, got, tick, cs;
    ULONG pscr_old, sdma_old, pc_old, pce_old, rxptr_old, uc_old[4];
    int queue_idle = 0;

    log_printf("\nRX-TEST Port 1 (empfaengt hoechstens 4 Frames)\n");
    log_printf("----------------------------------------------\n");

    if ((hw_rd(pp, 0x0048) & 0xff) || (hw_rd(pp, 0x0280) & 0xff))
    {
        log_printf("  ABBRUCH: TX- oder RX-Queue laeuft bereits\n");
        return;
    }
    if (hw_rd(pp, 0x003c) & 0x00000001)
    {
        log_printf("  ABBRUCH: Port ist bereits aktiviert\n");
        return;
    }
    if (!mdio_read(smi, 7, 17, &phy17) || !(phy17 & 0x0400))
    {
        log_printf("  ABBRUCH: PHY meldet keinen Link (Kabel stecken?)\n");
        return;
    }

    mem = (UBYTE *)IExec->AllocVecTags(size,
              AVT_Type, MEMF_SHARED,
              AVT_Contiguous, TRUE,
              AVT_Lock, TRUE,
              AVT_Alignment, 4096,
              AVT_PhysicalAlignment, 4096,
              AVT_ClearWithValue, 0,
              TAG_DONE);
    if (!mem)
    {
        log_printf("  ABBRUCH: AllocVecTags fehlgeschlagen\n");
        return;
    }

    memset(dmalist, 0, sizeof(dmalist));
    n = IExec->StartDMA(mem, size, DMAF_NoModify);
    if (n >= 1 && n <= 8)
        IExec->GetDMAList(mem, size, DMAF_NoModify, dmalist);
    IExec->EndDMA(mem, size, DMAF_NoModify);
    phys = (ULONG)dmalist[0].PhysicalAddress;

    log_printf("  Speicher virtuell 0x%08lx, physisch 0x%08lx, %lu Eintraege\n",
               (ULONG)mem, phys, n);

    if (n != 1 || dmalist[0].BlockLength < size ||
        (phys & 0xfff) || phys + size > 0x40000000UL)
    {
        log_printf("  ABBRUCH: physische Adresse ungeeignet\n");
        IExec->FreeVec(mem);
        return;
    }

    attr_old = mmu->GetMemoryAttrs(mem, 0);
    oldstack = IExec->SuperState();
    mmu->SetMemoryAttrs(mem, size,
        MEMATTRF_READ_WRITE | MEMATTRF_CACHEINHIBIT |
        MEMATTRF_COHERENT | MEMATTRF_GUARDED);
    IExec->UserState(oldstack);
    log_printf("  Attribute 0x%08lx -> 0x%08lx\n", attr_old,
               mmu->GetMemoryAttrs(mem, 0));

    memset(mem, 0, 4096);
    memset(mem + 4096, 0xA5, 8192);

    desc = (volatile struct rxd_be *)mem;
    for (i = 0; i < 5; i++)
    {
        desc[i].byte_cnt      = 0;
        desc[i].buf_size      = 2048;
        desc[i].cmd_sts       = 0;
        desc[i].next_desc_ptr = phys + ((i < 4) ? (i + 1) * 16 : 64);
        desc[i].buf_ptr       = phys + 4096 + ((i < 4) ? i : 0) * 2048;
    }
    hw_sync();
    for (i = 0; i < 4; i++)
    {
        desc[i].cmd_sts = 0xA0000000UL;
        hw_sync();
    }

    pscr_old  = hw_rd(pp, 0x003c);
    sdma_old  = hw_rd(pp, 0x001c);
    pc_old    = hw_rd(pp, 0x0000);
    pce_old   = hw_rd(pp, 0x0004);
    rxptr_old = hw_rd(pp, 0x020c);
    for (k = 0; k < 4; k++)
        uc_old[k] = hw_rd(sh, 0x1a00 + k * 4);

    /* wie port_start() in Linux, nur der Empfangsteil */
    hw_wr(pp, 0x0060, 0);
    hw_wr(pp, 0x0064, 0);
    hw_wr(pp, 0x001c, 0x00800004UL);
    hw_wr(pp, 0x0000, 0x02000000UL);
    hw_wr(pp, 0x0004, 0);
    nib = mac[5] & 0x0f;
    for (k = 0; k < 4; k++)
        hw_wr(sh, 0x1a00 + k * 4,
              (k == (nib >> 2)) ? (0x01UL << (8 * (nib & 3))) : 0);
    hw_wr(pp, 0x003c, pscr_old | 0x00000001UL);
    hw_wr(pp, 0x003c, pscr_old | 0x00000401UL);
    hw_wr(pp, 0x020c, phys);
    hw_sync();

    st = hw_rd(pp, 0x0044);
    log_printf("  PORT_STATUS = 0x%08lx\n", st);
    log_printf("  RX wird aktiviert. JETZT innerhalb von 10 s vom x86 senden:\n");
    log_printf("    sudo arping -I enp3s0 -c 5 192.168.1.1\n");

    hw_wr(pp, 0x0280, 0x00000001UL);

    got = 0;
    for (tick = 0; tick < 500; tick++)
    {
        got = 0;
        for (i = 0; i < 4; i++)
            if (!(desc[i].cmd_sts & 0x80000000UL))
                got++;
        if (got >= 4)
            break;
        IDOS->Delay(1);
    }

    /* wie rxq_disable() in Linux */
    hw_wr(pp, 0x0280, 0x00000100UL);
    for (i = 0; i < TX_TIMEOUT; i++)
    {
        if (!(hw_rd(pp, 0x0280) & 0x01))
        {
            queue_idle = 1;
            break;
        }
    }

    log_printf("  Empfangen: %lu Deskriptor(en) fertig, RX-Queue %s\n",
               got, queue_idle ? "steht" : "STEHT NICHT");
    log_printf("  INT_CAUSE=0x%08lx INT_CAUSE_EXT=0x%08lx RXQ=0x%08lx\n",
               hw_rd(pp, 0x0060), hw_rd(pp, 0x0064), hw_rd(pp, 0x0280));

    if (!queue_idle)
    {
        hw_wr(pp, 0x003c, pscr_old);
        hw_sync();
        log_printf("  WARNUNG: RX-Queue steht nicht. Port deaktiviert,\n");
        log_printf("  Speicher wird NICHT freigegeben. Bitte Neustart.\n");
        return;
    }

    for (i = 0; i < 4; i++)
    {
        cs  = desc[i].cmd_sts;
        buf = mem + 4096 + i * 2048;

        if (cs & 0x80000000UL)
        {
            log_printf("  Desc %lu: nicht benutzt (noch von DMA besessen)\n", i);
            continue;
        }

        log_printf("  Desc %lu: cmd_sts=0x%08lx byte_cnt=%lu (Nutzlaenge %ld) %s%s%s\n",
                   i, cs, (ULONG)desc[i].byte_cnt,
                   (long)desc[i].byte_cnt - 6,
                   (cs & 0x08000000UL) ? "FIRST " : "",
                   (cs & 0x04000000UL) ? "LAST "  : "",
                   (cs & 0x00000001UL) ? "FEHLER" : "");
        for (k = 0; k < 32; k += 16)
        {
            log_printf("    %02lx:", k);
            for (j = 0; j < 16; j++)
                log_printf(" %02lx", (ULONG)buf[2 + k + j]);
            log_printf("\n");
        }
    }

    /* Ausgangszustand wiederherstellen, Port abschalten */
    hw_wr(pp, 0x003c, pscr_old);
    hw_wr(pp, 0x020c, rxptr_old);
    for (k = 0; k < 4; k++)
        hw_wr(sh, 0x1a00 + k * 4, uc_old[k]);
    hw_wr(pp, 0x0004, pce_old);
    hw_wr(pp, 0x0000, pc_old);
    hw_wr(pp, 0x001c, sdma_old);
    hw_wr(pp, 0x0060, 0);
    hw_wr(pp, 0x0064, 0);
    hw_sync();

    log_printf("  Zustand danach: SERIAL_CTRL=0x%08lx RXQ=0x%08lx PORT_CONFIG=0x%08lx\n",
               hw_rd(pp, 0x003c), hw_rd(pp, 0x0280), hw_rd(pp, 0x0000));

    oldstack = IExec->SuperState();
    mmu->SetMemoryAttrs(mem, size,
        attr_old & ~(MEMATTRF_REFERENCED | MEMATTRF_CHANGED));
    IExec->UserState(oldstack);
    IExec->FreeVec(mem);
    log_printf("  Speicher freigegeben, RX-Test beendet\n");
}

#define ECHO_RXN      16
#define ECHO_TXN      4
#define ECHO_RXBUF    2048
#define ECHO_SIZE     (4096 + ECHO_RXN * ECHO_RXBUF)
#define ECHO_SECONDS  20
#define ECHO_MAXTX    64

static void echo_test(struct MMUIFace *mmu, volatile ULONG *smi)
{
    static const UBYTE mac[6]  = { 0x00, 0x0b, 0x2f, 0x6e, 0xf1, 0x3f };
    static const UBYTE myip[4] = { 192, 168, 1, 80 };
    volatile ULONG *pp = (volatile ULONG *)(MV64361_REG_BASE +
                          ETH_PORT_BASE + ETH_PORT_STRIDE);
    volatile ULONG *sh = (volatile ULONG *)(MV64361_REG_BASE + 0x2000);
    struct DMAEntry dmalist[8];
    volatile struct rxd_be *rxd;
    volatile struct txd_be *txd;
    UBYTE *mem, *fr, *txb;
    APTR oldstack;
    ULONG size = ECHO_SIZE, n, i, k, nib, phys, attr_old, phy17, st, cs, len, tick;
    ULONG pscr_old, sdma_old, pc_old, pce_old, rxptr_old, uc_old[4];
    ULONG rate_old, mtu_old, burst_old, prio_old, tok_old, conf_old;
    ULONG rx_cur = 0, tx_cur = 0;
    ULONG n_rx = 0, n_err = 0, n_uni = 0, n_arp = 0, n_tx = 0;
    ULONG n_drop = 0, n_kick = 0, n_txerr = 0, logged = 0;
    int rx_idle = 0, tx_idle = 0;

    log_printf("\nECHO-TEST Port 1 (ARP fuer 192.168.1.80, %d s)\n", ECHO_SECONDS);
    log_printf("----------------------------------------------\n");

    if ((hw_rd(pp, 0x0048) & 0xff) || (hw_rd(pp, 0x0280) & 0xff))
    {
        log_printf("  ABBRUCH: TX- oder RX-Queue laeuft bereits\n");
        return;
    }
    if (hw_rd(pp, 0x003c) & 0x00000001)
    {
        log_printf("  ABBRUCH: Port ist bereits aktiviert\n");
        return;
    }
    if (!mdio_read(smi, 7, 17, &phy17) || !(phy17 & 0x0400))
    {
        log_printf("  ABBRUCH: PHY meldet keinen Link (Kabel stecken?)\n");
        return;
    }

    mem = (UBYTE *)IExec->AllocVecTags(size,
              AVT_Type, MEMF_SHARED,
              AVT_Contiguous, TRUE,
              AVT_Lock, TRUE,
              AVT_Alignment, 4096,
              AVT_PhysicalAlignment, 4096,
              AVT_ClearWithValue, 0,
              TAG_DONE);
    if (!mem)
    {
        log_printf("  ABBRUCH: AllocVecTags fehlgeschlagen\n");
        return;
    }

    memset(dmalist, 0, sizeof(dmalist));
    n = IExec->StartDMA(mem, size, DMAF_NoModify);
    if (n >= 1 && n <= 8)
        IExec->GetDMAList(mem, size, DMAF_NoModify, dmalist);
    IExec->EndDMA(mem, size, DMAF_NoModify);
    phys = (ULONG)dmalist[0].PhysicalAddress;

    log_printf("  Speicher virtuell 0x%08lx, physisch 0x%08lx, %lu Eintraege\n",
               (ULONG)mem, phys, n);

    if (n != 1 || dmalist[0].BlockLength < size ||
        (phys & 0xfff) || phys + size > 0x40000000UL)
    {
        log_printf("  ABBRUCH: physische Adresse ungeeignet\n");
        IExec->FreeVec(mem);
        return;
    }

    attr_old = mmu->GetMemoryAttrs(mem, 0);
    oldstack = IExec->SuperState();
    mmu->SetMemoryAttrs(mem, size,
        MEMATTRF_READ_WRITE | MEMATTRF_CACHEINHIBIT |
        MEMATTRF_COHERENT | MEMATTRF_GUARDED);
    IExec->UserState(oldstack);
    log_printf("  Attribute 0x%08lx -> 0x%08lx\n", attr_old,
               mmu->GetMemoryAttrs(mem, 0));

    memset(mem, 0, size);
    rxd = (volatile struct rxd_be *)mem;
    txd = (volatile struct txd_be *)(mem + 0x100);

    for (i = 0; i < ECHO_RXN; i++)
    {
        rxd[i].byte_cnt      = 0;
        rxd[i].buf_size      = ECHO_RXBUF;
        rxd[i].cmd_sts       = 0;
        rxd[i].next_desc_ptr = phys + ((i + 1) % ECHO_RXN) * 16;
        rxd[i].buf_ptr       = phys + 4096 + i * ECHO_RXBUF;
    }
    for (i = 0; i < ECHO_TXN; i++)
    {
        txd[i].byte_cnt      = 0;
        txd[i].l4i_chk       = 0;
        txd[i].cmd_sts       = 0;
        txd[i].next_desc_ptr = phys + 0x100 + ((i + 1) % ECHO_TXN) * 16;
        txd[i].buf_ptr       = phys + 0x400 + i * 256;
    }
    hw_sync();
    for (i = 0; i < ECHO_RXN; i++)
        rxd[i].cmd_sts = 0xA0000000UL;
    hw_sync();

    pscr_old  = hw_rd(pp, 0x003c);
    sdma_old  = hw_rd(pp, 0x001c);
    pc_old    = hw_rd(pp, 0x0000);
    pce_old   = hw_rd(pp, 0x0004);
    rxptr_old = hw_rd(pp, 0x020c);
    rate_old  = hw_rd(pp, 0x0050);
    mtu_old   = hw_rd(pp, 0x0058);
    burst_old = hw_rd(pp, 0x005c);
    prio_old  = hw_rd(pp, 0x004c);
    tok_old   = hw_rd(pp, 0x0300);
    conf_old  = hw_rd(pp, 0x0304);
    for (k = 0; k < 4; k++)
        uc_old[k] = hw_rd(sh, 0x1a00 + k * 4);

    /* wie port_start() in Linux */
    hw_wr(pp, 0x0060, 0);
    hw_wr(pp, 0x0064, 0);
    hw_wr(pp, 0x001c, 0x00800004UL);
    hw_wr(pp, 0x0000, 0x02000000UL);
    hw_wr(pp, 0x0004, 0);
    nib = mac[5] & 0x0f;
    for (k = 0; k < 4; k++)
        hw_wr(sh, 0x1a00 + k * 4,
              (k == (nib >> 2)) ? (0x01UL << (8 * (nib & 3))) : 0);
    hw_wr(pp, 0x003c, pscr_old | 0x00000001UL);
    hw_wr(pp, 0x003c, pscr_old | 0x00000401UL);
    hw_wr(pp, 0x0050, 481);
    hw_wr(pp, 0x0058, 6);
    hw_wr(pp, 0x005c, 65535);
    hw_wr(pp, 0x0300, 481UL << 14);
    hw_wr(pp, 0x0304, (65535UL << 10) | 481UL);
    hw_wr(pp, 0x004c, prio_old | 1);
    hw_wr(pp, 0x02c0, phys + 0x100);
    hw_wr(pp, 0x020c, phys);
    hw_sync();

    log_printf("  PORT_STATUS = 0x%08lx\n", hw_rd(pp, 0x0044));
    log_printf("  ECHO aktiv. JETZT vom x86 aus starten:\n");
    log_printf("    sudo arping -I enp3s0 -c 5 192.168.1.80\n");

    hw_wr(pp, 0x0280, 0x00000001UL);

    for (tick = 0; tick < ECHO_SECONDS * 50; tick++)
    {
        for (;;)
        {
            cs = rxd[rx_cur].cmd_sts;
            if (cs & 0x80000000UL)
                break;

            len = rxd[rx_cur].byte_cnt;
            fr  = mem + 4096 + rx_cur * ECHO_RXBUF + 2;
            n_rx++;

            if ((cs & 0x0c000001UL) != 0x0c000000UL || len < 6 + 14)
            {
                n_err++;
            }
            else
            {
                len -= 6;

                if (logged < 12)
                {
                    log_printf("  RX %02lx:%02lx:%02lx:%02lx:%02lx:%02lx <- %02lx:%02lx:%02lx:%02lx:%02lx:%02lx typ %02lx%02lx len %lu\n",
                               (ULONG)fr[0], (ULONG)fr[1], (ULONG)fr[2],
                               (ULONG)fr[3], (ULONG)fr[4], (ULONG)fr[5],
                               (ULONG)fr[6], (ULONG)fr[7], (ULONG)fr[8],
                               (ULONG)fr[9], (ULONG)fr[10], (ULONG)fr[11],
                               (ULONG)fr[12], (ULONG)fr[13], len);
                    logged++;
                }

                if (memcmp(fr, mac, 6) == 0)
                    n_uni++;

                if (len >= 42 && fr[12] == 0x08 && fr[13] == 0x06 &&
                    fr[14] == 0 && fr[15] == 1 && fr[16] == 8 && fr[17] == 0 &&
                    fr[18] == 6 && fr[19] == 4 && fr[20] == 0 && fr[21] == 1 &&
                    memcmp(fr + 38, myip, 4) == 0)
                {
                    n_arp++;

                    if (n_tx < ECHO_MAXTX &&
                        !(txd[tx_cur].cmd_sts & 0x80000000UL) &&
                        !(txd[(tx_cur + 1) % ECHO_TXN].cmd_sts & 0x80000000UL))
                    {
                        txb = mem + 0x400 + tx_cur * 256;
                        memset(txb, 0, 64);
                        memcpy(txb, fr + 22, 6);
                        memcpy(txb + 6, mac, 6);
                        txb[12] = 0x08; txb[13] = 0x06;
                        txb[14] = 0;    txb[15] = 1;
                        txb[16] = 8;    txb[17] = 0;
                        txb[18] = 6;    txb[19] = 4;
                        txb[20] = 0;    txb[21] = 2;
                        memcpy(txb + 22, mac, 6);
                        memcpy(txb + 28, myip, 4);
                        memcpy(txb + 32, fr + 22, 6);
                        memcpy(txb + 38, fr + 28, 4);

                        txd[tx_cur].byte_cnt = 60;
                        txd[tx_cur].l4i_chk  = 0;
                        hw_sync();
                        txd[tx_cur].cmd_sts  = 0x80F82800UL;
                        hw_sync();
                        tx_cur = (tx_cur + 1) % ECHO_TXN;
                        hw_wr(pp, 0x0048, 0x00000001UL);
                        n_tx++;
                    }
                    else
                    {
                        n_drop++;
                    }
                }
            }

            /* Deskriptor an die Hardware zurueckgeben */
            rxd[rx_cur].byte_cnt = 0;
            hw_sync();
            rxd[rx_cur].cmd_sts = 0xA0000000UL;
            hw_sync();
            rx_cur = (rx_cur + 1) % ECHO_RXN;
        }

        /* wie txq_kick() in Linux */
        if (n_tx && !(hw_rd(pp, 0x0048) & 1) &&
            hw_rd(pp, 0x02c0) != phys + 0x100 + tx_cur * 16)
        {
            hw_wr(pp, 0x0048, 0x00000001UL);
            n_kick++;
        }

        IDOS->Delay(1);
    }

    log_printf("  INT_CAUSE=0x%08lx INT_CAUSE_EXT=0x%08lx\n",
               hw_rd(pp, 0x0060), hw_rd(pp, 0x0064));

    /* wie port_reset() in Linux */
    hw_wr(pp, 0x0048, 0x00000100UL);
    hw_wr(pp, 0x0280, 0x00000100UL);
    for (i = 0; i < TX_TIMEOUT; i++)
        if (!(hw_rd(pp, 0x0048) & 0x01)) { tx_idle = 1; break; }
    for (i = 0; i < TX_TIMEOUT; i++)
        if (!(hw_rd(pp, 0x0280) & 0x01)) { rx_idle = 1; break; }
    for (i = 0; i < TX_TIMEOUT; i++)
    {
        st = hw_rd(pp, 0x0044);
        if ((st & 0x480) == 0x400)
            break;
    }

    for (i = 0; i < ECHO_TXN; i++)
        if (txd[i].cmd_sts & 0x1)
            n_txerr++;

    log_printf("  Ergebnis: %lu Frames empfangen (%lu Unicast, %lu fehlerhaft)\n",
               n_rx, n_uni, n_err);
    log_printf("            %lu ARP-Anfragen fuer 192.168.1.80, %lu Antworten gesendet,\n",
               n_arp, n_tx);
    log_printf("            %lu verworfen, %lu TX-Neustarts, %lu TX-Fehler\n",
               n_drop, n_kick, n_txerr);
    log_printf("  TX-Queue %s, RX-Queue %s\n",
               tx_idle ? "steht" : "STEHT NICHT",
               rx_idle ? "steht" : "STEHT NICHT");

    if (!tx_idle || !rx_idle)
    {
        hw_wr(pp, 0x003c, pscr_old);
        hw_sync();
        log_printf("  WARNUNG: Queue steht nicht. Port deaktiviert,\n");
        log_printf("  Speicher wird NICHT freigegeben. Bitte Neustart.\n");
        return;
    }

    hw_wr(pp, 0x003c, pscr_old);
    hw_wr(pp, 0x02c0, 0);
    hw_wr(pp, 0x020c, rxptr_old);
    for (k = 0; k < 4; k++)
        hw_wr(sh, 0x1a00 + k * 4, uc_old[k]);
    hw_wr(pp, 0x004c, prio_old);
    hw_wr(pp, 0x0304, conf_old);
    hw_wr(pp, 0x0300, tok_old);
    hw_wr(pp, 0x005c, burst_old);
    hw_wr(pp, 0x0058, mtu_old);
    hw_wr(pp, 0x0050, rate_old);
    hw_wr(pp, 0x0004, pce_old);
    hw_wr(pp, 0x0000, pc_old);
    hw_wr(pp, 0x001c, sdma_old);
    hw_wr(pp, 0x0060, 0);
    hw_wr(pp, 0x0064, 0);
    hw_sync();

    log_printf("  Zustand danach: SERIAL_CTRL=0x%08lx PORT_CONFIG=0x%08lx\n",
               hw_rd(pp, 0x003c), hw_rd(pp, 0x0000));

    oldstack = IExec->SuperState();
    mmu->SetMemoryAttrs(mem, size,
        attr_old & ~(MEMATTRF_REFERENCED | MEMATTRF_CHANGED));
    IExec->UserState(oldstack);
    IExec->FreeVec(mem);
    log_printf("  Speicher freigegeben, ECHO-Test beendet\n");
}

/* liefert den Wert hinter einer Option ("-ip 192.168.1.80") oder NULL */
static const char *opt_value(int argc, char **argv, const char *name)
{
    int i;

    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return NULL;
}
int main(int argc, char **argv)
{
    const char *log_filename = "RAM:pegasos2-gbit.log";

    if (argc >= 3 &&
        strcmp(argv[1], "-log") == 0)
    {
        log_filename = argv[2];
    }

    if (!log_open(log_filename))
        return 10;

    log_printf("\n");
    log_printf("============================================================\n");
    log_printf("Pegasos2-Gigabit Diagnose gestartet\n");
    log_printf("Logdatei: %s\n", log_filename);
    log_printf("============================================================\n");
    struct Library *ExpansionBase;
    struct PCIIFace *IPCI;
    struct PCIDevice *dev;

    ULONG attrs_before;
    ULONG attrs_after;
    APTR oldStack;
    struct MMUIFace *IMMU;

    log_printf("Pegasos II MV64361 mapping diagnostic\n");
    log_printf("--------------------------------------\n");

    ExpansionBase = IExec->OpenLibrary("expansion.library", 0);
    if (!ExpansionBase) {
        log_printf("ERROR: expansion.library konnte nicht geoeffnet werden.\n");
        return 20;
    }

    IPCI = (struct PCIIFace *)
        IExec->GetInterface(
            ExpansionBase,
            "pci",
            1,
            NULL);

    if (!IPCI) {
        log_printf("ERROR: pci Interface konnte nicht geladen werden.\n");
        IExec->CloseLibrary(ExpansionBase);
        return 21;
    }

    IMMU = (struct MMUIFace *)IExec->GetInterface(
        (struct Library *)SysBase,
        "mmu",
        1,
        NULL);

    if (!IMMU) {
        log_printf("ERROR: mmu Interface konnte nicht geladen werden.\n");
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpansionBase);
        return 23;
    }

    log_printf("mmu Interface:     OK\n");
    log_printf("expansion.library: OK\n");
    log_printf("pci Interface:     OK\n");

    dev = IPCI->FindDeviceTags(
        FDT_VendorID, MARVELL_VENDOR_ID,
        FDT_DeviceID, MV64360_DEVICE_ID,
        TAG_DONE);

    if (!dev) {
        log_printf("Marvell MV64360 nicht gefunden.\n");

        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpansionBase);
        return 22;
    }

    log_printf("Marvell MV64360 gefunden!\n");
    log_printf("Vendor ID: 0x%04lx\n",
           (ULONG)dev->ReadConfigWord(0x00));

    log_printf("Device ID: 0x%04lx\n",
           (ULONG)dev->ReadConfigWord(0x02));

    log_printf("\nMV64361 Registerbereich:\n");
    log_printf("  Adresse: 0x%08lx\n", (ULONG)MV64361_REG_BASE);
    log_printf("  Groesse:  0x%08lx\n", (ULONG)MV64361_REG_SIZE);

    attrs_before =
        IMMU->GetMemoryAttrs((APTR)MV64361_REG_BASE, 0);

    log_printf("\nMemory Attributes vorher: 0x%08lx\n",
           attrs_before);

    log_printf("RemapMemory() wird aufgerufen...\n");

    oldStack = IExec->SuperState();

    IMMU->RemapMemory(
        (APTR)MV64361_REG_BASE,
        (APTR)MV64361_REG_BASE,
        MV64361_REG_SIZE,
        MEMATTRF_READ_WRITE |
        MEMATTRF_CACHEINHIBIT |
        MEMATTRF_GUARDED);

    IExec->UserState(oldStack);

    log_printf("RemapMemory(): zurueckgekehrt\n");

    attrs_after =
        IMMU->GetMemoryAttrs((APTR)MV64361_REG_BASE, 0);

    log_printf("Memory Attributes danach:  0x%08lx\n",
           attrs_after);

    log_printf("\nNoch kein Registerzugriff durchgefuehrt.\n");
    log_printf("Test erfolgreich beendet.\n");


   /*
    * PHY Interface Port Detail Scan
    */
   {
       int p;

       log_printf("\nPHY Interface Port Detail Scan:\n");
       log_printf("-------------------------------\n");

       for (p = 0; p < 2; p++)
       {
           volatile ULONG *port;

           port = (volatile ULONG *)(MV64361_REG_BASE + ETH_PORT_BASE + (p * ETH_PORT_STRIDE));

           log_printf("\nPort %d:\n", p);
           log_printf("  Basis              = 0x%08lx\n",
                      (ULONG)port);

           log_printf("  PORT_CONFIG        = 0x%08lx\n",
                      __builtin_bswap32(port[0x0000/4]));

           log_printf("  PORT_CONFIG_EXT    = 0x%08lx\n",
                      __builtin_bswap32(port[0x0004/4]));

           log_printf("  PORT_SERIAL_CTRL   = 0x%08lx\n",
                      __builtin_bswap32(port[0x003c/4]));

           log_printf("  PORT_STATUS        = 0x%08lx\n",
                      __builtin_bswap32(port[0x0044/4]));
       }
   }

    log_printf("\nMV64361 globale Register:\n");
    log_printf("--------------------------\n");

    log_printf("  CPU_CONFIG                  = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x0000,
                            "CPU_CONFIG"));

    log_printf("  INTERNAL_SPACE_BASE_ADDR   = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x0068,
                            "INTERNAL_SPACE_BASE_ADDR"));

    log_printf("  CPU_MODE                    = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x0120,
                            "CPU_MODE"));

    log_printf("  INTEGRATED_SRAM_BASE_ADDR = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x0268,
                            "INTEGRATED_SRAM_BASE_ADDR"));

    log_printf("  BASE_ADDR_ENABLE            = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x0278,
                            "BASE_ADDR_ENABLE"));

    log_printf("  SRAM_CONFIG                 = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x0380,
                            "SRAM_CONFIG"));

    log_printf("  ETH_BAR_4                  = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x2220,
                            "ETH_BAR_4"));

    log_printf("  ETH_SIZE_REG_4             = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x2224,
                            "ETH_SIZE_REG_4"));

    log_printf("  ETH_BASE_ADDR_ENABLE       = 0x%08lx\n",
               log_read_reg((volatile ULONG *)MV64361_REG_BASE,
                            0x2290,
                            "ETH_BASE_ADDR_ENABLE"));



    { int pn; for (pn = 0; pn < 2; pn++) {
        volatile ULONG *port;

        port = (volatile ULONG *)(MV64361_REG_BASE + ETH_PORT_BASE + (pn * ETH_PORT_STRIDE));

        log_printf("\nMV64361 Ethernet Port %d:\n", pn);
        log_printf("--------------------------\n");
        log_printf("  Basis = 0x%08lx\n",
                   (ULONG)port);

        log_printf("  PORT_CONFIG          = 0x%08lx\n",
                   log_read_reg(port, 0x0000, "PORT_CONFIG"));

        log_printf("  PORT_CONFIG_EXT      = 0x%08lx\n",
                   log_read_reg(port, 0x0004, "PORT_CONFIG_EXT"));

        {
            ULONG mac_low;
            ULONG mac_high;

            mac_low = log_read_reg(port, 0x0014, "MAC_ADDR_LOW");
            mac_high = log_read_reg(port, 0x0018, "MAC_ADDR_HIGH");

            log_printf("  MAC-Adresse         = %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                       (mac_high >> 24) & 0xff,
                       (mac_high >> 16) & 0xff,
                       (mac_high >> 8) & 0xff,
                       (mac_high >> 0) & 0xff,
                       (mac_low >> 8) & 0xff,
                       (mac_low >> 0) & 0xff);
        }

        log_printf("  SDMA_CONFIG          = 0x%08lx\n",
                   log_read_reg(port, 0x001c, "SDMA_CONFIG"));

        log_printf("  PORT_SERIAL_CONTROL  = 0x%08lx\n",
                   log_read_reg(port, 0x003c, "PORT_SERIAL_CONTROL"));

        log_printf("  PORT_STATUS          = 0x%08lx\n",
                   log_read_reg(port, 0x0044, "PORT_STATUS"));

        log_printf("  TXQ_COMMAND          = 0x%08lx\n",
                   log_read_reg(port, 0x0048, "TXQ_COMMAND"));

        log_printf("  INT_CAUSE            = 0x%08lx\n",
                   log_read_reg(port, 0x0060, "INT_CAUSE"));

        log_printf("  INT_CAUSE_EXT        = 0x%08lx\n",
                   log_read_reg(port, 0x0064, "INT_CAUSE_EXT"));

        log_printf("  INT_MASK             = 0x%08lx\n",
                   log_read_reg(port, 0x0068, "INT_MASK"));

        log_printf("  INT_MASK_EXT         = 0x%08lx\n",
                   log_read_reg(port, 0x006c, "INT_MASK_EXT"));


       /*
        * MMIO-Schreibtest:
        * INT_MASK Register Schreib-/Lesetest
        */
       {
           volatile ULONG *reg;
           ULONG old_value;
           ULONG test_value;
           ULONG readback;

           reg = port + (0x0068 / sizeof(ULONG));

           old_value = __builtin_bswap32(*reg);

           log_printf("\nMMIO-Schreibtest INT_MASK:\n");
           log_printf("----------------------------\n");
           log_printf("  vorher = 0x%08lx\n", old_value);

           test_value = 0x00000001;

           log_printf("  schreibe = 0x%08lx\n", test_value);

           *reg = __builtin_bswap32(test_value);

           readback = __builtin_bswap32(*reg);

           log_printf("  readback = 0x%08lx\n", readback);

           if (readback == test_value)
               log_printf("  Ergebnis: MMIO-Schreiben funktioniert\n");
           else
               log_printf("  Ergebnis: MMIO-Schreiben funktioniert NICHT\n");

           *reg = __builtin_bswap32(old_value);

           log_printf("  wiederhergestellt = 0x%08lx\n", __builtin_bswap32(*reg));
       }
    } }

 
    {
        ULONG w, pn;
        volatile ULONG *eth = (volatile ULONG *)(MV64361_REG_BASE + 0x2000);

        log_printf("\nPORT_STATUS dekodiert:\n");
        for (pn = 0; pn < 2; pn++)
        {
            volatile ULONG *pp = (volatile ULONG *)(MV64361_REG_BASE +
                                  ETH_PORT_BASE + (pn * ETH_PORT_STRIDE));
            decode_port_status(pn, log_read_reg(pp, 0x0044, "PORT_STATUS"));
        }

        log_printf("\nEthernet-DMA-Fenster:\n");
        log_printf("  WINDOW_BAR_ENABLE = 0x%08lx\n",
                   log_read_reg(eth, 0x0290, "WINDOW_BAR_ENABLE"));
        for (w = 0; w < 6; w++)
        {
            log_printf("  Fenster %lu: BASE=0x%08lx SIZE=0x%08lx PROTECT=0x%08lx\n",
                       w,
                       log_read_reg(eth, 0x0200 + (w << 3), "WINDOW_BASE"),
                       log_read_reg(eth, 0x0204 + (w << 3), "WINDOW_SIZE"),
                       log_read_reg(eth, 0x0294 + (w << 4), "WINDOW_PROTECT"));
        }
    }

    {
        ULONG pn, q;

        for (pn = 0; pn < 2; pn++)
        {
            volatile ULONG *pp = (volatile ULONG *)(MV64361_REG_BASE +
                                  ETH_PORT_BASE + (pn * ETH_PORT_STRIDE));

            log_printf("\nQueue-Register Port %lu:\n", pn);
            log_printf("  TXQ_COMMAND       (0x048) = 0x%08lx\n", log_read_reg(pp, 0x0048, "TXQ_COMMAND"));
            log_printf("  RXQ_COMMAND       (0x280) = 0x%08lx\n", log_read_reg(pp, 0x0280, "RXQ_COMMAND"));
            log_printf("  TXQ_FIX_PRIO_CONF (0x04c) = 0x%08lx\n", log_read_reg(pp, 0x004c, "TXQ_FIX_PRIO_CONF"));
            log_printf("  TX_BW_RATE        (0x050) = 0x%08lx\n", log_read_reg(pp, 0x0050, "TX_BW_RATE"));
            log_printf("  TX_BW_MTU         (0x058) = 0x%08lx\n", log_read_reg(pp, 0x0058, "TX_BW_MTU"));
            log_printf("  TX_BW_BURST       (0x05c) = 0x%08lx\n", log_read_reg(pp, 0x005c, "TX_BW_BURST"));
            log_printf("  TX_FIFO_URGENT    (0x074) = 0x%08lx\n", log_read_reg(pp, 0x0074, "TX_FIFO_URGENT"));
            log_printf("  TX_BW_RATE_MOVED  (0x0e0) = 0x%08lx\n", log_read_reg(pp, 0x00e0, "TX_BW_RATE_MOVED"));
            log_printf("  TX_BW_MTU_MOVED   (0x0e8) = 0x%08lx\n", log_read_reg(pp, 0x00e8, "TX_BW_MTU_MOVED"));
            log_printf("  TX_BW_BURST_MOVED (0x0ec) = 0x%08lx\n", log_read_reg(pp, 0x00ec, "TX_BW_BURST_MOVED"));
            for (q = 0; q < 8; q++)
                log_printf("  RXQ_CURRENT_DESC_PTR(%lu) = 0x%08lx\n", q,
                           log_read_reg(pp, 0x020c + (q << 4), "RXQ_CUR"));
            for (q = 0; q < 8; q++)
                log_printf("  TXQ_CURRENT_DESC_PTR(%lu) = 0x%08lx\n", q,
                           log_read_reg(pp, 0x02c0 + (q << 2), "TXQ_CUR"));
        }
    }

   volatile ULONG *smi;

        smi = (volatile ULONG *)(MV64361_REG_BASE + 0x2000);


        log_printf("\nMV64361 SMI / MDIO:\n");
        log_printf("------------------\n");
        log_printf("  Basis = 0x%08lx\n",
               (ULONG)(MV64361_REG_BASE + 0x2000));

        {
            ULONG phy_addr_reg;

            phy_addr_reg = log_read_reg(smi, 0x0000, "SMI PHY_ADDR");

            log_printf("  PHY_ADDR gesamt = 0x%08lx\n",
                       phy_addr_reg);

            log_printf("  PHY-Adresse Port 0 = %lu\n",
                       phy_addr_reg & 0x1f);

            log_printf("  PHY-Adresse Port 1 = %lu\n",
                       (phy_addr_reg >> 5) & 0x1f);

            /*
             * Testweise PHY-Adresse 7 fuer Port 1 setzen.
             * Nur Bits 5..9 werden veraendert.
             */
        }

        log_printf("  SMI_REG  [0x0004] = 0x%08lx\n",
                   log_read_reg(smi, 0x0004, "SMI REG"));

        {
            log_printf("\nMDIO PHY 7 (Port 1):\n");
            log_printf("----------------------\n");

            {
                ULONG scan_addr;
                ULONG phy_id1;
                ULONG phy_id2;

                for (scan_addr = 7; scan_addr <= 7; scan_addr++)
                {
                    log_printf("[SCAN] PHY %lu\n", scan_addr);

                    if (mdio_read(smi, scan_addr, 2, &phy_id1) && phy_id1 != 0x0000 && phy_id1 != 0xffff)
                    {
                        log_printf("[FOUND] PHY %lu: ID1=0x%04lx\n",
                                   scan_addr, phy_id1);

                        if (mdio_read(smi, scan_addr, 3, &phy_id2))
                        {
                            log_printf("[FOUND] PHY %lu: ID2=0x%04lx\n",
                                       scan_addr, phy_id2);
                        }
                        else
                        {
                            log_printf("[FOUND] PHY %lu: Register 3 konnte nicht gelesen werden.\n",
                                       scan_addr);
                        }
                    }
                    else
                    {
                        log_printf("[SCAN] PHY %lu keine gueltige Antwort\n",
                                   scan_addr);
                    }
                }
            }
        }

    

    log_printf("============================================================\n");
    {
        static const ULONG phy_regs[] = {
          0, 1, 2, 3, 4, 5, 6, 9, 10, 15, 16, 17, 20, 21, 27
      };
        ULONG k, v;
        log_printf("\nPHY 7 Registerdump:\n");
        for (k = 0; k < sizeof(phy_regs) / sizeof(phy_regs[0]); k++)
        {
            if (mdio_read(smi, 7, phy_regs[k], &v))
                log_printf("  REG %2lu = 0x%04lx\n", phy_regs[k], v);
        }
    }
    {
        ULONG st;

        if (mdio_read(smi, 7, 17, &st))
        {
            log_printf("\nPHY 7 Status dekodiert:\n");
            log_printf("  Link:       %s\n", (st & 0x0400) ? "UP" : "DOWN");
            log_printf("  Aufgeloest: %s\n", (st & 0x0800) ? "ja" : "nein");
            log_printf("  Speed:      %s\n",
                       ((st & 0xc000) == 0x8000) ? "1000" :
                       ((st & 0xc000) == 0x4000) ? "100" :
                       ((st & 0xc000) == 0x0000) ? "10" : "?");
            log_printf("  Duplex:     %s\n", (st & 0x2000) ? "voll" : "halb");
        }
    }
    if (argc >= 2 && strcmp(argv[argc - 1], "-dsoff") == 0)
    {
        ULONG ext, bmcr;

        log_printf("\nEXPERIMENT: REG 20 Bit 11 loeschen, PHY-Reset + Autoneg\n");

        if (mdio_read(smi, 7, 20, &ext))
        {
            mdio_write(smi, 7, 20, ext & ~0x0800);

            if (mdio_read(smi, 7, 20, &ext))
                log_printf("  REG 20 neu = 0x%04lx\n", ext);
        }

        if (mdio_read(smi, 7, 0, &bmcr))
            mdio_write(smi, 7, 0, bmcr | 0x9200);

        log_printf("  Reset + Autoneg angestossen. Programm in ca. 5 s ohne -dsoff erneut starten.\n");
    }

    if (argc >= 2 && strcmp(argv[argc - 1], "-g1000") == 0)
    {
        log_printf("\nEXPERIMENT: nur 1000BASE-T Vollduplex anbieten\n");
        mdio_write(smi, 7, 4, 0x0001);
        mdio_write(smi, 7, 9, 0x0200);
        mdio_write(smi, 7, 0, 0x1200);
        log_printf("  Autoneg neu gestartet. Nach ca. 5 s ohne Argument erneut starten.\n");
    }

    if (argc >= 2 && strcmp(argv[argc - 1], "-restore") == 0)
    {
        log_printf("\nPHY-Ausgangszustand wiederherstellen (REG 4/9/20/0)\n");
        mdio_write(smi, 7, 4, 0x01e1);
        mdio_write(smi, 7, 9, 0x0f00);
        mdio_write(smi, 7, 20, 0x0c60);
        mdio_write(smi, 7, 0, 0x1200);
        log_printf("  Autoneg neu gestartet.\n");
    }

    if (argc >= 2 && strcmp(argv[argc - 1], "-txprep") == 0)
    {
        struct tx_desc_be
        {
            UWORD byte_cnt;
            UWORD l4i_chk;
            ULONG cmd_sts;
            ULONG next_desc_ptr;
            ULONG buf_ptr;
        };

        static const UBYTE mac[6] = { 0x00, 0x0b, 0x2f, 0x6e, 0xf1, 0x3f };
        struct DMAEntry dmalist[8];
        struct tx_desc_be *desc;
        UBYTE *mem, *frame;
        ULONG size = 4096, n, i, j, attr_old, attr_new, phys = 0;
        APTR oldstack;

        log_printf("\nTX-VORBEREITUNG (nur Speicher, kein Zugriff auf den Port)\n");

        mem = (UBYTE *)IExec->AllocVecTags(size,
                  AVT_Type, MEMF_SHARED,
                  AVT_Contiguous, TRUE,
                  AVT_Lock, TRUE,
                  AVT_Alignment, 4096,
                  AVT_PhysicalAlignment, 4096,
                  AVT_ClearWithValue, 0,
                  TAG_DONE);

        if (!mem)
        {
            log_printf("  FEHLER: AllocVecTags fehlgeschlagen\n");
        }
        else
        {
            log_printf("  Puffer virtuell: 0x%08lx, Groesse %lu\n", (ULONG)mem, size);

            memset(dmalist, 0, sizeof(dmalist));
            n = IExec->StartDMA(mem, size, DMAF_NoModify | DMAF_ReadFromRAM);
            log_printf("  StartDMA: %lu Eintraege\n", n);
            if (n >= 1 && n <= 8)
                IExec->GetDMAList(mem, size, DMAF_NoModify | DMAF_ReadFromRAM, dmalist);
            IExec->EndDMA(mem, size, DMAF_NoModify | DMAF_ReadFromRAM);

            for (i = 0; i < n && i < 8; i++)
                log_printf("  DMA[%lu]: phys=0x%08lx laenge=%lu\n", i,
                           (ULONG)dmalist[i].PhysicalAddress, (ULONG)dmalist[i].BlockLength);

            phys = (ULONG)dmalist[0].PhysicalAddress;

            if (n != 1 || dmalist[0].BlockLength < size)
            {
                log_printf("  ABBRUCH: Puffer nicht in einem Stueck\n");
            }
            else if (phys + size > 0x40000000UL)
            {
                log_printf("  ABBRUCH: Puffer liegt ueber 1 GB (ausserhalb der DMA-Fenster)\n");
            }
            else
            {
                attr_old = IMMU->GetMemoryAttrs(mem, 0);
                log_printf("  Attribute vorher:  0x%08lx\n", attr_old);

                oldstack = IExec->SuperState();
                IMMU->SetMemoryAttrs(mem, size,
                    MEMATTRF_READ_WRITE | MEMATTRF_CACHEINHIBIT |
                    MEMATTRF_COHERENT | MEMATTRF_GUARDED);
                IExec->UserState(oldstack);

                attr_new = IMMU->GetMemoryAttrs(mem, 0);
                log_printf("  Attribute nachher: 0x%08lx\n", attr_new);

                memset(mem, 0, size);
                desc  = (struct tx_desc_be *)mem;
                frame = mem + 256;

                /* ARP-Probe: Broadcast, Sender-IP 0.0.0.0, Ziel 192.168.1.1 */
                memset(frame, 0xff, 6);
                memcpy(frame + 6, mac, 6);
                frame[12] = 0x08; frame[13] = 0x06;
                frame[14] = 0x00; frame[15] = 0x01;
                frame[16] = 0x08; frame[17] = 0x00;
                frame[18] = 6;    frame[19] = 4;
                frame[20] = 0x00; frame[21] = 0x01;
                memcpy(frame + 22, mac, 6);
                frame[38] = 192;  frame[39] = 168; frame[40] = 1; frame[41] = 1;

                desc->byte_cnt      = 60;
                desc->l4i_chk       = 0;
                desc->next_desc_ptr = phys;
                desc->buf_ptr       = phys + 256;
                desc->cmd_sts       = 0;

                log_printf("  Deskriptor @ phys 0x%08lx:\n", phys);
                log_printf("    byte_cnt=%lu cmd_sts=0x%08lx next=0x%08lx buf=0x%08lx\n",
                           (ULONG)desc->byte_cnt, desc->cmd_sts,
                           desc->next_desc_ptr, desc->buf_ptr);
                log_printf("    cmd_sts spaeter (Sendefall): 0x%08lx\n", 0x80F82800UL);
                log_printf("  Frame (60 Byte, erste 48):\n");
                for (i = 0; i < 48; i += 16)
                {
                    log_printf("    %02lx:", i);
                    for (j = 0; j < 16; j++)
                        log_printf(" %02lx", (ULONG)frame[i + j]);
                    log_printf("\n");
                }

                oldstack = IExec->SuperState();
                IMMU->SetMemoryAttrs(mem, size,
                    attr_old & ~(MEMATTRF_REFERENCED | MEMATTRF_CHANGED));
                IExec->UserState(oldstack);
                log_printf("  Attribute zurueck: 0x%08lx\n", IMMU->GetMemoryAttrs(mem, 0));
            }

            IExec->FreeVec(mem);
            log_printf("  Speicher freigegeben\n");
        }
    }

    if (argc >= 3 && strcmp(argv[argc - 2], "-txtest") == 0 &&
        strcmp(argv[argc - 1], "JA") == 0)
        tx_test(IMMU, smi);

    if (argc >= 3 && strcmp(argv[argc - 2], "-rxtest") == 0 &&
        strcmp(argv[argc - 1], "JA") == 0)
        rx_test(IMMU, smi);

    if (argc >= 3 && strcmp(argv[argc - 2], "-echotest") == 0 &&
        strcmp(argv[argc - 1], "JA") == 0)
        echo_test(IMMU, smi);

    {
        const char *hw = opt_value(argc, argv, "-hwtest");
        const char *ip = opt_value(argc, argv, "-ip");

        if (hw && strcmp(hw, "JA") == 0)
            mv_selftest(IExec, IMMU, log_printf);
    }

    IExec->DropInterface((struct Interface *)IMMU);
    IExec->DropInterface((struct Interface *)IPCI);
    IExec->CloseLibrary(ExpansionBase);

    log_printf("Pegasos2-Gigabit Diagnose beendet\n");
    log_printf("============================================================\n");

    log_close();

    return 0;
}
