#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wdl_calibration.py - zone Shashin definite dalla win probability, portate su SF19.

Principio (quello di ShashChess)
--------------------------------
Le zone sono funzioni di (W, D, L), non dei centipawn. La win probability e'
l'invariante: se il modello WDL e' calibrato sulla rete che produce la
valutazione, "75 %" significa la stessa cosa in SF18 e in SF19. Quindi:

  * le SOGLIE (5/10/15/20/24/49/50/75/79/84/89/94) NON si toccano;
  * si cambia il MODELLO: i coefficienti devono essere quelli di SF19, in UNA
    sola sorgente, usata da zone, trace, UCI (score cp / wdl) e learning.

Cosa controlla questo strumento
-------------------------------
  extract    legge i coefficienti direttamente da uci.cpp di SF19 (niente numeri
             ricopiati a mano) e li confronta con TUTTE le copie in ShashChess
  cpp        stampa il blocco C++ da incollare in wdl/win_probability.cpp
  trap       quanto sbaglia la zona chi porta a SF19 senza cambiare i coefficienti
  zones      mappa delle zone intorno all'equilibrio, per modello e per regola:
             'exact' (adottata) = probabilita' calcolata sui valori non arrotondati,
             simmetrica, con le soglie attuali e CAPABLANCA = probabilita' 50;
             'legacy' = la regola di ShashChess 41 (W + D/2 sui percentuali
             arrotondati, CAPABLANCA solo se D == 100);
             'exact-draw' = 'exact' piu' un vincolo sulla patta (variante scartata)
  capablanca ampiezza in valutazione interna della banda CAPABLANCA (probabilita' 50)
             e delle bande di caos CP [25,49] / CT [51,75], per modello
  scale      rapporto a19/a18: quanto e' cambiata la scala interna della
             valutazione (serve per le costanti in unita' interne e per i .exp)
  constants  elenca i letterali numerici nelle regioni [SC:*] che confrontano
             valori in unita' interne, con il valore riscalato di partenza

Esempi
------
  python tools/wdl_calibration.py --sf19 vendor/sf_19 extract --sc src
  python tools/wdl_calibration.py --sf19 vendor/sf_19 cpp
  python tools/wdl_calibration.py trap
  python tools/wdl_calibration.py zones --rule exact
  python tools/wdl_calibration.py zones --rule legacy --models sf18
  python tools/wdl_calibration.py capablanca
  python tools/wdl_calibration.py scale
  python tools/wdl_calibration.py constants --root src

I coefficienti "sf18"/"sf19" incorporati servono solo da riferimento quando non si
passa --sf19: extract li verifica contro il sorgente vero.
"""

import argparse
import math
import os
import re
import sys
from collections import Counter

BUILTIN = {
    'sf18': ([-72.32565836, 185.93832038, -144.58862193, 416.44950446],
             [83.86794042, -136.06112997, 69.98820887, 47.62901433]),
    'sf19': ([-142.72052667, 372.35176398, -340.71073572, 415.23490212],
             [5.93832785, 15.61267078, -30.57816876, 69.63866711]),
    # seconda copia presente oggi in ShashChess (win_rate_params(const Position&)):
    # alimenta UCIEngine::to_cp (score cp mostrato) e la normalizzazione del learning
    'sc41-pos': ([-37.45051876, 121.19101539, -132.78783573, 420.70576692],
                 [90.26261072, -137.26549898, 71.10130540, 51.35259597]),
}

THRESHOLDS = [(5, 'HP'), (10, 'MHP'), (15, 'MP'), (20, 'MLP'), (24, 'LP'), (49, 'CP'), (50, 'C'),
              (75, 'CT'), (79, 'LT'), (84, 'MLT'), (89, 'MT'), (94, 'MHT'), (100, 'HT')]
ORDER = ['HP', 'MHP', 'MP', 'MLP', 'LP', 'CP', 'C', 'CT', 'LT', 'MLT', 'MT', 'MHT', 'HT', 'CHAOS']
MIRROR = {'HP': 'HT', 'MHP': 'MHT', 'MP': 'MT', 'MLP': 'MLT', 'LP': 'LT', 'CP': 'CT', 'C': 'C',
          'CHAOS': 'CHAOS'}
MIRROR.update({v: k for k, v in list(MIRROR.items())})
LONG = {'HP': 'HIGH_PETROSIAN', 'MHP': 'MIDDLE_HIGH_PETROSIAN', 'MP': 'MIDDLE_PETROSIAN',
        'MLP': 'MIDDLE_LOW_PETROSIAN', 'LP': 'LOW_PETROSIAN', 'CP': 'CAPABLANCA_PETROSIAN',
        'C': 'CAPABLANCA', 'CT': 'CAPABLANCA_TAL', 'LT': 'LOW_TAL', 'MLT': 'MIDDLE_LOW_TAL',
        'MT': 'MIDDLE_TAL', 'MHT': 'MIDDLE_HIGH_TAL', 'HT': 'HIGH_TAL', 'CHAOS': 'TAL_CAPABLANCA_PETROSIAN'}


def cround(x):
    """std::round del C++: meta' lontano da zero (Python round() arrotonda al pari!)."""
    return math.floor(x + 0.5) if x >= 0 else -math.floor(-x + 0.5)


class Model:
    def __init__(self, name, cs, ds):
        self.name, self.cs, self.ds = name, cs, ds

    def ab(self, material):
        m = max(17, min(78, material)) / 58.0
        a = (((self.cs[0] * m + self.cs[1]) * m + self.cs[2]) * m) + self.cs[3]
        b = (((self.ds[0] * m + self.ds[1]) * m + self.ds[2]) * m) + self.ds[3]
        return a, b

    # --- replica esatta di WDLModel::init() + get_win_probability() di ShashChess 41 ---
    def wdl_pct_legacy(self, v, material):
        a, b = self.ab(material)
        v = max(-4000, min(4000, v))
        w = 0.5 + 1000 / (1 + math.exp((a - v) / b))
        l = 0.5 + 1000 / (1 + math.exp((a + v) / b))
        d = 1000 - w - l
        return int(cround(w / 10.0)), max(0, int(cround(d / 10.0))), int(cround(l / 10.0))

    def zone_legacy(self, v, material):
        W, D, L = self.wdl_pct_legacy(v, material)
        wp = W + D // 2
        if wp == 50:
            return 'C' if D == 100 else 'CHAOS'
        return zone_of(wp)

    def zone_trace_legacy(self, v, material):
        W, D, L = self.wdl_pct_legacy(v, material)
        return zone_of(W + D // 2)             # la trace non distingue CAOS

    # --- versione corretta: niente arrotondamenti intermedi, simmetrica ---
    def wdl_permille(self, v, material):
        a, b = self.ab(material)
        v = max(-4000, min(4000, v))
        w = 1000 / (1 + math.exp((a - v) / b))
        l = 1000 / (1 + math.exp((a + v) / b))
        return w, 1000 - w - l, l

    def winprob_fixed(self, v, material):
        w, d, l = self.wdl_permille(v, material)
        e = 500 + (w - l) / 2                  # punteggio atteso in per mille
        k = math.floor(abs(e - 500) / 10 + 0.5)
        return 50 + (k if e >= 500 else -k)

    # regola scelta per ShashChess 42: probabilita' esatta + tabella delle soglie,
    # CAPABLANCA = probabilita' esattamente 50 (nessuna condizione sulla patta).
    def zone_exact(self, v, material):
        return zone_of(self.winprob_fixed(v, material))

    # variante con un ulteriore vincolo sulla patta a probabilita' 50 (non adottata)
    def zone_exact_draw(self, v, material, draw_min):
        p = self.winprob_fixed(v, material)
        if p == 50:
            return 'C' if self.wdl_permille(v, material)[1] >= draw_min else 'CHAOS'
        return zone_of(p)


def zone_of(p):
    for t, n in THRESHOLDS:
        if p <= t:
            return n
    return 'HT'


def _read_source(path):
    """File, directory (radice o src/) oppure riferimento git (es. vendor/sf_19)."""
    if os.path.isdir(path):
        path = os.path.join(path, 'src', 'uci.cpp') if os.path.isdir(os.path.join(path, 'src')) \
            else os.path.join(path, 'uci.cpp')
    if os.path.isfile(path):
        return path, open(path, encoding='utf-8', errors='replace').read()
    import subprocess
    spec = path if ':' in path else f'{path}:src/uci.cpp'
    r = subprocess.run(['git', 'show', spec], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        sys.exit(f"non trovo '{path}': ne' file, ne' directory, ne' riferimento git")
    return spec, r.stdout


def parse_coeffs(path):
    """Estrae as[]/bs[] da un sorgente C++ (uci.cpp di Stockfish o win_probability.cpp)."""
    path, text = _read_source(path)
    out = []
    for m in re.finditer(r'constexpr\s+double\s+as\[\]\s*=\s*\{([^}]*)\};\s*'
                         r'constexpr\s+double\s+bs\[\]\s*=\s*\{([^}]*)\};', text):
        cs = [float(x) for x in m.group(1).replace('\n', ' ').split(',') if x.strip()]
        ds = [float(x) for x in m.group(2).replace('\n', ' ').split(',') if x.strip()]
        out.append((text.count('\n', 0, m.start()) + 1, cs, ds))
    return path, out


def models(args):
    ms = {k: Model(k, *v) for k, v in BUILTIN.items()}
    if getattr(args, 'sf19', None):
        p, found = parse_coeffs(args.sf19)
        if not found:
            sys.exit(f"coefficienti non trovati in {p}")
        ms['sf19'] = Model('sf19', found[0][1], found[0][2])
    return ms


# --------------------------------------------------------------------- comandi
def cmd_extract(args):
    ms = models(args)
    target = ms['sf19']
    print(f"Riferimento SF19: as={target.cs}\n                  bs={target.ds}\n")
    if not args.sc:
        return 0
    bad = 0
    for dp, dn, fn in os.walk(args.sc):
        for f in fn:
            if not f.endswith(('.cpp', '.h')):
                continue
            p = os.path.join(dp, f)
            _, found = parse_coeffs(p)
            for line, cs, ds in found:
                ok = all(abs(x - y) < 1e-9 for x, y in zip(cs + ds, target.cs + target.ds))
                bad += not ok
                print(f"  {'OK      ' if ok else 'DIVERSO '} {os.path.relpath(p, args.sc)}:{line}  as={cs}")
    print(f"\n{bad} copie divergenti dal modello SF19." if bad else "\nTutte le copie coincidono con SF19.")
    print("Obiettivo: UNA sola copia (win_rate_params(int material)), usata da tabella, zone,")
    print("trace, UCIEngine::to_cp, UCIEngine::wdl e learning.")
    return 1 if bad else 0


def cmd_cpp(args):
    m = models(args)['sf19']
    fmt = lambda xs: ', '.join(f'{x:.8f}' for x in xs)
    print("    // Modello WDL di Stockfish 19 (src/uci.cpp, tag sf_19).")
    print("    // UNICA copia in ShashChess: verificata da tools/wdl_calibration.py extract.")
    print(f"    constexpr double as[] = {{{fmt(m.cs)}}};")
    print(f"    constexpr double bs[] = {{{fmt(m.ds)}}};")
    return 0


def cmd_trap(args):
    ms = models(args)
    old, new = ms[args.stale], ms['sf19']
    tot = chg = 0
    toward_c = 0
    peak = (0, None)
    trans = Counter()
    for mat in range(17, 79):
        for v in range(-args.range, args.range + 1):
            z_ok, z_bad = new.zone_legacy(v, mat), old.zone_legacy(v, mat)
            p_ok = sum(new.wdl_pct_legacy(v, mat)[:1]) + new.wdl_pct_legacy(v, mat)[1] // 2
            p_bad = sum(old.wdl_pct_legacy(v, mat)[:1]) + old.wdl_pct_legacy(v, mat)[1] // 2
            tot += 1
            if abs(p_ok - p_bad) > peak[0]:
                peak = (abs(p_ok - p_bad), (v, mat, p_bad, p_ok))
            if z_ok != z_bad:
                chg += 1
                trans[(z_ok, z_bad)] += 1
                if abs(p_bad - 50) < abs(p_ok - 50):
                    toward_c += 1
    print(f"Trappola: valutazioni SF19 lette con i coefficienti '{args.stale}' invece di 'sf19'")
    print(f"Dominio: eval interno [-{args.range}, +{args.range}] x materiale [17, 78] = {tot} stati\n")
    print(f"  stati in zona sbagliata : {chg} ({100 * chg / tot:.1f} %)")
    print(f"  di cui spostati verso il centro (piu' 'Capablanca' del dovuto): {toward_c} "
          f"({100 * toward_c / max(1, chg):.0f} % degli errori)")
    v, mat, pb, po = peak[1]
    print(f"  errore massimo: {peak[0]} punti (eval {v:+d}, materiale {mat}: {pb} % invece di {po} %)\n")
    print("  zona corretta -> zona con coefficienti vecchi (le piu' frequenti):")
    for (a, b), n in trans.most_common(10):
        print(f"      {LONG[a]:<26} -> {LONG[b]:<26} {n:>7}")
    print("\nIl motore compila e il bench torna: solo un dump delle zone lo mostra.")
    return 0


def runs(fn, lo, hi):
    out, cur, start = [], None, lo
    for v in range(lo, hi + 1):
        z = fn(v)
        if z != cur:
            if cur is not None:
                out.append((start, v - 1, cur))
            cur, start = z, v
    out.append((start, hi, cur))
    return out


def cmd_zones(args):
    ms = models(args)
    mats = [int(x) for x in args.materials.split(',')]
    for name in args.models.split(','):
        m = ms[name]
        fn = {'legacy': lambda v, mat: m.zone_legacy(v, mat),
              'exact': lambda v, mat: m.zone_exact(v, mat),
              'exact-draw': lambda v, mat: m.zone_exact_draw(v, mat, args.draw_min)}[args.rule]
        print(f"=== modello {name}, regola {args.rule}"
              + (f" (CAPABLANCA se patta >= {args.draw_min} per mille)" if args.rule == 'exact-draw' else '')
              + " ===")
        for mat in mats:
            r = runs(lambda v: fn(v, mat), -args.window, args.window)
            print(f"  mat {mat:>2}: " + '  '.join(f"[{a:+d},{b:+d}]{z}" for a, b, z in r))
        asym = side = tot = 0
        cnt = Counter()
        for mat in range(17, 79):
            for v in range(-args.range, args.range + 1):
                z, zm = fn(v, mat), fn(-v, mat)
                cnt[z] += 1
                tot += 1
                if MIRROR[z] != zm:
                    asym += 1
                if (v > 0 and ORDER.index(z) < ORDER.index('C')) or \
                   (v < 0 and z in ('CT', 'LT', 'MLT', 'MT', 'MHT', 'HT')):
                    side += 1
        print(f"  asimmetrie zona(v) != specchio(zona(-v)): {asym} su {tot} ({100 * asym / tot:.2f} %)")
        print(f"  zona dalla parte sbagliata rispetto al segno della valutazione: {side} "
              f"({100 * side / tot:.2f} %)")
        print(f"  CAPABLANCA (prob. 50): {cnt['C']} stati   "
              f"caos CP [25,49]: {cnt['CP']} + CT [51,75]: {cnt['CT']} = {cnt['CP'] + cnt['CT']}   "
              f"enum indefinito: {cnt['CHAOS']}\n")
    return 0


def cmd_capablanca(args):
    """Ampiezza in valutazione interna delle bande centrali, con la regola 'exact'
    (CAPABLANCA = probabilita' 50; caos = CP [25,49] e CT [51,75])."""
    ms = models(args)
    names = args.models.split(',')
    print("Ampiezza delle bande centrali in valutazione interna (regola 'exact').")
    print("|C| = meta' ampiezza di CAPABLANCA; |C+CT| = confine superiore del caos (prob. 75).")
    print("Fra parentesi il valore normalizzato sulla scala del modello, cioe' diviso per")
    print("a(materiale) = la valutazione che vale il 50 % di vittoria.\n")
    print(f"{'mat':>4}" + ''.join(f"{n + ' |C|':>16}{n + ' |C+CT|':>18}" for n in names))
    for mat in (17, 20, 25, 30, 40, 50, 58, 66, 70, 74, 78):
        row = f"{mat:>4}"
        for n in names:
            m = ms[n]
            c = max((v for v in range(0, 2001) if m.zone_exact(v, mat) == 'C'), default=0)
            t = max((v for v in range(0, 4001) if m.zone_exact(v, mat) in ('C', 'CT')), default=0)
            a = m.ab(mat)[0]
            row += f"{c:>9} ({c / a:.3f}){t:>11} ({t / a:.3f})"
        print(row)
    print("\nCon la regola adottata non serve alcuna costante sulla patta: la banda CAPABLANCA")
    print("e' definita dalla probabilita' 50 e si stringe o allarga con il modello WDL.")
    return 0


def cmd_scale(args):
    ms = models(args)
    o, n = ms['sf18'], ms['sf19']
    print("Scala interna: a(materiale) = valutazione interna che vale il 50 % di vittoria\n")
    print(f"{'mat':>4}{'a18':>9}{'a19':>9}{'a19/a18':>10}{'b18':>8}{'b19':>8}")
    for mat in (17, 20, 25, 30, 35, 40, 45, 50, 55, 58, 62, 66, 70, 74, 78):
        a0, b0 = o.ab(mat)
        a1, b1 = n.ab(mat)
        print(f"{mat:>4}{a0:>9.1f}{a1:>9.1f}{a1 / a0:>10.3f}{b0:>8.1f}{b1:>8.1f}")
    r = [n.ab(m)[0] / o.ab(m)[0] for m in range(17, 79)]
    print(f"\nrapporto a19/a18 fra {min(r):.3f} e {max(r):.3f}; a materiale 58: {n.ab(58)[0] / o.ab(58)[0]:.3f}")
    print("Ogni costante ShashChess in unita' interne tarata su SF18 (margini, soglie di eval, i punteggi")
    print("salvati nei .exp) vale su SF19 circa il {:.0f} % in piu'.".format(100 * (o.ab(58)[0] / n.ab(58)[0] - 1)))
    return 0


VALUE_CTX = re.compile(r'\b(eval|Eval|value|Value|score|Score|alpha|beta|bestValue|ttValue|expTTValue|'
                       r'margin|Margin|delta|futility|threshold|Threshold|razor|improvement|staticEval|'
                       r'correctionValue)\w*')
INT_LIT = re.compile(r'(?<![\w.])(\d{2,5})(?![\w.])')
MARK_RE = re.compile(r'//\s*\[SC:([a-z0-9-]+)\]\s*(begin|end)\b', re.I)


def cmd_constants(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from hook_audit import find_regions            # stesse regioni di hook_audit (+ GOLD_DIGGER)
    ratio = models(args)['sf19'].ab(58)[0] / models(args)['sf18'].ab(58)[0]
    n = 0
    for dp, dn, fn in os.walk(args.root):
        if 'json' in dp or 'incbin' in dp:
            continue
        for f in sorted(fn):
            if not f.endswith(('.cpp', '.h')):
                continue
            p = os.path.join(dp, f)
            lines = open(p, encoding='utf-8', errors='replace').read().split('\n')
            owner = {}
            for tag, a, b in find_regions(lines):
                for k in range(a + 1, b):
                    owner.setdefault(k, tag)
            for i, l in enumerate(lines):
                if i not in owner and not args.all:
                    continue
                code = l.split('//')[0]
                if not VALUE_CTX.search(code):
                    continue
                lits = [x for x in INT_LIT.findall(code) if 20 <= int(x) <= 30000]
                if lits:
                    n += 1
                    sug = ', '.join(f"{x}->{round(int(x) * ratio)}" for x in lits)
                    print(f"{os.path.relpath(p, args.root)}:{i + 1} [SC:{owner.get(i, '-')}]  {l.strip()[:90]}")
                    print(f"      se in unita' interne, x{ratio:.3f}: {sug}")
    print(f"\n{n} righe candidate. Il riscalamento e' un punto di partenza per l'SPRT, non un risultato:")
    print("conteggi, profondita', riduzioni (1024 = 1 ply), percentuali e punteggi di history NON si riscalano.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sf19', help="uci.cpp di SF19: file, radice dell'albero o riferimento git (es. vendor/sf_19)")
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('extract')
    e.add_argument('--sc', help='radice src/ di ShashChess')
    e.set_defaults(fn=cmd_extract)
    c = sub.add_parser('cpp')
    c.set_defaults(fn=cmd_cpp)
    t = sub.add_parser('trap')
    t.add_argument('--stale', default='sf18', choices=['sf18', 'sc41-pos'])
    t.add_argument('--range', type=int, default=1000)
    t.set_defaults(fn=cmd_trap)
    z = sub.add_parser('zones')
    z.add_argument('--models', default='sf18,sf19')
    z.add_argument('--rule', choices=['legacy', 'exact', 'exact-draw'], default='exact')
    z.add_argument('--draw-min', type=float, default=995.0)
    z.add_argument('--materials', default='20,40,58,70,78')
    z.add_argument('--window', type=int, default=120)
    z.add_argument('--range', type=int, default=600)
    z.set_defaults(fn=cmd_zones)
    k = sub.add_parser('capablanca')
    k.add_argument('--models', default='sf18,sf19')
    k.add_argument('--split-material', type=int, default=52)
    k.set_defaults(fn=cmd_capablanca)
    s = sub.add_parser('scale')
    s.set_defaults(fn=cmd_scale)
    n = sub.add_parser('constants')
    n.add_argument('--root', default='src')
    n.add_argument('--all', action='store_true', help='anche fuori dalle regioni marcate')
    n.set_defaults(fn=cmd_constants)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
