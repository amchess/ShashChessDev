#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
zones_diff.py - confronto fra due dump di zone Shashin (v2).

Con le zone definite dalla win probability, ogni cambio di zona fra ShashChess
41 e 42 deve avere una di queste spiegazioni:

  PROBABILITA'  la win probability e' passata oltre una soglia: la rete nuova
                giudica la posizione diversamente. Fisiologico.
  REGOLA        stessa fascia di probabilita', zona diversa: e' l'effetto della
                regola CAPABLANCA/CAOS a probabilita' 50 (o di una regola cambiata
                di proposito). Va ricondotto a una decisione documentata.
  INCOERENTE    la zona riportata non corrisponde alla probabilita' riportata
                dallo stesso motore. E' un difetto: copia divergente del modello
                o delle soglie. Blocca il porting.

Uso
---
  python tools/zones_diff.py zones_sc41.tsv zones_sc42.tsv
  python tools/zones_diff.py zones_sc41.tsv zones_sc42.tsv --column zona --examples 20
  python tools/zones_diff.py zones_sc41.tsv zones_sc42.tsv --max-drift 1 --fail-on-drift

Salti di 2+ classi sono ammessi solo se spiegati dalla probabilita'; con
--fail-on-drift il comando esce con 1 se trova INCOERENTI o salti non spiegati.
"""

import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sc_uci import ZONE_ORDER, ZONE_SHORT, canon_zone, zone_of_probability  # noqa: E402


def load(path, column):
    rows = {}
    with open(path, encoding='utf-8', errors='replace') as fh:
        cols = fh.readline().rstrip('\n').split('\t')
        if 'fen' not in cols:
            sys.exit(f"{path}: intestazione inattesa")
        col = column if column in cols else ('zona_ricerca' if 'zona_ricerca' in cols else 'zona')
        ix = {c: i for i, c in enumerate(cols)}
        for line in fh:
            p = line.rstrip('\n').split('\t')
            get = lambda c: p[ix[c]] if c in ix and ix[c] < len(p) else ''
            # la zona di ricerca va confrontata con la probabilita' di chi muove
            wp = get('wp_tratto') if col == 'zona_ricerca' and 'wp_tratto' in ix else get('winprob')
            rows[get('fen')] = {'zone': canon_zone(get(col)), 'wp': int(wp) if wp.isdigit() else None,
                                'eval': get('eval')}
    return rows, col


def consistent(zone, wp):
    if wp is None or not zone:
        return True
    if wp == 50:
        return zone in ('CAPABLANCA', 'TAL_CAPABLANCA_PETROSIAN')
    return zone_of_probability(wp) == zone


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base')
    ap.add_argument('cand')
    ap.add_argument('--column', default='zona_ricerca', help='zona | zona_ricerca (default)')
    ap.add_argument('--max-drift', type=int, default=1)
    ap.add_argument('--fail-on-drift', action='store_true')
    ap.add_argument('--examples', type=int, default=10)
    args = ap.parse_args()

    A, ca = load(args.base, args.column)
    B, cb = load(args.cand, args.column)
    common = [f for f in A if f in B and A[f]['zone'] and B[f]['zone']]
    if not common:
        sys.exit("Nessuna FEN in comune con zona valorizzata.")
    idx = {z: i for i, z in enumerate(ZONE_ORDER)}
    mat, kinds = Counter(), Counter()
    drifts, incoh = [], []
    for fen in common:
        a, b = A[fen], B[fen]
        mat[(a['zone'], b['zone'])] += 1
        for side, r in (('base', a), ('candidato', b)):
            if not consistent(r['zone'], r['wp']):
                incoh.append((side, fen, r))
        if a['zone'] == b['zone']:
            continue
        if a['wp'] is not None and b['wp'] is not None and zone_of_probability(a['wp']) != zone_of_probability(b['wp']):
            kind = 'PROBABILITA'
        else:
            kind = 'REGOLA'
        kinds[kind] += 1
        ia, ib = idx.get(a['zone'], 13), idx.get(b['zone'], 13)
        chaos = 'TAL_CAPABLANCA_PETROSIAN' in (a['zone'], b['zone'])
        d = abs(ia - ib) if not chaos else 1
        drifts.append((d, kind, fen, a, b))

    n = len(common)
    same = n - len(drifts)
    print(f"base: {args.base} [{ca}]   candidato: {args.cand} [{cb}]   in comune: {n}\n")
    print(f"  zona identica          : {same:>7} ({100 * same / n:.1f} %)")
    print(f"  cambio per PROBABILITA : {kinds['PROBABILITA']:>7} ({100 * kinds['PROBABILITA'] / n:.1f} %)  fisiologico")
    print(f"  cambio per REGOLA      : {kinds['REGOLA']:>7} ({100 * kinds['REGOLA'] / n:.1f} %)  da ricondurre a una decisione")
    print(f"  INCOERENTI             : {len(incoh):>7}  <-- zona diversa da quella della propria probabilita'")
    big = [x for x in drifts if x[0] > args.max_drift and x[1] != 'PROBABILITA']
    print(f"  salti > {args.max_drift} non spiegati   : {len(big):>7}")

    present = [z for z in ZONE_ORDER if any(z in k for k in mat)]
    print("\nMatrice (righe = base, colonne = candidato)\n")
    print(' ' * 7 + ''.join(f"{ZONE_SHORT[z]:>6}" for z in present))
    for za in present:
        print(f"{ZONE_SHORT[za]:<7}" + ''.join(f"{(mat.get((za, zb), 0) or '.'):>6}" for zb in present))

    if drifts and args.examples:
        print(f"\nEsempi (per ampiezza):")
        for d, kind, fen, a, b in sorted(drifts, key=lambda x: -x[0])[:args.examples]:
            print(f"  [{d}] {kind:<11} {a['zone']} ({a['wp']}%) -> {b['zone']} ({b['wp']}%)   {fen}")
    if incoh:
        print("\nINCOERENTI (prime 10):")
        for side, fen, r in incoh[:10]:
            print(f"  {side}: zona {r['zone']} con probabilita' {r['wp']}%   {fen}")
    return 1 if args.fail_on_drift and (incoh or big) else 0


if __name__ == '__main__':
    sys.exit(main())
