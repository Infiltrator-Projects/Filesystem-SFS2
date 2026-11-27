# SPDX-License-Identifier: GPL-3.0-or-later
# Freestanding Common V2 SFS2 reader from same hosted format source.
cmake_minimum_required(VERSION 3.20)
project(Sfs2BootReader LANGUAGES C)
set(COMMON_INCLUDE_DIR "" CACHE PATH "Pinned Common V2 SDK includes")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
    message(FATAL_ERROR "COMMON_INCLUDE_DIR must be the pinned Common V2 SDK")
endif()
add_library(sfs2-boot MODULE
    ../sfs2_module.c
    ../sfs2_reader.c
    ../../native/filesystems/sfs2/core/sfs2_core.c
    runtime.c)
set_target_properties(sfs2-boot PROPERTIES PREFIX "" OUTPUT_NAME "SFS2"
    SUFFIX ".so" C_VISIBILITY_PRESET hidden)
target_compile_features(sfs2-boot PRIVATE c_std_11)
target_compile_definitions(sfs2-boot PRIVATE IFS_SFS2_FIRMWARE_BUILD=1)
target_include_directories(sfs2-boot PRIVATE "${COMMON_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native/filesystems/sfs2/core")
# Firmware uses AMD64 SysV without CET, SIMD state assumptions,
# packed relative relocations or non-4-KiB load alignment. Hosted builds
# retain their separate native optimization profile.
target_compile_options(sfs2-boot PRIVATE
    -Wall -Wextra -Wpedantic -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -mno-red-zone -fcf-protection=none -mgeneral-regs-only
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(sfs2-boot PRIVATE
    -nostdlib -Wl,-z,nopack-relative-relocs,-z,max-page-size=4096,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now)
