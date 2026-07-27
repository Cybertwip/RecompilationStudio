/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#pragma once

#include <stdint.h>

#define HORIZON_MAGIC 0x70417020u

typedef struct horizon_segment_header {
    uint64_t offset;
    uint64_t addr;
    uint32_t size;
} horizon_segment_header;

typedef struct horizon_codeset_header {
    horizon_segment_header segments[3];
    uint64_t memory_size;
} horizon_codeset_header;

typedef struct horizon_header {
    uint32_t magic;
    uint64_t title_id;
    uint8_t ideal_core;
    uint8_t is_64bit;
    uint8_t address_space_type;
    uint32_t system_resource_size;
    int32_t main_thread_priority;
    uint32_t num_codesets;
    horizon_codeset_header codesets[];
} horizon_header;
