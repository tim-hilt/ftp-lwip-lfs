/**
 * @file test_auth.cpp
 * @brief Credential tests for builds that define FTP_SERVER_USER.
 *
 * FTP_SERVER_USER / FTP_SERVER_PASS are compile-time constants baked into
 * ftp_server.c, so the login paths are unreachable from the default test
 * build. CMake compiles this file twice against two separately configured
 * copies of the library:
 *
 *   ftp_tests_auth      FTP_SERVER_USER + FTP_SERVER_PASS  -> USER/PASS flow
 *   ftp_tests_user_only FTP_SERVER_USER only               -> USER logs in
 *
 * The two configurations share every test below; the handful of assertions
 * that differ branch on FTP_SERVER_PASS.
 */
#include "ftp_test_support.hpp"

using namespace ftptest;

namespace {

/* Bind the macros to typed constants first: in the user-only build
 * FTP_SERVER_PASS expands to the bare NULL macro, which cannot take part in
 * pointer arithmetic or comparison until it has a pointer type. */
constexpr const char *kUser = FTP_SERVER_USER;
constexpr const char *kPass = FTP_SERVER_PASS;
constexpr bool kPasswordRequired = (kPass != nullptr);

/** Drive a full successful login for the configured credentials. */
void login(const Client &c)
{
    std::string user = send_cmd(c, std::string("USER ") + kUser + "\r\n");
    if constexpr (kPasswordRequired) {
        REQUIRE(user == "331 Please specify the password.\r\n");
        REQUIRE(send_cmd(c, std::string("PASS ") + kPass + "\r\n") ==
                "230 Login successful.\r\n");
    } else {
        REQUIRE(user == "230 Login successful.\r\n");
    }
}

} // namespace

TEST_CASE("an unauthenticated client is refused every real command", "[auth]")
{
    init_server();
    Client c = connect_client();

    const char *commands[] = {
        "SYST\r\n", "PWD\r\n", "CWD /x\r\n", "PASV\r\n", "LIST\r\n",
        "RETR a.txt\r\n", "STOR a.txt\r\n", "DELE a.txt\r\n", "MKD d\r\n",
        "RMD d\r\n", "SIZE a.txt\r\n", "NOOP\r\n", "FROB\r\n",
    };
    for (const char *cmd : commands) {
        INFO("command: " << cmd);
        REQUIRE(send_cmd(c, cmd) == "530 Please login with USER and PASS.\r\n");
    }
}

TEST_CASE("FEAT is answered before login", "[auth]")
{
    /* RFC 2389 section 3: FEAT may be issued before login — clients use the
     * feature list to decide what to send during the login sequence itself
     * (UTF8 above all), so gating it behind USER/PASS is backwards. */
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "FEAT\r\n") ==
            "211-Features:\r\n"
            " PASV\r\n"
            " SIZE\r\n"
            " UTF8\r\n"
            "211 End\r\n");

    /* It grants nothing: everything else still needs credentials. */
    REQUIRE(send_cmd(c, "PWD\r\n") == "530 Please login with USER and PASS.\r\n");
}

TEST_CASE("OPTS is answered before login", "[auth]")
{
    /* The other half of the FEAT exchange. Clients read UTF8 out of the
     * feature list and immediately act on it — the standard opening is FEAT,
     * then OPTS UTF8 ON, then USER — so answering 530 here rejects a client
     * for doing exactly what FEAT invited it to do. */
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "OPTS UTF8 ON\r\n") == "200 UTF8 set to on.\r\n");

    /* Still no credentials granted, and a bad option is still a 501 rather
     * than a login complaint. */
    REQUIRE(send_cmd(c, "OPTS NOPE\r\n") == "501 Option not understood.\r\n");
    REQUIRE(send_cmd(c, "PWD\r\n") == "530 Please login with USER and PASS.\r\n");

    /* And the login sequence that follows it still works. */
    login(c);
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("the configured user name is accepted", "[auth]")
{
    init_server();
    Client c = connect_client();
    login(c);
    REQUIRE(send_cmd(c, "SYST\r\n") == "215 UNIX Type: L8\r\n");
}

/** The reply a USER command that names nobody gets in this build. */
constexpr const char *kUnknownUserReply =
    kPasswordRequired ? "331 Please specify the password.\r\n"
                      : "530 Login incorrect.\r\n";

TEST_CASE("an unknown user name is rejected", "[auth]")
{
    init_server();
    Client c = connect_client();

    REQUIRE(send_cmd(c, "USER intruder\r\n") == kUnknownUserReply);
    REQUIRE(send_cmd(c, "SYST\r\n") == "530 Please login with USER and PASS.\r\n");

    if constexpr (kPasswordRequired) {
        /* The 331 promised nothing: no password gets in on a bad name, not
         * even the configured one. */
        REQUIRE(send_cmd(c, std::string("PASS ") + kPass + "\r\n") ==
                "530 Login incorrect.\r\n");
        REQUIRE(send_cmd(c, "SYST\r\n") == "530 Please login with USER and PASS.\r\n");
    }
}

