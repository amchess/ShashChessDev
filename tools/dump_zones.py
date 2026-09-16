#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dump_zones.py - per ogni FEN di un corpus: win probability, W/D/L e zona Shashin (v2).

Le zone sono funzioni della win probability. Questo dump registra quindi prima
di tutto W/D/L e la probabilita', e poi la zona; il confronto fra ShashChess 41
e 42 (zones_diff.py) dice se ogni cambio di zona e' spiegato da un cambio di
probabilita' (fisiologico: rete nuova) o no (difetto di porting).

Modalita'
---------
  static   comando 'eval' con UCI_ShowWDL: legge "Final Static Activity Evaluation",
           "WDL (Win/Draw/Loss)" e "Shashin Zone". Scrive due zone:
             zona         = quella stampata dalla trace (punto di vista del Bianco, senza CAOS)
             zona_ricerca = quella che userebbe la ricerca: punto di vista di chi
                            muove, regola di ShashChess 41 applicata agli stessi W/D/L
                            (wp_tratto = la sua win probability)
  shashin  comando UCI di debug 'shashin' (proposta: cpp/uci_shashin.snippet.cpp),
           che stampa una riga stabile con la zona VERA della ricerca. E' la
           modalita' da usare in CI appena il comando esiste.
  search   'go depth N': score, wdl (riportati al punto di vista del Bianco) e bestmove.

Uso
---
  python tools/dump_zones.py --engine ./sc41 --epd positions.epd --out zones_sc41.tsv
  python tools/dump_zones.py --engine ./sc42 --epd positions.epd --out zones_sc42.tsv
  python tools/dump_zones.py --engine ./sc42 --epd positions.epd --mode search --depth 14 --out z42_d14.tsv
  python tools/zones_diff.py zones_sc41.tsv zones_sc42.tsv

Il learning e il libro vengono spenti: il dump deve essere riproducibile.
I file vengono scritti direttamente (UTF-8): in PowerShell non usare '>' .
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sc_uci import EngineDied, Engine, canon_zone, material_of, read_epd, zone_legacy_from_wdl  # noqa: E402

ZONE_RE = re.compile(r'Shashin Zone:\s*(.+?)\s*$')
STATIC_RE = re.compile(r'Final Static Activity Evaluation:\s*([-+0-9.]+)\s*cp\s*\|\s*(\d+)%')
FINAL_RE = re.compile(r'Final evaluation\s+([-+0-9.]+)')
WDL_RE = re.compile(r'WDL \(Win/Draw/Loss\):\s*(\d+)%/(\d+)%/(\d+)%')
SHASHIN_RE = re.compile(r'^shashin\s+zone\s+(\S+)\s+winprob\s+(\d+)\s+wdl\s+(\d+)\s+(\d+)\s+(\d+)')
INFO_SCORE_RE = re.compile(r'\bscore (cp|mate) (-?\d+)')
INFO_WDL_RE = re.compile(r'\bwdl (\d+) (\d+) (\d+)')
BESTMOVE_RE = re.compile(r'^bestmove\s+(\S+)')

QUIET = {'Persisted learning': 'Off', 'Experience Book': 'false', 'MCTS by Shashin': 'false',
         'Variety': 'Off', 'Concurrent Experience': 'false'}


