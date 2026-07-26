/**
 * @file test_list_truncation.cpp
 * @brief LIST behaviour when an entry does not fit the transfer buffer.
 *
 * A LIST line is 45 bytes of fixed columns plus the file name and a CRLF, so it
 * overruns FTP_SERVER_DATA_BUF_SIZE once a name approaches the buffer. That is
 * unreachable in the default build (512-byte buffer against a 255-byte
 * LFS_NAME_MAX), so CMake compiles this file against a copy of the library
 * configured with FTP_SERVER_DATA_BUF_SIZE=64.
 */
#include "ftp_test_support.hpp"

#include <string>

using namespace ftptest;

namespace {

/* The name the single directory entry below reports — comfortably longer than
 * the 64-byte transfer buffer this build is configured with. */
const std::string kLongName(100, 'n');

/** lfs_dir_read hook yielding exactly one regular file, then EOF. */
int dir_read_one_long_name(lfs_t * /*lfs*/, lfs_dir_t * /*dir*/,
                           struct lfs_info *info)
{
    static int served;
    if (info == nullptr) return -1;
    if (served++ > 0) { served = 0; return 0; } /* EOF, reset for the next run */
    std::memset(info, 0, sizeof(*info));
    info->type = LFS_TYPE_REG;
    info->size = 1;
    std::memcpy(info->name, kLongName.c_str(), kLongName.size() + 1);
    return 1;
}

/** Run a LIST over a PASV data connection and return everything written. */
std::string list_output(const Client &c)
{
    REQUIRE(send_cmd(c, "PASV\r\n").rfind("227 ", 0) == 0);
    u16_t before = mock_tcp_write_len;
    REQUIRE(send_cmd(c, "LIST\r\n").empty());
    accept_pasv_data_connection();
    return written_since(before);
}

} // namespace

TEST_CASE("a LIST entry too long for the transfer buffer still ends in CRLF",
          "[list]")
{
    /* The CRLF is the very last thing on the line, so a formatter that simply
     * ran out of buffer would emit a shortened name with no terminator and the
     * client would parse this entry and the next one as a single mangled line.
     * The two bytes are reserved up front instead. */
    init_server();
    Client c = connect_client();
    mock_lfs_dir_read_fn = dir_read_one_long_name;

    std::string out = list_output(c);

    REQUIRE(out.find("150 Here comes the data.\r\n") != std::string::npos);
    REQUIRE(out.find("226 Transfer complete.\r\n") != std::string::npos);

    /* Isolate the listing itself: everything between the two control replies. */
    size_t start = out.find("150 Here comes the data.\r\n") +
                   std::strlen("150 Here comes the data.\r\n");
    size_t end   = out.find("226 Transfer complete.\r\n");
    REQUIRE(end > start);
    std::string entry = out.substr(start, end - start);

    /* Truncated to the buffer, terminated, and still recognisable. The entry is
     * handed on as a length rather than a C string, so the whole buffer is
     * usable and no byte is reserved for a NUL. */
    REQUIRE(entry.size() == FTP_SERVER_DATA_BUF_SIZE);
    REQUIRE(entry.compare(entry.size() - 2, 2, "\r\n") == 0);
    REQUIRE(entry.rfind("-rw-r--r--", 0) == 0);
    /* Exactly one line: nothing after the terminator, and no stray CR/LF
     * earlier that would split the entry in two. */
    REQUIRE(entry.find('\n') == entry.size() - 1);
}
