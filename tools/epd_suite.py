#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
epd_suite.py - suite di posizioni difficili: capacita' risolutiva, non Elo.

E' lo strumento principale della metodologia ShashChess per il porting: misura
esattamente quello che un match a tempi rapidi non vede, cioe' se il motore
trova ancora la mossa giusta in posizioni che puniscono il pruning aggressivo.
Ed e' molto piu' sensibile di un match: 325 posizioni danno un confronto
appaiato posizione per posizione, mentre 300 partite danno un solo numero con
un intervallo di confidenza di decine di Elo.

Suite nel repository:
  tests/Tests2026/325.epd                  325 posizioni, curate da Peter Martan,
                                           con punteggio per mossa (`c0 "Nf5=15, Nf6+=2"`)
  tests/HardPositions/258HardPositions.csv 258 posizioni con zona Shashin annotata
  tests/HardPositions/256-2023.epd, 384.epd, 2048.epd, HardPositions2022.epd

Punteggio
---------
Se l'opcode `c0` contiene coppie `mossa=punti`, si usa quel punteggio (e' la
convenzione delle suite di Martan: la mossa migliore vale 10-20, le alternative
accettabili 2-5). Altrimenti si usa `bm` (1 punto) e `am` (1 punto se la mossa
da evitare non viene giocata).

Oltre al punteggio si misurano:
  - tempo alla prima soluzione stabile: primo istante in cui la mossa migliore
    diventa corretta e non cambia piu' fino alla fine (dalle righe `info ... pv`).
    E' la misura che rivela una ricerca che "vede e poi perde" la mossa.
  - cambi di mossa migliore: quante volte la PV cambia radice. Un aumento netto
    dopo il porting segnala instabilita' della ricerca.
  - profondita' e nodi raggiunti: dicono se il motore e' piu' lento o piu' cieco.

Uso
---
  # baseline: ShashChess 41, 60 s per posizione
  python tools/epd_suite.py run --engine src/shashchess41 --epd tests/Tests2026/325.epd \\
      --movetime 60000 --threads 4 --hash 2048 --out porting/baseline/epd325_sc41.tsv

  # candidato
  python tools/epd_suite.py run --engine src/shashchess --epd tests/Tests2026/325.epd \\
      --movetime 60000 --threads 4 --hash 2048 --out porting/epd325_sc42.tsv

  # confronto appaiato (test di McNemar sulle posizioni risolte)
  python tools/epd_suite.py compare porting/baseline/epd325_sc41.tsv porting/epd325_sc42.tsv

  # per la CI: a nodi fissi, indipendente dall'hardware
  python tools/epd_suite.py run --engine src/shashchess --epd tests/Tests2026/325.epd \\
      --nodes 20000000 --out /tmp/epd325_ci.tsv

  # ripartizione per zona (corpus con annotazione c0 "z=..." o CSV con Position type)
  python tools/epd_suite.py compare a.tsv b.tsv --by-zone

