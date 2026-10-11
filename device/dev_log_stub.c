/*
 * dev_log_stub.c -- Logging deaktiviert fuer den normalen Build
 */

#define __USE_INLINE__ 1
#include "mv643xx.h"

void dbg_init(struct MyDev *md)
{
    (void)md;
}

void dbg_attach(struct MyDev *md, struct Task *task, ULONG sigmask)
{
    (void)md;
    (void)task;
    (void)sigmask;
}

void dbg_detach(struct MyDev *md)
{
    (void)md;
}

void dbg_openfile(struct MyDev *md)
{
    (void)md;
}

void dbg_closefile(struct MyDev *md)
{
    (void)md;
}

void dbg_flush(struct MyDev *md)
{
    (void)md;
}

void dbg_printf(const char *fmt, ...)
{
    (void)fmt;
}
