#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hook_audit.py - le regioni ShashChess prima, durante e dopo il porting.

Un merge che "compila" puo' aver perso mezza regione, averla incollata nel ramo
sbagliato di un if, o averla lasciata accanto a codice SF19 che nel frattempo ha
cambiato significato. Questo strumento rende visibili i tre casi.

Sottocomandi
------------
  inventory  elenca le regioni [SC:tag] begin/end e i blocchi #ifdef GOLD_DIGGER,
             con impronta del corpo e righe d'ancoraggio upstream -> JSON
  compare    confronta due inventari (tipicamente SC41 e il ramo di porting):
             regioni sparite, nuove, con corpo modificato
  context    PRIMA di risolvere i conflitti: per ogni regione dice se il codice
             Stockfish che la circonda e' cambiato fra sf_18 e sf_19
             (INVARIATO / CAMBIATO / ANCORA-SPARITA). E' la lista di revisione
             semantica: una regione in contesto CAMBIATO va riletta anche se il
             merge l'ha fusa senza conflitti.
  unmarked   modifiche ShashChess ai file condivisi FUORI dalle regioni marcate:
             invisibili a qualunque procedura "per famiglie", vanno marcate o
             giustificate.

Le versioni upstream si passano come directory oppure come riferimenti git del
ramo vendor (letti con `git show`, senza estrarre nulla):

  python tools/hook_audit.py inventory --root src --out hooks_sc41.json
  python tools/hook_audit.py context --root src --base vendor/sf_18 --theirs vendor/sf_19
  python tools/hook_audit.py unmarked --root src --base vendor/sf_18
  ... dopo il porting ...
  python tools/hook_audit.py inventory --root src --out hooks_sc42.json
  python tools/hook_audit.py compare hooks_sc41.json hooks_sc42.json
