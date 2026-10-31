/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent SFS\\2 identity/geometry reader. No SFS0 compatibility engine,
 * mutation, formatter, allocation or defragmentation capability is exported.
 */
#include "ifs_sfs2_module_v1.h"
#include "sfs2_core.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#define SFS2_MIN_BLOCK 512u
#define SFS2_MAX_BLOCK 65536u
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int size_of_device(int fd, uint64_t *bytes) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (S_ISREG(st.st_mode)) {
        if (st.st_size <= 0) return -1;
        *bytes = (uint64_t)st.st_size;
        return 0;
    }
#ifdef __linux__
    if (S_ISBLK(st.st_mode)) return ioctl(fd, BLKGETSIZE64, bytes) == 0 ? 0 : -1;
#endif
    return -1;
}
static int read_root(int fd, uint64_t device_bytes, uint32_t block_size,
                     uint32_t number, IfsSfs2RootRecord *record) {
    if (block_size < SFS2_MIN_BLOCK || block_size > SFS2_MAX_BLOCK ||
        (block_size & (block_size - 1u)) != 0u ||
        (uint64_t)number >= device_bytes / block_size)
        return -1;
    unsigned char *buffer = malloc(block_size);
    if (!buffer) return -1;
    const uint64_t at = (uint64_t)number * block_size;
    const ssize_t n = pread(fd, buffer, block_size, (off_t)at);
    const int valid =
        n == (ssize_t)block_size &&
        ifs_sfs2_validate_block_header(buffer, block_size, number, IFS_SFS2_ROOT_ID) &&
        ifs_sfs2_decode_root(buffer, block_size, record) == 0 &&
        ifs_sfs2_validate_root_record(record) == 0 &&
        record->block_size == block_size &&
        record->total_blocks >= 4u &&
        (uint64_t)record->total_blocks * block_size <= device_bytes &&
        record->last_byte < (uint64_t)record->total_blocks * block_size;
    free(buffer);
    return valid ? 0 : -1;
}
static int inspect_sfs2(const char *path, IfsSfs2Geometry *geometry) {
    if (!path || !*path || !geometry) return -1;
    memset(geometry, 0, sizeof(*geometry));
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    uint64_t device_bytes = 0;
    if (size_of_device(fd, &device_bytes) != 0 || device_bytes < 4u * 512u) {
        close(fd); return -1;
    }
    /* The primary may be damaged. Independently try a last-block backup at
       each supported block size, never interpreting SFS0 as SFS2. */
    IfsSfs2RootRecord primary = {0}, backup = {0};
    int pvalid = 0, bvalid = 0;
    unsigned char probe[512];
    if (pread(fd, probe, sizeof(probe), 0) == (ssize_t)sizeof(probe) &&
        be32(probe) == IFS_SFS2_ROOT_ID) {
        const uint32_t bs = be32(probe + 52u);
        const uint32_t total = be32(probe + 48u);
        if (bs >= SFS2_MIN_BLOCK && bs <= SFS2_MAX_BLOCK &&
            (bs & (bs - 1u)) == 0u && total >= 4u &&
            (uint64_t)total * bs <= device_bytes) {
            pvalid = read_root(fd, device_bytes, bs, 0u, &primary) == 0;
            bvalid = read_root(fd, device_bytes, bs, total - 1u, &backup) == 0;
        }
    }
    if (!pvalid && !bvalid) {
        for (uint32_t bs = SFS2_MIN_BLOCK; bs <= SFS2_MAX_BLOCK; bs <<= 1u) {
            if (device_bytes % bs != 0u || device_bytes / bs > UINT32_MAX) continue;
            const uint32_t last = (uint32_t)(device_bytes / bs - 1u);
            if (read_root(fd, device_bytes, bs, last, &backup) == 0 &&
                backup.total_blocks == last + 1u) {
                bvalid = 1;
                break;
            }
        }
    }
    close(fd);
    if (!pvalid && !bvalid) return -1;
    if (pvalid && bvalid &&
        (primary.block_size != backup.block_size ||
         primary.total_blocks != backup.total_blocks ||
         primary.first_byte != backup.first_byte ||
         primary.last_byte != backup.last_byte ||
         primary.bitmap_base != backup.bitmap_base ||
         primary.root_object_container != backup.root_object_container ||
         primary.extent_bnode_root != backup.extent_bnode_root ||
         primary.object_node_root != backup.object_node_root)) return -1;
    const IfsSfs2RootRecord *chosen =
        ifs_sfs2_select_root_copy(pvalid, primary.sequence, bvalid, backup.sequence)
        ? &backup : &primary;
    geometry->block_size = chosen->block_size;
    geometry->total_blocks = chosen->total_blocks;
    geometry->bitmap_base = chosen->bitmap_base;
    geometry->structure_version = chosen->version;
    geometry->root_sequence = chosen->sequence;
    geometry->filesystem_bytes = (uint64_t)chosen->block_size * chosen->total_blocks;
    geometry->device_bytes = device_bytes;
    geometry->primary_root_valid = (uint8_t)pvalid;
    geometry->backup_root_valid = (uint8_t)bvalid;
    return 0;
}
static int probe_sfs2(const char *path) {
    IfsSfs2Geometry result;
    return inspect_sfs2(path, &result);
}
static const IfsSfs2ModuleV1 module = {
    IFS_SFS2_MODULE_ABI_V1, sizeof(IfsSfs2ModuleV1), "sfs2",
    IFS_SFS2_CAP_PROBE | IFS_SFS2_CAP_GEOMETRY, probe_sfs2, inspect_sfs2
};
const IfsSfs2ModuleV1 *infiltrator_filesystem_sfs2_v1(void) {
    return &module;
}
