#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sc_rebrand.py - rebranding Stockfish -> ShashChess, deterministico e idempotente.

Il principio
------------
Il renaming NON si fa sul risultato del merge, ma sull'upstream PRIMA del merge.
Si definisce una trasformazione T tale che, per ogni file che ShashChess non ha
personalizzato,

        T(file Stockfish 18)  ==  file ShashChess   (byte per byte)

Allora un merge a tre vie fra T(SF18) (base), ShashChess (nostro) e T(SF19)
(loro) vede SOLO le differenze vere: niente conflitti su namespace, intestazioni
di licenza o nomi dell'eseguibile. E la stessa T servira' identica per SF20.

Regole di T
-----------
  C/C++ (.cpp .h .hpp .c .cc)
    * intestazione di licenza iniziale -> intestazione canonica (sc_header.txt)
    * namespace Stockfish / Stockfish::  -> namespace ShashChess / ShashChess::
    * Stockfish_##x (entry_*.cpp della build "universal" SF19) -> ShashChess_##x
    * NIENT'ALTRO: commenti, crediti e URL restano come upstream (e' cio' che
      ShashChess fa gia' oggi nei file condivisi).
  Makefile, .sh, .py, .awk
    * intestazione "# ..." -> canonica
    * stockfish/Stockfish/STOCKFISH come parola (anche con suffisso _xxx) ->
      shashchess/ShashChess/SHASHCHESS, preservando le maiuscole
    * gli URL e i domini (tests.stockfishchess.org, official-stockfish, ...) sono
      mascherati e non vengono mai toccati
  File di terze parti (incbin/, livebook/json/): mai toccati.

Sottocomandi
------------
  apply    applica T a un albero upstream (usato da sc_vendor.py)
  headers  canonicalizza le intestazioni di un albero ShashChess
  verify   confronta T(upstream) con ShashChess file per file; con
           --fix-whitespace riallinea i file che differiscono solo per spazi
  modes    normalizza i permessi git (sorgenti 100644, script 100755)
  check    controllo per la CI: residui di Stockfish/Glaurung dove non devono stare

Esempi
------
  python tools/sc_rebrand.py verify --sc . --upstream vendor/sf_18
  python tools/sc_rebrand.py verify --sc . --upstream /tmp/sf_18
  python tools/sc_rebrand.py headers --root . --dry-run
  python tools/sc_rebrand.py headers --root .
  python tools/sc_rebrand.py verify --sc . --upstream vendor/sf_18 --fix-whitespace
  python tools/sc_rebrand.py modes --repo .
  python tools/sc_rebrand.py check --root .

