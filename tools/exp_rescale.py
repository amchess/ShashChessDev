#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
exp_rescale.py - i file .exp dopo il cambio di scala della valutazione.

Il problema
-----------
Il formato binario dei .exp NON cambia fra ShashChess 41 e 42: record da 24 byte

    u64 key | i32 depth | i32 score | u16 move | 2 byte di padding | i32 performance

(PersistedLearningMove, little-endian, allineamento naturale). Ma 'score' e' in
UNITA' INTERNE, ed e' confrontato con alpha/beta dalla ricerca (expTTValue in
search.cpp). SF19 ha cambiato la scala interna: la valutazione che vale il 50 %
di vittoria passa da ~385 a ~304 a materiale 58 (fattore 0,789; da 0,741 a
0,885 secondo il materiale). Un .exp scritto da ShashChess 41 letto da
ShashChess 42 ha quindi punteggi gonfiati di circa il 27 %.

'performance' e' una win probability (0-100): non dipende dalla scala e resta.
I punteggi di matto e tablebase (|score| >= VALUE_TB_WIN_IN_MAX_PLY) e
VALUE_NONE non si toccano: le costanti sono identiche in SF18 e SF19.

Il materiale di ogni posizione non e' salvato nel record, quindi il fattore e'
unico per tutto il file (default: materiale 58). E' un'approssimazione migliore
del non fare nulla; l'alternativa e' lasciare che il learning riscriva le voci.

Uso
---
  python tools/exp_rescale.py stats experience.exp
  python tools/exp_rescale.py rescale experience.exp experience_sf19.exp
  python tools/exp_rescale.py rescale experience.exp out.exp --factor 0.80
  python tools/exp_rescale.py verify experience.exp experience_sf19.exp

Il file originale non viene mai modificato: si scrive sempre un file nuovo.
"""

import argparse
import os
import struct
import sys
from collections import Counter

REC = struct.Struct('<QiiH2xi')
assert REC.size == 24

MAX_PLY = 246
VALUE_MATE = 32000
VALUE_NONE = 32002
VALUE_TB = VALUE_MATE - MAX_PLY - 1
VALUE_TB_WIN_IN_MAX_PLY = VALUE_TB - MAX_PLY          # 31507

# a(materiale) dei modelli WDL (vedi wdl_calibration.py): fattore = a19/a18
AS18 = [-72.32565836, 185.93832038, -144.58862193, 416.44950446]
AS19 = [-142.72052667, 372.35176398, -340.71073572, 415.23490212]


def a_of(cs, material):
    m = max(17, min(78, material)) / 58.0
    return (((cs[0] * m + cs[1]) * m + cs[2]) * m) + cs[3]


def factor_for(material):
    return a_of(AS19, material) / a_of(AS18, material)


def records(path):
    data = open(path, 'rb').read()
    if len(data) % REC.size:
        sys.exit(f"{path}: {len(data)} byte, non multiplo di {REC.size}: non e' un .exp v0 di ShashChess")
    for off in range(0, len(data), REC.size):
        yield REC.unpack_from(data, off)


def is_eval_score(s):
    return abs(s) < VALUE_TB_WIN_IN_MAX_PLY and s != VALUE_NONE


def cmd_stats(args):
    n = mates = none = 0
    depth = Counter()
    buckets = Counter()
    for key, d, s, mv, perf in records(args.file):
        n += 1
        depth[min(d, 40)] += 1
        if s == VALUE_NONE:
            none += 1
        elif not is_eval_score(s):
            mates += 1
        else:
            buckets[max(-8, min(8, int(s / 100)))] += 1
    print(f"{args.file}: {n} voci ({os.path.getsize(args.file)} byte)")
    print(f"  punteggi di valutazione: {n - mates - none}   matto/TB: {mates}   VALUE_NONE: {none}")
    print("  distribuzione dei punteggi (centinaia di unita' interne):")
    for b in sorted(buckets):
        print(f"    {b * 100:+6d}  {buckets[b]:>9}")
    print("  profondita': " + ' '.join(f"{k}:{v}" for k, v in sorted(depth.items())))
    return 0


def cmd_rescale(args):
    if os.path.abspath(args.src) == os.path.abspath(args.dst):
        sys.exit("src e dst coincidono: scrivo sempre un file nuovo.")
    f = args.factor if args.factor else factor_for(args.material)
    tmp = args.dst + '.tmp'
    n = changed = 0
    with open(tmp, 'wb') as out:
        for key, d, s, mv, perf in records(args.src):
            n += 1
            if is_eval_score(s):
                s2 = int(round(s * f))
                changed += int(s2 != s)
                s = s2
            out.write(REC.pack(key, d, s, mv, perf))
    if not args.dry_run:
        os.replace(tmp, args.dst)
    else:
        os.remove(tmp)
    print(f"{n} voci, {changed} punteggi riscalati con fattore {f:.4f}"
          + (f" (materiale {args.material})" if not args.factor else '')
          + ('' if not args.dry_run else '  [dry-run: nessun file scritto]'))
    if not args.dry_run:
        print(f"scritto {args.dst}. Chiavi, mosse, profondita' e performance invariate.")
    return 0


def cmd_verify(args):
    A, B = list(records(args.a)), list(records(args.b))
    if len(A) != len(B):
        print(f"numero di voci diverso: {len(A)} vs {len(B)}")
        return 1
    bad = 0
    ratios = []
    for (k1, d1, s1, m1, p1), (k2, d2, s2, m2, p2) in zip(A, B):
        if (k1, d1, m1, p1) != (k2, d2, m2, p2):
            bad += 1
        if is_eval_score(s1) and s1 and abs(s1) >= 50:
            ratios.append(s2 / s1)
        elif not is_eval_score(s1) and s1 != s2:
            bad += 1
    print(f"{len(A)} voci: {bad} con chiave/mossa/profondita'/performance o matti diversi")
    if ratios:
        ratios.sort()
        print(f"rapporto dei punteggi: min {ratios[0]:.3f}  mediana {ratios[len(ratios) // 2]:.3f}  max {ratios[-1]:.3f}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('stats')
    s.add_argument('file')
    s.set_defaults(fn=cmd_stats)
    r = sub.add_parser('rescale')
    r.add_argument('src')
    r.add_argument('dst')
    r.add_argument('--material', type=int, default=58)
    r.add_argument('--factor', type=float)
    r.add_argument('--dry-run', action='store_true')
    r.set_defaults(fn=cmd_rescale)
    v = sub.add_parser('verify')
    v.add_argument('a')
    v.add_argument('b')
    v.set_defaults(fn=cmd_verify)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
