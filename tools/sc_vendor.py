#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sc_vendor.py - ramo "vendor" di Stockfish rebrandizzato, per merge a tre vie veri.

Perche' serve
-------------
La storia di ShashChess non ha antenati in comune con quella di Stockfish
(verificato: `git merge-base` non restituisce nulla). Un `git merge sf_19`
diretto viene quindi rifiutato ("refusing to merge unrelated histories"), e con
--allow-unrelated-histories ogni file diventa un conflitto add/add.

La soluzione classica e' il ramo vendor:

    vendor/stockfish:   V18 = T(sf_18)  --->  V19 = T(sf_19)  ---> (V20 ...)
                         |
    port/sf19:  main --- M (merge -s ours: "main contiene gia' V18") --- merge V19

dove T e' la trasformazione di sc_rebrand.py. Dopo il merge "-s ours", git sa
che la base comune e' V18: il merge di V19 applica esattamente il delta
SF18->SF19, gia' con il marchio ShashChess, e si ferma SOLO dove ShashChess ha
modificato le stesse righe.

Sottocomandi
------------
  import  crea un commit sul ramo vendor con T(<tag>) (src/, scripts/, tests/)
  graft   sul ramo corrente: merge -s ours del commit vendor di base
  status  mostra lo stato del ramo vendor e la base di merge corrente

Esempio completo (dalla radice di ShashChessDev)
-----------------------------------------------
  python tools/sc_vendor.py import --tag sf_18
  python tools/sc_vendor.py import --tag sf_19
  git switch -c port/sf19 main
  python tools/sc_vendor.py graft --base vendor/sf_18
  git merge vendor/sf_19

Per SF20 bastera':  import --tag sf_20  e  git merge vendor/sf_20.

Il working tree non viene mai toccato da import: i commit vendor sono costruiti
con un indice temporaneo (GIT_INDEX_FILE) e `git commit-tree`.
"""

import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sc_rebrand as R  # noqa: E402

TARBALL_URL = 'https://codeload.github.com/official-stockfish/Stockfish/tar.gz/refs/tags/{tag}'
BRANCH = 'vendor/stockfish'


def git(repo, *args, env=None, check=True, capture=True):
    r = subprocess.run(['git', '-C', str(repo)] + list(args), env=env, text=True,
                       capture_output=capture)
    if check and r.returncode != 0:
        sys.stderr.write(r.stdout or '')
        sys.stderr.write(r.stderr or '')
        raise SystemExit(f"git {' '.join(args)} fallito ({r.returncode})")
    return (r.stdout or '').strip()


def fetch_upstream(tag, source, dest):
    """Estrae l'albero upstream in dest. Restituisce (sha_commit|None, insieme dei file eseguibili)."""
    exec_files = set()
    sha = None
    if source and Path(source).is_dir():
        src = Path(source)
        for p in src.rglob('*'):
            if p.is_file() and '.git' not in p.parts:
                rel = p.relative_to(src)
                q = dest / rel
                q.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, q)
                if os.access(p, os.X_OK):
                    exec_files.add(rel.as_posix())
        return sha, exec_files
    if source and Path(source).is_file():
        data = Path(source).read_bytes()
    else:
        url = source or TARBALL_URL.format(tag=tag)
        print(f"  scarico {url}")
        with urllib.request.urlopen(url, timeout=120) as r:
            data = r.read()
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as tf:
        sha = (tf.pax_headers or {}).get('comment')
        for m in tf.getmembers():
            parts = m.name.split('/', 1)
            if len(parts) < 2 or not parts[1] or not m.isfile():
                continue
            rel = parts[1]
            if '..' in Path(rel).parts:
                continue
            q = dest / rel
            q.parent.mkdir(parents=True, exist_ok=True)
            with tf.extractfile(m) as fh:
                q.write_bytes(fh.read())
            if m.mode & 0o111:
                exec_files.add(rel)
    return sha, exec_files


def t_fingerprint(template):
    h = hashlib.sha256()
    h.update(Path(R.__file__).read_bytes())
    h.update(Path(template).read_bytes())
    return h.hexdigest()[:12]


