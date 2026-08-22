#!/usr/bin/env python3
"""Convert binary PPM (P6) to PNG. Dev convenience only - the engine
itself never depends on Python."""
import sys, zlib, struct

def read_ppm(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'P6'):
        raise SystemExit(f'{path}: not a P6 PPM')
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(d) and d[pos:pos+1].isspace():
            pos += 1
        if d[pos:pos+1] == b'#':
            while d[pos:pos+1] not in (b'\n', b''):
                pos += 1
            continue
        start = pos
        while pos < len(d) and not d[pos:pos+1].isspace():
            pos += 1
        fields.append(int(d[start:pos]))
    pos += 1
    w, h, _maxv = fields
    return w, h, d[pos:pos + w*h*3]

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)

for src in sys.argv[1:]:
    w, h, rgb = read_ppm(src)
    dst = src.rsplit('.', 1)[0] + '.png'
    write_png(dst, w, h, rgb)
    print(f'{dst}  {w}x{h}')
