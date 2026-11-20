# udpfs

POSIX-style remote file access over a datagram link: UDP, or a winject-l3 upstream between a
rover and its ground station. The client acts as root on the server's filesystems (on the rover
the TF card at `/sdcard` and the config partition at `/winject`): it opens, reads, writes, seeks,
stats, lists, creates and removes files and directories, with retransmission that is safe for
every operation. There is no authentication; use it on a trusted link only.

- [Wire protocol](docs/protocol.md)

## Layout

| Path | What |
|---|---|
| `src/udpfs/wire.*` | op codes, wire errno / flags, little-endian reader / writer, CRC-32 |
| `src/udpfs/server.*` | the server over POSIX calls: no threads, no sockets; feed it datagrams |
| `src/udpfs/client.*` | the client: every operation, a request window, bulk read / write / CRC |
| `src/udpfs/udp_transport.*` | a connected UDP socket as the client's transport |
| `src/udpfs_idf/service.*`, `idf/udpfs` | ESP-IDF: the server on its own bfc reactor and thread (internal-RAM stack), fed by an L3 upstream's queues and/or a UDP socket |
| `tools/udpfs.cpp` | the `udpfs` command line client |
| `tools/udpfs_serve.cpp` | serves this machine's files on UDP (tests, a Linux board) |

A project supplies `<udpfs_config.hpp>` (errno / open-flag maps; see `src/udpfs/config.hpp`),
like bfc's `<bfc_config.hpp>`; `udpfs/posix_map.hpp` covers any POSIX C library.

## Command line

```sh
cmake -S . -B build && cmake --build build
build/udpfs get 192.168.253.19:/sdcard/photo/rov_0001.jpg .      # port 2440 by default
build/udpfs put winject.conf 192.168.253.19:/winject/winject.conf
build/udpfs cp -r 192.168.253.19:/sdcard/rec ./rec
build/udpfs ls -l 192.168.253.19:/sdcard
build/udpfs list_open 192.168.253.19                              # debug: open handles
```

`HOST[:PORT]:/path` is the remote side, as with scp. Transfers keep 8 requests in flight (`-w`),
resend after 400 ms (`-t`), and are checked with the server's CRC32 (`--no-verify` skips it); a
download is written to `<file>.part` and renamed when it checks out.

## Tests

```sh
build/udpfs_tests
```

Server operations and errors, and client against server over a link with 20-25% loss,
duplicates and reordering.
