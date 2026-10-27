#!/usr/bin/env python3
"""Checks DKR .eep and .mpk save files against the N64 layout (big-endian), independently of the port's code.

Usage: n64_save_check.py dkr.eep dkr.mpk ...  Prints each checksum the game and libultra check, and the ghosts.
"""
import struct, sys

def check_eep(d):
    out = []
    v, = struct.unpack('>Q', d[0x78:0x80])
    ck = 5 + sum((v >> (i * 4)) & 0xF for i in range(14))
    out.append('settings block 15: %016x stored checksum %02x computed %02x %s' % (v, v >> 56, ck & 0xFF, 'OK' if (v >> 56) == (ck & 0xFF) else 'BAD'))
    for f in range(3):
        b = d[f * 40:(f + 1) * 40]
        if b == b'\xff' * 40:
            out.append('file %d: empty' % f); continue
        s, = struct.unpack('>H', b[:2])
        c = (5 + sum(b[2:40])) & 0xFFFF
        out.append('file %d: checksum %04x computed %04x %s' % (f, s, c, 'OK' if s == c else 'BAD'))
    for name, off in (('flap times', 0x80), ('course times', 0x80 + 192)):
        b = d[off:off + 192]
        s, = struct.unpack('>H', b[:2])
        c = (5 + sum(b[2:192])) & 0xFFFF
        out.append('%s: checksum %04x computed %04x %s' % (name, s, c, 'OK' if s == c else ('(never written)' if b == bytes(192) else 'BAD')))
    return out

def check_mpk(d):
    out = []
    for off in (0x20, 0x60, 0x80, 0xC0):
        words = struct.unpack('>14H', d[off:off + 28])
        cs, ics = struct.unpack('>HH', d[off + 28:off + 32])
        s = sum(words) & 0xFFFF
        out.append('id@%02x: checksum %04x computed %04x inv %04x %s banks %d' % (off, cs, s, ics,
                   'OK' if cs == s and ics == (0xFFF2 - s) & 0xFFFF else 'BAD', d[off + 0x1A]))
    for t in (0x100, 0x200):
        ents = struct.unpack('>128H', d[t:t + 256])
        sp = [e for e in ents[5:]]
        free = sum(1 for e in sp if e == 3)
        eof = sum(1 for e in sp if e == 1)
        links = [e for e in sp if e >= 5]
        bad = [e for e in sp if e not in (1, 3) and not (5 <= (e & 0xFF) < 128 and (e >> 8) == 0)]
        ck = sum(d[t + 10:t + 256]) & 0xFF
        out.append('inode@%03x: free %d eof %d links %d odd %s checksum byte %02x computed %02x %s' % (t, free, eof, len(links), bad[:5], d[t + 1], ck, 'OK' if d[t + 1] == ck else 'BAD'))
    for i in range(16):
        e = d[0x300 + i * 32:0x320 + i * 32]
        if e == bytes(32):
            continue
        game, comp, start = struct.unpack('>IHH', e[:8])
        pages = []
        p = start
        while 5 <= p < 128 and len(pages) < 128:
            pages.append(p)
            p, = struct.unpack('>H', d[0x100 + p * 2:0x102 + p * 2])
        data = b''.join(d[p * 256:(p + 1) * 256] for p in pages)
        out.append('file %d: game %r company %04x start %d pages %d end %d tag %r' % (i, game.to_bytes(4, 'big'), comp, start, len(pages), p, data[:4]))
        if data[:4] == b'GHSS':
            for k in range(7):
                lv, vh, o = struct.unpack('>BBH', data[4 + k * 4:8 + k * 4])
                if lv == 0xFF:
                    continue
                cs, ch, un, tm, n = struct.unpack('>hBBhh', data[o:o + 8])
                c = sum(data[o + 2:o + 8 + n * 12]) & 0xFFFF
                c = c - 0x10000 if c >= 0x8000 else c
                x, y, z = struct.unpack('>hhh', data[o + 8:o + 14])
                out.append('  ghost level %d vehicle %d off %d char %d time %d nodes %d checksum %s first node %d,%d,%d' % (lv, vh, o, ch, tm, n, 'OK' if c == cs else 'BAD %d vs %d' % (cs, c), x, y, z))
    return out

for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    print(path)
    # By size, not name: an EEPROM is 512 bytes (4 Kbit) or 2 KB (16 Kbit), a Controller Pak 32 KB.
    if len(d) <= 2048:
        lines = check_eep(d)
    elif len(d) == 32768:
        lines = check_mpk(d)
    else:
        lines = ['unknown save size %d bytes (EEPROM: 512 or 2048, Controller Pak: 32768)' % len(d)]
    for line in lines:
        print('  ' + line)
