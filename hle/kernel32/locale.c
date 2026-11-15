/* SPDX-License-Identifier: MIT */
/*
 * kernel32: code pages, character types, string mapping and comparison,
 * and locale information, for one locale: English (United States), code
 * page 1252, as the C runtime of the time asks for it.
 */
#include <string.h>

#include "k32.h"

#define LOCALE_EN_US 0x0409u

/* Code page 1252: 0x80..0x9f differ from Latin-1. */
static const uint16_t cp1252_high[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

static uint16_t to_wide(uint8_t b)
{
    return b >= 0x80 && b < 0xa0 ? cp1252_high[b - 0x80] : b;
}

static int from_wide(uint16_t w)
{
    if (w < 0x80 || (w >= 0xa0 && w <= 0xff)) {
        return w;
    }
    for (int i = 0; i < 32; i++) {
        if (cp1252_high[i] == w) {
            return 0x80 + i;
        }
    }
    return -1;
}

/* Latin-1 letters: 0xc0..0xde upper (but 0xd7), 0xdf..0xff lower (but 0xf7). */
static int is_upper(uint16_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7); }
static int is_lower(uint16_t c) { return (c >= 'a' && c <= 'z') || (c >= 0xdf && c <= 0xff && c != 0xf7); }
static uint16_t lower(uint16_t c) { return is_upper(c) ? (uint16_t)(c + 32) : c; }
static uint16_t upper(uint16_t c) { return is_lower(c) && c != 0xdf && c != 0xff ? (uint16_t)(c - 32) : c; }

/* CT_CTYPE1 flags. */
static uint16_t ctype1(uint16_t c)
{
    uint16_t f = 0;
    if (is_upper(c)) {
        f |= 0x0001 | 0x0100;
    }
    if (is_lower(c)) {
        f |= 0x0002 | 0x0100;
    }
    if (c >= '0' && c <= '9') {
        f |= 0x0004;
    }
    if (c == ' ' || (c >= 9 && c <= 13) || c == 0xa0) {
        f |= 0x0008;
    }
    if (c == ' ' || c == '\t' || c == 0xa0) {
        f |= 0x0040;
    }
    if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0)) {
        f |= 0x0020;
    }
    if ((c > 0x20 && c < 0x7f && !(f & 0x0104)) || (c > 0xa0 && c <= 0xbf) || c == 0xd7 || c == 0xf7) {
        f |= 0x0010;
    }
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
        f |= 0x0080;
    }
    return f;
}

static void k_GetACP(rc_cpu *c) { RC_RETURN(c, 1252, 0); }
static void k_GetOEMCP(rc_cpu *c) { RC_RETURN(c, 437, 0); }
static void k_IsValidCodePage(rc_cpu *c)
{
    uint32_t cp = RC_ARG(c, 0);
    RC_RETURN(c, cp == 1252 || cp == 437 || cp == 850, 1);
}

static void k_GetCPInfo(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    memset(rc_ptr(p), 0, 18);
    rc_st32(p, 1);       /* MaxCharSize */
    rc_st8(p + 4, '?'); /* DefaultChar */
    RC_RETURN(c, 1, 2);
}

static void k_GetUserDefaultLCID(rc_cpu *c) { RC_RETURN(c, LOCALE_EN_US, 0); }
static void k_GetUserDefaultLangID(rc_cpu *c) { RC_RETURN(c, LOCALE_EN_US, 0); }
static void k_IsValidLocale(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 0) == LOCALE_EN_US, 2); }

static void k_EnumSystemLocalesA(rc_cpu *c)
{
    uint32_t fn = RC_ARG(c, 0);
    uint32_t name = rc_proc_str("00000409");
    uint32_t args[1] = {name};
    rc_call_guest(c, fn, 1, args);
    RC_RETURN(c, 1, 2);
}

static const char *locale_info(uint32_t type)
{
    switch (type & 0xffffu) {
    case 0x0001: return "0409";           /* LOCALE_ILANGUAGE */
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";            /* LOCALE_SABBREVLANGNAME */
    case 0x0005: return "1";              /* LOCALE_ICOUNTRY */
    case 0x0006: return "United States";
    case 0x0007: return "USA";            /* LOCALE_SABBREVCTRYNAME */
    case 0x000b: return "437";            /* LOCALE_IDEFAULTCODEPAGE */
    case 0x000c: return ",";              /* LOCALE_SLIST */
    case 0x000e: return ".";              /* LOCALE_SDECIMAL */
    case 0x000f: return ",";              /* LOCALE_STHOUSAND */
    case 0x0010: return "3;0";            /* LOCALE_SGROUPING */
    case 0x0014: return "$";              /* LOCALE_SCURRENCY */
    case 0x0016: return ".";              /* LOCALE_SMONDECIMALSEP */
    case 0x0017: return ",";              /* LOCALE_SMONTHOUSANDSEP */
    case 0x0018: return "3;0";            /* LOCALE_SMONGROUPING */
    case 0x001d: return "/";              /* LOCALE_SDATE */
    case 0x001e: return ":";              /* LOCALE_STIME */
    case 0x001f: return "M/d/yyyy";       /* LOCALE_SSHORTDATE */
    case 0x0028: return "AM";
    case 0x0029: return "PM";
    case 0x1001: return "English";        /* LOCALE_SENGLANGUAGE */
    case 0x1002: return "United States";  /* LOCALE_SENGCOUNTRY */
    case 0x1004: return "1252";           /* LOCALE_IDEFAULTANSICODEPAGE */
    default: return NULL;
    }
}

