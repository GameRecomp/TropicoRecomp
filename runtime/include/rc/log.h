/* SPDX-License-Identifier: MIT */
/* Diagnostics: messages and fatal errors. */
#ifndef RC_LOG_H
#define RC_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RC_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define RC_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define RC_PRINTF(f, a)
#define RC_NORETURN __declspec(noreturn)
#else
#define RC_PRINTF(f, a)
#define RC_NORETURN
#endif

void rc_log(const char *fmt, ...) RC_PRINTF(1, 2);

/* Whether RC_TRACE (a comma-separated list) names `what`: "imports",
 * "files", ... */
int rc_trace(const char *what);
RC_NORETURN void rc_fatal(const char *fmt, ...) RC_PRINTF(1, 2);

/* Also tell the user about a fatal error some other way (a message box,
 * where the program has no terminal); called with the message once. */
void rc_set_fatal_hook(void (*hook)(const char *message));

#ifdef __cplusplus
}
#endif

#endif
