/* SPDX-License-Identifier: MIT */
/*
 * kernel32: the process, its threads (one), modules, the environment, time
 * and synchronisation. The program is single-threaded, so critical
 * sections and interlocked operations need no host synchronisation.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "k32.h"
#include "rc/log.h"
#include "rc/vm.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Pseudo module handles for DLLs we answer for: high, aligned, unmapped. */
#define MODULE_BASE 0x7a000000u
static const char *const known_modules[] = {
    "KERNEL32", "USER32", "GDI32", "DDRAW", "MSS32", "BINKW32", "ADVAPI32", "WINMM", "OLE32", "VERSION",
};
#define NMODULES (sizeof known_modules / sizeof known_modules[0])

static int module_index(const char *name)
{
    char base[64];
    const char *s = strrchr(name, '\\');
    snprintf(base, sizeof base, "%s", s ? s + 1 : name);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = 0;
    }
    for (char *p = base; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 32);
        }
    }
    for (unsigned i = 0; i < NMODULES; i++) {
        if (strcmp(base, known_modules[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void k_GetVersion(rc_cpu *c)
{
    /* Windows XP SP3: 5.1, build 2600. */
    RC_RETURN(c, 0x0a280105u, 0);
}

static void k_GetVersionExA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t size = rc_ld32(p);
    if (size < 148) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 1);
    }
    rc_st32(p + 4, 5);
    rc_st32(p + 8, 1);
    rc_st32(p + 12, 2600);
    rc_st32(p + 16, 2); /* VER_PLATFORM_WIN32_NT */
    memset(rc_ptr(p + 20), 0, 128);
    rc_put_str(p + 20, 128, "Service Pack 3");
    RC_RETURN(c, 1, 1);
}

static void k_GetCommandLineA(rc_cpu *c) { RC_RETURN(c, rc_proc.cmdline, 0); }

static void k_GetStartupInfoA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    memset(rc_ptr(p), 0, 68);
    rc_st32(p, 68);
    RC_RETURN(c, 0, 1);
}

static void k_GetModuleHandleA(rc_cpu *c)
{
    uint32_t name = RC_ARG(c, 0);
    if (name == 0) {
        RC_RETURN(c, rc_proc.image_base, 1);
    }
    int i = module_index(rc_str(name));
    if (i >= 0) {
        RC_RETURN(c, MODULE_BASE + 0x100000u * (uint32_t)i, 1);
    }
    rc_last_error = 126; /* ERROR_MOD_NOT_FOUND */
    RC_RETURN(c, 0, 1);
}

static void k_LoadLibraryA(rc_cpu *c)
{
    int i = module_index(rc_str(RC_ARG(c, 0)));
    if (i >= 0) {
        RC_RETURN(c, MODULE_BASE + 0x100000u * (uint32_t)i, 1);
    }
    rc_log("LoadLibraryA(\"%s\"): not provided", rc_str(RC_ARG(c, 0)));
    rc_last_error = 126;
    RC_RETURN(c, 0, 1);
}

static void k_FreeLibrary(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static void k_GetProcAddress(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), name = RC_ARG(c, 1);
    const char *dll = NULL;
    if (h >= MODULE_BASE && (h - MODULE_BASE) % 0x100000u == 0 && (h - MODULE_BASE) / 0x100000u < NMODULES) {
        dll = known_modules[(h - MODULE_BASE) / 0x100000u];
    }
    if (dll == NULL || name < 0x10000) {
        rc_last_error = 127; /* ERROR_PROC_NOT_FOUND */
        RC_RETURN(c, 0, 2);
    }
    /* The name must outlive the guest string. */
    const char *n = rc_str(rc_proc_str(rc_str(name)));
    RC_RETURN(c, rc_import_address(dll, n), 2);
}

static void k_GetModuleFileNameA(rc_cpu *c)
{
    uint32_t buf = RC_ARG(c, 1), n = RC_ARG(c, 2);
    RC_RETURN(c, rc_put_str(buf, n, rc_str(rc_proc.module_path)), 3);
}

