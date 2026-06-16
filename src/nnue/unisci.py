#!/usr/bin/env python3
"""
merge_sources.py
----------------
Unisce tutti i file .cpp e .h trovati nella directory corrente
(e in tutte le sottodirectory) in un unico file di testo.

Uso:
    python merge_sources.py                  # output: merged_sources.txt
    python merge_sources.py -o output.txt    # output personalizzato
    python merge_sources.py -d /altro/path   # directory di partenza diversa
"""

import os
import sys
import argparse
from datetime import datetime

EXTENSIONS = (".cpp", ".h")
DEFAULT_OUTPUT = "merged_sources.txt"


def collect_files(root: str) -> list[str]:
    """Raccoglie tutti i file .cpp e .h in ordine alfabetico per percorso."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        # Ordine deterministico nelle sottocartelle
        dirnames.sort()
        for fname in sorted(filenames):
            if fname.lower().endswith(EXTENSIONS):
                found.append(os.path.join(dirpath, fname))
    return found


def merge(files: list[str], output_path: str, root: str) -> None:
    """Scrive il file unificato con intestazioni per ogni sorgente."""
    with open(output_path, "w", encoding="utf-8") as out:
        # Intestazione globale
        out.write("=" * 78 + "\n")
        out.write(f"  File generato da merge_sources.py\n")
        out.write(f"  Data    : {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        out.write(f"  Root    : {os.path.abspath(root)}\n")
        out.write(f"  Sorgenti: {len(files)} file\n")
        out.write("=" * 78 + "\n\n")

        for i, filepath in enumerate(files, 1):
            rel = os.path.relpath(filepath, root)
            separator = "=" * 78
            out.write(f"{separator}\n")
            out.write(f"  [{i}/{len(files)}]  {rel}\n")
            out.write(f"{separator}\n\n")

            try:
                with open(filepath, "r", encoding="utf-8", errors="replace") as src:
                    content = src.read()
                out.write(content)
                # Garantisce una riga vuota di separazione
                if content and not content.endswith("\n"):
                    out.write("\n")
                out.write("\n")
            except OSError as e:
                out.write(f"[ERRORE lettura: {e}]\n\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Unisce tutti i file .cpp e .h in un unico file di testo."
    )
    parser.add_argument(
        "-d", "--directory",
        default=".",
        help="Directory di partenza (default: directory corrente)"
    )
    parser.add_argument(
        "-o", "--output",
        default=DEFAULT_OUTPUT,
        help=f"Nome del file di output (default: {DEFAULT_OUTPUT})"
    )
    args = parser.parse_args()

    root = os.path.abspath(args.directory)
    output_path = os.path.abspath(args.output)

    if not os.path.isdir(root):
        print(f"Errore: '{root}' non è una directory valida.", file=sys.stderr)
        sys.exit(1)

    print(f"Scansione di: {root}")
    files = collect_files(root)

    if not files:
        print("Nessun file .cpp o .h trovato.")
        sys.exit(0)

    # Evita di includere sé stesso nell'output (caso estremo)
    files = [f for f in files if os.path.abspath(f) != output_path]

    print(f"Trovati {len(files)} file. Scrittura in: {output_path}")
    merge(files, output_path, root)
    print("Fatto.")


if __name__ == "__main__":
    main()