static void k_GetLocaleInfoA(rc_cpu *c)
{
    const char *v = locale_info(RC_ARG(c, 1));
    uint32_t buf = RC_ARG(c, 2), n = RC_ARG(c, 3);
    if (!v) {
        rc_last_error = ERROR_INVALID_PARAMETER;
        RC_RETURN(c, 0, 4);
    }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (n == 0) {
        RC_RETURN(c, len, 4);
    }
    if (n < len) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 4);
    }
    rc_put_str(buf, n, v);
    RC_RETURN(c, len, 4);
}

static void k_GetLocaleInfoW(rc_cpu *c)
{
    const char *v = locale_info(RC_ARG(c, 1));
    uint32_t buf = RC_ARG(c, 2), n = RC_ARG(c, 3);
    if (!v) {
        rc_last_error = ERROR_INVALID_PARAMETER;
        RC_RETURN(c, 0, 4);
    }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (n == 0) {
        RC_RETURN(c, len, 4);
    }
    if (n < len) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 4);
    }
    for (uint32_t i = 0; i < len; i++) {
        rc_st16(buf + 2 * i, to_wide((uint8_t)v[i]));
    }
    RC_RETURN(c, len, 4);
}

/* Length of a counted string: -1 means NUL-terminated, counting the NUL. */
static uint32_t count_a(uint32_t s, uint32_t n)
{
    return n == 0xffffffffu ? (uint32_t)strlen(rc_str(s)) + 1 : n;
}

static uint32_t count_w(uint32_t s, uint32_t n)
{
    if (n != 0xffffffffu) {
        return n;
    }
    uint32_t k = 0;
    while (rc_ld16(s + 2 * k)) {
        k++;
    }
    return k + 1;
}

static void k_GetStringTypeA(rc_cpu *c)
{
    uint32_t type = RC_ARG(c, 1), s = RC_ARG(c, 2), out = RC_ARG(c, 4);
    uint32_t n = count_a(s, RC_ARG(c, 3));
    if (type != 1) {
        memset(rc_ptr(out), 0, 2 * n);
        RC_RETURN(c, 1, 5);
    }
    for (uint32_t i = 0; i < n; i++) {
        rc_st16(out + 2 * i, ctype1(rc_ld8(s + i)));
    }
    RC_RETURN(c, 1, 5);
}

static void k_GetStringTypeW(rc_cpu *c)
{
    uint32_t type = RC_ARG(c, 0), s = RC_ARG(c, 1), out = RC_ARG(c, 3);
    uint32_t n = count_w(s, RC_ARG(c, 2));
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = rc_ld16(s + 2 * i);
        rc_st16(out + 2 * i, type == 1 && ch <= 0xff ? ctype1(ch) : 0);
    }
    RC_RETURN(c, 1, 4);
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u

static uint16_t map_char(uint32_t flags, uint16_t ch)
{
    if (flags & LCMAP_LOWERCASE) {
        return lower(ch);
    }
    if (flags & LCMAP_UPPERCASE) {
        return upper(ch);
    }
    return ch;
}

static void k_LCMapStringA(rc_cpu *c)
{
    uint32_t flags = RC_ARG(c, 1), s = RC_ARG(c, 2), d = RC_ARG(c, 4), dn = RC_ARG(c, 5);
    uint32_t n = count_a(s, RC_ARG(c, 3));
    if (dn == 0) {
        RC_RETURN(c, n, 6);
    }
    if (dn < n) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 6);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = map_char(flags, rc_ld8(s + i));
        rc_st8(d + i, (uint8_t)(ch <= 0xff ? ch : '?'));
    }
    RC_RETURN(c, n, 6);
}

static void k_LCMapStringW(rc_cpu *c)
{
    uint32_t flags = RC_ARG(c, 1), s = RC_ARG(c, 2), d = RC_ARG(c, 4), dn = RC_ARG(c, 5);
    uint32_t n = count_w(s, RC_ARG(c, 3));
    if (dn == 0) {
        RC_RETURN(c, n, 6);
    }
    if (dn < n) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 6);
    }
    for (uint32_t i = 0; i < n; i++) {
        rc_st16(d + 2 * i, map_char(flags, rc_ld16(s + 2 * i)));
    }
    RC_RETURN(c, n, 6);
}