TEST_CASE("USER does not reveal whether the name exists", "[auth][security]")
{
    /* RFC 2577 section 3: with credentials configured there is exactly one
     * valid user name, so answering a wrong USER differently from the right
     * one hands it over for free — the attacker then only has to guess the
     * password. Both must be answered identically and rejected at PASS. */
    if constexpr (!kPasswordRequired) {
        SUCCEED("no password configured, so USER must answer conclusively");
    } else {
        init_server();

        Client good = connect_client();
        Client bad  = connect_client();

        std::string good_reply = send_cmd(good, std::string("USER ") + kUser + "\r\n");
        std::string bad_reply  = send_cmd(bad,  "USER intruder\r\n");
        REQUIRE(good_reply == bad_reply);
        REQUIRE(good_reply == "331 Please specify the password.\r\n");

        /* The distinction survives where it belongs — in what PASS does. */
        REQUIRE(send_cmd(bad, std::string("PASS ") + kPass + "\r\n") ==
                "530 Login incorrect.\r\n");
        REQUIRE(send_cmd(good, std::string("PASS ") + kPass + "\r\n") ==
                "230 Login successful.\r\n");
    }
}

TEST_CASE("USER without an argument is rejected", "[auth]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "USER\r\n") == kUnknownUserReply);
    REQUIRE(send_cmd(c, "SYST\r\n") == "530 Please login with USER and PASS.\r\n");
}

TEST_CASE("PASS before USER is rejected", "[auth]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "PASS whatever\r\n") == "503 Login with USER first.\r\n");
}

TEST_CASE("QUIT works without logging in", "[auth]")
{
    init_server();
    Client c = connect_client();
    REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");
    REQUIRE(mock_tcp_cb_recv[c.idx] == nullptr);
}

TEST_CASE("a wrong password sends the client back to USER", "[auth]")
{
    if constexpr (!kPasswordRequired) {
        SUCCEED("build has no password configured");
    } else {
        init_server();
        Client c = connect_client();

        REQUIRE(send_cmd(c, std::string("USER ") + kUser + "\r\n") ==
                "331 Please specify the password.\r\n");
        REQUIRE(send_cmd(c, "PASS wrong\r\n") == "530 Login incorrect.\r\n");
        REQUIRE(send_cmd(c, "SYST\r\n") == "530 Please login with USER and PASS.\r\n");

        /* A second PASS is refused: the failure reset the state to WAIT_USER. */
        REQUIRE(send_cmd(c, std::string("PASS ") + kPass + "\r\n") ==
                "503 Login with USER first.\r\n");

        /* Starting over still works. */
        login(c);
        REQUIRE(send_cmd(c, "SYST\r\n") == "215 UNIX Type: L8\r\n");
    }
}

TEST_CASE("PASS without an argument is rejected", "[auth]")
{
    if constexpr (!kPasswordRequired) {
        SUCCEED("build has no password configured");
    } else {
        init_server();
        Client c = connect_client();
        REQUIRE(send_cmd(c, std::string("USER ") + kUser + "\r\n") ==
                "331 Please specify the password.\r\n");
        REQUIRE(send_cmd(c, "PASS\r\n") == "530 Login incorrect.\r\n");
    }
}

TEST_CASE("a fresh connection starts unauthenticated", "[auth]")
{
    init_server();
    Client c = connect_client();
    login(c);
    REQUIRE(send_cmd(c, "QUIT\r\n") == "221 Goodbye.\r\n");

    /* The session slot is reused — its auth state must not be. */
    Client c2 = connect_client();
    REQUIRE(send_cmd(c2, "SYST\r\n") == "530 Please login with USER and PASS.\r\n");
}

TEST_CASE("a rejected USER drops an established login", "[auth][security]")
{
    init_server();
    Client c = connect_client();
    login(c);
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");

    /* The client asked to become somebody else, so it must not keep the
     * session it already had — whether the refusal lands on this USER or is
     * deferred to the PASS that has to follow it. */
    REQUIRE(send_cmd(c, "USER intruder\r\n") == kUnknownUserReply);
    REQUIRE(send_cmd(c, "PWD\r\n") == "530 Please login with USER and PASS.\r\n");

    /* Logging back in as the configured user still works. */
    login(c);
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}

TEST_CASE("USER can be re-issued to restart authentication", "[auth]")
{
    init_server();
    Client c = connect_client();
    login(c);
    login(c);
    REQUIRE(send_cmd(c, "PWD\r\n") == "257 \"/\" is the current directory.\r\n");
}
