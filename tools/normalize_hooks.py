#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
normalize_hooks.py - normalizza i marcatori delle regioni ShashChess (v2).

Perche' serve
-------------
Le regioni ShashChess sono delimitate da commenti scritti in molte grafie
("//from Shashih begin", "//fom shashin end", "// end variety",
"// Crystal-Shashin QSearch optimization begin (Early Exit logic)" ...).
Senza una forma canonica non si possono estrarre, contare, confrontare prima e
dopo il porting (hook_audit.py) ne' misurare l'ablation per famiglia.

Forma canonica (idempotente):

    // [SC:shashin] begin          ...          // [SC:shashin] end
    // [SC:shashin-crystal] begin (Early Exit logic)

Novita' della v2
----------------
  * riconosce anche la parola chiave in testa ("// end variety")
  * accetta una coda fra parentesi dopo begin/end, conservandola come nota
  * e' idempotente: i marcatori gia' canonici vengono riconosciuti tali e quali
  * per ogni sbilanciamento propone la riga piu' probabile del begin/end mancante
  * salta il codice di terze parti (incbin, livebook/json)

Uso
---
    python tools/normalize_hooks.py --root src --check
    python tools/normalize_hooks.py --root src --apply
    python tools/normalize_hooks.py --root src --inventory hooks_sc41.tsv

Il comando esce con codice 1 se restano sbilanciamenti: va eseguito (e reso
pulito) PRIMA del merge, nel commit di canonicalizzazione.
"""

import argparse
import os
import re
import sys
from collections import Counter, defaultdict

TAXONOMY = [
    (r'shashin.?crystal|crystal.?shashin',              'shashin-crystal'),
    (r'shashin personalit',                              'shashin-personalities'),
    (r'\bshashin\b|\bshashih\b|\bshashn\b',              'shashin'),
    (r'probe experience book|experience book',           'learning-expbook'),
    (r'\bbook and exp\b',                                'learning-expbook'),
    (r'step 3 bis learning',                             'learning'),
    (r'\blearning\b',                                    'learning'),
    (r'montecarlo|\bmcts\b',                             'mcts'),
    (r'livebook depth|live ?book',                       'livebook'),
    (r'book and live ?book|books? management|polyfish',  'book'),
    (r'full ?threads? ?(patch)?',                        'fullthreads'),
    (r'\bvariety\b',                                     'variety'),
    (r'anti ?dancing',                                   'antidancing'),
    (r'gold ?digger',                                    'golddigger'),
    (r'combo management',                                'uci-combo'),
    (r'minthink|slowmover|move overhead',                'uci-timing'),
    (r'\bwdl\b|win probability',                         'wdl'),
    (r'shashchess specific|from shashchess|shashchess',  'shashchess'),
]

FALSE_POSITIVES = [
    r'updates stats at the end', r'wait for an input or an end', r'returns? a pointer to the end',
    r'unmasked vector stores near the end', r'will thus be validated and end', r'at the end of',
    r'to the end of', r'until the end', r'end of file', r'end of the', r'restore the default at the end',
    r'a marker to the end', r'dependency chains and merge at the end', r'perhaps to do',
]

CANON_RE = re.compile(r'\[SC:([a-z0-9-]+)\]\s*(begin|end)\b(.*)$', re.I)
KW_LAST_RE = re.compile(r'^(?P<text>.*?)\b(?P<kw>begin|end)\b(?P<tail>\s*(?:\([^)]*\))?\s*[.:;]?\s*)$', re.I)
KW_FIRST_RE = re.compile(r'^(?P<kw>begin|end)\b\s+(?P<text>[^()]+?)\s*$', re.I)
SRC_RE = re.compile(r'\.(cpp|h|hpp)$')
SKIP_DIRS = ('incbin', 'livebook/json', '.git')


def split_comment(line):
    """Restituisce (codice, testo_commento) separando il primo // fuori dalle stringhe."""
    in_str = None
    i = 0
    while i < len(line) - 1:
        c = line[i]
        if in_str:
            if c == '\\':
                i += 2
                continue
            if c == in_str:
                in_str = None
        elif c in '"\'':
            in_str = c
        elif c == '/' and line[i + 1] == '/':
            return line[:i], line[i + 2:]
        i += 1
    return None


def classify(text):
    t = re.sub(r'\s+', ' ', text.lower()).strip(' -*/:')
    for fp in FALSE_POSITIVES:
        if re.search(fp, t):
            return None
    for pat, tag in TAXONOMY:
        if re.search(pat, t):
            return tag
    return '?'


def parse_marker(line):
    """(tag, kw, nota, codice) oppure None."""
    sc = split_comment(line)
    if not sc:
        return None
    code, comment = sc
    m = CANON_RE.search(comment)
    if m:
        return m.group(1).lower(), m.group(2).lower(), m.group(3).strip(), code
    body = comment.strip()
    m = KW_LAST_RE.match(body)
    if m:
        tag = classify(m.group('text') + ' ' + m.group('kw'))
        if tag is None:
            return None
        note = m.group('tail').strip().strip('.:;').strip()
        return tag, m.group('kw').lower(), note, code
    m = KW_FIRST_RE.match(body)
    if m:
        tag = classify(m.group('text'))
        if tag in (None, '?'):
            return None
        return tag, m.group('kw').lower(), '', code
    return None