Tutti i file vengono letti e scritti come UTF-8 con surrogateescape e con i fine
riga originali: il tool non introduce mai modifiche non richieste.
"""

import argparse
import difflib
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_TEMPLATE = HERE / 'sc_header.txt'

CPP_EXT = {'.cpp', '.h', '.hpp', '.c', '.cc', '.hh', '.cxx'}
SCRIPT_EXT = {'.sh', '.py', '.awk'}
# Radici che T considera "upstream": tutto il resto del repository e' ShashChess.
UPSTREAM_ROOTS = ('src', 'scripts', 'tests')
# Codice di terze parti con licenza propria: mai toccato.
THIRD_PARTY = ('src/incbin/', 'src/livebook/json/')
# File solo-ShashChess che non devono ricevere un'intestazione automatica
# (provenienza da verificare prima: codice derivato da altri progetti).
NO_AUTO_HEADER = ('src/book/polyglot/', 'src/book/ctg/')

LICENSE_MARK = 'General Public License'
UPSTREAM_MARK = re.compile(r'(Stockfish|ShashChess), a UCI chess playing engine derived from')

URL_RE = re.compile(
    r'(https?://[^\s"\'<>)\]]+'                        # URL completi
    r'|\b[\w.-]*stockfishchess\.org[^\s"\'<>)\]]*'    # domini
    r'|\bofficial-stockfish/[^\s"\'<>)\]]*'            # percorsi GitHub
    r'|\bgithub\.com/[^\s"\'<>)\]]*)',
    re.IGNORECASE)
WORD_RE = re.compile(r'(?<![A-Za-z0-9\-])(stockfish|Stockfish|STOCKFISH)(?![A-Za-z0-9])')
CASE_MAP = {'stockfish': 'shashchess', 'Stockfish': 'ShashChess', 'STOCKFISH': 'SHASHCHESS'}


# --------------------------------------------------------------------------- I/O
def read_text(p):
    with open(p, 'r', encoding='utf-8', errors='surrogateescape', newline='') as fh:
        return fh.read()


def write_text(p, text):
    with open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='') as fh:
        fh.write(text)


def eol_of(text):
    return '\r\n' if '\r\n' in text[:4000] else '\n'


def rel_posix(path, root):
    return Path(path).resolve().relative_to(Path(root).resolve()).as_posix()


def is_third_party(rel):
    return any(rel.startswith(tp) for tp in THIRD_PARTY)


def kind_of(rel):
    name = rel.rsplit('/', 1)[-1]
    ext = os.path.splitext(name)[1].lower()
    if ext in CPP_EXT:
        return 'cpp'
    if name == 'Makefile' or name.endswith('.mk'):
        return 'make'
    if ext in SCRIPT_EXT:
        return 'script'
    return None


# ----------------------------------------------------------------- intestazioni
def load_template(path):
    lines = read_text(path).replace('\r\n', '\n').split('\n')
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def render_header(tpl_lines, style, year, eol='\n'):
    body = [l.replace('{year}', year) for l in tpl_lines]
    if style == 'cpp':
        out = ['/*'] + [('  ' + l) if l.strip() else '' for l in body] + ['*/']
    else:
        out = [('# ' + l) if l.strip() else '#' for l in body]
    return eol.join(out) + eol


def header_year(text, default):
    m = re.search(r'Copyright \(C\) 2004-(\d{4})', text[:3000])
    return m.group(1) if m else default


def split_cpp_header(text):
    """(prefisso, blocco, resto): blocco e' il /* ... */ iniziale se contiene la GPL."""
    m = re.match(r'(\ufeff?\s*)(/\*.*?\*/[ \t]*(?:\r?\n)?)', text, re.S)
    if m and LICENSE_MARK in m.group(2):
        return m.group(1), m.group(2), text[m.end():]
    return None


def split_hash_header(text):
    """Blocco di righe '#' contigue in testa (dopo un eventuale shebang) con la GPL."""
    lines = text.splitlines(keepends=True)
    i = 0
    shebang = ''
    if lines and lines[0].startswith('#!'):
        shebang = lines[0]
        i = 1
        while i < len(lines) and not lines[i].strip():
            shebang += lines[i]
            i += 1
    j = i
    while j < len(lines) and lines[j].startswith('#') and not lines[j].startswith('#!'):
        j += 1
    block = ''.join(lines[i:j])
    if LICENSE_MARK in block and UPSTREAM_MARK.search(block):
        return shebang, block, ''.join(lines[j:])
    return None


def set_header(text, kind, tpl, year=None, add_missing=False):
    """Sostituisce (o aggiunge) l'intestazione canonica. Restituisce (testo, azione)."""
    eol = eol_of(text)
    year = year or header_year(text, '2026')
    if kind == 'cpp':
        parts = split_cpp_header(text)
        new_hdr = render_header(tpl, 'cpp', year, eol)
        if parts:
            pre, blk, rest = parts
            if not blk.endswith(('\n', '\r\n')):
                rest = eol + rest.lstrip('\r\n')
            return pre + new_hdr + rest, ('ok' if blk == new_hdr else 'sostituita')
        if add_missing:
            return new_hdr + eol + text.lstrip('\ufeff'), 'aggiunta'
        return text, 'assente'
    if kind in ('make', 'script'):
        parts = split_hash_header(text)
        new_hdr = render_header(tpl, 'hash', year, eol)
        if parts:
            pre, blk, rest = parts
            return pre + new_hdr + rest, ('ok' if blk == new_hdr else 'sostituita')
        return text, 'assente'
    return text, 'n/a'


# ------------------------------------------------------------------ rebranding
def rebrand_cpp(text):
    text = re.sub(r'\bnamespace Stockfish\b', 'namespace ShashChess', text)
    text = re.sub(r'\bStockfish::', 'ShashChess::', text)
    text = re.sub(r'\bStockfish_##', 'ShashChess_##', text)
    return text


def rebrand_words(text):
    """Rinomina case-preserving fuori dagli URL (Makefile, script, test Python)."""
    out, pos = [], 0
    for m in URL_RE.finditer(text):
        out.append(WORD_RE.sub(lambda w: CASE_MAP[w.group(1)], text[pos:m.start()]))
        out.append(m.group(0))
        pos = m.end()
    out.append(WORD_RE.sub(lambda w: CASE_MAP[w.group(1)], text[pos:]))
    text = ''.join(out)
    # build "universal" SF19: il namespace di ogni slice e' ottenuto con -D<ns>=<ns>_<arch>
    text = text.replace('-DStockfish=', '-DShashChess=')
    return text


