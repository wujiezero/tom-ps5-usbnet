# libsmb2 (vendored)

The SMB2/3 client behind the page's NAS tab: `src/smb_client.c` calls it to list a server's
shares, browse them and read files, and `src/smb.c` copies what it reads to the console.

## Source

[sahlberg/libsmb2](https://github.com/sahlberg/libsmb2) at `fc710a3` (2026-10-08), with these
upstream commits cherry-picked on top, as vendored in
[ps5upload](https://github.com/phantomptr/ps5upload) (`payload/third_party/libsmb2`):

| upstream | |
|---|---|
| `e94eee9` | client: enforce sealed replies, validate transform headers, cap receives (#493) |
| `ee56559` | sync: do not leak state when the connection fails mid-call |
| `b4a409e` | Free everything when a call is cancelled or the context is destroyed |
| `ccc53a1` | libsmb2: fstat: don't use a related compound without a CREATE |
| `8fa34fd` | libsmb2: set an error string when stat/truncate/rename/fstat/echo fail |
| `6cd18a5` | read: refuse a READ reply with more data than was asked for |
| `753e218` | libsmb2: stat/fstat/statvfs: fail a QUERY_INFO reply with no output buffer |
| `5199123` | NTLM: do not memcpy() from a NULL buffer when the encoder first grows |

Only the client library is here: `lib/` (core sources, no Kerberos, no QUIC / SMB Direct, no
platform ports), `include/`, and the licence files. The sources are unmodified.

Ours:
- `config.h`, written by hand for the PS5 SDK's FreeBSD libc and the host test builds. It also
  defines `_U_`, which `aes128ccm.c` uses, and `SOL_TCP`, so that `TCP_NODELAY` is set without
  `getprotobyname()` (the console has no `/etc/protocols`).
- Every file is compiled with `-include include/smb_sock.h`, which sends the library's socket
  calls to `src/smb_net.c`. That is how a connection runs over the USB adapter's lwIP stack as
  well as over the console's own network. The library's text is not touched.

## Licence and what it asks of us

The library (`lib/`, `include/`) is **LGPL-2.1-or-later** (`COPYING`, `LICENCE-LGPL-2.1.txt`;
every file carries its notice). tom-ps5-usbnet is GPL-3.0-or-later and links it statically. LGPL-2.1
§3 allows the code to be used under the GPL (version 2 or any later version), and the ELF is
distributed under GPL-3.0 with its complete source.

Obligations we keep:
- ship the licence texts and keep every copyright notice (this folder; nothing removed);
- say what we changed and when (this file);
- make the complete corresponding source of the ELF available: this repository, including this
  library and `scripts/build_ps5.py`, which builds it (`make ps5`).

## Updating

Copy `lib/*.c`, `lib/*.h`, `include/` and the licence files from a newer upstream, then run
`make test` and `make ps5`. Check SMB on a console: list a share, copy a folder down, and pull
the network cable mid-copy.
