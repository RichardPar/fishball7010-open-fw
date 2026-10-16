#!/usr/bin/env python3
"""Extract named files from a newc cpio (including several concatenated archives; later entries win), keeping their file mode.
usage: cpio-extract.py <archive.cpio> <outdir> <path-in-archive>..."""
import os, sys

def entries(data):
    i = 0
    while True:
        j = data.find(b'070701', i)
        if j < 0:
            return
        i = j
        field = lambda k: int(data[i + 6 + 8 * k:i + 14 + 8 * k], 16)
        mode, size, nsize = field(1), field(6), field(11)
        name = data[i + 110:i + 110 + nsize - 1].decode()
        off = (i + 110 + nsize + 3) & ~3
        i = (off + size + 3) & ~3
        if name != 'TRAILER!!!':
            yield name, mode, data[off:off + size]

if __name__ == '__main__':
    archive, outdir, wanted = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    found = {}
    for name, mode, body in entries(open(archive, 'rb').read()):
        if name in wanted:
            found[name] = (mode, body)
    missing = wanted - set(found)
    if missing:
        sys.exit('not in archive: ' + ', '.join(sorted(missing)))
    for name, (mode, body) in found.items():
        path = os.path.join(outdir, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(body)
        os.chmod(path, mode & 0o7777)
