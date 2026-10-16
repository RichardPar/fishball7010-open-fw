#!/usr/bin/env python3
"""Compare two newc cpio archives (concatenated archives allowed) by path, mode and content hash.
usage: cpio-diff.py <a.cpio> <b.cpio>"""
import hashlib, sys

def read(fn):
    d = open(fn, 'rb').read(); i = 0; files = {}
    while True:
        j = d.find(b'070701', i)
        if j < 0:
            return files
        i = j
        f = lambda k: int(d[i + 6 + 8 * k:i + 14 + 8 * k], 16)
        mode, size, nsize = f(1), f(6), f(11)
        name = d[i + 110:i + 110 + nsize - 1].decode(); off = (i + 110 + nsize + 3) & ~3
        i = (off + size + 3) & ~3
        if name != 'TRAILER!!!':
            files[name] = (mode, hashlib.sha1(d[off:off + size]).hexdigest(), size)

a, b = read(sys.argv[1]), read(sys.argv[2])
print('%d entries vs %d entries' % (len(a), len(b)))
diff = 0
for n in sorted(set(a) | set(b)):
    if a.get(n) != b.get(n):
        diff += 1
        print('ADDED  ' if n not in a else 'REMOVED' if n not in b else 'CHANGED', n, a.get(n, ('', '', 0))[2], '->', b.get(n, ('', '', 0))[2])
print('identical' if not diff else '%d differences' % diff)
