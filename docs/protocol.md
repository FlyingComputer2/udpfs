# udpfs wire protocol

udpfs gives a client POSIX-style access to files on a server over a datagram link (UDP, or a
winject-l3 upstream). One request is one datagram and one response is one datagram. The link
may lose, duplicate or reorder datagrams; it is assumed not to corrupt them (L3 and UDP check
their frames). The client retransmits a request until its response arrives; the rules in
[Retransmission](#retransmission-and-the-reply-cache) make that safe for every operation.

All integers are little-endian. `uN` / `iN` are unsigned / signed N-bit integers. `bytes` and
`path` run to the end of the datagram unless a length field says otherwise.

## Header

Every request:

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `op` | u8 | operation, table below |
| 1 | `reqid` | u32 | chosen by the client, see [Request ids](#request-ids) |

Every response:

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `op` | u8 | the request's `op` with bit 7 set (`op \| 0x80`) |
| 1 | `reqid` | u32 | the request's `reqid` |
| 5 | `ret` | i32 | result: ≥ 0 on success (meaning per operation), −1 on failure |
| 9 | `err` | u32 | wire errno ([Errors](#errors)); 0 on success |
| 13 | … | | operation payload, only when `ret` ≥ 0 |

A failed response carries no payload. A response header is 13 bytes.

## Operations

| `op` | Name | Request after the header | `ret` on success | Response payload |
|---|---|---|---|---|
| 0x01 | `OPEN` | u32 `flags`, u32 `mode`, `path` | file handle `fd` | — |
| 0x02 | `CLOSE` | i32 `fd` | 0 | — |
| 0x03 | `READ` | i32 `fd`, u64 `off`, u32 `len` | bytes read (0 = end of file) | `bytes` (`ret` of them) |
| 0x04 | `WRITE` | i32 `fd`, u64 `off`, u32 `len`, `bytes` (`len` of them) | bytes written | — |
| 0x05 | `LSEEK` | i32 `fd`, i64 `off`, u8 `whence` | 0 | i64 `pos`, the new position |
| 0x06 | `FSTAT` | i32 `fd` | 0 | stat block |
| 0x07 | `STAT` | `path` | 0 | stat block |
| 0x08 | `UNLINK` | `path` | 0 | — |
| 0x09 | `MKDIR` | u32 `mode`, `path` | 0 | — |
| 0x0A | `RMDIR` | `path` | 0 | — |
| 0x0B | `OPENDIR` | `path` | directory handle `dir` | — |
| 0x0C | `READDIR` | i32 `dir`, u8 `rflags` | entries in this response (0 = end) | entries |
| 0x0D | `CLOSEDIR` | i32 `dir` | 0 | — |
| 0x0E | `RENAME` | u16 `from_len`, `from` (`from_len` bytes), `to` | 0 | — |
| 0x0F | `FSYNC` | i32 `fd` | 0 | — |
| 0x10 | `FTRUNCATE` | i32 `fd`, u64 `len` | 0 | — |
| 0x11 | `INFO` | — | 0 | server limits, below |
| 0x12 | `CRC32` | i32 `fd`, u64 `off`, u64 `len`, u32 `seed` | 0 | u32 `crc`, u64 `n` |
| 0x13 | `LIST_OPEN` | — | open handles listed | open-handle entries (debug) |

`READ` and `WRITE` take the offset in the request (`pread` / `pwrite`). They do not use or move
the handle's position. `LSEEK` keeps POSIX meaning for the position it reports: `LSEEK fd, 0,
SEEK_END` gives the current file size. The position is otherwise only used by `LSEEK` itself.

`WRITE`: `len` must equal the bytes that follow (`EPROTO` otherwise). A short write
(`ret` < `len`) is not an error, just as with POSIX. The client writes the rest at `off + ret`.

`READ`: the server returns at most `len` bytes, and at most what fits the server's datagram
limit (`max_datagram` − 13). A short read is not end of file; only `ret` = 0 is.

`RENAME` is POSIX `rename()`, and takes whatever the server's filesystem does with an existing
`to`: Linux replaces it atomically, but ESP-IDF's FAT and SPIFFS fail with `EEXIST`. There the
client has to `UNLINK` the target first, and for a moment neither file exists.

`FSYNC` flushes the file to its medium; `CLOSE` does too. A client that needs a file to
survive power loss syncs (or closes) before it reports success. `FTRUNCATE` sets the size,
shrinking or zero-extending.

`CRC32` is the CRC-32 of zlib and PNG (IEEE 802.3, reflected, `crc32(seed, bytes)` with
`seed` 0 for the first piece) over `n` bytes from `off`: `len` bytes, fewer at end of file, and at
most the server's `crc_max` per request. The client continues at `off + n` with the returned
`crc` as `seed` until it has covered what it wants (`n` = 0 is end of file). This lets a client
check a transfer without reading the file back over the link.

`LIST_OPEN` is for debugging: the server's open handles, one entry each, as many as fit the
datagram (`max_handles` is small). Entry: i32 `handle`, u8 `type` (1 file, 2 directory), i64
`pos` (the `LSEEK` position; 0 for directories), u16 `path_len`, `path` (as opened).

`INFO` lets a client size its requests and window to the server:

| Field | Type | Meaning |
|---|---|---|
| `max_datagram` | u32 | [Configuration](#configuration) |
| `max_path` | u32 | |
| `max_handles` | u32 | |
| `reply_cache` | u32 | |
| `idle_timeout_ms` | u32 | |
| `crc_max` | u32 | |
| `open_handles` | u32 | handles open now |

`READDIR`: the server packs as many entries as fit the datagram and continues from there on
the next `READDIR`. Bit 0 of `rflags` asks for `size` and `mtime` (one `stat` per entry on the
server); without it both are 0. `.` and `..` are never returned. Entry:

| Field | Type | Meaning |
|---|---|---|
| `type` | u8 | 1 file, 2 directory, 3 other |
| `size` | u64 | bytes (files), when asked for |
| `mtime` | i64 | seconds since 1970-01-01 UTC, when asked for |
| `name_len` | u8 | 1–255 |
| `name` | `name_len` bytes | the entry's name, no NUL |

Stat block (`STAT`, `FSTAT`):

| Field | Type | Meaning |
|---|---|---|
| `type` | u8 | 1 file, 2 directory, 3 other |
| `mode` | u32 | permission bits (0777 mask) |
| `size` | u64 | bytes |
| `mtime` | i64 | seconds since 1970-01-01 UTC |

An unknown `op` gets a response with `ret` = −1, `err` = `ENOSYS`. A request too short for its
fields, or with a field out of range, gets `EPROTO`; one longer than the server's
`max_datagram` gets `EMSGSIZE`. A datagram shorter than the header gets
no response. The server never sends unsolicited datagrams.

## Paths

`path` is UTF-8, absolute (`/sdcard/photo/rov_0001.jpg`), 1 to `max_path` bytes, with no NUL.
A relative path fails with `EINVAL`. The server passes it to its filesystem as it is, with the
server's full rights: the client acts as root and can reach every mounted filesystem, the
link's configuration included (`/winject/winject.conf` on the rover). Nothing is translated or
fenced off; `.`, `..`, repeated `/` and a trailing `/` mean what the server's filesystem says.

## Flags, modes and whence

`OPEN` `flags` (wire values; Linux numbering):

| Name | Value |
|---|---|
| `O_RDONLY` | 0x0 |
| `O_WRONLY` | 0x1 |
| `O_RDWR` | 0x2 |
| `O_CREAT` | 0x40 |
| `O_EXCL` | 0x80 |
| `O_TRUNC` | 0x200 |

Any other bit fails with `EINVAL`. There is no `O_APPEND`: `WRITE` always carries its offset.
The client appends by writing at the size `LSEEK SEEK_END` reports. `mode` is the permission
bits (0777 mask) for `O_CREAT` and `MKDIR`; a server whose filesystem has none (FAT, SPIFFS)
ignores them.

`whence`: 0 `SEEK_SET`, 1 `SEEK_CUR`, 2 `SEEK_END`; any other value fails with `EINVAL`.

## Errors

`err` uses Linux errno numbers. Each side maps between these and its own C library's values
through its [configuration](#configuration); a native value with no wire value becomes `EIO`.

| Name | Value | | Name | Value |
|---|---|---|---|---|
| `EPERM` | 1 | | `ENOTDIR` | 20 |
| `ENOENT` | 2 | | `EISDIR` | 21 |
| `EIO` | 5 | | `EINVAL` | 22 |
| `EBADF` | 9 | | `ENFILE` | 23 |
| `EAGAIN` | 11 | | `EMFILE` | 24 |
| `ENOMEM` | 12 | | `EFBIG` | 27 |
| `EACCES` | 13 | | `ENOSPC` | 28 |
| `EBUSY` | 16 | | `EROFS` | 30 |
| `EEXIST` | 17 | | `ENAMETOOLONG` | 36 |
| `EXDEV` | 18 | | `ENOSYS` | 38 |
| `ENOTEMPTY` | 39 | | `EPROTO` | 71 |
| `ENOTSUP` | 95 | | `EMSGSIZE` | 90 |

`EBADF` also answers a handle the server does not know (closed, or dropped at the idle
timeout). `EMFILE` answers an `OPEN` / `OPENDIR` past the server's `max_handles`.

## Handles

`fd` and `dir` are server-chosen non-negative numbers, one namespace for both. The client keeps
its own mapping from them to its file and `DIR*` objects. The server never reuses a number while
it runs: it starts at a random value in [1, 2^30) when it starts and counts up, wrapping from
2^31 − 1 to 1. So a late or duplicate request for a closed handle gets `EBADF`, never another
file, also across a server restart.

## Request ids

The client starts `reqid` at a random 32-bit value and adds 1 for each new request. A
retransmission repeats the `reqid` and the bytes of the original. The random start keeps a
restarted client, or a second one, from repeating ids the server still has cached. The counter
keeps one client's ids distinct. A client may have several requests in flight (for example a
window of `READ`s at different offsets); responses can arrive in any order, matched by `reqid`.

One client is assumed per handle at a time. The protocol has no authentication and no access
control: whoever can reach the server can read, replace or delete any file it can. Expose it
only on a trusted link.

## Retransmission and the reply cache

`READ`, `WRITE`, `STAT`, `FSTAT`, `FSYNC`, `FTRUNCATE`, `INFO`, `CRC32` and `LIST_OPEN` are
idempotent: the server runs them again for a retransmission. A repeated `WRITE` at the same offset writes the same
bytes.

Every other operation changes state, so a retransmission must not run it twice. A second
`OPEN` would leak a handle, a second `UNLINK` would report `ENOENT`, and a second `READDIR`
would skip entries. The server keeps the responses to these in a reply cache of the last
`reply_cache` entries. Each is keyed by (`op`, handle or −1, `reqid`), where the handle is the
request's `fd` / `dir` for `CLOSE`, `LSEEK`, `READDIR`, `CLOSEDIR`, and −1 for the path
operations (`OPEN`, `UNLINK`, `MKDIR`, `RMDIR`, `OPENDIR`, `RENAME`). A request whose key is in the cache gets the cached response again, byte for byte,
without running. The cache is cleared at the idle timeout.

## Idle timeout

When no request arrives for `idle_timeout_ms`, the server closes every handle and clears its
reply cache. A client that comes back gets `EBADF` for its old handles and reopens them.

## Configuration

**Compile time**, like bfc's `<bfc_config.hpp>`. The project (the firmware or the application,
never a library built into it) provides `<udpfs_config.hpp>`. A missing one is a compile error.
It defines:

- `UDPFS_CONFIG_ID`: a string naming the config.
- In `namespace udpfs::config`, the four maps between wire and native values:
  `uint32_t errno_to_wire(int)`, `int errno_from_wire(uint32_t)`,
  `int open_flags_from_wire(uint32_t)` (returns −1 for unknown bits) and
  `uint32_t open_flags_to_wire(int)`.

`<udpfs/posix_map.hpp>` implements all four by name: each wire value is paired with the native
macro of the same name (`ENOENT`, `O_CREAT`, …). It therefore fits any POSIX C library, glibc
and ESP-IDF's newlib alike, and a config normally just forwards to it. A project with an
unusual C library writes its own.

**Run time** (`udpfs::server_config`, given to the server at start):

| Field | Default | Meaning |
|---|---|---|
| `max_datagram` | 1400 | largest datagram the server sends or accepts; L3's FEC limit is 1438 |
| `max_path` | 255 | longest `path` accepted (`ENAMETOOLONG` past it) |
| `max_handles` | 4 | open `fd`s plus `dir`s at once (ESP-IDF FAT also caps open files per mount) |
| `reply_cache` | 32 | cached responses ([above](#retransmission-and-the-reply-cache)) |
| `idle_timeout_ms` | 30000 | [Idle timeout](#idle-timeout) |
| `crc_max` | 1048576 | bytes one `CRC32` request covers at most (bounds the time it holds the server) |

The client has its own `max_datagram`, retransmit interval, retry limit and read/write window.

## Size and rate

With `max_datagram` 1400 a `READ` returns up to 1387 bytes (13-byte response header) and a `WRITE` carries up to 1379 (21-byte request header).
Over the rover link (RTT ~110 ms) one request at a time moves ~12 KB/s. A window of 8 requests
in flight moves ~100 KB/s, until the link's own rate limits it.

## ESP-IDF notes

ESP-IDF's SPIFFS has no directories: names hold `/` (`/winject/a/b.txt` is one file),
`MKDIR` / `RMDIR` fail with `ENOTSUP`, and `OPENDIR` on the mount point lists every file.

Any SPIFFS or other SPI flash access turns the cache off while it runs. IDF then requires the
calling task's stack to be in internal RAM (it asserts this in `spi_flash`). A server that can
be asked for a file on SPI flash (SPIFFS at `/winject` on the rover) must therefore run on an
internal-RAM stack. Since every mounted filesystem is reachable, that is any server on a board
with a flash filesystem mounted. While the cache is off, the other core and every task
on a PSRAM stack wait, so SPIFFS operations pause other work, video included, for their
duration.
