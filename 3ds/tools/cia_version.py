#!/usr/bin/env python3
"""Prints a CIA's title ID and version (major.minor.micro, from its title metadata):

    python tools/cia_version.py build/openmw/apps/openmw/openmw.cia

CIA layout: a 0x2020-byte header (header size, type, version, certificate chain size, ticket size, TMD size...), then
the certificate chain, the ticket and the TMD, each 64-byte aligned. The TMD starts with its signature (type
0x10004: RSA-2048 with SHA-256, 0x100 bytes and 0x3C of padding), then its header: title ID at 0x4C, title version at
0x9C (big endian).
"""

import struct
import sys


def align(n):
    return (n + 63) // 64 * 64


def main():
    blob = open(sys.argv[1], "rb").read()
    header_size, _, _, cert_size, ticket_size, tmd_size = struct.unpack_from("<IHHIII", blob, 0)
    tmd = align(header_size) + align(cert_size) + align(ticket_size)
    sig_type = struct.unpack_from(">I", blob, tmd)[0]
    sig_sizes = {0x10000: 0x200 + 0x3C, 0x10001: 0x100 + 0x3C, 0x10002: 0x3C + 0x40,
                 0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}
    body = tmd + 4 + sig_sizes[sig_type]
    title_id = struct.unpack_from(">Q", blob, body + 0x4C)[0]
    version = struct.unpack_from(">H", blob, body + 0x9C)[0]
    print(f"title {title_id:016X} version {version} = {version >> 10}.{(version >> 4) & 0x3F}.{version & 0xF}")


if __name__ == "__main__":
    main()
