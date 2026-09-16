#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sc_baseline.py - fotografa ShashChess 41 prima del porting (FASE 0).

ShashChessDev non ha release: la baseline e' il tag sc41-sf18-base PIU' questa
cartella di riferimenti, generata dal binario compilato da quel tag e committata
in porting/baseline/. La CI (ci/port-gates.yml) confronta ogni build del ramo di
porting con questi file, senza bisogno di conservare il vecchio eseguibile.

Produce:
  positions.epd        corpus stratificato zona x materiale  (make_epd_corpus.py)
  positions_ci.epd     sottoinsieme di 2000 posizioni per la CI
  zones_sc41.tsv       win probability, W/D/L e zone        (dump_zones.py, static)
  keys_sc41.tsv        chiavi Zobrist                        (engine_parity.py keys)
  options_sc41.json    opzioni UCI                           (engine_parity.py options)
  hooks_sc41.json      inventario regioni [SC:*]             (hook_audit.py inventory)
  bench_sc41.txt       output di 'bench' (firma e nps)
  epd60_sc41_nodes.tsv smoke test della CI: 60 posizioni della 325 a nodi fissi

Le suite complete (325 a 60 s, 258 con zone) si producono a parte, perche' durano ore:
  python tools/epd_suite.py run --engine <sc41> --epd tests/Tests2026/325.epd \
      --movetime 60000 --threads 4 --hash 2048 --out porting/baseline/epd325_sc41.tsv

Uso (dalla radice di ShashChessDev, sul commit taggato sc41-sf18-base):
  python tools/sc_baseline.py --engine src/shashchess --out porting/baseline -n 6000
  python tools/sc_baseline.py --engine src/shashchess --out porting/baseline --corpus mio.epd
"""

import argparse
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(*args):
    print('>>', ' '.join(args))
    subprocess.run([sys.executable] + list(args), check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--engine', required=True)
    ap.add_argument('--out', default='porting/baseline')
    ap.add_argument('--corpus', help='corpus gia\' pronto (altrimenti viene generato)')
    ap.add_argument('-n', type=int, default=6000)
    ap.add_argument('--ci-size', type=int, default=2000)
    ap.add_argument('--src', default='src')
    ap.add_argument('--ci-epd', default='tests/Tests2026/325.epd',
                    help='suite da cui ricavare lo smoke test della CI')
    ap.add_argument('--ci-positions', type=int, default=60)
    ap.add_argument('--ci-nodes', type=int, default=2000000)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    T = lambda name: os.path.join(HERE, name)
    O = lambda name: os.path.join(args.out, name)

    corpus = args.corpus or O('positions.epd')
    if not args.corpus:
        run(T('make_epd_corpus.py'), '--engine', args.engine, '--out', corpus, '-n', str(args.n))
    lines = [l for l in open(corpus, encoding='utf-8') if l.strip() and not l.startswith('#')]
    random.seed(20260911)
    sample = lines if len(lines) <= args.ci_size else random.sample(lines, args.ci_size)
    with open(O('positions_ci.epd'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.writelines(sample)

    run(T('dump_zones.py'), '--engine', args.engine, '--epd', corpus, '--out', O('zones_sc41.tsv'))
    run(T('engine_parity.py'), 'keys', '--a', args.engine, '--epd', corpus, '--save', O('keys_sc41.tsv'))
    run(T('engine_parity.py'), 'options', '--a', args.engine, '--save', O('options_sc41.json'))
    run(T('hook_audit.py'), 'inventory', '--root', args.src, '--out', O('hooks_sc41.json'))
    if args.ci_epd and os.path.isfile(args.ci_epd):
        sub = O('epd60_ci.epd')
        with open(args.ci_epd, encoding='utf-8', errors='replace') as fh, \
             open(sub, 'w', encoding='utf-8', newline='\n') as out:
            for k, line in enumerate(fh):
                if k >= args.ci_positions:
                    break
                out.write(line)
        run(T('epd_suite.py'), 'run', '--engine', args.engine, '--epd', sub,
            '--nodes', str(args.ci_nodes), '--threads', '1', '--hash', '256',
            '--out', O('epd60_sc41_nodes.tsv'))
    try:
        bench = subprocess.run([os.path.abspath(args.engine), 'bench'], capture_output=True, text=True,
                               input='quit\n', timeout=1800,
                               cwd=os.path.dirname(os.path.abspath(args.engine)) or '.')
    except subprocess.TimeoutExpired:
        sys.exit("bench oltre 30 minuti: interrotto")
    with open(O('bench_sc41.txt'), 'w', encoding='utf-8') as fh:
        fh.write(bench.stdout[-2000:] + bench.stderr[-2000:])
    print(f"\nBaseline pronta in {args.out}: fai 'git add {args.out}' e committala sul ramo di porting.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