static void k_ExitProcess(rc_cpu *c) { rc_exit(RC_ARG(c, 0)); }

static void k_TerminateProcess(rc_cpu *c)
{
    if (RC_ARG(c, 0) == 0xffffffffu) {
        rc_exit(RC_ARG(c, 1));
    }
    RC_RETURN(c, 0, 2);
}

static void k_FatalAppExitA(rc_cpu *c)
{
    rc_log("FatalAppExitA: %s", rc_str(RC_ARG(c, 1)));
    rc_exit(255);
}

static void k_GetCurrentProcess(rc_cpu *c) { RC_RETURN(c, 0xffffffffu, 0); }
static void k_GetCurrentThread(rc_cpu *c) { RC_RETURN(c, 0xfffffffeu, 0); }
static void k_GetCurrentThreadId(rc_cpu *c) { RC_RETURN(c, 1, 0); }

static void k_GetLastError(rc_cpu *c) { RC_RETURN(c, rc_last_error, 0); }

static void k_SetLastError(rc_cpu *c)
{
    rc_last_error = RC_ARG(c, 0);
    RC_RETURN(c, 0, 1);
}

/* ---- thread-local storage ---- */
#define TLS_SLOTS 64u
static uint32_t tls_used[TLS_SLOTS], tls_value[TLS_SLOTS];

static void k_TlsAlloc(rc_cpu *c)
{
    for (uint32_t i = 0; i < TLS_SLOTS; i++) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            tls_value[i] = 0;
            RC_RETURN(c, i, 0);
        }
    }
    RC_RETURN(c, 0xffffffffu, 0);
}

static void k_TlsFree(rc_cpu *c)
{
    uint32_t i = RC_ARG(c, 0);
    if (i < TLS_SLOTS) {
        tls_used[i] = 0;
    }
    RC_RETURN(c, i < TLS_SLOTS, 1);
}

static void k_TlsGetValue(rc_cpu *c)
{
    uint32_t i = RC_ARG(c, 0);
    rc_last_error = ERROR_SUCCESS;
    RC_RETURN(c, i < TLS_SLOTS ? tls_value[i] : 0, 1);
}

static void k_TlsSetValue(rc_cpu *c)
{
    uint32_t i = RC_ARG(c, 0);
    if (i < TLS_SLOTS) {
        tls_value[i] = RC_ARG(c, 1);
    }
    RC_RETURN(c, i < TLS_SLOTS, 2);
}

/* ---- synchronisation: one thread ---- */
static void k_InitializeCriticalSection(rc_cpu *c)
{
    memset(rc_ptr(RC_ARG(c, 0)), 0, 24);
    rc_st32(RC_ARG(c, 0) + 4, 0xffffffffu); /* LockCount */
    RC_RETURN(c, 0, 1);
}

static void k_CriticalSectionNop(rc_cpu *c) { RC_RETURN(c, 0, 1); }

static void k_InterlockedIncrement(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), v = rc_ld32(p) + 1;
    rc_st32(p, v);
    RC_RETURN(c, v, 1);
}

static void k_InterlockedDecrement(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), v = rc_ld32(p) - 1;
    rc_st32(p, v);
    RC_RETURN(c, v, 1);
}

static uint32_t next_event = 0x2000;

static void k_CreateEventA(rc_cpu *c)
{
    next_event += 4;
    RC_RETURN(c, next_event, 4);
}

/* ---- time ---- */
static uint64_t now_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static uint64_t start_ms;

static void k_GetTickCount(rc_cpu *c)
{
    if (!start_ms) {
        start_ms = now_ms() - 1000000; /* as if the machine had been up for a while */
    }
    RC_RETURN(c, (uint32_t)(now_ms() - start_ms), 0);
}

static void k_Sleep(rc_cpu *c)
{
    uint32_t ms = RC_ARG(c, 0);
#if defined(_WIN32)
    Sleep(ms); /* the host's */
#else
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
    RC_RETURN(c, 0, 1);
}

static void k_GetTimeZoneInformation(rc_cpu *c)
{
    memset(rc_ptr(RC_ARG(c, 0)), 0, 172);
    RC_RETURN(c, 0, 1); /* TIME_ZONE_ID_UNKNOWN: no daylight saving */
}