Richiede `python-chess` (pip install chess) per interpretare le mosse in
notazione algebrica delle suite. Tutto il resto degli strumenti non ha dipendenze.
"""

import argparse
import csv
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sc_uci import EngineDied, Engine, canon_zone  # noqa: E402

try:
    import chess
except ImportError:
    chess = None

INFO_DEPTH_RE = re.compile(r'\bdepth (\d+)')
INFO_NODES_RE = re.compile(r'\bnodes (\d+)')
INFO_TIME_RE = re.compile(r'\btime (\d+)')
INFO_PV_RE = re.compile(r'\bpv ([a-h][1-8][a-h][1-8][qrbnQRBN]?)')
INFO_SCORE_RE = re.compile(r'\bscore (cp|mate) (-?\d+)')
BESTMOVE_RE = re.compile(r'^bestmove\s+(\S+)')
PTS_RE = re.compile(r'(\S+?)\s*=\s*(\d+)')

QUIET = {'Persisted learning': 'Off', 'Experience Book': 'false', 'Concurrent Experience': 'false',
         'Variety': 'Off', 'LiveBook Lichess Games': 'false', 'LiveBook Lichess Masters': 'false',
         'LiveBook ChessDB': 'false'}


# --------------------------------------------------------------------- lettura suite
def parse_epd_line(line):
    """(fen, {mossa_san: punti}, set(am_san), id, zona) da una riga EPD."""
    line = line.strip()
    if not line or line.startswith('#'):
        return None
    fen_part = line.split(' bm ')[0].split(' am ')[0].split(' c0 ')[0].split(' id ')[0].strip().rstrip(';')
    p = fen_part.split()
    if len(p) < 4:
        return None
    fen = ' '.join(p[:6]) if len(p) >= 6 and p[4].isdigit() and p[5].isdigit() else ' '.join(p[:4]) + ' 0 1'
    pts, avoid = {}, set()
    m = re.search(r'\bc0\s+"([^"]*)"', line)
    if m and '=' in m.group(1):
        for part in m.group(1).split(','):
            mm = PTS_RE.match(part.strip())
            if mm:
                pts[mm.group(1).rstrip('!?')] = int(mm.group(2))
    m = re.search(r'\bbm\s+([^;]+)', line)
    if m and not pts:
        for mv in m.group(1).split():
            pts[mv.strip().rstrip(',;!?')] = 1
    m = re.search(r'\bam\s+([^;]+)', line)
    if m:
        avoid = {mv.strip().rstrip(',;!?') for mv in m.group(1).split()}
    ident = (re.search(r'\bid\s+"([^"]*)"', line) or [None, ''])[1] if re.search(r'\bid\s+"', line) else ''
    zm = re.search(r'\bc0\s+"[^"]*\bz=(\S+)', line)
    zone = canon_zone(zm.group(1)) if zm else ''
    return fen, pts, avoid, ident, zone


def read_suite(path):
    """Legge .epd oppure il CSV di HardPositions (colonne Epd / Position type)."""
    out = []
    if path.lower().endswith('.csv'):
        with open(path, encoding='utf-8-sig', errors='replace', newline='') as fh:
            sample = fh.read(4096)
            fh.seek(0)
            delim = ';' if sample.count(';') > sample.count(',') else ','
            rd = csv.reader(fh, delimiter=delim)
            head = next(rd, [])
            col = lambda frag: next((i for i, c in enumerate(head)
                                     if c.strip().lower().startswith(frag)), None)
            i_epd = col('epd') if col('epd') is not None else 0
            i_type = next((i for i, c in enumerate(head) if 'position type' in c.strip().lower()), None)
            i_prob = next((i for i, c in enumerate(head) if 'win probability' in c.strip().lower()), None)
            for row in rd:
                if not row or i_epd >= len(row):
                    continue
                epd = row[i_epd].strip().rstrip(',').rstrip(';')
                r = parse_epd_line(epd)
                if not r:
                    continue
                fen, pts, avoid, ident, zone = r
                if i_type is not None and i_type < len(row):
                    zone = canon_zone(row[i_type]) or zone
                if not ident and i_prob is not None and i_prob < len(row):
                    ident = f"winprob={row[i_prob].strip()}"
                out.append((fen, pts, avoid, ident, zone))
        return out
    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            r = parse_epd_line(line)
            if r and (r[1] or r[2]):
                out.append(r)
    return out


def san_to_uci(fen, names):
    """{san: uci} per le mosse indicate; ignora quelle illegali segnalandole."""
    if chess is None:
        sys.exit("epd_suite.py ha bisogno di python-chess per le mosse in notazione algebrica:\n"
                 "  pip install chess")
    board = chess.Board(fen)
    out, bad = {}, []
    for n in names:
        try:
            out[n] = board.parse_san(n).uci()
        except Exception:
            try:
                out[n] = board.parse_uci(n.lower()).uci()
            except Exception:
                bad.append(n)
    return out, bad


# --------------------------------------------------------------------- esecuzione
def probe(eng, fen, limit):
    """Analizza una posizione. Restituisce bestmove, storia della PV radice, depth, nodi, tempo."""
    eng.send('ucinewgame')
    eng.isready()
    eng.position(fen)
    t0 = time.time()
    eng.send(f'go {limit}')
    hist, best, depth, nodes, ms, score = [], '', 0, 0, 0, ''
    for l in eng.wait_for('bestmove', collect=True, timeout=3600):
        m = INFO_PV_RE.search(l)
        if m:
            d = int((INFO_DEPTH_RE.search(l) or [0, 0])[1]) if INFO_DEPTH_RE.search(l) else 0
            t = int(INFO_TIME_RE.search(l).group(1)) if INFO_TIME_RE.search(l) else int(1000 * (time.time() - t0))
            if not hist or hist[-1][0] != m.group(1):
                hist.append((m.group(1), t, d))
        if INFO_DEPTH_RE.search(l) and ' pv ' in l:
            depth = max(depth, int(INFO_DEPTH_RE.search(l).group(1)))
        if INFO_NODES_RE.search(l):
            nodes = max(nodes, int(INFO_NODES_RE.search(l).group(1)))
        if INFO_TIME_RE.search(l):
            ms = max(ms, int(INFO_TIME_RE.search(l).group(1)))
        m = INFO_SCORE_RE.search(l)
        if m:
            score = ('#' if m.group(1) == 'mate' else '') + m.group(2)
        m = BESTMOVE_RE.match(l)
        if m:
            best = m.group(1)
    return best, hist, depth, nodes, ms or int(1000 * (time.time() - t0)), score


def stable_time(hist, good):
    """Primo istante in cui la mossa migliore diventa corretta e non cambia piu'."""
    t = None
    for mv, ms, _d in hist:
        if mv in good:
            if t is None:
                t = ms
        else:
            t = None
    return t