def probe_static(eng, fen):
    lines = eng.command(f'position fen {fen}\neval')
    ev = wp = zone = ''
    W = D = L = None
    for l in lines:
        m = STATIC_RE.search(l)
        if m:
            ev, wp = m.group(1), m.group(2)
        elif not ev:
            m = FINAL_RE.search(l)
            if m:
                ev = m.group(1)
        m = WDL_RE.search(l)
        if m:
            W, D, L = map(int, m.groups())
        m = ZONE_RE.search(l)
        if m:
            zone = canon_zone(m.group(1))
    zr = wp_stm = ''
    if W is not None:
        Ws, Ls = (W, L) if fen.split()[1] == 'w' else (L, W)     # la ricerca ragiona per chi muove
        zr, wp_stm = zone_legacy_from_wdl(Ws, D, Ls), str(Ws + D // 2)
    return ev, wp, (W, D, L), zone, wp_stm, zr, ''


def probe_shashin(eng, fen):
    for l in eng.command(f'position fen {fen}\nshashin'):
        m = SHASHIN_RE.match(l.strip())
        if m:
            z, p, w, d, lo = m.groups()                  # prospettiva di chi muove, come la ricerca
            w, d, lo = int(w), int(d), int(lo)
            Ww, Lw = (w, lo) if fen.split()[1] == 'w' else (lo, w)
            return '', str(Ww + d // 2), (Ww, d, Lw), '', p, canon_zone(z), ''
        if l.startswith('Unknown command'):
            sys.exit("il motore non ha il comando 'shashin': usa --mode static, oppure integra "
                     "cpp/uci_shashin.snippet.cpp")
    return '', '', (None, None, None), '', '', '', ''


def probe_search(eng, fen, depth):
    eng.send('ucinewgame')
    eng.position(fen)
    eng.send(f'go depth {depth}')
    score = best = ''
    wdl = None
    for l in eng.wait_for('bestmove', collect=True):
        m = INFO_SCORE_RE.search(l)
        if m:
            score = ('#' if m.group(1) == 'mate' else '') + m.group(2)
        m = INFO_WDL_RE.search(l)
        if m:
            wdl = tuple(int(x) for x in m.groups())
        m = BESTMOVE_RE.match(l)
        if m:
            best = m.group(1)
    if not wdl:
        return score, '', (None, None, None), '', '', '', best
    ws, d, ls = (round(x / 10) for x in wdl)           # UCI: punto di vista di chi muove
    zr = zone_legacy_from_wdl(ws, d, ls)
    W, L = (ws, ls) if fen.split()[1] == 'w' else (ls, ws)
    return score, str(W + d // 2), (W, d, L), '', str(ws + d // 2), zr, best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--engine', required=True)
    ap.add_argument('--epd', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--mode', choices=('static', 'shashin', 'search'), default='static')
    ap.add_argument('--depth', type=int, default=14)
    ap.add_argument('--hash', type=int, default=64)
    ap.add_argument('--threads', type=int, default=1)
    ap.add_argument('--syzygy')
    ap.add_argument('--limit', type=int)
    args = ap.parse_args()

    positions = read_epd(args.epd)[:args.limit] if args.limit else read_epd(args.epd)
    opts = dict(QUIET, UCI_ShowWDL='true')
    if args.syzygy:
        opts['SyzygyPath'] = args.syzygy
    eng = Engine(args.engine, opts, hash_mb=args.hash, threads=args.threads)
    n_ok = n_blank = 0
    try:
        with open(args.out, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('fen\tmateriale\teval\twinprob\tW\tD\tL\tzona\twp_tratto\tzona_ricerca\tbestmove\n')
            for i, (fen, _) in enumerate(positions, 1):
                if args.mode == 'static':
                    r = probe_static(eng, fen)
                elif args.mode == 'shashin':
                    r = probe_shashin(eng, fen)
                else:
                    r = probe_search(eng, fen, args.depth)
                ev, wp, (W, D, L), zone, wps, zr, best = r
                if zone or zr:
                    n_ok += 1
                else:
                    n_blank += 1
                fmt = lambda x: '' if x is None else str(x)
                fh.write(f"{fen}\t{material_of(fen)}\t{ev}\t{wp}\t{fmt(W)}\t{fmt(D)}\t{fmt(L)}\t{zone}\t{wps}\t{zr}\t{best}\n")
                if i % 250 == 0:
                    print(f"  {i}/{len(positions)}", file=sys.stderr)
    finally:
        eng.quit()
    print(f"Scritto {args.out}: {len(positions)} posizioni ({eng.id_name}), {n_ok} con zona, {n_blank} senza "
          f"(le posizioni sotto scacco non hanno valutazione statica).")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (EngineDied, RuntimeError) as e:
        sys.exit(f"ERRORE: {e}")
