cmake_minimum_required(VERSION 3.20)
project(FilesystemSFS2 VERSION 0.1.0 LANGUAGES C)
include(CTest)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_library(sfs2-driver SHARED
    native/filesystems/sfs2/core/sfs2_core.c
    module/sfs2_module.c)
set_target_properties(sfs2-driver PROPERTIES PREFIX "" OUTPUT_NAME "SFS2" SUFFIX ".so")
target_include_directories(sfs2-driver PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
target_include_directories(sfs2-driver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/sfs2/core")
target_compile_definitions(sfs2-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(sfs2-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(BUILD_TESTING)
    add_executable(sfs2-module-test tests/test_sfs2_module.c)
    target_link_libraries(sfs2-module-test PRIVATE sfs2-driver)
    target_compile_definitions(sfs2-module-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(sfs2-module-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME sfs2-independent-module COMMAND sfs2-module-test)
endif()
install(TARGETS sfs2-driver LIBRARY DESTINATION /System/Filesystems)
# Install only this filesystem's illustrated PDF beside its native shared object.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/SFS2.pdf"
        DESTINATION /System/Resources/Documentation/Filesystem-SFS2)
# ABI header is distributed in source, not installed as a runtime payload.
