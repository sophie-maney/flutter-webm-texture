#!/usr/bin/env python3
"""Rewrites a WebM's Segment and Cluster sizes to "unknown" (all value bits
set, same length), the way a muxer that can't seek back writes them.

Usage: tool/unknown_size_webm.py IN.webm OUT.webm

Makes fuzz seeds (fuzz/corpus/README.md); native_test/webm_test.cc does the
same rewrite in memory.
"""
import sys


def vint(b, pos):
    first = b[pos]
    length = 1
    while not first & (0x80 >> (length - 1)):
        length += 1
    value = first & (0xFF >> length)
    for i in range(1, length):
        value = (value << 8) | b[pos + i]
    return length, value


def make_unknown(b, pos, length):
    b[pos] = 0xFF >> (length - 1)
    for i in range(1, length):
        b[pos + i] = 0xFF


def main():
    src = open(sys.argv[1], "rb").read()
    out = bytearray(src)
    id_len, _ = vint(src, 0)  # EBML header
    size_len, size = vint(src, id_len)
    pos = id_len + size_len + size
    id_len, _ = vint(src, pos)  # Segment
    pos += id_len
    size_len, size = vint(src, pos)
    make_unknown(out, pos, size_len)
    pos += size_len
    end = pos + size
    while pos < end:
        id_len, _ = vint(src, pos)
        element_id = int.from_bytes(src[pos:pos + id_len], "big")
        pos += id_len
        size_len, size = vint(src, pos)
        if element_id == 0x1F43B675:  # Cluster
            make_unknown(out, pos, size_len)
        pos += size_len + size
    open(sys.argv[2], "wb").write(out)


if __name__ == "__main__":
    main()