/* Counted strings without their terminating NUL, for comparison. */
static int compare(uint32_t flags, const uint16_t *a, uint32_t na, const uint16_t *b, uint32_t nb)
{
    for (uint32_t i = 0;; i++) {
        if (i == na || i == nb) {
            return na == nb ? 2 : (na < nb ? 1 : 3);
        }
        uint16_t x = a[i], y = b[i];
        if (flags & 1) { /* NORM_IGNORECASE */
            x = lower(x);
            y = lower(y);
        }
        if (x != y) {
            return x < y ? 1 : 3;
        }
    }
}

#define CMP_MAX 4096u

static uint32_t load_a(uint32_t s, uint32_t n, uint16_t *out)
{
    n = n == 0xffffffffu ? (uint32_t)strlen(rc_str(s)) : n;
    if (n > CMP_MAX) {
        n = CMP_MAX;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = to_wide(rc_ld8(s + i));
    }
    return n;
}

static uint32_t load_w(uint32_t s, uint32_t n, uint16_t *out)
{
    n = n == 0xffffffffu ? count_w(s, n) - 1 : n;
    if (n > CMP_MAX) {
        n = CMP_MAX;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = rc_ld16(s + 2 * i);
    }
    return n;
}

static void k_CompareStringA(rc_cpu *c)
{
    static uint16_t a[CMP_MAX], b[CMP_MAX];
    uint32_t na = load_a(RC_ARG(c, 2), RC_ARG(c, 3), a);
    uint32_t nb = load_a(RC_ARG(c, 4), RC_ARG(c, 5), b);
    RC_RETURN(c, compare(RC_ARG(c, 1), a, na, b, nb), 6);
}

static void k_CompareStringW(rc_cpu *c)
{
    static uint16_t a[CMP_MAX], b[CMP_MAX];
    uint32_t na = load_w(RC_ARG(c, 2), RC_ARG(c, 3), a);
    uint32_t nb = load_w(RC_ARG(c, 4), RC_ARG(c, 5), b);
    RC_RETURN(c, compare(RC_ARG(c, 1), a, na, b, nb), 6);
}

static void k_MultiByteToWideChar(rc_cpu *c)
{
    uint32_t s = RC_ARG(c, 2), d = RC_ARG(c, 4), dn = RC_ARG(c, 5);
    uint32_t n = count_a(s, RC_ARG(c, 3));
    if (dn == 0) {
        RC_RETURN(c, n, 6);
    }
    if (dn < n) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 6);
    }
    for (uint32_t i = 0; i < n; i++) {
        rc_st16(d + 2 * i, to_wide(rc_ld8(s + i)));
    }
    RC_RETURN(c, n, 6);
}

static void k_WideCharToMultiByte(rc_cpu *c)
{
    uint32_t s = RC_ARG(c, 2), d = RC_ARG(c, 4), dn = RC_ARG(c, 5), used = RC_ARG(c, 7);
    uint32_t n = count_w(s, RC_ARG(c, 3));
    if (dn == 0) {
        RC_RETURN(c, n, 8);
    }
    if (dn < n) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 8);
    }
    uint32_t defaulted = 0;
    for (uint32_t i = 0; i < n; i++) {
        int b = from_wide(rc_ld16(s + 2 * i));
        if (b < 0) {
            b = '?';
            defaulted = 1;
        }
        rc_st8(d + i, (uint8_t)b);
    }
    if (used) {
        rc_st32(used, defaulted);
    }
    RC_RETURN(c, n, 8);
}

const rc_import_def rc_k32_locale[] = {
    {K32, "GetACP", k_GetACP},
    {K32, "GetOEMCP", k_GetOEMCP},
    {K32, "IsValidCodePage", k_IsValidCodePage},
    {K32, "GetCPInfo", k_GetCPInfo},
    {K32, "GetUserDefaultLCID", k_GetUserDefaultLCID},
    {K32, "GetUserDefaultLangID", k_GetUserDefaultLangID},
    {K32, "IsValidLocale", k_IsValidLocale},
    {K32, "EnumSystemLocalesA", k_EnumSystemLocalesA},
    {K32, "GetLocaleInfoA", k_GetLocaleInfoA},
    {K32, "GetLocaleInfoW", k_GetLocaleInfoW},
    {K32, "GetStringTypeA", k_GetStringTypeA},
    {K32, "GetStringTypeW", k_GetStringTypeW},
    {K32, "LCMapStringA", k_LCMapStringA},
    {K32, "LCMapStringW", k_LCMapStringW},
    {K32, "CompareStringA", k_CompareStringA},
    {K32, "CompareStringW", k_CompareStringW},
    {K32, "MultiByteToWideChar", k_MultiByteToWideChar},
    {K32, "WideCharToMultiByte", k_WideCharToMultiByte},
};
const uint32_t rc_k32_locale_count = sizeof rc_k32_locale / sizeof rc_k32_locale[0];
