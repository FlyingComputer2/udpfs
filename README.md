# udpfs

POSIX-style remote file access over a datagram link: UDP, or a winject-l3 upstream between a
rover and its ground station. The client acts as root on the server's filesystems (on the rover the TF
card at `/sdcard` and the config partition at `/winject`): it opens, reads, writes, seeks,
stats, lists, creates and removes files and directories, with retransmission that is safe for
every operation. There is no authentication; use it on a trusted link only.

- [Wire protocol](docs/protocol.md)

Planned layout: `core` (wire format, server over POSIX calls, host-tested), `client` (library
and an scp-like CLI), and an ESP-IDF component that serves the protocol on its own reactor.
