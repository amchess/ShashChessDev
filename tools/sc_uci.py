#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sc_uci.py - driver UCI minimale condiviso dagli strumenti del porting.

Nessuna dipendenza esterna. Legge l'output del motore in un thread, cosi' un
motore che si blocca produce un timeout con le ultime righe ricevute, e un
motore che muore (es. 'info string CRITICAL ERROR' + exit(1) di SF19 su una FEN
invalida) produce un errore leggibile invece di un blocco.
"""

import os
import queue
import re
import subprocess
import threading
from collections import deque

MATERIAL_VALUE = {'p': 1, 'n': 3, 'b': 3, 'r': 5, 'q': 9}


class EngineDied(RuntimeError):
    pass


class Engine:
    def __init__(self, path, options=None, hash_mb=16, threads=1, name=None, timeout=300):
        self.path = path
        self.name = name or os.path.basename(path)
        self.timeout = timeout
        self.tail = deque(maxlen=12)
        self.proc = subprocess.Popen(
            [os.path.abspath(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, encoding='utf-8', errors='replace',
            cwd=os.path.dirname(os.path.abspath(path)) or '.')
        self.q = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.send('uci')
        self.uci_lines = self.wait_for('uciok', collect=True)
        self.options = parse_options(self.uci_lines)
        self.id_name = next((l[8:].strip() for l in self.uci_lines if l.startswith('id name ')), '?')
        for k, v in (options or {}).items():
            self.set_option(k, v)
        if 'Threads' in self.options:
            self.set_option('Threads', threads)
        if 'Hash' in self.options:
            self.set_option('Hash', hash_mb)
        self.isready()

    def _read(self):
        for line in self.proc.stdout:
            line = line.rstrip('\r\n')
            self.tail.append(line)
            self.q.put(line)
        self.q.put(None)

    def send(self, cmd):
        try:
            self.proc.stdin.write(cmd + '\n')
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            raise EngineDied(self._diag(f"pipe chiusa inviando '{cmd}'"))

    HINTS = [('Network evaluation parameters',
              "rete NNUE mancante: 'make net' nella cartella src, oppure setoption name EvalFile "
              "value <percorso completo>"),
             ('CRITICAL ERROR',
              "FEN rifiutata: da SF19 il motore termina il processo. Valida le posizioni "
              "(make_epd_corpus.py valida gia' ogni FEN)"),
             ('Unknown command', "comando non riconosciuto da questo motore")]

    def _diag(self, what):
        tail = list(self.tail)
        hints = [h for frag, h in self.HINTS if any(frag in l for l in tail)]
        msg = f"{self.name}: {what}."
        for h in hints:
            msg += f"\n  -> {h}"
        return msg + "\n  Ultime righe:\n  " + '\n  '.join(tail)

    def wait_for(self, token, collect=False, timeout=None):
        out = []
        while True:
            try:
                line = self.q.get(timeout=timeout or self.timeout)
            except queue.Empty:
                raise RuntimeError(self._diag(f"timeout in attesa di '{token}'"))
            if line is None:
                raise EngineDied(self._diag("il motore e' terminato"))
            if collect:
                out.append(line)
            if line == token or line.startswith(token):
                return out

    def isready(self):
        self.send('isready')
        self.wait_for('readyok')

    def set_option(self, name, value):
        if self.options and name not in self.options:
            return False
        self.send(f'setoption name {name} value {value}')
        return True

    def command(self, cmd):
        """Invia un comando e raccoglie tutto l'output fino al readyok successivo."""
        self.send(cmd)
        self.send('isready')
        return self.wait_for('readyok', collect=True)[:-1]

    def position(self, fen, moves=()):
        cmd = 'position startpos' if fen in ('startpos', None) else f'position fen {fen}'
        if moves:
            cmd += ' moves ' + ' '.join(moves)
        self.send(cmd)

    def quit(self):
        try:
            self.send('quit')
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


OPT_RE = re.compile(r'^option name (?P<name>.+?) type (?P<type>\S+)(?P<rest>.*)$')


def parse_options(lines):
    """{nome: {type, default, min, max, var[]}} dall'output di 'uci'."""
    opts = {}
    for l in lines:
        m = OPT_RE.match(l)
        if not m:
            continue
        rest = m.group('rest')
        d = {'type': m.group('type')}
        for key in ('default', 'min', 'max'):
            mm = re.search(rf'\b{key} (.*?)(?= (?:default|min|max|var) |$)', rest)
            if mm:
                d[key] = mm.group(1).strip()
        d['var'] = re.findall(r'\bvar (.*?)(?= var |$)', rest)
        opts[m.group('name')] = d
    return opts