def iter_sources(root):
    for dp, dn, fn in os.walk(root):
        rel_dir = os.path.relpath(dp, root).replace('\\', '/')
        if any(rel_dir == s or rel_dir.startswith(s + '/') for s in SKIP_DIRS):
            dn[:] = []
            continue
        for f in sorted(fn):
            if SRC_RE.search(f):
                yield os.path.join(dp, f)


def suggest(lines, idx, tag, upward):
    """Riga piu' probabile per il marcatore mancante: il commento piu' vicino che
    nomina la famiglia (verso l'alto per un begin mancante, verso il basso per un end)."""
    word = {'shashin-crystal': 'crystal', 'learning-expbook': 'experience'}.get(tag, tag.split('-')[0])
    rng = range(idx - 1, max(-1, idx - 250), -1) if upward else range(idx + 1, min(len(lines), idx + 250))
    for j in rng:
        sc = split_comment(lines[j])
        if sc and word in sc[1].lower() and not parse_marker(lines[j]):
            return j + 1, lines[j].strip()[:90]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default='src')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--inventory', metavar='FILE.tsv')
    args = ap.parse_args()
    if not os.path.isdir(args.root):
        sys.exit(f"ERRORE: '{args.root}' non e' una directory")

    tag_count, unknown, unbalanced, regions = Counter(), [], [], []
    files_changed = 0
    for path in iter_sources(args.root):
        rel = os.path.relpath(path, args.root).replace('\\', '/')
        with open(path, encoding='utf-8', errors='surrogateescape', newline='') as fh:
            raw = fh.read()
        eol = '\r\n' if '\r\n' in raw else '\n'
        lines = raw.split(eol)
        found = []
        for i, line in enumerate(lines):
            pm = parse_marker(line)
            if pm:
                found.append((i,) + pm)
        if not found:
            continue
        stack = defaultdict(list)
        for i, tag, kw, note, code in found:
            tag_count[tag] += 1
            if tag == '?':
                unknown.append((rel, i + 1, lines[i].strip()[:100]))
                continue
            if kw == 'begin':
                stack[tag].append(i)
            elif stack[tag]:
                s = stack[tag].pop()
                regions.append((rel, s + 1, i + 1, tag, i - s - 1))
            else:
                unbalanced.append((rel, i + 1, tag, 'end senza begin', suggest(lines, i, tag, True)))
        for tag, opens in stack.items():
            for s in opens:
                unbalanced.append((rel, s + 1, tag, 'begin senza end', suggest(lines, s, tag, False)))

        if args.apply:
            changed = False
            for i, tag, kw, note, code in found:
                if tag == '?':
                    continue
                canon = f"// [SC:{tag}] {kw}" + (f" {note}" if note else '')
                indent = re.match(r'\s*', lines[i]).group(0)
                code = code.rstrip()
                new = f"{code} {canon}" if code.strip() else f"{indent}{canon}"
                if new != lines[i]:
                    lines[i] = new
                    changed = True
            if changed:
                with open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='') as fh:
                    fh.write(eol.join(lines))
                files_changed += 1

    print("=" * 72 + "\nMARCATORI RICONOSCIUTI PER TAG\n" + "=" * 72)
    for tag, n in tag_count.most_common():
        print(f"  {'NON CLASSIFICATI' if tag == '?' else f'[SC:{tag}]':<28} {n:>4}")
    print("\n" + "=" * 72 + f"\nREGIONI CHIUSE CORRETTAMENTE: {len(regions)}\n" + "=" * 72)
    by_tag = defaultdict(list)
    for r in regions:
        by_tag[r[3]].append(r[4])
    for tag in sorted(by_tag, key=lambda t: -sum(by_tag[t])):
        s = by_tag[tag]
        print(f"  [SC:{tag}]".ljust(30) + f"{len(s):>3} regioni, {sum(s):>5} righe, max {max(s):>4}")
    if unknown:
        print("\n" + "=" * 72 + f"\nDA CLASSIFICARE A MANO: {len(unknown)}\n" + "=" * 72)
        for rel, ln, txt in unknown:
            print(f"  {rel}:{ln}: {txt}")
    if unbalanced:
        print("\n" + "=" * 72 + f"\n!! SBILANCIAMENTI: {len(unbalanced)}  <-- da sistemare PRIMA del merge\n" + "=" * 72)
        for rel, ln, tag, why, sug in unbalanced:
            print(f"  {rel}:{ln}  [SC:{tag}]  {why}")
            if sug:
                print(f"      candidato: riga {sug[0]}: {sug[1]}")
    if args.inventory:
        with open(args.inventory, 'w', encoding='utf-8') as fh:
            fh.write("file\triga_begin\triga_end\ttag\trighe\n")
            for r in sorted(regions):
                fh.write("\t".join(map(str, r)) + "\n")
        print(f"\nInventario scritto in {args.inventory} ({len(regions)} regioni)")
    print(f"\nRiscritti {files_changed} file." if args.apply else
          "\n(dry-run: nessun file modificato. Usa --apply per riscrivere.)")
    return 1 if unbalanced else 0


if __name__ == '__main__':
    sys.exit(main())
