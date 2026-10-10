/*
 * mv64361_test.c -- Selbsttest der Hardwareschicht (mv64361.c)
 *
 * Wiederholt den Echotest ueber die neue API: 20 Sekunden lang werden
 * Frames empfangen und ARP-Anfragen fuer 192.168.1.80 beantwortet.
 */

#include <string.h>
#include <exec/types.h>
#include <interfaces/exec.h>
#include <proto/dos.h>
#include "mv64361.h"

#define SELFTEST_SECONDS 20
#define SELFTEST_MAXTX   64

void mv_selftest(struct ExecIFace *iexec, struct MMUIFace *mmu,
                 void (*logf)(const char *, ...))
{
    static const UBYTE myip[4] = { 192, 168, 1, 80 };
    struct mv_eth e;
    UBYTE mac[6];
    ULONG speed = 0, tick, len, n_rx = 0, n_arp = 0, n_tx = 0, n_drop = 0, logged = 0;
    int full = 0, r;
    const UBYTE *fr;
    UBYTE *txb;

    logf("\nHW-SELBSTTEST Port 1 (ARP fuer 192.168.1.80, %ld s)\n", (long)SELFTEST_SECONDS);
    logf("---------------------------------------------------\n");

    r = mv_eth_open(&e, iexec, mmu, logf);
    if (r)
    {
        logf("  mv_eth_open: Fehler %ld\n", (long)r);
        return;
    }

    mv_eth_get_mac(&e, mac);
    logf("  MAC %02lx:%02lx:%02lx:%02lx:%02lx:%02lx, PHY-Adresse %lu\n",
         (unsigned long)mac[0], (unsigned long)mac[1], (unsigned long)mac[2],
         (unsigned long)mac[3], (unsigned long)mac[4], (unsigned long)mac[5],
         (unsigned long)e.phy);
    logf("  DMA-Speicher virtuell 0x%08lx, physisch 0x%08lx\n",
         (unsigned long)e.mem, (unsigned long)e.phys);

    r = mv_eth_link(&e, &speed, &full);
    logf("  Link: %s, %lu Mbit, %s\n", (r == 1) ? "UP" : "DOWN",
         (unsigned long)speed, full ? "voll" : "halb");
    if (r != 1)
    {
        logf("  ABBRUCH: kein Link (Kabel stecken?)\n");
        mv_eth_close(&e);
        return;
    }

    r = mv_eth_start(&e);
    if (r)
    {
        logf("  mv_eth_start: Fehler %ld\n", (long)r);
        mv_eth_close(&e);
        return;
    }

    logf("  ECHO aktiv. JETZT vom x86 aus starten:\n");
    logf("    sudo arping -I enp3s0 -c 5 192.168.1.80\n");

    for (tick = 0; tick < SELFTEST_SECONDS * 50; tick++)
    {
        mv_eth_poll(&e);

        while (mv_eth_rx_peek(&e, &fr, &len))
        {
            n_rx++;

            if (logged < 12)
            {
                logf("  RX %02lx:%02lx:%02lx:%02lx:%02lx:%02lx <- %02lx:%02lx:%02lx:%02lx:%02lx:%02lx typ %02lx%02lx len %lu\n",
                     (unsigned long)fr[0], (unsigned long)fr[1], (unsigned long)fr[2],
                     (unsigned long)fr[3], (unsigned long)fr[4], (unsigned long)fr[5],
                     (unsigned long)fr[6], (unsigned long)fr[7], (unsigned long)fr[8],
                     (unsigned long)fr[9], (unsigned long)fr[10], (unsigned long)fr[11],
                     (unsigned long)fr[12], (unsigned long)fr[13], (unsigned long)len);
                logged++;
            }

            if (len >= 42 && fr[12] == 0x08 && fr[13] == 0x06 &&
                fr[14] == 0 && fr[15] == 1 && fr[16] == 8 && fr[17] == 0 &&
                fr[18] == 6 && fr[19] == 4 && fr[20] == 0 && fr[21] == 1 &&
                memcmp(fr + 38, myip, 4) == 0)
            {
                n_arp++;

                txb = (n_tx < SELFTEST_MAXTX) ? mv_eth_tx_buf(&e) : NULL;
                if (txb)
                {
                    memset(txb, 0, 64);
                    memcpy(txb, fr + 22, 6);        /* Ziel: Absender der Anfrage */
                    memcpy(txb + 6, mac, 6);
                    txb[12] = 0x08; txb[13] = 0x06;
                    txb[14] = 0;    txb[15] = 1;
                    txb[16] = 8;    txb[17] = 0;
                    txb[18] = 6;    txb[19] = 4;
                    txb[20] = 0;    txb[21] = 2;    /* Antwort */
                    memcpy(txb + 22, mac, 6);
                    memcpy(txb + 28, myip, 4);
                    memcpy(txb + 32, fr + 22, 6);
                    memcpy(txb + 38, fr + 28, 4);

                    if (mv_eth_tx_submit(&e, 60) == MV_OK)
                        n_tx++;
                    else
                        n_drop++;
                }
                else
                {
                    n_drop++;
                }
            }

            mv_eth_rx_release(&e);
        }

        IDOS->Delay(1);
    }

    r = mv_eth_stop(&e);

    logf("  Ergebnis: %lu Frames empfangen (%lu fehlerhaft verworfen)\n",
         (unsigned long)e.stats.rx_frames, (unsigned long)e.stats.rx_errors);
    logf("            %lu ARP-Anfragen fuer 192.168.1.80, %lu Antworten abgeschickt,\n",
         (unsigned long)n_arp, (unsigned long)n_tx);
    logf("            %lu verworfen, TX fertig %lu, TX-Fehler %lu\n",
         (unsigned long)n_drop, (unsigned long)e.stats.tx_frames,
         (unsigned long)e.stats.tx_errors);
    logf("            TX-Neustarts %lu, RX-Neustarts %lu\n",
         (unsigned long)e.stats.tx_kicks, (unsigned long)e.stats.rx_restarts);

    if (r)
    {
        logf("  WARNUNG: Queues stehen nicht (Fehler %ld).\n", (long)r);
        logf("  Speicher wird NICHT freigegeben. Bitte Neustart.\n");
        return;
    }

    r = mv_eth_close(&e);
    if (r)
        logf("  mv_eth_close: Fehler %ld, Speicher bleibt reserviert\n", (long)r);
    else
        logf("  Speicher freigegeben, HW-Selbsttest beendet\n");
}
