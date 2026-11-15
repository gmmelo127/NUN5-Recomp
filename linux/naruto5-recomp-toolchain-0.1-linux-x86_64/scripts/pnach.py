#!/usr/bin/env python3
import struct
import sys

USAGE = 'usage: pnach.py crc <elf> | pnach.py apply <elf> <patched elf> <runtime list> [--section NAME]... <pnach>...'
SIZES = {'byte': 1, 'short': 2, 'word': 4, 'double': 8, 'beshort': 2, 'beword': 4, 'bedouble': 8}
EXTENDED = {0: 1, 1: 2, 2: 4}
PAD = 16


def crc(data):
    value = 0
    for (word,) in struct.iter_unpack('<I', data[:len(data) & ~3]):
        value ^= word
    return value


def segments(elf):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    for i in range(phnum):
        kind, offset, vaddr, _, filesz, memsz = struct.unpack_from('<6I', elf, phoff + i * phentsize)
        if kind == 1:
            yield vaddr & 0x1FFFFFFF, offset, filesz, memsz


def caves(patches, loads):
    image = {}
    for place, address, size, value in patches:
        if any(vaddr <= address < vaddr + memsz or vaddr < address + size <= vaddr + memsz for vaddr, _, _, memsz in loads):
            continue
        for k, byte in enumerate(value.to_bytes(size, 'little')):
            image[address + k] = byte
    blocks = []
    for address in sorted(image):
        if blocks and address <= blocks[-1][0] + len(blocks[-1][1]) + PAD:
            start, data = blocks[-1]
            data.extend(bytes(address - start - len(data)))
            data.append(image[address])
        else:
            blocks.append((address, bytearray([image[address]])))
    return [(start, bytes(data) + bytes(PAD)) for start, data in blocks]


def add_segments(elf, blocks):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    table = bytearray(elf[phoff:phoff + phentsize * phnum])
    for address, data in blocks:
        elf.extend(bytes(-len(elf) % 16))
        offset = len(elf)
        elf.extend(data)
        table += struct.pack('<8I', 1, offset, address, address, len(data), len(data), 7, 16) + bytes(phentsize - 32)
    elf.extend(bytes(-len(elf) % 4))
    struct.pack_into('<I', elf, 28, len(elf))
    struct.pack_into('<H', elf, 44, phnum + len(blocks))
    elf.extend(table)


def parse(path, wanted, sections, skipped):
    patches = []
    section = None
    for number, raw in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
        line = raw.split('//', 1)[0].strip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].strip()
            sections.setdefault(section, {'used': section in wanted, 'aspect': None})
            continue
        if section is not None and section not in wanted:
            continue
        if line.lower().startswith('gsaspectratio=') and section is not None:
            sections[section]['aspect'] = line.split('=', 1)[1].strip()
        if not line.lower().startswith('patch='):
            continue
        fields = [f.strip() for f in line[6:].split(',')]
        if len(fields) != 5:
            skipped.append('%s:%d malformed' % (path, number))
            continue
        place, cpu, address, kind, value = fields
        kind = kind.lower()
        if cpu.upper() != 'EE':
            skipped.append('%s:%d %s patch' % (path, number, cpu))
            continue
        try:
            place, address, value = int(place), int(address, 16), int(value, 16)
        except ValueError:
            skipped.append('%s:%d malformed' % (path, number))
            continue
        if kind == 'extended':
            size = EXTENDED.get(address >> 28)
            if not size:
                skipped.append('%s:%d extended code type %X' % (path, number, address >> 28))
                continue
            address &= 0x0FFFFFFF
        elif kind in SIZES:
            size = SIZES[kind]
            if kind.startswith('be'):
                value = int.from_bytes(value.to_bytes(size, 'big'), 'little')
        else:
            skipped.append('%s:%d %s patch' % (path, number, kind))
            continue
        patches.append((place, address & 0x1FFFFFFF, size, value & ((1 << (8 * size)) - 1)))
    return patches


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'crc':
        print('%08X' % crc(open(sys.argv[2], 'rb').read()))
        return
    if len(sys.argv) < 6 or sys.argv[1] != 'apply':
        sys.exit(USAGE)
    elf = bytearray(open(sys.argv[2], 'rb').read())
    args, wanted, files = sys.argv[5:], set(), []
    while args:
        if args[0] == '--section' and len(args) > 1:
            wanted.add(args[1])
            args = args[2:]
        else:
            files.append(args.pop(0))
    skipped, patches, sections = [], [], {}
    for path in files:
        patches += parse(path, wanted, sections, skipped)
    loads = list(segments(elf))
    in_elf = 0
    for place, address, size, value in patches:
        for vaddr, offset, filesz, _ in loads:
            if vaddr <= address and address + size <= vaddr + filesz:
                at = offset + address - vaddr
                elf[at:at + size] = value.to_bytes(size, 'little')
                in_elf += 1
                break
    blocks = caves(patches, loads)
    if blocks:
        add_segments(elf, blocks)
    open(sys.argv[3], 'wb').write(elf)
    with open(sys.argv[4], 'w') as out:
        for place, address, size, value in patches:
            out.write('%d %08X %d %X\n' % (place, address, size, value))
    print('%d patches, %d of them in the executable' % (len(patches), in_elf))
    for address, data in blocks:
        print('new code at %08X..%08X outside the executable joins it as a segment' % (address, address + len(data) - PAD))
    for name, info in sections.items():
        print('%s [%s]%s' % ('applied' if info['used'] else 'left out', name, ', meant for a %s screen' % info['aspect'] if info['aspect'] else ''))
    for name in sorted(wanted - set(sections)):
        print('no section named [%s]' % name)
    for reason in skipped:
        print('skipped', reason)


if __name__ == '__main__':
    main()
