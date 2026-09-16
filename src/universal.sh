#!/usr/bin/env bash
# ==========================================================================
#  universal.sh  -  ShashChess 42  -  binario UNIVERSAL x86-64 (Linux / GCC)
#  Da lanciare DENTRO la cartella "src" del sorgente.
#
#  ARCH=x86-64-universal compila 8 slice
#  (x86-64, sse41-popcnt, avx2, bmi2, avxvnni, avx512, vnni512, avx512icl)
#  e le linka in un unico eseguibile che sceglie a runtime quella giusta.
# ==========================================================================

set -e

# --- Intel SDE: serve SOLO per il PGO -------------------------------------
# Profile-build esegue il bench di OGNI slice, comprese AVX-512/VNNI/ICL:
# senza emulatore su CPU vecchie si verifica "Illegal instruction" (SIGILL).
# Impostare SDE_DIR con il percorso di Intel SDE (es. /opt/sde), 
# oppure lasciare vuoto per build no-PGO.
SDE_DIR="${SDE_DIR:-/opt/sde}"

# Rilevamento automatico dei core CPU disponibili
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

TARGET="build"
RUN_PREFIX=""

# Cerca l'eseguibile sde nel percorso specificato o nel PATH di sistema
if [ -x "$SDE_DIR/sde" ]; then
    SDE_BIN="$SDE_DIR/sde"
elif command -v sde >/dev/null 2>&1; then
    SDE_BIN=$(command -v sde)
fi

if [ -n "$SDE_BIN" ]; then
    RUN_PREFIX="$SDE_BIN -future --"
    TARGET="profile-build"
    echo "[universal] Intel SDE trovato ($SDE_BIN): build CON PGO (profile-build)"
else
    echo "[universal] Intel SDE non trovato: build SENZA PGO (build)"
fi

make clean

echo "[universal] Avvio compilazione ARCH=x86-64-universal con -j$NPROC..."

if [ -n "$RUN_PREFIX" ]; then
    make "\(TARGET" ARCH=x86-64-universal COMP=gcc RUN_PREFIX="\)RUN_PREFIX" -j"$NPROC"
else
    make "\(TARGET" ARCH=x86-64-universal COMP=gcc -j"\)NPROC"
fi

if [ ! -f shashchess ]; then
    echo ""
    echo "*** BUILD UNIVERSAL FALLITA: eseguibile non generato ***"
    exit 1
fi

strip shashchess
mv shashchess ShashChess19-universal

# --- Verifica: stampa la slice selezionata sulla CPU corrente -------------
echo ""
echo "[universal] Test di esecuzione (compiler info):"
./ShashChess19-universal compiler

make clean

echo ""
echo "[universal] OK: ShashChess19-universal"
exit 0