def cmd_run(args):
    suite = read_suite(args.epd)
    if args.limit:
        suite = suite[:args.limit]
    limit = (f'nodes {args.nodes}' if args.nodes else
             f'depth {args.depth}' if args.depth else f'movetime {args.movetime}')
    opts = dict(QUIET)
    if args.syzygy:
        opts['SyzygyPath'] = args.syzygy
    eng = Engine(args.engine, opts, hash_mb=args.hash, threads=args.threads, timeout=3600)
    tot_pts = got_pts = solved = 0
    with open(args.out, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('fen\tid\tzona\tmax_punti\tpunti\trisolta\tmossa\tatteso\t'
                 'ms_stabile\tcambi_pv\tdepth\tnodi\tms\tscore\n')
        try:
            for i, (fen, pts, avoid, ident, zone) in enumerate(suite, 1):
                uci_pts, bad = san_to_uci(fen, list(pts) + list(avoid))
                good = {uci_pts[n] for n in pts if n in uci_pts}
                bad_moves = {uci_pts[n] for n in avoid if n in uci_pts}
                best, hist, depth, nodes, ms, score = probe(eng, fen, limit)
                maxp = max(pts.values()) if pts else 1
                if pts:
                    p = next((v for n, v in pts.items() if uci_pts.get(n) == best), 0)
                else:
                    p = 1 if best not in bad_moves else 0
                ok = int(p == maxp) if pts else int(best not in bad_moves)
                tot_pts += maxp
                got_pts += p
                solved += ok
                st = stable_time(hist, good if pts else None) if pts else None
                fh.write(f"{fen}\t{ident}\t{zone}\t{maxp}\t{p}\t{ok}\t{best}\t"
                         f"{'/'.join(sorted(pts)) if pts else 'am:' + '/'.join(sorted(avoid))}\t"
                         f"{'' if st is None else st}\t{max(0, len(hist) - 1)}\t{depth}\t{nodes}\t{ms}\t{score}\n")
                fh.flush()
                if bad:
                    print(f"  riga {i}: mosse non interpretabili {bad} ({fen})", file=sys.stderr)
                if i % 25 == 0:
                    print(f"  {i}/{len(suite)}  punti {got_pts}/{tot_pts}  risolte {solved}", file=sys.stderr)
        finally:
            eng.quit()
    print(f"{args.out}: {len(suite)} posizioni ({eng.id_name}, {limit}, Threads={args.threads})")
    print(f"  punti    : {got_pts}/{tot_pts}  ({100 * got_pts / max(1, tot_pts):.1f} %)")
    print(f"  risolte  : {solved}/{len(suite)}  ({100 * solved / max(1, len(suite)):.1f} %)")
    return 0


# --------------------------------------------------------------------- confronto
def load_tsv(path):
    rows = {}
    with open(path, encoding='utf-8', errors='replace') as fh:
        cols = fh.readline().rstrip('\n').split('\t')
        ix = {c: i for i, c in enumerate(cols)}
        for line in fh:
            p = line.rstrip('\n').split('\t')
            g = lambda c, d='': p[ix[c]] if c in ix and ix[c] < len(p) else d
            num = lambda c: int(g(c) or 0)
            rows[g('fen')] = {'id': g('id'), 'zona': g('zona'), 'maxp': num('max_punti'),
                              'p': num('punti'), 'ok': num('risolta'), 'mossa': g('mossa'),
                              'stable': int(g('ms_stabile')) if g('ms_stabile') else None,
                              'flips': num('cambi_pv'), 'depth': num('depth'),
                              'nodi': num('nodi'), 'ms': num('ms')}
    return rows


def mcnemar(b, c):
    """p-value esatto a due code per il confronto appaiato (b e c = discordanze)."""
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    tail = sum(math.comb(n, i) for i in range(0, k + 1)) / (2 ** n)
    return min(1.0, 2 * tail)


def cmd_compare(args):
    A, B = load_tsv(args.base), load_tsv(args.cand)
    common = [f for f in A if f in B]
    if not common:
        sys.exit("nessuna posizione in comune fra i due file")
    pa = sum(A[f]['p'] for f in common)
    pb = sum(B[f]['p'] for f in common)
    mx = sum(A[f]['maxp'] for f in common)
    sa = sum(A[f]['ok'] for f in common)
    sb = sum(B[f]['ok'] for f in common)
    only_a = [f for f in common if A[f]['ok'] and not B[f]['ok']]
    only_b = [f for f in common if B[f]['ok'] and not A[f]['ok']]
    p = mcnemar(len(only_a), len(only_b))

    print(f"base      : {args.base}")
    print(f"candidato : {args.cand}")
    print(f"posizioni in comune: {len(common)}\n")
    print(f"  punti    : {pa} -> {pb}  su {mx}   ({100 * pa / mx:.1f} % -> {100 * pb / mx:.1f} %, "
          f"{pb - pa:+d})")
    print(f"  risolte  : {sa} -> {sb}   ({sb - sa:+d})")
    print(f"  perse    : {len(only_a)}   guadagnate: {len(only_b)}")
    print(f"  McNemar (test appaiato sulle discordanze): p = {p:.4f}"
          + ("   <-- differenza significativa al 5 %" if p < 0.05 else "   (nessuna differenza dimostrata)"))

    both = [f for f in common if A[f]['stable'] and B[f]['stable']]
    if both:
        da = sorted(A[f]['stable'] for f in both)
        db = sorted(B[f]['stable'] for f in both)
        med = lambda xs: xs[len(xs) // 2]
        faster = sum(1 for f in both if B[f]['stable'] < A[f]['stable'])
        print(f"\n  tempo alla soluzione stabile (su {len(both)} posizioni risolte da entrambi):")
        print(f"      mediana {med(da)} ms -> {med(db)} ms;  piu' veloce nel candidato in {faster} "
              f"casi su {len(both)}")
    dd = [B[f]['depth'] - A[f]['depth'] for f in common if A[f]['depth'] and B[f]['depth']]
    if dd:
        print(f"  profondita' media: {sum(A[f]['depth'] for f in common) / len(common):.1f} -> "
              f"{sum(B[f]['depth'] for f in common) / len(common):.1f}  ({sum(dd) / len(dd):+.2f} ply)")
    fa = sum(A[f]['flips'] for f in common)
    fb = sum(B[f]['flips'] for f in common)
    print(f"  cambi di mossa migliore (totali): {fa} -> {fb}  ({fb - fa:+d})")

    if args.by_zone:
        zones = sorted({A[f]['zona'] for f in common if A[f]['zona']})
        if zones:
            print("\n  per zona Shashin (punti base -> candidato, posizioni):")
            for z in zones:
                sel = [f for f in common if A[f]['zona'] == z]
                print(f"      {z:<26} {sum(A[f]['p'] for f in sel):>5} -> "
                      f"{sum(B[f]['p'] for f in sel):<5} su {sum(A[f]['maxp'] for f in sel):<5} "
                      f"({len(sel)} pos.)")
        else:
            print("\n  (nessuna annotazione di zona: usa il CSV 258HardPositions o un corpus "
                  "generato da make_epd_corpus.py)")

    if only_a and args.examples:
        print(f"\n  posizioni PERSE dal candidato (prime {min(args.examples, len(only_a))}):")
        for f in only_a[:args.examples]:
            print(f"      atteso {A[f]['mossa']:<12} giocata {B[f].get('mossa', '?')}   {A[f]['id'][:60]}")
            print(f"      {f}")
    if only_b and args.examples:
        print(f"\n  posizioni GUADAGNATE (prime {min(args.examples, len(only_b))}):")
        for f in only_b[:args.examples]:
            print(f"      {A[f]['id'][:60] or f}")
    return 1 if (args.fail_on_loss and len(only_a) > args.tolerance) else 0


def cmd_info(args):
    suite = read_suite(args.epd)
    mx = sum(max(p.values()) if p else 1 for _f, p, _a, _i, _z in suite)
    zones = {}
    for _f, _p, _a, _i, z in suite:
        zones[z or '(senza zona)'] = zones.get(z or '(senza zona)', 0) + 1
    print(f"{args.epd}: {len(suite)} posizioni, punteggio massimo {mx}")
    for z, n in sorted(zones.items(), key=lambda x: -x[1]):
        print(f"  {z:<28} {n}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--engine', required=True)
    r.add_argument('--epd', required=True)
    r.add_argument('--out', required=True)
    r.add_argument('--movetime', type=int, default=60000, help='ms per posizione (default 60000)')
    r.add_argument('--depth', type=int)
    r.add_argument('--nodes', type=int, help='limite a nodi: riproducibile fra macchine diverse')
    r.add_argument('--threads', type=int, default=4)
    r.add_argument('--hash', type=int, default=2048)
    r.add_argument('--syzygy')
    r.add_argument('--limit', type=int)
    r.set_defaults(fn=cmd_run)
    c = sub.add_parser('compare')
    c.add_argument('base')
    c.add_argument('cand')
    c.add_argument('--by-zone', action='store_true')
    c.add_argument('--examples', type=int, default=10)
    c.add_argument('--fail-on-loss', action='store_true', help='exit 1 se il candidato perde posizioni')
    c.add_argument('--tolerance', type=int, default=0)
    c.set_defaults(fn=cmd_compare)
    i = sub.add_parser('info')
    i.add_argument('--epd', required=True)
    i.set_defaults(fn=cmd_info)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (EngineDied, RuntimeError) as e:
        sys.exit(f"ERRORE: {e}")
