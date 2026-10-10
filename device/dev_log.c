/*
 * dev_log.c -- Debug-Ausgabe des Devices in eine Textdatei
 *
 * Jeder Kontext (Init, Open, BeginIO, ...) darf dbg_printf() rufen: Die
 * Zeile landet in einem kleinen Ringpuffer im Speicher. Nur der
 * Unit-Prozess schreibt den Puffer in die Datei (DOS-Aufrufe sind in
 * anderen Kontexten nicht erlaubt). Zeilen, die vor dem Start des
 * Prozesses entstehen, werden gepuffert und beim ersten Flush geschrieben.
 *
 * Bei einem Absturz gehen die noch nicht geschriebenen Zeilen verloren.
 * Der Puffer ist begrenzt; ist er voll, werden neue Zeilen verworfen.
 */

#define __USE_INLINE__ 1
#include "mv643xx.h"
#undef IDOS
#undef Open
#undef Write
#undef Close
#undef ChangeFilePosition

#define LOGBUF_SIZE 16384

static char            logbuf[LOGBUF_SIZE];
static volatile ULONG  log_head;      /* Schreibindex */
static volatile ULONG  log_tail;      /* Leseindex */
static ULONG           log_lost;
static struct MyDev   *dbg_md;
static struct Task    *dbg_task;
static ULONG           dbg_sigmask;
static BPTR            dbg_fh;

/* ---- kleiner Formatierer ohne C-Bibliothek ------------------------ */

static ULONG put_char(char *buf, ULONG max, ULONG pos, char c)
{
    if (pos + 1 < max)
        buf[pos] = c;
    return pos + 1;
}

/* Unterstuetzt: %% %c %s %d %i %u %x %X %p, optional '0', Breite, 'l' */
static ULONG dbg_vformat(char *buf, ULONG max, const char *fmt, va_list ap)
{
    static const char digits_lo[] = "0123456789abcdef";
    static const char digits_up[] = "0123456789ABCDEF";
    ULONG pos = 0;

    while (*fmt)
    {
        char c = *fmt++;
        int zero = 0, neg = 0, n, i, upper = 0, base = 10, width = 0, pad;
        char tmp[16];
        ULONG v = 0;

        if (c != '%')
        {
            pos = put_char(buf, max, pos, c);
            continue;
        }

        c = *fmt++;
        if (c == '0')
        {
            zero = 1;
            c = *fmt++;
        }
        while (c >= '0' && c <= '9')
        {
            width = width * 10 + (c - '0');
            c = *fmt++;
        }
        while (c == 'l')
            c = *fmt++;

        switch (c)
        {
            case '%':
                pos = put_char(buf, max, pos, '%');
                continue;

            case 'c':
                pos = put_char(buf, max, pos, (char)va_arg(ap, int));
                continue;

            case 's':
            {
                const char *s = va_arg(ap, const char *);
                if (!s)
                    s = "(null)";
                while (*s)
                    pos = put_char(buf, max, pos, *s++);
                continue;
            }

            case 'd':
            case 'i':
            {
                LONG sv = va_arg(ap, LONG);
                if (sv < 0)
                {
                    neg = 1;
                    v = (ULONG)(-sv);
                }
                else
                    v = (ULONG)sv;
                break;
            }

            case 'u':
                v = va_arg(ap, ULONG);
                break;

            case 'x':
                v = va_arg(ap, ULONG);
                base = 16;
                break;

            case 'X':
                v = va_arg(ap, ULONG);
                base = 16;
                upper = 1;
                break;

            case 'p':
                v = (ULONG)va_arg(ap, void *);
                base = 16;
                zero = 1;
                width = 8;
                pos = put_char(buf, max, pos, '0');
                pos = put_char(buf, max, pos, 'x');
                break;

            default:
                pos = put_char(buf, max, pos, '%');
                pos = put_char(buf, max, pos, c);
                continue;
        }

        n = 0;
        do
        {
            tmp[n++] = (upper ? digits_up : digits_lo)[v % (ULONG)base];
            v /= (ULONG)base;
        }
        while (v && n < (int)sizeof(tmp));

        pad = width - n - neg;

        if (neg && zero)
            pos = put_char(buf, max, pos, '-');
        while (pad-- > 0)
            pos = put_char(buf, max, pos, zero ? '0' : ' ');
        if (neg && !zero)
            pos = put_char(buf, max, pos, '-');
        for (i = n - 1; i >= 0; i--)
            pos = put_char(buf, max, pos, tmp[i]);
    }

    if (max)
        buf[(pos < max) ? pos : max - 1] = '\0';
    return (pos < max) ? pos : (max ? max - 1 : 0);
}

