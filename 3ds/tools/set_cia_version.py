#!/usr/bin/env python3
"""Writes a title version into a CIA's ticket and title metadata (TMD), where FBI and ctrtool read it:

    python tools/set_cia_version.py <file.cia> <major.minor.micro>

makerom v0.19.0 drops the minor number of -ver (1.1.0 comes out 1.0.0), and the port's versions are 0.x.y, so the
CIA build sets it here after makerom. Neither field is covered by the content hashes, and a homebrew CIA's
signatures are placeholders the console's custom firmware doesn't check. The version is packed as the 3DS does:
major << 10 | minor << 4 | micro (major and minor up to 63, micro up to 15).

CIA layout: a header (its size, type, version, then the certificate chain, ticket and TMD sizes), then the
certificate chain, the ticket and the TMD, each 64-byte aligned. Ticket and TMD each start with a signature (type,
then its bytes and padding), then their body: the ticket's title version at 0xA6, the TMD's at 0x9C (big endian).
"""

import re
import struct
import sys

SIGNATURE_SIZES = {0x10000: 0x200 + 0x3C, 0x10001: 0x100 + 0x3C, 0x10002: 0x3C + 0x40,
                   0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}


def align(n):
    return (n + 63) // 64 * 64


def body(blob, offset):
    return offset + 4 + SIGNATURE_SIZES[struct.unpack_from(">I", blob, offset)[0]]


def main():
    path, text = sys.argv[1], sys.argv[2]
    match = re.match(r"^(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        sys.exit(f"not a version: {text}")
    major, minor, micro = (int(x) for x in match.groups())
    if major > 63 or minor > 63 or micro > 15:
        sys.exit(f"version {text} out of range (63.63.15)")
    version = major << 10 | minor << 4 | micro
    with open(path, "r+b") as f:
        blob = bytearray(f.read())
        header_size, _, _, cert_size, ticket_size, _ = struct.unpack_from("<IHHIII", blob, 0)
        ticket = align(header_size) + align(cert_size)
        tmd = ticket + align(ticket_size)
        struct.pack_into(">H", blob, body(blob, ticket) + 0xA6, version)
        struct.pack_into(">H", blob, body(blob, tmd) + 0x9C, version)
        f.seek(0)
        f.write(blob)
    print(f"{path}: title version {major}.{minor}.{micro} ({version})")


if __name__ == "__main__":
    main()
