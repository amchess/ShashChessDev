#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_epd_corpus.py - corpus EPD stratificato per ZONA SHASHIN x MATERIALE (v2).

Perche'
-------
Le posizioni di partite reali stanno quasi tutte vicino alla parita': le zone
estreme e quelle strette (LOW_PETROSIAN 21-24, LOW_TAL 76-79, CAPABLANCA = 50)
restano quasi vuote, e proprio li' si vede un modello WDL o una regola di zona
sbagliati. Questo script riempie una griglia zona x fascia di materiale, con lo
stesso numero di posizioni per cella, usando la zona che la RICERCA di
ShashChess 41 assegnerebbe (regola di getShashinRange sui W/D/L della trace).

Come genera le posizioni
------------------------
Pilota il motore di baseline via UCI (nessuna dipendenza esterna):
  go perft 1 -> mosse legali;  d -> FEN;  go depth N -> mossa del motore;
  eval -> W/D/L e zona.
Le passeggiate alternano mosse del motore e mosse casuali: le prime tengono le
posizioni plausibili, le seconde le spingono verso le zone estreme.
Ogni FEN e' validata in Python PRIMA di arrivare al motore: da SF19 una FEN
invalida stampa 'CRITICAL ERROR' e termina il processo.

Uso
---
  python tools/make_epd_corpus.py --engine ./sc41 --out positions.epd -n 6000
  python tools/make_epd_corpus.py --engine ./sc41 --seed-epd suite325.epd --out positions.epd -n 10000
  python tools/make_epd_corpus.py --analyze positions.epd
