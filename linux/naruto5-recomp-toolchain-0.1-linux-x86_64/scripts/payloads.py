#!/usr/bin/env python3
"""Feed code payloads found on the disc to the recompiler as window images.

The game loads whole programs from files such as PRG/ADV.BIN into the
executable segments the ELF leaves empty. Those files are ordinary game data,
so instead of waiting for the game to unpack them during exploration, this
finds them on the extracted disc, checks that they really hold MIPS code and
writes them to the overlays directory as <base>_<crc>.bin -- exactly the name
windows.py uses for a captured image.

  payloads.py <elf> <payload-dir> <overlays-dir>

It prints one line per payload accepted, and nothing for files that are data
(text tables, pads, etc.).
"""
import binascii
import os
import struct
import sys
import zlib

UNIT = 4
MIN_SIZE = 0x400
MIN_PROLOGUES = 8


def program_headers(elf):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    for i in range(phnum):
        yield struct.unpack_from('<8I', elf, phoff + i * phentsize)


def empty_windows(elf):
    """Executable LOAD segments with no file contents, the code windows."""
    out = []
    for p_type, _, vaddr, _, filesz, memsz, flags, _ in program_headers(elf):
        if p_type == 1 and filesz == 0 and (flags & 1) and memsz:
            out.append((vaddr, memsz))
    return sorted(out, key=lambda w: w[1])


def prologues(data):
    count = 0
    for at in range(0, len(data) - 3, 4):
        word, = struct.unpack_from('<I', data, at)
        if (word & 0xFFFF8000) in (0x27BD8000, 0x67BD8000):
            count += 1
    return count


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf = open(sys.argv[1], 'rb').read()
    source = sys.argv[2]
    outdir = sys.argv[3]
    windows = empty_windows(elf)
    if not windows:
        return
    os.makedirs(outdir, exist_ok=True)
    for root, _, files in os.walk(source):
        for name in sorted(files):
            path = os.path.join(root, name)
            try:
                data = open(path, 'rb').read()
            except OSError:
                continue
            if len(data) < MIN_SIZE or len(data) & 3:
                continue
            if prologues(data) < MIN_PROLOGUES:
                continue
            # The payload belongs to the smallest window that can hold it.
            fits = [w for w in windows if w[1] >= len(data)]
            if not fits:
                continue
            base = fits[0][0] & ~(UNIT - 1)
            crc = zlib.crc32(data) & 0xFFFFFFFF
            stem = '%08X_%08X' % (base, crc)
            target = os.path.join(outdir, stem + '.bin')
            if not os.path.exists(target):
                open(target, 'wb').write(data)
            print('payload %s %s %d bytes, %d prologues' % (stem, os.path.relpath(path, source), len(data),
                                                            prologues(data)))


if __name__ == '__main__':
    main()
