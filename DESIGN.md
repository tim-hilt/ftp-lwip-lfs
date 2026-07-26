# Design Notes

Rationale behind the behaviour of `ftp_server.c` — why the server answers the way it does, and which RFC requirement or lwIP/littlefs detail forced the choice. For the user-facing API, configuration and build instructions see [README.md](README.md).

## Resource model

- Designed for constrained targets: no dynamic allocation beyond the static `s_sessions[FTP_SERVER_MAX_CLIENTS]` table.
- Each session's LFS file cache buffer (`FTP_SERVER_FILE_CACHE_SIZE`) must be >= `lfs_config.cache_size`; `ftp_server_init()` returns `ERR_ARG` otherwise.
- Only one data connection is active per session at a time. A transfer command issued while another is still pending is answered `450 Transfer already in progress`; `PASV` and `ABOR` both reset the state.
- A connection arriving with every session slot taken is answered `421 Too many users, try again later.` and closed, rather than reset.
- Idle sessions are dropped after `FTP_SERVER_IDLE_TIMEOUT_POLLS` `tcp_poll` intervals. Traffic on either the control *or* the data connection counts as activity, so a long transfer is never cut short — only a stalled one.

## Data transfers

- Transfers are byte-transparent: no CRLF translation is performed in either direction. `TYPE A` and `TYPE I` are both accepted (RFC 959 §3.1.1.1 makes ASCII mandatory and the default) with an optional `N` format parameter, and behave identically — the reply to `TYPE A` says so rather than claiming a conversion that does not happen. Every other type is refused with `504`. `MODE` and `STRU` are accepted for their RFC 959 §5.1 default values (`S` and `F`) and refused with `504` otherwise.
- A send that lwIP cannot queue at all is retried from a `tcp_poll` on the data connection. `tcp_write()` returns `ERR_MEM` both for a full send window — where the peer's ACK brings the `tcp_sent` callback along to resume the transfer — and for an exhausted pbuf/segment pool, where it queues nothing, so no ACK and no `tcp_sent` ever follow. Without the poll a `RETR`/`LIST` unlucky enough to hit the second case would sit untouched until the idle timeout killed the whole session.
- Listing entries are queued with one `tcp_write()` each but pushed with a single `tcp_output()`, so lwIP can coalesce a directory's worth of ~55-byte lines into full segments instead of dribbling out one per entry.
- A client that opens the data connection and starts sending before its `STOR` has been processed does not lose the head of the upload: the pbuf is refused rather than discarded, which parks it in lwIP's `pcb->refused_data` until the command arms the transfer (`tcp_fasttmr` re-offers it every 250 ms). Costs no buffer of its own, and does not count as activity, so a connection that only ever sends unsolicited data still hits the idle timeout.
- A filesystem error part-way through a transfer ends it with `451`; a data-connection reset, an unusable data socket, or a download the client closes early all end it with `426` — never a misleading `226`. A client closing the data connection only means "transfer complete" for an upload (`STOR`), and only if the final `lfs_file_close()` flush succeeds — littlefs writes the tail of a file there, so a volume that fills up on the last chunk is reported as `451` rather than `226`.

## Security

- Both data-connection directions are pinned to the client ([RFC 2577](https://www.rfc-editor.org/rfc/rfc2577)): `PORT` only accepts the control connection's own peer address (FTP-bounce mitigation, `501` otherwise), and a `PASV` data connection arriving from any other host is dropped, so nobody on the network can race the client onto the passive port and take over the transfer. If the peer cannot be determined, both fail closed.
- When both `FTP_SERVER_USER` and `FTP_SERVER_PASS` are configured, every `USER` is answered `331` and the verdict is deferred to `PASS` ([RFC 2577](https://www.rfc-editor.org/rfc/rfc2577) §3). There is only ever one valid user name, so rejecting a wrong one at `USER` would hand it to an attacker for free. With `FTP_SERVER_USER` alone there is no later step to defer to, and `USER` necessarily answers `230`/`530` outright.
- `FEAT` is answered before login (RFC 2389 §3) — clients use the feature list to decide what to send during the login sequence. Everything else except `USER`, `PASS` and `QUIT` needs credentials.
- A pending `RNFR` is cancelled by any intervening command, including `USER`, `PASS` and `QUIT`, so a rename cannot survive a re-login and be completed under different credentials.

## Protocol handling

- Telnet control sequences are stripped from the control connection before a command is parsed. RFC 959 §4.1 has clients precede an in-transfer command with `IAC IP` + `IAC DM`, and lwIP delivers those bytes inline, so without this every conforming `ABOR` would read as an unknown command.
- Pathnames in `257` replies (`PWD`, `MKD`) are quoted with the RFC 959 Appendix II quote-doubling convention, so a name containing `"` still parses.
- `DELE` refuses directories and `RMD` refuses regular files, even though littlefs removes both with `lfs_remove()`.
- Path resolution normalises `.`/`..` straight into the result buffer, so `FTP_SERVER_PATH_MAX` bounds the *resolved* path: `CWD ../short` works from a directory too deep for `cwd + "/../short"` to fit. A single component that cannot itself be buffered is still rejected with `550`.