"""

import argparse
import os
import random
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sc_uci import (EngineDied, Engine, ZONE_ORDER, ZONE_SHORT, canon_zone, material_of,  # noqa: E402
                    zone_legacy_from_wdl)

CURATED = [
    ("startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
    ("kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"),
    ("perft3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"),
    ("perft4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1"),
    ("perft5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"),
    ("perft6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10"),
    ("zugzwang-lasker", "8/k7/3p4/p2P1p2/P2P1P2/8/8/K7 w - - 0 1"),
    ("endgame-kp", "8/8/8/8/8/6k1/6p1/6K1 w - - 0 1"),
    ("lucena", "1K1k4/1P6/8/8/8/8/r7/2R5 w - - 0 1"),
    ("philidor", "8/8/8/8/4pk2/8/R7/4K2r w - - 0 1"),
    ("najdorf", "rnbqkb1r/1p3ppp/p2ppn2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w KQkq - 0 7"),
    ("chess960", "bqnb1rkr/pp3ppp/3ppn2/2p5/5P2/P2P4/NPP1P1PP/BQ1BNRKR w HFhf - 2 9"),
]
PIECES = set('pnbrqkPNBRQK')
MOVE_RE = re.compile(r'^([a-h][1-8][a-h][1-8][qrbnQRBN]?):\s*(\d+)\s*$')
FEN_RE = re.compile(r'^Fen:\s*(.+?)\s*$')
WDL_RE = re.compile(r'WDL \(Win/Draw/Loss\):\s*(\d+)%/(\d+)%/(\d+)%')
ZONE_RE = re.compile(r'Shashin Zone:\s*(.+?)\s*$')
NONE_RE = re.compile(r'Final evaluation:\s*none')
BEST_RE = re.compile(r'^bestmove\s+(\S+)')


def validate_fen(fen):
    parts = fen.split()
    if len(parts) < 4:
        return "campi insufficienti"
    board, stm, castling, ep = parts[:4]
    ranks = board.split('/')
    if len(ranks) != 8:
        return "traverse != 8"
    kings = Counter()
    for i, rank in enumerate(ranks):
        n = 0
        for ch in rank:
            if ch.isdigit():
                n += int(ch)
            elif ch in PIECES:
                n += 1
                kings[ch] += ch in 'kK'
                if ch in 'pP' and i in (0, 7):
                    return "pedone in prima/ottava"
            else:
                return f"carattere '{ch}'"
        if n != 8:
            return "traversa incompleta"
    if kings['K'] != 1 or kings['k'] != 1:
        return "numero di re"
    if stm not in 'wb' or len(stm) != 1:
        return "tratto"
    if castling != '-' and not re.fullmatch(r'[KQkqA-Ha-h]+', castling):
        return "arrocco"
    if ep != '-' and not re.fullmatch(r'[a-h][36]', ep):
        return "en passant"
    if len(parts) >= 6 and (not parts[4].isdigit() or not parts[5].isdigit()):
        return "contatori"
    return None


def material_bucket(m):
    return 'M17-20' if m <= 20 else 'M21-32' if m <= 32 else 'M33-46' if m <= 46 else \
        'M47-62' if m <= 62 else 'M63-78'


def probe_legal(eng):
    return [m.group(1) for l in eng.command('go perft 1') for m in [MOVE_RE.match(l.strip())] if m]


def probe_fen(eng):
    for l in eng.command('d'):
        m = FEN_RE.match(l.strip())
        if m:
            return m.group(1)
    return None


def probe_zone(eng):
    """(zona_ricerca, winprob) oppure None se sotto scacco / trace illeggibile."""
    lines = eng.command('eval')
    if any(NONE_RE.search(l) for l in lines):
        return None
    for l in lines:
        m = WDL_RE.search(l)
        if m:
            W, D, L = map(int, m.groups())
            return zone_legacy_from_wdl(W, D, L), W + D // 2
    for l in lines:
        m = ZONE_RE.search(l)
        if m:
            return canon_zone(m.group(1)), None
    return None


def probe_best(eng, depth):
    eng.send(f'go depth {depth}')
    for l in eng.wait_for('bestmove', collect=True):
        m = BEST_RE.match(l)
        if m:
            return None if m.group(1) in ('(none)', '0000') else m.group(1)
    return None


def analyze(path):
    cells, n = Counter(), 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'c0 "z=(\S+) p=(\d+|-) m=(\d+)"', line)
        if m:
            n += 1
            cells[(m.group(1), material_bucket(int(m.group(3))))] += 1
    buckets = ['M17-20', 'M21-32', 'M33-46', 'M47-62', 'M63-78']
    print(f"{path}: {n} posizioni annotate\n")
    print(f"{'zona':<7}" + ''.join(f"{b:>9}" for b in buckets))
    for z in ZONE_ORDER:
        print(f"{ZONE_SHORT[z]:<7}" + ''.join(f"{cells.get((z, b), 0):>9}" for b in buckets))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--engine')
    ap.add_argument('--out')
    ap.add_argument('-n', '--target', type=int, default=6000)
    ap.add_argument('--per-cell', type=int, default=0, help='posizioni per cella (default: target / celle)')
    ap.add_argument('--seed-epd')
    ap.add_argument('--analyze', metavar='FILE.epd')
    ap.add_argument('--walk-min', type=int, default=6)
    ap.add_argument('--walk-max', type=int, default=90)
    ap.add_argument('--engine-move-prob', type=float, default=0.6)
    ap.add_argument('--walk-depth', type=int, default=6)
    ap.add_argument('--sample-every', type=int, default=3)
    ap.add_argument('--max-walks', type=int, default=200000)
    ap.add_argument('--seed', type=int, default=20260911)
    args = ap.parse_args()
    if args.analyze:
        return analyze(args.analyze)
    if not args.engine or not args.out:
        ap.error("servono --engine e --out (oppure --analyze)")
    random.seed(args.seed)
    n_cells = len(ZONE_ORDER) * 5
    cap = args.per_cell or max(1, args.target // n_cells + 1)

    seeds = [fen for _, fen in CURATED if not validate_fen(fen)]
    if args.seed_epd:
        for raw in open(args.seed_epd, encoding='utf-8', errors='replace'):
            p = raw.split()
            if len(p) < 4 or raw.startswith('#'):
                continue
            fen = ' '.join(p[:6]) if len(p) >= 6 and p[4].isdigit() and p[5].isdigit() else ' '.join(p[:4]) + ' 0 1'
            if not validate_fen(fen):
                seeds.append(fen)
    eng = Engine(args.engine, {'Persisted learning': 'Off', 'UCI_ShowWDL': 'true', 'Experience Book': 'false'})
    grid = defaultdict(list)
    seen, kept, walks = set(), 0, 0

    def consider(fen):
        nonlocal kept
        key = ' '.join(fen.split()[:4])
        if key in seen or validate_fen(fen):
            return
        seen.add(key)
        zp = probe_zone(eng)
        if not zp:
            return
        cell = (zp[0], material_bucket(material_of(fen)))
        if len(grid[cell]) < cap:
            grid[cell].append((fen, zp[0], zp[1]))
            kept += 1

    try:
        for fen in seeds:
            eng.position(fen)
            consider(fen)
        while kept < args.target and walks < args.max_walks:
            walks += 1
            root, moves = random.choice(seeds), []
            for ply in range(random.randint(args.walk_min, args.walk_max)):
                eng.position(root, moves)
                legal = probe_legal(eng)
                if not legal:
                    break
                mv = probe_best(eng, args.walk_depth) if random.random() < args.engine_move_prob else None
                moves.append(mv if mv in legal else random.choice(legal))
                if ply >= args.walk_min and ply % args.sample_every == 0:
                    eng.position(root, moves)
                    fen = probe_fen(eng)
                    if fen:
                        consider(fen)
            if walks % 50 == 0:
                full = sum(1 for c in grid.values() if len(c) >= cap)
                print(f"  passeggiate {walks}, posizioni {kept}, celle piene {full}/{n_cells}", file=sys.stderr)
    finally:
        eng.quit()

    with open(args.out, 'w', encoding='utf-8', newline='\n') as fh:
        for cell in sorted(grid):
            for fen, z, p in grid[cell]:
                fh.write(f'{fen} c0 "z={z} p={p if p is not None else "-"} m={material_of(fen)}";\n')
    empty = [c for c in ((z, b) for z in ZONE_ORDER for b in ('M17-20', 'M21-32', 'M33-46', 'M47-62', 'M63-78'))
             if not grid.get(c)]
    print(f"Scritto {args.out}: {kept} posizioni in {len(grid)} celle (cap {cap}); {walks} passeggiate.")
    if empty:
        print(f"Celle vuote: {len(empty)} (alcune combinazioni sono rare per natura, es. CAPABLANCA con molto "
              f"materiale). Aggiungi semi con --seed-epd per coprirle.")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (EngineDied, RuntimeError) as e:
        sys.exit(f"ERRORE: {e}")