/* ---- environment ---- */
#define ENV_MAX 64
static char env[ENV_MAX][256];
static int nenv;
static uint32_t env_block_a, env_block_w;

static void env_init(void)
{
    if (nenv) {
        return;
    }
    static const char *const init[] = {
        "ALLUSERSPROFILE=C:\\Documents and Settings\\All Users",
        "COMSPEC=C:\\WINDOWS\\system32\\cmd.exe",
        "OS=Windows_NT",
        "PATH=C:\\WINDOWS\\system32;C:\\WINDOWS",
        "SYSTEMROOT=C:\\WINDOWS",
        "TEMP=C:\\WINDOWS\\TEMP",
        "TMP=C:\\WINDOWS\\TEMP",
        "WINDIR=C:\\WINDOWS",
    };
    for (unsigned i = 0; i < sizeof init / sizeof init[0]; i++) {
        snprintf(env[nenv++], sizeof env[0], "%s", init[i]);
    }
}

static void k_GetEnvironmentStrings(rc_cpu *c)
{
    env_init();
    if (!env_block_a) {
        uint32_t n = 1;
        for (int i = 0; i < nenv; i++) {
            n += (uint32_t)strlen(env[i]) + 1;
        }
        env_block_a = rc_proc_alloc(n);
        uint32_t p = env_block_a;
        for (int i = 0; i < nenv; i++) {
            p += rc_put_str(p, 256, env[i]) + 1;
        }
        rc_st8(p, 0);
    }
    RC_RETURN(c, env_block_a, 0);
}

static void k_GetEnvironmentStringsW(rc_cpu *c)
{
    env_init();
    if (!env_block_w) {
        uint32_t n = 1;
        for (int i = 0; i < nenv; i++) {
            n += (uint32_t)strlen(env[i]) + 1;
        }
        env_block_w = rc_proc_alloc(2 * n);
        uint32_t p = env_block_w;
        for (int i = 0; i < nenv; i++) {
            for (const char *s = env[i]; *s; s++) {
                rc_st16(p, (uint8_t)*s);
                p += 2;
            }
            rc_st16(p, 0);
            p += 2;
        }
        rc_st16(p, 0);
    }
    RC_RETURN(c, env_block_w, 0);
}

static void k_FreeEnvironmentStrings(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static int env_find(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < nenv; i++) {
        if (strlen(env[i]) > n && env[i][n] == '=') {
            int same = 1;
            for (size_t k = 0; k < n; k++) {
                char a = env[i][k], b = name[k];
                if (a >= 'a' && a <= 'z') {
                    a = (char)(a - 32);
                }
                if (b >= 'a' && b <= 'z') {
                    b = (char)(b - 32);
                }
                if (a != b) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                return i;
            }
        }
    }
    return -1;
}

static void k_GetEnvironmentVariableA(rc_cpu *c)
{
    env_init();
    int i = env_find(rc_str(RC_ARG(c, 0)));
    if (i < 0) {
        rc_last_error = ERROR_ENVVAR_NOT_FOUND;
        RC_RETURN(c, 0, 3);
    }
    const char *v = strchr(env[i], '=') + 1;
    uint32_t len = (uint32_t)strlen(v);
    if (len + 1 > RC_ARG(c, 2)) {
        RC_RETURN(c, len + 1, 3);
    }
    RC_RETURN(c, rc_put_str(RC_ARG(c, 1), RC_ARG(c, 2), v), 3);
}

static void k_SetEnvironmentVariableA(rc_cpu *c)
{
    env_init();
    const char *name = rc_str(RC_ARG(c, 0)), *value = rc_str(RC_ARG(c, 1));
    int i = env_find(name);
    if (value == NULL) {
        if (i >= 0) {
            memmove(env[i], env[i + 1], sizeof env[0] * (size_t)(nenv - i - 1));
            nenv--;
        }
    } else {
        if (i < 0) {
            if (nenv == ENV_MAX) {
                RC_RETURN(c, 0, 2);
            }
            i = nenv++;
        }
        snprintf(env[i], sizeof env[0], "%s=%s", name, value);
    }
    RC_RETURN(c, 1, 2);
}

