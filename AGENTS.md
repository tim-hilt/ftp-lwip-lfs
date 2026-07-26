# FTP Server for LwIP + LFS

## Guidelines

- Targets embedded devices -> minimal binary size required
- No heap allocations in ftp_server.c or ftp_server.h
    - Heap allocations are fine for tests and mocks
- Tests follow BDD structure (Given, When, Then)
- Document design decisions in [DESIGN.md](/DESIGN.md)