def cmd_import(args):
    repo = Path(args.repo).resolve()
    tpl = R.load_template(args.template)
    tmp = Path(tempfile.mkdtemp(prefix='sc_vendor_'))
    try:
        up, tree = tmp / 'up', tmp / 'tree'
        up.mkdir()
        tree.mkdir()
        sha, exec_files = fetch_upstream(args.tag, args.source, up)
        n_files = n_changed = 0
        for p, rel in R.iter_files(up, R.UPSTREAM_ROOTS):
            q = tree / rel
            q.parent.mkdir(parents=True, exist_ok=True)
            if R.kind_of(rel) is None or R.is_third_party(rel):
                q.write_bytes(p.read_bytes())
            else:
                text = R.read_text(p)
                new = R.transform(text, rel, tpl)
                R.write_text(q, new)
                n_changed += int(new != text)
            n_files += 1
        if not n_files:
            raise SystemExit("albero upstream vuoto: tag o sorgente errati?")

        env = dict(os.environ, GIT_INDEX_FILE=str(tmp / 'index'))
        gitdir = git(repo, 'rev-parse', '--absolute-git-dir')
        base = ['git', '--git-dir', gitdir, '--work-tree', str(tree)]
        subprocess.run(base + ['add', '-A', '--force', '.'], env=env, check=True)
        listed = subprocess.run(base + ['ls-files'], env=env, check=True, text=True,
                                capture_output=True).stdout.split('\n')
        want_x = [f for f in listed if f and (f in exec_files or f.endswith('.sh'))]
        want_nx = [f for f in listed if f and f not in want_x]
        for flag, paths in (('+x', want_x), ('-x', want_nx)):
            for i in range(0, len(paths), 200):
                if paths[i:i + 200]:
                    subprocess.run(base + ['update-index', f'--chmod={flag}', '--'] + paths[i:i + 200],
                                   env=env, check=True)
        tree_sha = subprocess.run(base + ['write-tree'], env=env, check=True, text=True,
                                  capture_output=True).stdout.strip()

        branch = args.branch
        parent = git(repo, 'rev-parse', '--verify', '-q', f'refs/heads/{branch}', check=False)
        if parent:
            ptree = git(repo, 'rev-parse', f'{parent}^{{tree}}')
            if ptree == tree_sha:
                print(f"{branch} contiene gia' questo albero ({args.tag}): nulla da fare.")
                git(repo, 'tag', '-f', f'vendor/{args.tag}', parent)
                return 0
        msg = (f"vendor: Stockfish {args.tag} con marchio ShashChess\n\n"
               f"Upstream: official-stockfish/Stockfish {args.tag}"
               + (f" ({sha})" if sha else '') + "\n"
               f"Trasformazione: tools/sc_rebrand.py, impronta {t_fingerprint(args.template)}\n"
               f"Percorsi: {', '.join(R.UPSTREAM_ROOTS)} - {n_files} file, {n_changed} rebrandizzati\n\n"
               "Commit generato da tools/sc_vendor.py: non modificarlo a mano.\n")
        cmd = ['commit-tree', tree_sha, '-m', msg] + (['-p', parent] if parent else [])
        commit = git(repo, *cmd)
        git(repo, 'update-ref', f'refs/heads/{branch}', commit, *( [parent] if parent else [] ))
        git(repo, 'tag', '-f', f'vendor/{args.tag}', commit)
        print(f"{branch} -> {commit[:12]}  (tag vendor/{args.tag}; {n_files} file, "
              f"{n_changed} rebrandizzati; upstream {sha[:12] if sha else 'n/d'})")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cmd_graft(args):
    repo = Path(args.repo).resolve()
    cur = git(repo, 'rev-parse', '--abbrev-ref', 'HEAD')
    if cur in ('main', 'master') and not args.force:
        raise SystemExit("Sei su '%s'. Esegui il graft sul ramo di porting (git switch -c port/sf19), "
                         "oppure usa --force se vuoi davvero farlo sul ramo principale." % cur)
    if git(repo, 'status', '--porcelain', '--untracked-files=no'):
        raise SystemExit("Working tree con modifiche non committate: fai commit o stash prima del graft.")
    base = git(repo, 'rev-parse', '--verify', args.base)
    if subprocess.run(['git', '-C', str(repo), 'merge-base', '--is-ancestor', base, 'HEAD']).returncode == 0:
        print(f"{args.base} e' gia' antenato di HEAD: graft non necessario.")
        return 0
    before = git(repo, 'rev-parse', 'HEAD^{tree}')
    git(repo, 'merge', '-s', 'ours', '--allow-unrelated-histories', '--no-edit', '-m',
        f"vendor: registra {args.base} come base di merge (nessuna modifica ai file)\n\n"
        "Merge 'ours': dichiara che ShashChess contiene gia' il Stockfish di base,\n"
        "cosi' il merge successivo del ramo vendor applica solo il delta upstream.", base)
    after = git(repo, 'rev-parse', 'HEAD^{tree}')
    assert before == after, "il graft non deve cambiare alcun file"
    print(f"graft ok: {args.base} ({base[:12]}) registrato come antenato di {cur}. Albero invariato.")
    print("Prossimo passo:  git merge vendor/<tag nuovo>")
    return 0


def cmd_status(args):
    repo = Path(args.repo).resolve()
    tip = git(repo, 'rev-parse', '--verify', '-q', f'refs/heads/{args.branch}', check=False)
    if not tip:
        print(f"Ramo {args.branch} assente: esegui 'import --tag sf_18'.")
        return 1
    print(git(repo, 'log', '--format=%h %s', args.branch))
    print("\ntag vendor:", ' '.join(git(repo, 'tag', '-l', 'vendor/*').split()))
    mb = git(repo, 'merge-base', 'HEAD', args.branch, check=False)
    if mb:
        print("base di merge fra HEAD e", args.branch, ":", git(repo, 'log', '-1', '--format=%h %s', mb))
    else:
        print("HEAD e il ramo vendor non hanno ancora antenati comuni: serve 'graft'.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default='.')
    ap.add_argument('--branch', default=BRANCH)
    ap.add_argument('--template', default=str(R.DEFAULT_TEMPLATE))
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('import')
    i.add_argument('--tag', required=True, help='tag upstream, es. sf_18, sf_19')
    i.add_argument('--source', help='URL di un tarball, file .tar.gz locale o directory estratta')
    i.set_defaults(fn=cmd_import)
    g = sub.add_parser('graft')
    g.add_argument('--base', required=True, help='commit vendor di base, es. vendor/sf_18')
    g.add_argument('--force', action='store_true')
    g.set_defaults(fn=cmd_graft)
    s = sub.add_parser('status')
    s.set_defaults(fn=cmd_status)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
