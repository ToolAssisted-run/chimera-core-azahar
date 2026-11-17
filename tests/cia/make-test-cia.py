#!/usr/bin/env python3
"""Writes a small, valid, decrypted .cia of no game's: a title with the id you
give and one content of made-up bytes (or of the file you give). It is what
the gate's .cia legs install as downloadable content or as an update, so they
need no commercial file. Nothing in it is signed or encrypted, which Azahar
does not check when it installs.

    make-test-cia.py <out.cia> [--title-id 0004008C00123400] [--version 1]
                     [--content FILE | --ncsd DUMP | --size BYTES] [--encrypted] [--seed N]

--ncsd takes a decrypted cartridge dump (.3ds, .cci), puts its program (the
first partition, a decrypted NCCH) in as the content and uses the title id
that program says, so the result is a decrypted .cia of that game. It is for
trying the game slot on a dump you have; the file it makes is never committed.

A title id that starts 0004008C is downloadable content, 0004000E an update,
00040000 a game (one that will not start, as its content is not a program).
"""
import argparse
import hashlib
import struct
import sys


def align(n, a=0x40):
    return (n + a - 1) // a * a


def pad(b, a=0x40):
    return b + b"\0" * (align(len(b), a) - len(b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--title-id", default="0004008C00123400")
    ap.add_argument("--version", type=int, default=1)
    ap.add_argument("--content")
    ap.add_argument("--ncsd")
    ap.add_argument("--size", type=int, default=0x4000)
    ap.add_argument("--encrypted", action="store_true",
                    help="mark the content encrypted, as a retail .cia's is (it is not)")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    tid = int(a.title_id, 16)
    if a.ncsd:
        with open(a.ncsd, "rb") as f:
            head = f.read(0x200)
            if head[0x100:0x104] != b"NCSD":
                sys.exit("%s is not a cartridge dump" % a.ncsd)
            off, size = struct.unpack_from("<II", head, 0x120)
            f.seek(off * 0x200)
            content = f.read(size * 0x200)
        if content[0x100:0x104] != b"NCCH":
            sys.exit("the first partition is not an NCCH")
        tid = struct.unpack_from("<Q", content, 0x118)[0]
    elif a.content:
        content = open(a.content, "rb").read()
    else:
        # bytes that depend on the seed only
        content = hashlib.sha256(b"seed%d" % a.seed).digest() * (a.size // 32)
    content_id = 0

    # certificate chain: made-up, three entries of a plain RSA-2048 shape
    cert = pad(b"\0" * 0x700)

    # ticket: RSA-2048 signature, the body (Azahar's Ticket::Body), then the
    # content index
    sig = pad(struct.pack(">I", 0x00010004) + b"\0" * 0x100)
    body = bytearray(0x164)
    body[0:7] = b"Root-CA"
    body[0x7C] = 1                                  # version
    struct.pack_into(">Q", body, 0x90, 1)           # ticket id
    struct.pack_into(">Q", body, 0x9C, tid)         # title id
    struct.pack_into(">H", body, 0xA6, a.version)   # title version
    index = struct.pack(">IIIIIIII", 0x00010014, 0x30, 0x14, 0x00010014, 0, 0x28, 1, 0x84)
    index += struct.pack(">I", 0x84) + struct.pack(">I", 0x00030000) + b"\0" * 8
    ticket = sig + bytes(body) + index

    # title metadata: RSA-2048 signature, the body (Azahar's TitleMetadata::Body),
    # one content chunk
    tsig = pad(struct.pack(">I", 0x00010004) + b"\0" * 0x100)
    tb = bytearray(0x9C4)
    tb[0:7] = b"Root-CA"
    struct.pack_into(">Q", tb, 0x4C, tid)           # title id
    struct.pack_into(">I", tb, 0x54, 0x40)          # title type
    struct.pack_into(">H", tb, 0x9C, a.version)     # title version
    struct.pack_into(">H", tb, 0x9E, 1)             # content count
    chunk = struct.pack(">IHHQ", content_id, 0, 1 if a.encrypted else 0, len(content)) + hashlib.sha256(content).digest()
    tmd = tsig + bytes(tb) + chunk

    meta = b""
    header = struct.pack("<IHHIIIIQ", 0x2020, 0, 0, len(cert), len(ticket), len(tmd), len(meta), len(content))
    present = bytearray(0x2000)
    present[0] = 0x80
    header = pad(header + bytes(present))
    out = header + pad(cert) + pad(ticket) + pad(tmd) + pad(content) + meta
    open(a.out, "wb").write(out)
    print("%s: title %016X version %d, content %d bytes, %d bytes in all" % (a.out, tid, a.version, len(content), len(out)))


if __name__ == "__main__":
    sys.exit(main())
