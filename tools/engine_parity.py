#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
engine_parity.py - le invarianti che il porting NON deve rompere.

  keys     chiave Zobrist di ogni FEN (comando 'd', riga "Key:"): deve essere
           IDENTICA fra ShashChess 41 e 42. Da essa dipendono i file .exp, il
           flusso F dell'Avatar Analyzer e hashkeygenerator.
  perft    conteggi perft su posizioni standard, promozioni, en passant e
           Chess960: identici fra i due motori (e ai valori noti).
  options  elenco delle opzioni UCI: nessuna opzione ShashChess deve sparire;
           le differenze attese (EvalFileSmall rimossa) vanno solo confermate.
  trace    struttura del comando 'eval' (numeri sostituiti da #): l'Avatar
           Analyzer la analizza secondo ShashChessTraceSpecification, quindi le
           etichette non devono cambiare.

Ogni lato puo' essere un eseguibile oppure un TSV prodotto in precedenza con
--save (utile perche' ShashChessDev non ha release: si conserva la baseline):

  python tools/engine_parity.py keys --a ./sc41 --epd positions.epd --save keys_sc41.tsv
  python tools/engine_parity.py keys --a keys_sc41.tsv --b ./sc42 --epd positions.epd
  python tools/engine_parity.py perft --a ./sc41 --b ./sc42 --depth 4
  python tools/engine_parity.py options --a ./sc41 --b ./sc42
  python tools/engine_parity.py trace --a ./sc41 --b ./sc42 --epd positions.epd --limit 200

Esce con codice 1 se trova differenze: adatto alla CI.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sc_uci import EngineDied, Engine, read_epd  # noqa: E402

KEY_RE = re.compile(r'^Key:\s*([0-9A-Fa-f]+)')
NODES_RE = re.compile(r'Nodes searched:\s*(\d+)')

PERFT_SUITE = [
    # nome, fen, chess960, {profondita': conteggio noto}
    ('startpos', 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', False, {4: 197281, 5: 4865609}),
    ('kiwipete', 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1', False, {3: 97862, 4: 4085603}),
    ('pos3-ep', '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1', False, {4: 43238, 5: 674624}),
    ('pos4-promo', 'r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1', False, {4: 422333}),
    ('pos5', 'rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8', False, {4: 2103487}),
    ('pos6', 'r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10', False, {4: 3894594}),
    ('doppio-scacco', '4k3/8/8/8/8/8/3r4/R3K2R w KQ - 0 1', False, {}),
    ('chess960-a', 'bqnb1rkr/pp3ppp/3ppn2/2p5/5P2/P2P4/NPP1P1PP/BQ1BNRKR w HFhf - 2 9', True, {}),
    ('chess960-b', '2nnrbkr/p1qppppp/8/1ppb4/6PP/3PP3/PPP2P2/BQNNRBKR w HEhe - 1 9', True, {}),
]


def load_side(spec, need, epd=None, limit=None):
    """spec = eseguibile -> Engine; spec = .tsv -> dizionario salvato."""
    if spec and spec.lower().endswith('.tsv'):
        d = {}
        with open(spec, encoding='utf-8') as fh:
            next(fh)
            for line in fh:
                k, v = line.rstrip('\n').split('\t', 1)
                d[k] = v
        return d
    return Engine(spec, hash_mb=16, threads=1)


def probe_key(eng, fen):
    for l in eng.command(f'position fen {fen}\nd'):
        m = KEY_RE.match(l.strip())
        if m:
            return m.group(1).upper()
    return ''


def cmd_keys(args):
    pos = read_epd(args.epd)[:args.limit] if args.limit else read_epd(args.epd)
    res = {}
    for side in ('a', 'b'):
        spec = getattr(args, side)
        if not spec:
            continue
        s = load_side(spec, 'keys')
        if isinstance(s, dict):
            res[side] = s
            continue
        res[side] = {fen: probe_key(s, fen) for fen, _ in pos}
        s.quit()
    if args.save:
        with open(args.save, 'w', encoding='utf-8') as fh:
            fh.write('fen\tkey\n')
            for fen, k in res['a'].items():
                fh.write(f'{fen}\t{k}\n')
        print(f"salvate {len(res['a'])} chiavi in {args.save}")
    if 'b' not in res:
        return 0
    diff = [(f, res['a'].get(f), res['b'].get(f)) for f, _ in pos if res['a'].get(f) != res['b'].get(f)]
    empty = sum(1 for f, _ in pos if not res['b'].get(f))
    print(f"chiavi confrontate: {len(pos)}   diverse: {len(diff)}   mancanti nel candidato: {empty}")
    for f, ka, kb in diff[:20]:
        print(f"  {ka} != {kb}   {f}")
    if not diff:
        print("OK: chiavi Zobrist identiche -> .exp, hashkeygenerator e flusso F restano compatibili.")
    return 1 if diff else 0


def perft(eng, fen, depth, c960):
    eng.set_option('UCI_Chess960', 'true' if c960 else 'false')
    eng.send(f'position fen {fen}')
    eng.send(f'go perft {depth}')
    for l in eng.wait_for('Nodes searched', collect=True):
        m = NODES_RE.search(l)
        if m:
            return int(m.group(1))
    return -1


def cmd_perft(args):
    engs = {s: Engine(getattr(args, s)) for s in ('a', 'b') if getattr(args, s)}
    bad = 0
    print(f"{'posizione':<15}{'prof':>5}{'A':>12}{'B':>12}{'atteso':>12}")
    for name, fen, c960, known in PERFT_SUITE:
        depth = args.depth
        r = {s: perft(e, fen, depth, c960) for s, e in engs.items()}
        exp = known.get(depth, '')
        ok = len(set(r.values())) == 1 and (exp == '' or exp == r.get('a'))
        bad += not ok
        print(f"{name:<15}{depth:>5}{r.get('a', ''):>12}{r.get('b', ''):>12}{exp:>12}  {'' if ok else '<-- DIVERSO'}")
    for e in engs.values():
        e.quit()
    print("\nperft identico." if not bad else f"\n{bad} differenze: il porting si ferma qui.")
    return 1 if bad else 0


EXPECTED_GONE = {'EvalFileSmall'}


def _options_of(spec):
    if spec.lower().endswith('.json'):
        import json
        d = json.load(open(spec, encoding='utf-8'))
        return d['id'], d['options']
    e = Engine(spec)
    r = (e.id_name, e.options)
    e.quit()
    return r


def cmd_options(args):
    ida, oa = _options_of(args.a)
    if args.save:
        import json
        json.dump({'id': ida, 'options': oa}, open(args.save, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
        print(f"salvate {len(oa)} opzioni di {ida} in {args.save}")
        if not args.b:
            return 0
    idb, ob = _options_of(args.b)
    gone = sorted(set(oa) - set(ob))
    new = sorted(set(ob) - set(oa))
    changed = sorted(n for n in set(oa) & set(ob) if oa[n] != ob[n])
    print(f"A: {ida} ({len(oa)} opzioni)   B: {idb} ({len(ob)} opzioni)\n")
    for n in gone:
        print(f"  SPARITA  {n}" + ("   (attesa: rimossa in SF19)" if n in EXPECTED_GONE else "   <-- verificare"))
    for n in new:
        print(f"  NUOVA    {n}  {ob[n]}")
    for n in changed:
        print(f"  CAMBIATA {n}: {oa[n]} -> {ob[n]}")
    unexpected = [n for n in gone if n not in EXPECTED_GONE]
    print("\nnessuna opzione ShashChess persa." if not unexpected else f"\n{len(unexpected)} opzioni sparite senza motivo noto.")
    return 1 if unexpected else 0


NUM_RE = re.compile(r'[-+]?\d+(?:\.\d+)?')


def skeleton(lines):
    out = []
    for l in lines:
        s = NUM_RE.sub('#', l.strip())
        s = re.sub(r'\s+', ' ', s)
        if s and not s.startswith(('readyok', 'info string')):
            out.append(s)
    return out


def cmd_trace(args):
    pos = read_epd(args.epd)[:args.limit]
    a, b = Engine(args.a, options={'UCI_ShowWDL': 'true'}), Engine(args.b, options={'UCI_ShowWDL': 'true'})
    la, lb = {}, {}
    for fen, _ in pos:
        for eng, acc in ((a, la), (b, lb)):
            for s in skeleton(eng.command(f'position fen {fen}\neval')):
                acc[s] = acc.get(s, 0) + 1
    a.quit()
    b.quit()
    only_a = sorted(set(la) - set(lb))
    only_b = sorted(set(lb) - set(la))
    print(f"righe-tipo della trace: A {len(la)}, B {len(lb)}")
    for s in only_a[:40]:
        print(f"  solo A: {s[:120]}")
    for s in only_b[:40]:
        print(f"  solo B: {s[:120]}")
    if not only_a and not only_b:
        print("OK: stessa struttura -> i parser dell'Avatar Analyzer non vanno toccati.")
    else:
        print("Ogni riga 'solo A' e' un'etichetta che il parser non trovera' piu': confrontala con")
        print("ShashChessTraceSpecification e decidi se ripristinarla o aggiornare la specifica.")
    return 1 if only_a else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    k = sub.add_parser('keys')
    k.add_argument('--a', required=True)
    k.add_argument('--b')
    k.add_argument('--epd', required=True)
    k.add_argument('--limit', type=int)
    k.add_argument('--save')
    k.set_defaults(fn=cmd_keys)
    p = sub.add_parser('perft')
    p.add_argument('--a', required=True)
    p.add_argument('--b')
    p.add_argument('--depth', type=int, default=4)
    p.set_defaults(fn=cmd_perft)
    o = sub.add_parser('options')
    o.add_argument('--a', required=True, help='eseguibile oppure .json salvato con --save')
    o.add_argument('--b')
    o.add_argument('--save')
    o.set_defaults(fn=cmd_options)
    t = sub.add_parser('trace')
    t.add_argument('--a', required=True)
    t.add_argument('--b', required=True)
    t.add_argument('--epd', required=True)
    t.add_argument('--limit', type=int, default=200)
    t.set_defaults(fn=cmd_trace)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (EngineDied, RuntimeError) as e:
        sys.exit(f"ERRORE: {e}")