def transform(text, rel, tpl):
    """La trasformazione T: da file upstream a file con marchio ShashChess."""
    kind = kind_of(rel)
    if kind is None or is_third_party(rel):
        return text
    if kind == 'cpp':
        text = rebrand_cpp(text)
    else:
        text = rebrand_words(text)
    text, _ = set_header(text, kind, tpl)
    return text


def iter_files(root, subdirs=UPSTREAM_ROOTS):
    root = Path(root)
    for sd in subdirs:
        base = root / sd
        if not base.is_dir():
            continue
        for dp, dn, fn in os.walk(base):
            dn[:] = [d for d in dn if d not in ('.git',)]
            for f in sorted(fn):
                p = Path(dp) / f
                yield p, p.relative_to(root).as_posix()


# ------------------------------------------------------------------ comandi
def cmd_apply(args):
    tpl = load_template(args.template)
    src = Path(args.src)
    dst = Path(args.dst) if args.dst else src
    n = 0
    for p, rel in iter_files(src):
        if kind_of(rel) is None or is_third_party(rel):
            if dst != src:
                q = dst / rel
                q.parent.mkdir(parents=True, exist_ok=True)
                q.write_bytes(p.read_bytes())
            continue
        text = read_text(p)
        new = transform(text, rel, tpl)
        q = dst / rel
        q.parent.mkdir(parents=True, exist_ok=True)
        if new != text or dst != src:
            write_text(q, new)
            n += int(new != text)
    print(f"apply: {n} file trasformati in {dst}")
    return 0


def cmd_headers(args):
    tpl = load_template(args.template)
    stats = {}
    changed = []
    for p, rel in iter_files(args.root, ('src', 'scripts', 'tests')):
        kind = kind_of(rel)
        if kind is None or is_third_party(rel):
            continue
        text = read_text(p)
        add = args.add_missing and kind == 'cpp' and not rel.startswith(NO_AUTO_HEADER)
        new, action = set_header(text, kind, tpl, add_missing=add)
        stats[action] = stats.get(action, 0) + 1
        if new != text:
            changed.append((rel, action))
            if not args.dry_run:
                write_text(p, new)
    for rel, action in changed:
        print(f"  {action:<11} {rel}")
    print("\nRiepilogo:", ', '.join(f"{k}={v}" for k, v in sorted(stats.items())))
    if stats.get('assente') and not args.add_missing:
        print("  ('assente' = file senza intestazione GPL: usa --add-missing per aggiungerla;\n"
              "   book/polyglot e book/ctg sono esclusi finche' non ne verifichi la provenienza)")
    print("(dry-run: nessun file modificato)" if args.dry_run else f"{len(changed)} file riscritti")
    return 0


def _norm_nonblank(text):
    return [l.rstrip() for l in text.replace('\r\n', '\n').split('\n') if l.strip()]


def _unbrand(text):
    """Riporta ogni marchio ShashChess a Stockfish: serve solo a riconoscere i file
    che differiscono dall'upstream unicamente per un rebranding incoerente."""
    return re.sub(r'shashchess|ShashChess|SHASHCHESS',
                  lambda m: {'shashchess': 'stockfish', 'ShashChess': 'Stockfish',
                             'SHASHCHESS': 'STOCKFISH'}[m.group(0)], text)


def _strip_hdr(text, kind):
    parts = split_cpp_header(text) if kind == 'cpp' else split_hash_header(text)
    return parts[2] if parts else text