"""

import argparse
import difflib
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

MARK_RE = re.compile(r'//\s*\[SC:([a-z0-9-]+)\]\s*(begin|end)\b', re.I)
IFDEF_GD_RE = re.compile(r'^\s*#\s*if(def)?\s+.*\bGOLD_DIGGER\b')
PP_OPEN_RE = re.compile(r'^\s*#\s*if')
PP_CLOSE_RE = re.compile(r'^\s*#\s*endif')
SRC_RE = re.compile(r'\.(cpp|h|hpp)$')
SKIP = ('incbin', 'livebook/json')
ANCHOR_N = 3
WINDOW = 12


# ----------------------------------------------------------------- sorgenti
class Source:
    """Directory o riferimento git (es. vendor/sf_18) da cui leggere 'src/<rel>'."""

    def __init__(self, spec, repo='.'):
        self.spec, self.repo = spec, repo
        self.is_dir = os.path.isdir(spec)

    def read(self, rel):
        if self.is_dir:
            for cand in (os.path.join(self.spec, rel), os.path.join(self.spec, 'src', rel)):
                if os.path.isfile(cand):
                    return open(cand, encoding='utf-8', errors='replace').read()
            return None
        r = subprocess.run(['git', '-C', self.repo, 'show', f'{self.spec}:src/{rel}'],
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        return r.stdout if r.returncode == 0 else None


def iter_sources(root):
    for dp, dn, fn in os.walk(root):
        rel_dir = os.path.relpath(dp, root).replace('\\', '/')
        if any(rel_dir == s or rel_dir.startswith(s + '/') for s in SKIP):
            dn[:] = []
            continue
        for f in sorted(fn):
            if SRC_RE.search(f):
                p = os.path.join(dp, f)
                yield p, os.path.relpath(p, root).replace('\\', '/')


# ----------------------------------------------------------------- regioni
def find_regions(lines):
    """[(tag, i_begin, i_end)] con indici 0-based delle righe marcatore."""
    out, stack = [], defaultdict(list)
    for i, l in enumerate(lines):
        m = MARK_RE.search(l)
        if m:
            tag, kw = m.group(1).lower(), m.group(2).lower()
            if kw == 'begin':
                stack[tag].append(i)
            elif stack[tag]:
                out.append((tag, stack[tag].pop(), i))
    # blocchi #ifdef GOLD_DIGGER ... #endif (con annidamento)
    i = 0
    while i < len(lines):
        if IFDEF_GD_RE.match(lines[i]):
            depth, j = 0, i
            while j < len(lines):
                if PP_OPEN_RE.match(lines[j]):
                    depth += 1
                elif PP_CLOSE_RE.match(lines[j]):
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            out.append(('golddigger', i, min(j, len(lines) - 1)))
            i = j
        i += 1
    return sorted(out, key=lambda r: r[1])


def region_mask(lines, regions):
    mask = [False] * len(lines)
    for _, a, b in regions:
        for k in range(a, b + 1):
            mask[k] = True
    return mask


def norm(l):
    return re.sub(r'\s+', ' ', l.strip())


def body_hash(lines, a, b):
    body = [norm(l) for l in lines[a + 1:b] if l.strip()]
    return hashlib.sha1('\n'.join(body).encode()).hexdigest()[:12], body


def anchors(lines, mask, a, b):
    before, after = [], []
    k = a - 1
    while k >= 0 and len(before) < ANCHOR_N:
        if not mask[k] and lines[k].strip():
            before.insert(0, norm(lines[k]))
        k -= 1
    k = b + 1
    while k < len(lines) and len(after) < ANCHOR_N:
        if not mask[k] and lines[k].strip():
            after.append(norm(lines[k]))
        k += 1
    return before, after


def build_inventory(root, warn_conflicts=True):
    inv, conflicted = [], []
    for p, rel in iter_sources(root):
        lines = open(p, encoding='utf-8', errors='replace').read().split('\n')
        if any(l.startswith('<<<<<<< ') for l in lines):
            conflicted.append(rel)
        regs = find_regions(lines)
        if not regs:
            continue
        mask = region_mask(lines, regs)
        for tag, a, b in regs:
            h, body = body_hash(lines, a, b)
            bef, aft = anchors(lines, mask, a, b)
            inv.append({'file': rel, 'tag': tag, 'begin': a + 1, 'end': b + 1, 'hash': h,
                        'lines': len(body), 'head': body[0][:80] if body else '',
                        'body': body, 'before': bef, 'after': aft})
    if conflicted and warn_conflicts:
        print(f"ATTENZIONE: {len(conflicted)} file contengono ancora marcatori di conflitto "
              f"({', '.join(conflicted[:4])}...).", file=sys.stderr)
        print("  'inventory' e 'context' vanno eseguiti su un albero pulito: 'context' PRIMA del merge "
              "(sul ramo di porting appena creato), 'inventory' DOPO averlo concluso.", file=sys.stderr)
    return inv


def locate(seq_lines, anchor):
    """Indice (nelle righe non vuote normalizzate) in cui compare la sequenza anchor."""
    if not anchor:
        return None
    n = len(anchor)
    for i in range(len(seq_lines) - n + 1):
        if seq_lines[i:i + n] == anchor:
            return i
    return None


# ----------------------------------------------------------------- comandi
def cmd_inventory(args):
    inv = build_inventory(args.root)
    with open(args.out, 'w', encoding='utf-8') as fh:
        json.dump(inv, fh, ensure_ascii=False, indent=1)
    c = Counter(r['tag'] for r in inv)
    print(f"{len(inv)} regioni in {len({r['file'] for r in inv})} file -> {args.out}")
    for tag, n in c.most_common():
        print(f"  [SC:{tag}]".ljust(30) + f"{n:>3} regioni, {sum(r['lines'] for r in inv if r['tag'] == tag):>5} righe")
    return 0


def cmd_compare(args):
    A = json.load(open(args.old, encoding='utf-8'))
    B = json.load(open(args.new, encoding='utf-8'))
    used = set()
    missing, changed, same = [], [], 0
    for r in A:
        exact = [i for i, s in enumerate(B) if i not in used and s['tag'] == r['tag'] and s['hash'] == r['hash']]
        if exact:
            used.add(exact[0])
            same += 1
            continue
        best, score = None, 0.0
        for i, s in enumerate(B):
            if i in used or s['tag'] != r['tag']:
                continue
            ratio = difflib.SequenceMatcher(None, r['body'], s['body'], autojunk=False).ratio()
            if s['file'] == r['file']:
                ratio += 0.05
            if ratio > score:
                best, score = i, ratio
        if best is not None and score >= 0.5:
            used.add(best)
            changed.append((r, B[best], score))
        else:
            missing.append(r)
    added = [s for i, s in enumerate(B) if i not in used]

    print(f"vecchio: {len(A)} regioni   nuovo: {len(B)} regioni\n")
    print(f"  identiche             : {same}")
    print(f"  con corpo modificato  : {len(changed)}   (atteso: adattamenti API)")
    print(f"  SPARITE               : {len(missing)}   <-- ognuna va giustificata")
    print(f"  nuove                 : {len(added)}\n")
    ta, tb = Counter(r['tag'] for r in A), Counter(r['tag'] for r in B)
    print("  per famiglia (vecchio -> nuovo):")
    for tag in sorted(set(ta) | set(tb)):
        flag = '' if ta[tag] == tb[tag] else '   <--'
        print(f"    [SC:{tag}]".ljust(32) + f"{ta[tag]:>3} -> {tb[tag]:<3}{flag}")
    if missing:
        print("\nSPARITE:")
        for r in missing:
            print(f"  {r['file']}:{r['begin']} [SC:{r['tag']}] {r['lines']} righe - {r['head']}")
    if changed and args.verbose:
        print("\nMODIFICATE:")
        for r, s, sc in changed:
            print(f"  {r['file']}:{r['begin']} -> {s['file']}:{s['begin']} [SC:{r['tag']}] somiglianza {sc:.2f}")
            for l in difflib.unified_diff(r['body'], s['body'], lineterm='', n=0):
                if not l.startswith(('---', '+++', '@@')):
                    print('      ' + l[:120])
    return 1 if missing else 0


def cmd_context(args):
    base, theirs = Source(args.base, args.repo), Source(args.theirs, args.repo)
    inv = build_inventory(args.root)
    stats = Counter()
    rows = []
    for r in inv:
        b_txt, t_txt = base.read(r['file']), theirs.read(r['file'])
        if b_txt is None:
            stats['solo-ShashChess'] += 1
            continue
        if t_txt is None:
            rows.append(('FILE-RIMOSSO', r, []))
            stats['FILE-RIMOSSO'] += 1
            continue
        B = [norm(l) for l in b_txt.split('\n') if l.strip()]
        T = [norm(l) for l in t_txt.split('\n') if l.strip()]
        ib = locate(B, r['before']) if r['before'] else None
        ia = locate(B, r['after']) if r['after'] else None
        anchor, pos_b = (r['before'], ib) if ib is not None else (r['after'], ia)
        if pos_b is None:
            rows.append(('ANCORA-NON-UPSTREAM', r, []))
            stats['ANCORA-NON-UPSTREAM'] += 1
            continue
        pos_t = locate(T, anchor)
        if pos_t is None:
            rows.append(('ANCORA-SPARITA', r, []))
            stats['ANCORA-SPARITA'] += 1
            continue
        wb = B[max(0, pos_b - WINDOW): pos_b + len(anchor) + WINDOW]
        wt = T[max(0, pos_t - WINDOW): pos_t + len(anchor) + WINDOW]
        if wb == wt:
            stats['INVARIATO'] += 1
            if args.all:
                rows.append(('INVARIATO', r, []))
        else:
            d = [l for l in difflib.unified_diff(wb, wt, lineterm='', n=0)
                 if not l.startswith(('---', '+++', '@@'))]
            rows.append(('CAMBIATO', r, d))
            stats['CAMBIATO'] += 1
    order = {'ANCORA-SPARITA': 0, 'FILE-RIMOSSO': 1, 'CAMBIATO': 2, 'ANCORA-NON-UPSTREAM': 3, 'INVARIATO': 4}
    for kind, r, d in sorted(rows, key=lambda x: (order[x[0]], x[1]['file'], x[1]['begin'])):
        print(f"[{kind}] {r['file']}:{r['begin']}-{r['end']} [SC:{r['tag']}] {r['lines']} righe - {r['head']}")
        for l in d[:args.max_diff]:
            print('      ' + l[:130])
        if len(d) > args.max_diff:
            print(f"      ... altre {len(d) - args.max_diff} righe")
    print("\nRiepilogo:", ', '.join(f"{k}={v}" for k, v in sorted(stats.items())))
    print("Priorita' di revisione: ANCORA-SPARITA > CAMBIATO > ANCORA-NON-UPSTREAM.")
    return 0


COMMENT_RE = re.compile(r'^\s*(//|/\*|\*|\*/)')


def cmd_unmarked(args):
    """Modifiche ShashChess ai file condivisi fuori dalle regioni marcate."""
    base = Source(args.base, args.repo)
    total = com = 0
    for p, rel in iter_sources(args.root):
        b_txt = base.read(rel)
        if b_txt is None:
            continue
        lines = open(p, encoding='utf-8', errors='replace').read().split('\n')
        regs = find_regions(lines)
        mask = region_mask(lines, regs)
        sc = [(i, norm(l)) for i, l in enumerate(lines) if not mask[i] and l.strip()
              and not MARK_RE.search(l)]
        up = [norm(l) for l in b_txt.split('\n') if l.strip()]
        sm = difflib.SequenceMatcher(None, up, [x for _, x in sc], autojunk=False)
        hunks = []
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op == 'equal':
                continue
            removed, added = up[i1:i2], [x for _, x in sc[j1:j2]]
            if all('Copyright' in x or 'derived from' in x or x.startswith(('ShashChess is', 'Stockfish is'))
                   for x in removed + added):
                continue
            only_comments = all(COMMENT_RE.match(x) for x in removed + added)
            if only_comments:
                com += 1
                if not args.comments:
                    continue
            ln = sc[j1][0] + 1 if j1 < len(sc) else len(lines)
            hunks.append((ln, removed, added, only_comments))
        if hunks:
            print(f"=== {rel}: {len(hunks)} modifiche di codice fuori dalle regioni marcate")
            for ln, rem, add, oc in hunks[:args.max_hunks]:
                print(f"  riga ~{ln}:" + ('  (solo commenti)' if oc else ''))
                for x in rem[:4]:
                    print(f"    - {x[:110]}")
                for x in add[:4]:
                    print(f"    + {x[:110]}")
            if len(hunks) > args.max_hunks:
                print(f"  ... altre {len(hunks) - args.max_hunks}")
            total += len(hunks)
    print(f"\nTotale: {total} modifiche di codice non marcate" +
          (f" ({com} sole-commenti escluse: --comments per vederle)" if not args.comments else ''))
    print("Marcale con // [SC:tag] begin/end oppure annotale nel commit: dopo il merge nessuno")
    print("le cercherebbe, e il merge non segnala nulla se SF19 tocca le stesse righe.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default='.', help='repository per leggere i riferimenti git')
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('inventory')
    i.add_argument('--root', default='src')
    i.add_argument('--out', required=True)
    i.set_defaults(fn=cmd_inventory)
    c = sub.add_parser('compare')
    c.add_argument('old')
    c.add_argument('new')
    c.add_argument('-v', '--verbose', action='store_true')
    c.set_defaults(fn=cmd_compare)
    x = sub.add_parser('context')
    x.add_argument('--root', default='src')
    x.add_argument('--base', required=True, help='dir o ref git di sf_18 rebrandizzato (vendor/sf_18)')
    x.add_argument('--theirs', required=True, help='dir o ref git di sf_19 rebrandizzato (vendor/sf_19)')
    x.add_argument('--all', action='store_true', help='elenca anche le regioni INVARIATO')
    x.add_argument('--max-diff', type=int, default=8)
    x.set_defaults(fn=cmd_context)
    u = sub.add_parser('unmarked')
    u.add_argument('--root', default='src')
    u.add_argument('--base', required=True)
    u.add_argument('--max-hunks', type=int, default=30)
    u.add_argument('--comments', action='store_true', help='include le modifiche di soli commenti')
    u.set_defaults(fn=cmd_unmarked)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
