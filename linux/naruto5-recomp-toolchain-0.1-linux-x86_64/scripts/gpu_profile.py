import collections
import sys

KINDS = ['triangle', 'sprite', 'line', 'point']


def main():
    if len(sys.argv) < 2:
        print('usage: gpu_profile.py <file written by PS2_GPU_PROFILE=from,to,file> [rows]')
        return 1
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    frames = set()
    pipelines = collections.Counter()
    batches = collections.defaultdict(lambda: [0, 0, 0, 0])
    for line in open(sys.argv[1]):
        p = line.split()
        frames.add(p[0])
        ns = int(p[3])
        pipelines[p[2]] += ns
        if len(p) < 14:
            continue
        prims, tiles, kind, formats, flags, textured = int(p[6]), int(p[7]), int(p[10]), int(p[11]), int(p[12]), int(p[13])
        size = 'over 1000' if prims > 1000 else 'over 16' if prims > 16 else 'up to 16'
        key = (p[2], size, KINDS[kind] if kind < 4 else '?', 'frame %02x' % (formats & 0xFF), 'texture %02x' % ((formats >> 8) & 0xFF) if textured else 'untextured',
               'z %02x' % ((formats >> 16) & 0xFF) if flags & 512 else 'no z test', 'blend' if flags & 8 else '')
        b = batches[key]
        b[0] += ns
        b[1] += 1
        b[2] += prims
        b[3] += tiles
    n = max(1, len(frames))
    print('%d frames, %.2f ms of GPU time per frame' % (len(frames), sum(pipelines.values()) / n / 1e6))
    for name, ns in pipelines.most_common():
        print('  %-12s %6.2f ms' % (name, ns / n / 1e6))
    print('batches per frame by kind: ms, batches, primitives, tiles')
    for key, b in sorted(batches.items(), key=lambda kv: -kv[1][0])[:rows]:
        print('  %6.2f %6.1f %8.0f %8.0f  %s' % (b[0] / n / 1e6, b[1] / n, b[2] / n, b[3] / n, ', '.join(k for k in key if k)))
    return 0


sys.exit(main())
