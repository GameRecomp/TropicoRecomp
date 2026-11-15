/* SPDX-License-Identifier: MIT */
/* Shared by the kernel32 sources. */
#ifndef RC_K32_H
#define RC_K32_H

#include "rc/hle.h"
#include "rc/process.h"

#define K32 "KERNEL32.dll"

/* Error codes the functions set. */
#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_HANDLE_EOF 38u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_ENVVAR_NOT_FOUND 203u

#define INVALID_HANDLE 0xffffffffu

extern const rc_import_def rc_k32_process[];
extern const uint32_t rc_k32_process_count;
extern const rc_import_def rc_k32_memory[];
extern const uint32_t rc_k32_memory_count;
extern const rc_import_def rc_k32_locale[];
extern const uint32_t rc_k32_locale_count;
extern const rc_import_def rc_k32_file[];
extern const uint32_t rc_k32_file_count;

#endif
