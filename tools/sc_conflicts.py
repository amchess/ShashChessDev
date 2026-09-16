#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sc_conflicts.py - quadro dei conflitti del merge e risoluzione dei soli banali.

Da usare DURANTE il merge di vendor/sf_19, con merge.conflictStyle=zdiff3
(serve la sezione "base" per giudicare un blocco).

  stats           per file: blocchi, righe, quanti sono solo spaziatura, e quali
                  famiglie [SC:*] compaiono nel lato ShashChess di ogni blocco
  resolve-trivial risolve i blocchi in cui UNO dei due lati coincide con la base
                  a meno di spazi e righe vuote: vince l'altro lato. Con --apply
                  riscrive i file e fa `git add` di quelli rimasti senza conflitti.

Tutto il resto resta a te: e' esattamente il lavoro che richiede giudizio.

  python tools/sc_conflicts.py stats
  python tools/sc_conflicts.py resolve-trivial            # anteprima
  python tools/sc_conflicts.py resolve-trivial --apply

Se un file e' stato risolto male:  git checkout --conflict=zdiff3 -- <file>
"""

import argparse
import re
import subprocess
import sys
from collections import Counter

MARK_RE = re.compile(r'\[SC:([a-z0-9-]+)\]', re.I)


def conflicted():
    out = subprocess.run(['git', 'diff', '--name-only', '--diff-filter=U'],
                         capture_output=True, text=True, check=True).stdout
    return [f for f in out.split('\n') if f]


def parse(text):
    """Sequenza di pezzi: str (testo normale) o dict(ours, base, theirs, head, mid, sep, tail)."""
    lines = text.split('\n')
    out, buf, i = [], [], 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('<<<<<<< '):
            if buf:
                out.append('\n'.join(buf))
                buf = []
            blk = {'head': l, 'ours': [], 'base': None, 'theirs': [], 'mid': None, 'sep': None}
            cur = 'ours'
            i += 1
            while i < len(lines) and not lines[i].startswith('>>>>>>> '):
                x = lines[i]
                if x.startswith('||||||| ') and cur == 'ours':
                    blk['mid'], blk['base'], cur = x, [], 'base'
                elif x == '=======' and cur in ('ours', 'base'):
                    blk['sep'], cur = x, 'theirs'
                else:
                    blk[cur].append(x)
                i += 1
            blk['tail'] = lines[i] if i < len(lines) else '>>>>>>> ?'
            out.append(blk)
        else:
            buf.append(l)
        i += 1
    if buf:
        out.append('\n'.join(buf))
    return out


def ws(lines):
    return [re.sub(r'\s+', ' ', l.strip()) for l in lines if l.strip()]


def verdict(b):
    if b['base'] is None:
        return 'senza-base'
    o, s, t = ws(b['ours']), ws(b['base']), ws(b['theirs'])
    if o == s:
        return 'theirs'          # ShashChess ha cambiato solo spazi: vince SF19
    if t == s:
        return 'ours'            # SF19 ha cambiato solo spazi: vince ShashChess
    if o == t:
        return 'ours'            # stesso contenuto dai due lati
    return None


def render(pieces):
    out = []
    for p in pieces:
        if isinstance(p, str):
            out.append(p)
        else:
            out.append('\n'.join([p['head']] + p['ours'] + ([p['mid']] + p['base'] if p['base'] is not None else [])
                                 + [p['sep']] + p['theirs'] + [p['tail']]))
    return '\n'.join(out)


def cmd_stats(args):
    files = conflicted()
    if not files:
        print("Nessun file in conflitto.")
        return 0
    tot = Counter()
    fam_tot = Counter()
    print(f"{'file':<24}{'blocchi':>8}{'righe':>8}{'banali':>8}   famiglie [SC:*] coinvolte")
    for f in files:
        pieces = parse(open(f, encoding='utf-8', errors='surrogateescape').read())
        blocks = [p for p in pieces if isinstance(p, dict)]
        nlines = sum(len(b['ours']) + len(b['theirs']) + len(b['base'] or []) for b in blocks)
        triv = sum(1 for b in blocks if verdict(b) in ('ours', 'theirs'))
        fams = Counter(t.lower() for b in blocks for l in b['ours'] for t in MARK_RE.findall(l))
        fam_tot.update(fams)
        tot.update(blocchi=len(blocks), righe=nlines, banali=triv)
        no_base = any(b['base'] is None for b in blocks)
        print(f"{f:<24}{len(blocks):>8}{nlines:>8}{triv:>8}   "
              + ', '.join(f"{k}:{v}" for k, v in fams.most_common()) + ('   (manca la base: usa zdiff3)' if no_base else ''))
    print(f"\nTotale: {tot['blocchi']} blocchi, {tot['righe']} righe, {tot['banali']} risolvibili "
          f"automaticamente (resolve-trivial).")
    if fam_tot:
        print("Marcatori di famiglia dentro i conflitti:", ', '.join(f"{k}:{v}" for k, v in fam_tot.most_common()))
    return 0


def cmd_resolve(args):
    files = conflicted()
    done_files, solved = [], 0
    for f in files:
        text = open(f, encoding='utf-8', errors='surrogateescape').read()
        pieces = parse(text)
        changed = False
        for k, p in enumerate(pieces):
            if isinstance(p, dict):
                v = verdict(p)
                if v in ('ours', 'theirs'):
                    pieces[k] = '\n'.join(p[v])
                    solved += 1
                    changed = True
        left = sum(1 for p in pieces if isinstance(p, dict))
        if changed:
            print(f"  {f}: risolti i blocchi banali, restano {left}")
            if args.apply:
                with open(f, 'w', encoding='utf-8', errors='surrogateescape', newline='') as fh:
                    fh.write(render(pieces))
                if left == 0:
                    subprocess.run(['git', 'add', '--', f], check=True)
                    done_files.append(f)
    print(f"\n{solved} blocchi banali " + ("risolti" if args.apply else "risolvibili (anteprima: usa --apply)"))
    if done_files:
        print("File completamente risolti e aggiunti all'indice:", ', '.join(done_files))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('stats')
    s.set_defaults(fn=cmd_stats)
    r = sub.add_parser('resolve-trivial')
    r.add_argument('--apply', action='store_true')
    r.set_defaults(fn=cmd_resolve)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
