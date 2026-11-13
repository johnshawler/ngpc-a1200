#!/usr/bin/env python3
"""check_gates.py - decide whether a bench run passes the brief's gates.

usage: check_gates.py <bench.txt> --class planar|perpixel --ticks N [--host HEX] [--expect HEX]

planar:   late must be 0 and worst_pct at most 80.
perpixel: no 50 Hz gate; shown_fps is reported.
both:     ticks must equal N, and the checksum must equal the host test's (--host) when one is given.
"""
import re, sys

def main():
    a = sys.argv[1:]
    if not a: print(__doc__); return 2
    path, opt = a[0], dict(zip(a[1::2], a[2::2]))
    try: text = open(path).read()
    except OSError: print('FAIL: no bench output (%s)' % path); return 1
    f = dict(re.findall(r'(\w+)=([0-9a-fA-F.]+)', text))
    need = ['frames', 'late', 'worst_pct', 'shown_fps', 'ticks', 'checksum']
    missing = [k for k in need if k not in f]
    if missing:
        print('FAIL: the program did not write a complete bench.txt (missing %s). Did it crash, hang, or run out of time?' % ', '.join(missing))
        print(text.strip()); return 1
    cls, problems = opt.get('--class', 'planar'), []
    if int(f['ticks']) != int(opt.get('--ticks', f['ticks'])):
        problems.append('ticks=%s, expected %s' % (f['ticks'], opt['--ticks']))
    if cls == 'planar':
        if int(f['late']) != 0: problems.append('late=%s: %s vertical blanks went by without a new picture (must be 0)' % (f['late'], f['late']))
        if int(f['worst_pct']) > 80: problems.append('worst_pct=%s: the slowest frame is over 80%% of the budget' % f['worst_pct'])
    for name, key in (('host test', '--host'), ('expected value', '--expect')):
        if opt.get(key):
            want = opt[key].lower().zfill(8)
            if f['checksum'].lower().zfill(8) != want: problems.append('checksum %s differs from the %s %s' % (f['checksum'], name, want))
    print('class=%s (the gates applied)' % cls)
    print('frames=%s late=%s worst_pct=%s avg_pct=%s shown_fps=%s ticks=%s checksum=%s' % (f['frames'], f['late'], f['worst_pct'], f.get('avg_pct', '?'), f['shown_fps'], f['ticks'], f['checksum']))
    if 'game_speed' in f: print('game_speed=%s%%: game time against the clock (100 = kept)' % f['game_speed'])
    if not opt.get('--host') and not opt.get('--expect'): print('note: no host test in port.conf, so the checksum was not compared')
    if problems:
        for p in problems: print('FAIL:', p)
        return 1
    speed = ('logic by the clock' if int(f.get('game_speed', '100')) >= 99 else
             'game time at %s%% of the clock: it falls behind where a pass takes more than 8 frames' % f['game_speed'])
    print('PASS (%s)' % ('50 Hz held' if cls == 'planar' else 'per-pixel game: %s pictures a second, %s' % (f['shown_fps'], speed)))
    return 0

sys.exit(main())
