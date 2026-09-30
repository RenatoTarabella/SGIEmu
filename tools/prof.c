/*
 * prof.c - Profiler a campionamento per indy_boot (--prof), solo Windows
 *
 * Un thread ferma quello principale circa 1000 volte al secondo e ne legge
 * il RIP. I campioni nel codice del JIT si contano per blocco (pc MIPS di
 * inizio blocco), gli altri per funzione con i simboli del PDB (DbgHelp).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prof.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>

#define MAX_HOST 65536
#define MAX_JIT  65536

static R4k *cpu;
static HANDLE target, thread;
static volatile LONG running;
static uint64_t total, in_jit, jit_thunk;
static struct { uint64_t rip, n; } host[MAX_HOST];       /* indirizzo -> campioni */
static struct { uint64_t va, n; } jit[MAX_JIT];           /* blocco -> campioni */

static void add_host(uint64_t rip)
{
    uint32_t i = (uint32_t)(rip * 0x9E3779B97F4A7C15ull >> 48) & (MAX_HOST - 1);
    for (int k = 0; k < MAX_HOST; k++, i = (i + 1) & (MAX_HOST - 1))
        if (host[i].rip == rip || !host[i].n) { host[i].rip = rip; host[i].n++; return; }
}

static void add_jit(uint64_t va)
{
    uint32_t i = (uint32_t)(va * 0x9E3779B97F4A7C15ull >> 48) & (MAX_JIT - 1);
    for (int k = 0; k < MAX_JIT; k++, i = (i + 1) & (MAX_JIT - 1))
        if (jit[i].va == va || !jit[i].n) { jit[i].va = va; jit[i].n++; return; }
}

static DWORD WINAPI sampler(LPVOID arg)
{
    (void)arg;
    while (running) {
        Sleep(1);
        if (SuspendThread(target) == (DWORD)-1) continue;
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(target, &ctx)) {
            uint64_t va;
            total++;
            int r = r4k_jit_owner(cpu, (const void *)(uintptr_t)ctx.Rip, &va);
            if (r == 1) { in_jit++; add_jit(va); }
            else if (r == 2) { in_jit++; jit_thunk++; }
            else add_host(ctx.Rip);
        }
        ResumeThread(target);
    }
    return 0;
}

int prof_start(R4k *c)
{
    cpu = c;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                         0, FALSE, DUPLICATE_SAME_ACCESS))
        return -1;
    timeBeginPeriod(1);
    running = 1;
    thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    return thread ? 0 : -1;
}

typedef struct { char name[64]; uint64_t n; } Sym;

static int cmp_sym(const void *a, const void *b)
{
    uint64_t x = ((const Sym *)a)->n, y = ((const Sym *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

void prof_stop_print(void)
{
    if (!thread) return;
    running = 0;
    WaitForSingleObject(thread, INFINITE);
    timeEndPeriod(1);
    if (!total) return;

    /* funzioni dell'host */
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    static Sym syms[4096];
    int nsym = 0;
    for (int i = 0; i < MAX_HOST; i++) {
        if (!host[i].n) continue;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        memset(buf, 0, sizeof buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char *name = "?";
        if (SymFromAddr(proc, host[i].rip, &disp, si)) name = si->Name;
        int k;
        for (k = 0; k < nsym; k++) if (!strcmp(syms[k].name, name)) break;
        if (k == nsym) {
            if (nsym == 4096) continue;
            snprintf(syms[nsym].name, sizeof syms[nsym].name, "%s", name);
            syms[nsym++].n = 0;
        }
        syms[k].n += host[i].n;
    }
    qsort(syms, (size_t)nsym, sizeof *syms, cmp_sym);
    printf("profilo: %llu campioni, JIT %.1f%% (di cui fuori dai blocchi %.1f%%)\n",
           (unsigned long long)total, 100.0 * in_jit / total, 100.0 * jit_thunk / total);
    for (int k = 0; k < nsym && k < 25; k++)
        printf("  %5.1f%%  %s\n", 100.0 * syms[k].n / total, syms[k].name);

    /* blocchi del JIT */
    printf("blocchi JIT piu' caldi:\n");
    for (int shown = 0; shown < 15; shown++) {
        int best = -1;
        for (int i = 0; i < MAX_JIT; i++) if (jit[i].n && (best < 0 || jit[i].n > jit[best].n)) best = i;
        if (best < 0) break;
        printf("  %5.1f%%  %016llX\n", 100.0 * jit[best].n / total, (unsigned long long)jit[best].va);
        jit[best].n = 0;
    }
    SymCleanup(proc);
}

#else

int prof_start(R4k *c) { (void)c; return -1; }
void prof_stop_print(void) {}

#endif
