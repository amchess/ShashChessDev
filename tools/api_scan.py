#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
api_scan.py - la checklist di migrazione SF18 -> SF19, ma eseguibile.

La checklist statica (API_MIGRATION_CHECKLIST.md) riportava righe di SC41: dopo
il merge i numeri di riga non valgono piu'. Questo scanner cerca nell'albero
CORRENTE le API di SF18 rimosse o rinominate in SF19 e dice cosa resta da fare.
Rilancialo dopo ogni gruppo di correzioni: quando esce 0, la migrazione API e'
completa (il compilatore fara' il resto).

Con --sf19 verifica prima se stesso: ogni schema deve avere ZERO occorrenze in
Stockfish 19, altrimenti e' ambiguo e viene segnalato come tale.

  python tools/api_scan.py --root src
  python tools/api_scan.py --root src --sf19 /tmp/sf_19/src      # auto-verifica
  python tools/api_scan.py --root src --only shashin,mcts,learn,book,livebook,wdl
"""

import argparse
import os
import re
import sys

# (id, regex, cosa fare)
RULES = [
    ('rete-singola', r'\bNNUE::Networks\b|\bnetworks\.(big|small)\b|\bNetworks&',
     "SF19 ha una rete sola: NNUE::Networks -> NNUE::Network, networks.big -> network. Niente struct fittizie."),
    ('evalfile-small', r'\bEvalFileSmall\b|\bEvalFileDefaultName(Big|Small)\b',
     "Opzione e rete secondaria rimosse: si cancellano, non si rimappano. Nome rete: EvalFileDefaultName."),
    ('cache-doppia', r'\bcaches\s*(->|\.)\s*(big|small)\b|\bCache<\w+>&',
     "AccumulatorCaches unica: niente .big/.small; passare AccumulatorCaches& ."),
    ('completedDepth', r'\bcompletedDepth\b',
     "Campo rimosso dal Worker: usare rootDepth o un campo ShashChess dedicato (learning, MCTS)."),
    ('null-move-tt', r'(\.|->)do_null_move\s*\([^()]*,\s*\w*\s*\)|do_null_move\s*\([^()]*TranspositionTable',
     "do_null_move(StateInfo&) non riceve piu' la TranspositionTable."),
    ('score-bounds', r'\bscore(Lower|Upper)bound\b',
     "Sostituiti da inexactLower/inexactUpper (+ previousScoreExact, is_inexact())."),
    ('output-pv', r'main_manager\(\)\s*->\s*pv\s*\(|\bSearchManager::pv\b',
     "SearchManager::pv() -> output_pv()."),
    ('elapsed-time', r'\bWorker::elapsed_time\b|\belapsed_time\(\)\s*const\s*;',
     "Worker::elapsed_time() rimosso: usare main_manager()->tm.elapsed_time() (TimeManager)."),
    ('attacks-ns', r'(?<!Attacks::)(?<![\w:])attacks_bb\s*<',
     "attacks_bb vive in namespace Attacks (attacks.h): prefisso Attacks:: oppure "
     "'using namespace Attacks;' nel .cpp, come fa position.cpp di SF19."),
    ('attacks-pawn-sq', r'\bpawn_attacks_bb\s*\(\s*\w+\s*,',
     "pawn_attacks_bb(Color, Square) -> Attacks::attacks_bb(s, c). (La forma template su Bitboard resta.)"),
    ('depth-offset', r'\bDEPTH_ENTRY_OFFSET\b', "Rinominata DEPTH_NONE (types.h)."),
    ('dirty', r'\bDirtyThreats\b.*&|\bscratch_(dp|dts)\b',
     "DirtyPiece + DirtyThreats confluiscono in Dirties (scratchDirties)."),
    ('key-after', r'\bkey_after\s*\(', "Rinominata prefetch_key()."),
    ('header-shm', r'#include\s+"(shm_linux|thread_win32_osx)\.h"',
     "Header rinominati: shm_unix.h, thread_native.h."),
    ('reduction-depth', r'\bDepth\s+(Search::Worker::)?reduction\s*\(',
     "reduction() restituisce int in SF19: aggiornare firma e hook LMR."),
    ('pos-set-ignored', r'^\s*\w+(\.|->)set\s*\(\s*[^;]*,\s*&\w+\s*\)\s*;',
     "Position::set() restituisce std::optional<PositionSetError>: su FEN invalida gestire l'errore "
     "(book/, livebook/, learn/), altrimenti uci.cpp termina il processo."),
    ('binary-dir-string', r'\bstd::string\s+binaryDirectory\b|\bbinaryDirectory\s*\+\s*"',
     "Percorsi come std::filesystem::path (+ path_from_utf8): book, livebook, experience."),
    ('mcts-reduction-copy', r'delta\s*\*\s*608\s*/\s*rootDelta|reductionScale\s*\*\s*238\s*/\s*512',
     "Copia della formula reduction() di SF18 (608/238/1182): SF19 usa 577/197/982. Risincronizzare."),
    ('wdl-coeff-copy', r'constexpr\s+double\s+as\[\]',
     "Copia dei coefficienti WDL: deve restarne UNA (wdl/win_probability.cpp) = SF19. "
     "Verifica: wdl_calibration.py extract."),
    ('simple-eval', r'\b(simple_eval|use_smallnet)\s*\(',
     "Rimosse in SF19. In SC41 le usava solo evaluate.cpp di SF18: spariscono con il merge."),
]

SRC_RE = re.compile(r'\.(cpp|h|hpp)$')
SKIP = ('incbin', 'livebook/json')


def scan(root, only=None):
    hits = {rid: [] for rid, _, _ in RULES}
    comp = [(rid, re.compile(rx, re.M)) for rid, rx, _ in RULES]
    for dp, dn, fn in os.walk(root):
        rel_dir = os.path.relpath(dp, root).replace('\\', '/')
        if any(rel_dir == s or rel_dir.startswith(s + '/') for s in SKIP):
            dn[:] = []
            continue
        if only and not any(rel_dir == o or rel_dir.startswith(o + '/') for o in only):
            continue
        for f in fn:
            if not SRC_RE.search(f):
                continue
            p = os.path.join(dp, f)
            text = open(p, encoding='utf-8', errors='replace').read()
            code = re.sub(r'//[^\n]*', '', text)          # ignora i commenti di riga
            uses_attacks_ns = 'using namespace Attacks' in code
            for rid, rx in comp:
                if rid == 'attacks-ns' and uses_attacks_ns:
                    continue
                for m in rx.finditer(code):
                    ln = code.count('\n', 0, m.start()) + 1
                    hits[rid].append((os.path.relpath(p, root).replace('\\', '/'), ln,
                                      code.split('\n')[ln - 1].strip()[:100]))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default='src')
    ap.add_argument('--sf19', help='src/ di Stockfish 19 per l\'auto-verifica degli schemi')
    ap.add_argument('--only', help='solo queste sottodirectory, separate da virgola')
    ap.add_argument('--max', type=int, default=12)
    args = ap.parse_args()

    ambiguous = set()
    if args.sf19:
        ref = scan(args.sf19)
        ambiguous = {rid for rid, h in ref.items() if h}
        for rid in sorted(ambiguous):
            print(f"[auto-verifica] schema '{rid}' compare {len(ref[rid])} volte in SF19: trattalo come indicativo")
    only = args.only.split(',') if args.only else None
    hits = scan(args.root, only)
    total = 0
    for rid, _, todo in RULES:
        h = hits[rid]
        if not h:
            continue
        total += len(h) if rid not in ambiguous else 0
        tag = ' (indicativo)' if rid in ambiguous else ''
        print(f"\n[{rid}]{tag} {len(h)} occorrenze - {todo}")
        for f, ln, s in h[:args.max]:
            print(f"   {f}:{ln}: {s}")
        if len(h) > args.max:
            print(f"   ... altre {len(h) - args.max}")
    print(f"\nTotale da migrare: {total}" + (" - migrazione API completa." if not total else ""))
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