def material_of(fen):
    return sum(MATERIAL_VALUE.get(ch.lower(), 0) for ch in fen.split()[0] if ch.isalpha())


def read_epd(path):
    """(fen, riga_originale) da un file EPD/FEN; tronca gli opcode EPD."""
    out = []
    with open(path, encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            p = line.split()
            fen = ' '.join(p[:6]) if len(p) >= 6 and p[4].isdigit() and p[5].isdigit() \
                else ' '.join(p[:4]) + ' 0 1'
            out.append((fen, line))
    return out


# --------------------------------------------------------------------- zone
ZONE_ORDER = ['HIGH_PETROSIAN', 'MIDDLE_HIGH_PETROSIAN', 'MIDDLE_PETROSIAN', 'MIDDLE_LOW_PETROSIAN',
              'LOW_PETROSIAN', 'CAPABLANCA_PETROSIAN', 'CAPABLANCA', 'CAPABLANCA_TAL', 'LOW_TAL',
              'MIDDLE_LOW_TAL', 'MIDDLE_TAL', 'MIDDLE_HIGH_TAL', 'HIGH_TAL', 'TAL_CAPABLANCA_PETROSIAN']
ZONE_SHORT = dict(zip(ZONE_ORDER, ['HP', 'MHP', 'MP', 'MLP', 'LP', 'CP', 'C', 'CT', 'LT', 'MLT', 'MT', 'MHT',
                                   'HT', 'CHAOS']))
THRESHOLDS = [(5, 'HIGH_PETROSIAN'), (10, 'MIDDLE_HIGH_PETROSIAN'), (15, 'MIDDLE_PETROSIAN'),
              (20, 'MIDDLE_LOW_PETROSIAN'), (24, 'LOW_PETROSIAN'), (49, 'CAPABLANCA_PETROSIAN'),
              (50, 'CAPABLANCA'), (75, 'CAPABLANCA_TAL'), (79, 'LOW_TAL'), (84, 'MIDDLE_LOW_TAL'),
              (89, 'MIDDLE_TAL'), (94, 'MIDDLE_HIGH_TAL'), (100, 'HIGH_TAL')]
# stringhe stampate da get_shashin_zone() in evaluate.cpp -> enum (l'ordine conta)
TRACE_ALIASES = [
    ('tal-capablanca-petrosian', 'TAL_CAPABLANCA_PETROSIAN'), ('high-middle petrosian', 'MIDDLE_HIGH_PETROSIAN'),
    ('middle-low petrosian', 'MIDDLE_LOW_PETROSIAN'), ('high petrosian', 'HIGH_PETROSIAN'),
    ('middle petrosian', 'MIDDLE_PETROSIAN'), ('low petrosian', 'LOW_PETROSIAN'),
    ('capablanca-petrosian', 'CAPABLANCA_PETROSIAN'), ('capablanca-tal', 'CAPABLANCA_TAL'),
    ('high-middle tal', 'MIDDLE_HIGH_TAL'), ('middle-low tal', 'MIDDLE_LOW_TAL'), ('high tal', 'HIGH_TAL'),
    ('middle tal', 'MIDDLE_TAL'), ('low tal', 'LOW_TAL'), ('capablanca', 'CAPABLANCA')]


def canon_zone(s):
    if not s:
        return ''
    up = s.strip().upper().replace(' ', '_').replace('-', '_')
    if up in ZONE_ORDER:
        return up
    if up in ZONE_SHORT.values():
        return {v: k for k, v in ZONE_SHORT.items()}[up]
    low = s.lower()
    for frag, name in TRACE_ALIASES:
        if frag in low:
            return name
    return s.strip()


def zone_of_probability(p):
    for t, name in THRESHOLDS:
        if p <= t:
            return name
    return 'HIGH_TAL'


def zone_legacy_from_wdl(W, D, L):
    """Regola di ShashinManager::getShashinRange() in ShashChess 41 (percentuali arrotondate)."""
    wp = W + D // 2
    if wp == 50:
        return 'CAPABLANCA' if D == 100 else 'TAL_CAPABLANCA_PETROSIAN'
    return zone_of_probability(wp)