static void k_SetConsoleCtrlHandler(rc_cpu *c) { RC_RETURN(c, 1, 2); }

static void k_UnhandledExceptionFilter(rc_cpu *c)
{
    rc_log("UnhandledExceptionFilter: the program crashed");
    RC_RETURN(c, 1, 1); /* EXCEPTION_EXECUTE_HANDLER: the process ends */
}

static void k_RaiseException(rc_cpu *c)
{
    rc_fatal("RaiseException(%#x, flags %#x) from %#010x: structured exceptions are not "
             "modelled yet",
             RC_ARG(c, 0), RC_ARG(c, 1), rc_ld32(c->r[RC_ESP]));
}

static void k_RtlUnwind(rc_cpu *c)
{
    rc_fatal("RtlUnwind from %#010x: structured exceptions are not modelled yet",
             rc_ld32(c->r[RC_ESP]));
}

const rc_import_def rc_k32_process[] = {
    {K32, "GetVersion", k_GetVersion},
    {K32, "GetVersionExA", k_GetVersionExA},
    {K32, "GetCommandLineA", k_GetCommandLineA},
    {K32, "GetStartupInfoA", k_GetStartupInfoA},
    {K32, "GetModuleHandleA", k_GetModuleHandleA},
    {K32, "LoadLibraryA", k_LoadLibraryA},
    {K32, "FreeLibrary", k_FreeLibrary},
    {K32, "GetProcAddress", k_GetProcAddress},
    {K32, "GetModuleFileNameA", k_GetModuleFileNameA},
    {K32, "ExitProcess", k_ExitProcess},
    {K32, "TerminateProcess", k_TerminateProcess},
    {K32, "FatalAppExitA", k_FatalAppExitA},
    {K32, "GetCurrentProcess", k_GetCurrentProcess},
    {K32, "GetCurrentThread", k_GetCurrentThread},
    {K32, "GetCurrentThreadId", k_GetCurrentThreadId},
    {K32, "GetLastError", k_GetLastError},
    {K32, "SetLastError", k_SetLastError},
    {K32, "TlsAlloc", k_TlsAlloc},
    {K32, "TlsFree", k_TlsFree},
    {K32, "TlsGetValue", k_TlsGetValue},
    {K32, "TlsSetValue", k_TlsSetValue},
    {K32, "InitializeCriticalSection", k_InitializeCriticalSection},
    {K32, "DeleteCriticalSection", k_CriticalSectionNop},
    {K32, "EnterCriticalSection", k_CriticalSectionNop},
    {K32, "LeaveCriticalSection", k_CriticalSectionNop},
    {K32, "InterlockedIncrement", k_InterlockedIncrement},
    {K32, "InterlockedDecrement", k_InterlockedDecrement},
    {K32, "CreateEventA", k_CreateEventA},
    {K32, "GetTickCount", k_GetTickCount},
    {K32, "Sleep", k_Sleep},
    {K32, "GetTimeZoneInformation", k_GetTimeZoneInformation},
    {K32, "GetEnvironmentStrings", k_GetEnvironmentStrings},
    {K32, "GetEnvironmentStringsW", k_GetEnvironmentStringsW},
    {K32, "FreeEnvironmentStringsA", k_FreeEnvironmentStrings},
    {K32, "FreeEnvironmentStringsW", k_FreeEnvironmentStrings},
    {K32, "GetEnvironmentVariableA", k_GetEnvironmentVariableA},
    {K32, "SetEnvironmentVariableA", k_SetEnvironmentVariableA},
    {K32, "SetConsoleCtrlHandler", k_SetConsoleCtrlHandler},
    {K32, "UnhandledExceptionFilter", k_UnhandledExceptionFilter},
    {K32, "RaiseException", k_RaiseException},
    {K32, "RtlUnwind", k_RtlUnwind},
};
const uint32_t rc_k32_process_count = sizeof rc_k32_process / sizeof rc_k32_process[0];
