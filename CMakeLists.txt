cmake_minimum_required(VERSION 3.10)
project(ftp_lwip_lfs C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Opt-in clang-tidy pass over ftp_server.c/.h only (see .clang-tidy).
# Off by default so a missing clang-tidy binary never breaks a plain build.
option(FTP_SERVER_ENABLE_CLANG_TIDY "Run clang-tidy on ftp_server sources" OFF)
if(FTP_SERVER_ENABLE_CLANG_TIDY)
    find_program(FTP_SERVER_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(FTP_SERVER_CLANG_TIDY_COMMAND "${FTP_SERVER_CLANG_TIDY_EXE}")
endif()

# lwIP (raw TCP API + IPv4 only — matches ftp_server.c's usage)
set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lwip)
set(LWIP_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/port/include
    ${LWIP_DIR}/src/include
)

# Source lists are inlined (rather than including lwip/src/Filelists.cmake)
# so that this build never touches files inside the lwip submodule — that
# file's inclusion would regenerate lwip/src/include/lwip/init.h in place
# via configure_file(). Only the raw TCP API + IPv4 core is needed here.
set(LWIP_CORE_SRCS
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/altcp.c
    ${LWIP_DIR}/src/core/altcp_alloc.c
    ${LWIP_DIR}/src/core/altcp_tcp.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
    ${LWIP_DIR}/src/core/ipv4/acd.c
    ${LWIP_DIR}/src/core/ipv4/autoip.c
    ${LWIP_DIR}/src/core/ipv4/dhcp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/igmp.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
)

add_library(lwip STATIC ${LWIP_CORE_SRCS})
target_include_directories(lwip PUBLIC ${LWIP_INCLUDE_DIRS})

# --------------------------------------------------------------------
# LittleFS
# --------------------------------------------------------------------
add_library(lfs STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/littlefs/lfs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/littlefs/lfs_util.c
)
target_include_directories(lfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/littlefs)

# --------------------------------------------------------------------
# ftp_server
# --------------------------------------------------------------------
add_library(ftp_server STATIC ${CMAKE_CURRENT_SOURCE_DIR}/ftp_server.c)
target_include_directories(ftp_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ftp_server PUBLIC lwip lfs)
target_compile_options(ftp_server PRIVATE
    -Werror
    -Wunused-function
    -Wunused-variable
    -Wunused-parameter
    -Wunused-but-set-variable
    -Wunused-macros
    -Wunused-const-variable
)
if(FTP_SERVER_ENABLE_CLANG_TIDY)
    set_target_properties(ftp_server PROPERTIES C_CLANG_TIDY "${FTP_SERVER_CLANG_TIDY_COMMAND}")
endif()
