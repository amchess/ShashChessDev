#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
match_report.py - analisi dei match sulle pepite (protocollo ShashChess).

Mette insieme i PGN delle ripetizioni e risponde a tre domande:

  1. Chi ha vinto e di quanto      punteggio, Elo con intervallo di confidenza
  2. Cosa questo campione PUO' dire potere risolutivo: la differenza minima di Elo
                                    che 900 partite riescono a distinguere dal caso.
                                    Serve a non leggere come regressione una
                                    fluttuazione, e a non chiamare "parita'" un
                                    campione che non avrebbe visto nemmeno 30 Elo.
  3. Se il match e' stato pulito    sconfitte per tempo scaduto, mosse illegali,
                                    crash, aggiudicazioni: un porting rotto si vede
                                    prima qui che nel punteggio.

Piu' la ripartizione per apertura (pepita) e per colore, e il confronto fra le
ripetizioni: se le tre ripetizioni divergono piu' di quanto il caso giustifichi,
la causa e' la macchina (oversubscription, learning attivo, thermal throttling),
non il motore.

Uso
---
  python tools/match_report.py porting/matches/vs41_run*.pgn
  python tools/match_report.py porting/matches/vs41_run*.pgn --engine sc42 --per-opening
  python tools/match_report.py porting/matches/*.pgn --csv porting/matches/riepilogo.csv

Nessuna dipendenza esterna: legge le sole intestazioni PGN.
"""

import argparse
import glob
import math
import os
import re
import sys
from collections import Counter, defaultdict

TAG_RE = re.compile(r'^\[(\w+)\s+"(.*)"\]\s*$')
FORFEIT_RE = re.compile(r'loses on time|forfeits|illegal|stall|crash|disconnect|terminat', re.I)


def read_games(paths):
    games, cur, in_tags = [], {}, False
    for p in paths:
        with open(p, encoding='utf-8', errors='replace') as fh:
            for line in fh:
                m = TAG_RE.match(line)
                if m:
                    if not in_tags and cur:
                        games.append(cur)
                        cur = {}
                    in_tags = True
                    cur[m.group(1)] = m.group(2)
                    cur.setdefault('_file', os.path.basename(p))
                elif line.strip():
                    in_tags = False
    if cur:
        games.append(cur)
    return [g for g in games if 'Result' in g and 'White' in g]


def elo(score):
    if score <= 0:
        return float('-inf')
    if score >= 1:
        return float('inf')
    return -400 * math.log10(1 / score - 1)


def stats(w, d, l):
    """(punteggio, elo, meta'-ampiezza dell'intervallo al 95 %, errore standard in Elo)."""
    n = w + d + l
    if n == 0:
        return 0, 0, 0, 0
    score = (w + 0.5 * d) / n
    # varianza del punteggio per partita (stima campionaria, non normale-approssimata a 1/4)
    var = (w * (1 - score) ** 2 + d * (0.5 - score) ** 2 + l * score ** 2) / n
    se = math.sqrt(var / n) if var > 0 else 0
    lo, hi = max(1e-9, score - 1.96 * se), min(1 - 1e-9, score + 1.96 * se)
    e = elo(score)
    half = (elo(hi) - elo(lo)) / 2 if 0 < lo < hi < 1 else float('inf')
    se_elo = half / 1.96 if half != float('inf') else float('inf')
    return score, e, half, se_elo


def pair_stats(pairs):
    """Analisi a coppie (pentanomiale): ogni pepita giocata nei due colori e' UNA prova.
    Con aperture sbilanciate come le pepite, l'effetto dell'apertura si annulla nella
    coppia, quindi l'intervallo e' piu' stretto di quello calcolato partita per partita.
    Restituisce (n_coppie, punteggio_per_partita, elo, meta'_intervallo, distribuzione)."""
    vals = [v for v in pairs if v is not None]
    N = len(vals)
    if N < 2:
        return 0, 0, 0, float('inf'), Counter()
    m = sum(vals) / N
    var = sum((v - m) ** 2 for v in vals) / (N - 1)
    se = math.sqrt(var / N)
    score = m / 2
    lo, hi = max(1e-9, score - 1.96 * se / 2), min(1 - 1e-9, score + 1.96 * se / 2)
    e = elo(score)
    half = (elo(hi) - elo(lo)) / 2 if 0 < lo < hi < 1 else float('inf')
    return N, score, e, half, Counter(vals)


def resolving_power(n, draw_rate):
    """(differenza minima rilevabile, meta' ampiezza dell'intervallo) in Elo, per n partite."""
    se = math.sqrt((1 - draw_rate) / 4 / n)     # errore standard del punteggio a ~0,5
    mde = elo(0.5 + (1.96 + 0.84) * se)          # significativita' 95 % + potenza 80 %
    return mde, elo(0.5 + 1.96 * se)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pgn', nargs='+')
    ap.add_argument('--engine', help='nome del motore candidato (default: il primo incontrato)')
    ap.add_argument('--per-opening', action='store_true')
    ap.add_argument('--csv')
    ap.add_argument('--min-elo', type=float, help='exit 1 se il limite inferiore scende sotto questo valore')
    args = ap.parse_args()

    paths = [p for pat in args.pgn for p in sorted(glob.glob(pat))] or args.pgn
    games = read_games(paths)
    if not games:
        sys.exit("nessuna partita trovata nei PGN indicati")

    names = Counter()
    for g in games:
        names[g['White']] += 1
        names[g.get('Black', '?')] += 1
    if len(names) < 2:
        sys.exit(f"trovato un solo motore ({list(names)}): serve un match fra due")
    cand = args.engine or names.most_common()[0][0]
    other = next(n for n in names if n != cand)

    by_file, by_open, by_color = defaultdict(Counter), defaultdict(Counter), defaultdict(Counter)
    pair_score = defaultdict(list)
    anomalies, term = [], Counter()
    tot = Counter()
    for g in games:
        res = g['Result']
        if res not in ('1-0', '0-1', '1/2-1/2'):
            term['*(non terminata)'] += 1
            continue
        white = g['White'] == cand
        if res == '1/2-1/2':
            o = 'd'
        elif (res == '1-0') == white:
            o = 'w'
        else:
            o = 'l'
        tot[o] += 1
        by_file[g['_file']][o] += 1
        by_color['bianco' if white else 'nero'][o] += 1
        key = g.get('Opening') or g.get('FEN') or g.get('Round', '?')
        by_open[key][o] += 1
        rnd = str(g.get('Round', '')).split('.')[0]
        pair_score[(g['_file'], rnd or key)].append({'w': 1.0, 'd': 0.5, 'l': 0.0}[o])
        t = g.get('Termination', '')
        if t:
            term[t] += 1
        blob = ' '.join(g.get(k, '') for k in ('Termination', 'Annotator', 'Comment'))
        if FORFEIT_RE.search(blob):
            who = cand if cand in blob else (other if other in blob else '?')
            anomalies.append((g.get('Round', '?'), g['White'], g.get('Black', '?'), t, who))

    w, d, l = tot['w'], tot['d'], tot['l']
    n = w + d + l
    score, e, half, se = stats(w, d, l)
    draw_rate = d / n if n else 0
    mde, ci95 = resolving_power(n, draw_rate)

    print(f"File           : {len(paths)} PGN, {len(games)} partite lette")
    print(f"Candidato      : {cand}    avversario: {other}\n")
    print(f"  {w} vittorie / {d} patte / {l} sconfitte su {n} partite")
    print(f"  punteggio    : {100 * score:.2f} %   patte: {100 * draw_rate:.1f} %")
    if abs(e) != float('inf'):
        print(f"  Elo          : {e:+.1f}  ±{half:.1f}  (95 %: da {e - half:+.1f} a {e + half:+.1f})")
    else:
        print("  Elo          : non calcolabile (punteggio 0 % o 100 %)")
    print(f"\n  Potere risolutivo di {n} partite con questo tasso di patte:")
    print(f"    differenza minima rilevabile : {mde:.1f} Elo (95 %, potenza 80 %)")
    print(f"    intervallo atteso a parita'  : ±{ci95:.1f} Elo")
    print(f"    Quindi questo match dimostra l'assenza di regressioni maggiori di ~{mde:.0f} Elo,")
    print(f"    non la parita' entro pochi Elo. Per le differenze fini servono la suite EPD")
    print(f"    (confronto appaiato, epd_suite.py) e i cancelli di zona.")

    pairs = [sum(v) for v in pair_score.values() if len(v) == 2]
    odd = sum(1 for v in pair_score.values() if len(v) != 2)
    if len(pairs) >= 10:
        N, pscore, pe, phalf, dist = pair_stats(pairs)
        print(f"\n  Analisi a coppie (pentanomiale) su {N} coppie colore-invertito:")
        print(f"    punteggio    : {100 * pscore:.2f} %")
        if abs(pe) != float('inf'):
            print(f"    Elo          : {pe:+.1f}  ±{phalf:.1f}  (95 %: da {pe - phalf:+.1f} a {pe + phalf:+.1f})")
        print("    distribuzione: " + '  '.join(
            f"{k}={dist.get(k, 0)}" for k in (0.0, 0.5, 1.0, 1.5, 2.0)))
        if phalf < half:
            print(f"    L'intervallo si stringe da ±{half:.1f} a ±{phalf:.1f} Elo: e' il guadagno")
            print("    dell'accoppiamento per colore su aperture sbilanciate. E' questo il numero")
            print("    da citare, non quello calcolato partita per partita.")
        else:
            print(f"    Qui l'accoppiamento non stringe l'intervallo (±{phalf:.1f} contro ±{half:.1f}):")
            print("    le pepite usate si comportano come aperture equilibrate.")
        if odd:
            print(f"    {odd} round senza coppia completa: servono -games 2 e -repeat.")

    print("\n  Per colore:")
    for c in ('bianco', 'nero'):
        t = by_color[c]
        nn = sum(t.values())
        if nn:
            s, ee, hh, _ = stats(t['w'], t['d'], t['l'])
            print(f"    {c:<7} {t['w']}/{t['d']}/{t['l']}  punteggio {100 * s:.1f} %"
                  + (f"  Elo {ee:+.0f}" if abs(ee) != float('inf') else ''))
    sq = sum(by_color['bianco'].values()), sum(by_color['nero'].values())
    if min(sq) and abs(sq[0] - sq[1]) > 0.02 * n:
        print(f"    ATTENZIONE: colori sbilanciati ({sq[0]} vs {sq[1]}): controlla -repeat e -games 2.")

    if len(by_file) > 1:
        print("\n  Per ripetizione:")
        for f in sorted(by_file):
            t = by_file[f]
            nn = sum(t.values())
            s, ee, hh, _ = stats(t['w'], t['d'], t['l'])
            print(f"    {f:<32} {t['w']}/{t['d']}/{t['l']}  {100 * s:.1f} %"
                  + (f"  Elo {ee:+.0f} ±{hh:.0f}" if abs(ee) != float('inf') else ''))
        sc = [stats(t['w'], t['d'], t['l'])[0] for t in by_file.values()]
        spread = max(sc) - min(sc)
        expected = 2 * 1.96 * math.sqrt((1 - draw_rate) / 4 / (n / len(by_file)))
        print(f"    dispersione fra ripetizioni: {100 * spread:.1f} punti percentuali "
              f"(attesa dal caso: fino a {100 * expected:.1f})")
        if spread > expected:
            print("    Oltre l'atteso: cerca la causa nella macchina (oversubscription, learning")
            print("    attivo, throttling) prima che nel motore.")

    if anomalies:
        per_engine = Counter(a[4] for a in anomalies)
        print(f"\n  ANOMALIE: {len(anomalies)} partite con tempo scaduto / mossa illegale / crash")
        for r, wh, bl, t, who in anomalies[:10]:
            print(f"    round {r}: {wh} vs {bl} - {t}")
        if len(anomalies) > 10:
            print(f"    ... altre {len(anomalies) - 10}")
        print("    per motore: " + ', '.join(f"{k}={v}" for k, v in per_engine.most_common()))
        if per_engine.get(cand):
            print(f"    {per_engine[cand]} a carico del candidato: vanno spiegate una per una prima di")
            print("    accettare il risultato. Se riguardano entrambi i motori in proporzione simile,")
            print("    la causa e' quasi sempre la macchina sovraccarica (concorrenza x thread > core).")
    if term:
        print("\n  Terminazioni:", ', '.join(f"{k}={v}" for k, v in term.most_common(6)))

    if args.per_opening:
        print("\n  Aperture piu' decisive (pepite che hanno prodotto risultati):")
        rows = sorted(by_open.items(), key=lambda kv: -(kv[1]['w'] + kv[1]['l']))
        for k, t in rows[:15]:
            print(f"    {str(k)[:56]:<56} {t['w']}/{t['d']}/{t['l']}")
        dead = [k for k, t in by_open.items() if t['w'] + t['l'] == 0 and sum(t.values()) >= 2]
        if dead:
            print(f"    {len(dead)} pepite hanno prodotto solo patte: candidate alla sostituzione")
            print("    (l'idea della pepita e' proprio di evitare le posizioni drawish).")

    if args.csv:
        with open(args.csv, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('gruppo,vittorie,patte,sconfitte,punteggio,elo,meta_intervallo\n')
            fh.write(f'totale,{w},{d},{l},{score:.4f},{e:.1f},{half:.1f}\n')
            for f in sorted(by_file):
                t = by_file[f]
                s, ee, hh, _ = stats(t['w'], t['d'], t['l'])
                fh.write(f'{f},{t["w"]},{t["d"]},{t["l"]},{s:.4f},{ee:.1f},{hh:.1f}\n')
        print(f"\n  Riepilogo in {args.csv}")

    if args.min_elo is not None and abs(e) != float('inf') and e - half < args.min_elo:
        print(f"\n  ESITO: limite inferiore {e - half:+.1f} sotto la soglia {args.min_elo:+.1f}")
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