class Upstream:
    """Albero upstream: directory estratta oppure riferimento git (es. vendor/sf_18)."""

    def __init__(self, spec, repo='.'):
        self.spec, self.repo = spec, repo
        self.is_dir = os.path.isdir(spec)
        if not self.is_dir:
            r = subprocess.run(['git', '-C', repo, 'ls-tree', '-r', '--name-only', spec],
                               capture_output=True, text=True)
            if r.returncode != 0:
                raise SystemExit(f"'{spec}' non e' ne' una directory ne' un riferimento git")
            self.files = [f for f in r.stdout.split('\n')
                          if f and f.split('/', 1)[0] in UPSTREAM_ROOTS]

    def items(self):
        if self.is_dir:
            for p, rel in iter_files(self.spec):
                yield rel, None
        else:
            for rel in self.files:
                yield rel, None

    def read(self, rel):
        if self.is_dir:
            return read_text(Path(self.spec) / rel)
        r = subprocess.run(['git', '-C', self.repo, 'show', f'{self.spec}:{rel}'],
                           capture_output=True, text=True, encoding='utf-8',
                           errors='surrogateescape')
        return r.stdout

    def read_bytes(self, rel):
        if self.is_dir:
            return (Path(self.spec) / rel).read_bytes()
        return subprocess.run(['git', '-C', self.repo, 'show', f'{self.spec}:{rel}'],
                              capture_output=True).stdout


def cmd_verify(args):
    tpl = load_template(args.template)
    up = Upstream(args.upstream, args.sc)
    rows = []
    for rel, _ in up.items():
        kind = kind_of(rel)
        q = Path(args.sc) / rel
        if not q.exists():
            rows.append((rel, 'ASSENTE-IN-SC', 0, 0))
            continue
        if kind is None or is_third_party(rel):
            same = up.read_bytes(rel) == q.read_bytes()
            rows.append((rel, 'IDENTICO' if same else 'ALTRO', 0, 0))
            continue
        t = transform(up.read(rel), rel, tpl)
        s = read_text(q)
        if t == s:
            rows.append((rel, 'IDENTICO', 0, 0))
            continue
        tb, sb = _strip_hdr(t, kind), _strip_hdr(s, kind)
        if tb == sb:
            rows.append((rel, 'SOLO-INTESTAZIONE', 0, 0))
            continue
        if _norm_nonblank(tb) == _norm_nonblank(sb):
            rows.append((rel, 'SOLO-SPAZI', 0, 0))
            continue
        if _norm_nonblank(_unbrand(tb)) == _norm_nonblank(_unbrand(sb)):
            rows.append((rel, 'SOLO-MARCHIO', 0, 0))
            continue
        d = [l for l in difflib.unified_diff(_norm_nonblank(tb), _norm_nonblank(sb), lineterm='', n=0)
             if l[:1] in '+-' and not l.startswith(('+++', '---'))]
        rows.append((rel, 'MODIFICATO', sum(l[0] == '+' for l in d), sum(l[0] == '-' for l in d)))

    order = ['IDENTICO', 'SOLO-INTESTAZIONE', 'SOLO-SPAZI', 'SOLO-MARCHIO', 'MODIFICATO', 'ALTRO',
             'ASSENTE-IN-SC']
    count = {k: sum(1 for r in rows if r[1] == k) for k in order}
    print("Confronto T(upstream) vs ShashChess (upstream = %s)\n" % args.upstream)
    for k in order:
        print(f"  {k:<18} {count[k]:>4}")
    print()
    for k in order[1:]:
        sel = [r for r in rows if r[1] == k]
        if not sel:
            continue
        print(f"--- {k} ---")
        for rel, _, a, r in sorted(sel, key=lambda x: -(x[2] + x[3])):
            extra = f"  SC +{a}/-{r}" if k == 'MODIFICATO' else ''
            print(f"  {rel}{extra}")
        print()

    if args.fix_whitespace:
        fixed = 0
        for rel, k, _, _ in rows:
            if k in ('SOLO-INTESTAZIONE', 'SOLO-SPAZI', 'SOLO-MARCHIO'):
                write_text(Path(args.sc) / rel, transform(up.read(rel), rel, tpl))
                fixed += 1
        print(f"--fix-whitespace: {fixed} file riallineati byte per byte a T(upstream).")
        print("Sono modifiche di forma (spazi, intestazione, marchio incoerente): il bench")
        print("deve restare IDENTICO. Controlla comunque i SOLO-MARCHIO con 'git diff'.")

    if args.report:
        with open(args.report, 'w', encoding='utf-8') as fh:
            fh.write("file\tstato\tsc_add\tsc_del\n")
            for r in sorted(rows):
                fh.write(f"{r[0]}\t{r[1]}\t{r[2]}\t{r[3]}\n")
        print(f"Report scritto in {args.report}")
    return 0