static ULONG dbg_format(char *buf, ULONG max, const char *fmt, ...)
{
    va_list ap;
    ULONG n;

    va_start(ap, fmt);
    n = dbg_vformat(buf, max, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- Ringpuffer --------------------------------------------------- */

void dbg_init(struct MyDev *md)
{
    dbg_md = md;
    log_head = 0;
    log_tail = 0;
    log_lost = 0;
    dbg_task = NULL;
    dbg_sigmask = 0;
    dbg_fh = 0;
}

static void dbg_put(const char *s, ULONG n)
{
    struct MyDev *md = dbg_md;
    ULONG i;

    if (!md)
        return;

    Forbid();
    for (i = 0; i < n; i++)
    {
        ULONG next = (log_head + 1) % LOGBUF_SIZE;

        if (next == log_tail)
        {
            log_lost++;
            break;
        }
        logbuf[log_head] = s[i];
        log_head = next;
    }
    Permit();

    if (dbg_task)
        Signal(dbg_task, dbg_sigmask);
}

void dbg_printf(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    ULONG n;

    if (!dbg_md)
        return;

    va_start(ap, fmt);
    n = dbg_vformat(line, sizeof(line), fmt, ap);
    va_end(ap);

    dbg_put(line, n);
}

/* ---- nur im Unit-Prozess aufrufen --------------------------------- */

void dbg_attach(struct MyDev *md, struct Task *task, ULONG sigmask)
{
    (void)md;
    dbg_sigmask = sigmask;
    dbg_task = task;
}

void dbg_detach(struct MyDev *md)
{
    (void)md;
    dbg_task = NULL;
    dbg_sigmask = 0;
}

void dbg_openfile(struct MyDev *dev)
{
    static const char head[] = "\n==== " VERS " ====\n";
    struct DOSIFace *dos = dev->IDOS;

    if (dbg_fh)
        return;

    dbg_fh = dos->Open(DEFAULT_LOGFILE, MODE_NEWFILE);
    if (dbg_fh)
    {
        dos->Close(dbg_fh);
        dbg_fh = dos->Open(DEFAULT_LOGFILE, MODE_READWRITE);
    }
    if (dbg_fh)
    {
        dos->Write(dbg_fh, head, sizeof(head) - 1);
    }
}

void dbg_flush(struct MyDev *md)
{
    char chunk[512];
    ULONG n;

    if (!dbg_fh)
        return;

    for (;;)
    {
        n = 0;

        Forbid();
        while (log_tail != log_head && n < sizeof(chunk))
        {
            chunk[n++] = logbuf[log_tail];
            log_tail = (log_tail + 1) % LOGBUF_SIZE;
        }
        Permit();

        if (n == 0)
            break;

        md->IDOS->Write(dbg_fh, chunk, n);
    }

    if (log_lost)
    {
        char msg[64];
        ULONG len;

        len = dbg_format(msg, sizeof(msg), "[Puffer voll, %lu Zeichen verworfen]\n",
                         (ULONG)log_lost);
        md->IDOS->Write(dbg_fh, msg, len);
        log_lost = 0;
    }
}

void dbg_closefile(struct MyDev *md)
{
    if (dbg_fh)
    {
        md->IDOS->Close(dbg_fh);
        dbg_fh = 0;
    }
}
