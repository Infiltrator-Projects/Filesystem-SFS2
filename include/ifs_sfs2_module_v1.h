/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_SFS2_MODULE_V1_H
#define IFS_SFS2_MODULE_V1_H
#include <stdint.h>
#define IFS_SFS2_MODULE_ABI_V1 1u
#define IFS_SFS2_CAP_PROBE UINT64_C(1)
#define IFS_SFS2_CAP_GEOMETRY UINT64_C(2)
typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_base;
    uint16_t structure_version;
    uint16_t root_sequence;
    uint64_t filesystem_bytes;
    uint64_t device_bytes;
    uint8_t primary_root_valid;
    uint8_t backup_root_valid;
} IfsSfs2Geometry;
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    /* Zero is success. Both operations are strictly read-only and fail closed. */
    int (*probe)(const char *path);
    int (*geometry)(const char *path, IfsSfs2Geometry *result);
} IfsSfs2ModuleV1;
const IfsSfs2ModuleV1 *infiltrator_filesystem_sfs2_v1(void);
#endif