def cmd_modes(args):
    """Sorgenti, Makefile e testi a 100644; script .sh a 100755. Funziona anche su
    Windows (core.filemode=false), perche' agisce sull'indice e non sul disco."""
    out = subprocess.run(['git', '-C', args.repo, 'ls-files', '-s'], capture_output=True,
                         text=True, check=True).stdout
    to_minus, to_plus = [], []
    for line in out.splitlines():
        meta, path = line.split('\t', 1)
        mode = meta.split()[0]
        if mode not in ('100644', '100755'):
            continue                      # symlink, submodule: non si toccano
        in_vendor_roots = path.split('/', 1)[0] in UPSTREAM_ROOTS
        if path.endswith('.sh'):
            if mode == '100644':
                to_plus.append(path)
        elif in_vendor_roots or kind_of(path) in ('cpp', 'make'):
            if mode == '100755':
                to_minus.append(path)
    for flag, paths in (('-x', to_minus), ('+x', to_plus)):
        for i in range(0, len(paths), 200):
            chunk = paths[i:i + 200]
            if chunk and not args.dry_run:
                subprocess.run(['git', '-C', args.repo, 'update-index', f'--chmod={flag}', '--'] + chunk,
                               check=True)
        if not args.dry_run and os.name != 'nt':
            # allinea anche il disco, altrimenti con core.filemode=true git vedrebbe
            # di nuovo il file come modificato
            for path in paths:
                fp = Path(args.repo) / path
                if fp.is_file():
                    m = fp.stat().st_mode
                    fp.chmod((m | 0o111) if flag == '+x' else (m & ~0o111))
    print(f"modes: {len(to_minus)} file -> 100644, {len(to_plus)} script -> 100755"
          + (" (dry-run)" if args.dry_run else " (registrati nell'indice: ora 'git commit')"))
    return 0


def cmd_check(args):
    tpl = load_template(args.template)
    bad = []
    for p, rel in iter_files(args.root, ('src', 'scripts', 'tests')):
        kind = kind_of(rel)
        if kind is None or is_third_party(rel):
            continue
        text = read_text(p)
        if kind == 'cpp':
            for m in re.finditer(r'\bnamespace Stockfish\b|\bStockfish::|\bStockfish_##', text):
                ln = text.count('\n', 0, m.start()) + 1
                bad.append((rel, ln, 'identificatore Stockfish: ' + m.group(0)))
        if kind == 'make':
            for m in re.finditer(r'-DStockfish=|\bEXE\s*=\s*stockfish', text):
                ln = text.count('\n', 0, m.start()) + 1
                bad.append((rel, ln, 'Makefile: ' + m.group(0)))
        head = text[:1500]
        if 'derived from Glaurung' in head:
            bad.append((rel, 1, "intestazione 'derived from Glaurung'"))
        elif UPSTREAM_MARK.search(head):
            new, action = set_header(text, kind, tpl)
            if action == 'sostituita':
                bad.append((rel, 1, 'intestazione non canonica'))
    for rel, ln, why in bad:
        print(f"{rel}:{ln}: {why}")
    print(f"\ncheck: {len(bad)} problemi" + ("" if bad else " - albero pulito"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--template', default=str(DEFAULT_TEMPLATE), help='modello di intestazione')
    sub = ap.add_subparsers(dest='cmd', required=True)

    a = sub.add_parser('apply', help='applica T a un albero upstream')
    a.add_argument('--src', required=True)
    a.add_argument('--dst')
    a.set_defaults(fn=cmd_apply)

    h = sub.add_parser('headers', help='canonicalizza le intestazioni in un albero ShashChess')
    h.add_argument('--root', default='.')
    h.add_argument('--add-missing', action='store_true',
                   help="aggiunge l'intestazione ai sorgenti che non ce l'hanno")
    h.add_argument('--dry-run', action='store_true')
    h.set_defaults(fn=cmd_headers)

    v = sub.add_parser('verify', help='confronta T(upstream) con ShashChess')
    v.add_argument('--sc', default='.')
    v.add_argument('--upstream', required=True,
                   help='albero Stockfish: directory estratta (radice con src/) oppure riferimento git, '
                        'es. vendor/sf_18')
    v.add_argument('--fix-whitespace', action='store_true')
    v.add_argument('--report')
    v.set_defaults(fn=cmd_verify)

    m = sub.add_parser('modes', help='normalizza i permessi registrati in git')
    m.add_argument('--repo', default='.')
    m.add_argument('--dry-run', action='store_true')
    m.set_defaults(fn=cmd_modes)

    c = sub.add_parser('check', help='gate CI: residui di rebranding')
    c.add_argument('--root', default='.')
    c.set_defaults(fn=cmd_check)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
