# udpfs

POSIX-style remote file access over a datagram link: UDP, or a winject-l3 upstream between a
rover and its ground station. The server exposes configured roots (for example a TF card at
`/sdcard` and a config partition at `/winject`); the client opens, reads, writes, seeks, stats,
lists, creates and removes files and directories there, with retransmission that is safe for
every operation.

- [Wire protocol](docs/protocol.md)

Planned layout: `core` (wire format, server over POSIX calls, host-tested), `client` (library
and an scp-like CLI), and an ESP-IDF component that serves the protocol on its own reactor.
