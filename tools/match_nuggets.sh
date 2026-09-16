#!/usr/bin/env bash
# match_nuggets.sh - match sulle pepite, metodologia ShashChess (non fishtest).
#
# 300 partite da 100 pepite a 25+10, ripetute 3 volte: e' il protocollo descritto in
#   https://github.com/amchess/ShashChess/wiki/TestingStrategy
# Ricalca doc/general/TestingStrategy/run_match_cli.sh, parametrizzandolo e aggiungendo
# i controlli che servono durante un porting.
#
# Uso
#   tools/match_nuggets.sh <nome> <motore_nuovo> <motore_base> [nome_nuovo] [nome_base]
#
# Variabili (con i default)
#   NUGGETS=tests/TestSuiteCenterType/Test2025.pgn   aperture (pepite) in PGN
#   TC=0/25:00+10        tempo di riflessione (cutechess: mm:ss per lato + incremento)
#   ROUNDS=150           round; con GAMES=2 e -repeat -> 300 partite
#   GAMES=2              partite per round (una per colore, stessa apertura)
#   REPEATS=3            ripetizioni del match
#   THREADS=4  HASH=2048
#   CONCURRENCY=         se non impostata: (core fisici) / THREADS, senza oversubscription
#   SYZYGY=              percorso delle tavole Syzygy
#   PLIES=60             semimosse dell'apertura da riprodurre
#   OPTS_NEW / OPTS_BASE opzioni extra, es. OPTS_NEW="option.Variety=Off"
#   NEUTRAL=1            spegne learning, experience book, MCTS, Variety su ENTRAMBI
#   OUT=porting/matches  cartella dei PGN e dei log
#   CLI=cutechess-cli    percorso del binario
#
# Dopo:  python tools/match_report.py porting/matches/<nome>_run*.pgn
set -euo pipefail
[[ $# -lt 3 ]] && { sed -n '2,28p' "$0"; exit 1; }

NAME=$1; NEW=$2; BASE=$3; NEW_NAME=${4:-new}; BASE_NAME=${5:-base}
NUGGETS=${NUGGETS:-tests/TestSuiteCenterType/Test2025.pgn}
TC=${TC:-0/25:00+10}; ROUNDS=${ROUNDS:-150}; GAMES=${GAMES:-2}; REPEATS=${REPEATS:-3}
THREADS=${THREADS:-4}; HASH=${HASH:-2048}; PLIES=${PLIES:-60}
OUT=${OUT:-porting/matches}; CLI=${CLI:-cutechess-cli}

command -v "$CLI" >/dev/null || { echo "cutechess-cli non trovato (variabile CLI)"; exit 1; }
for e in "$NEW" "$BASE"; do [[ -x "$e" ]] || { echo "motore non eseguibile: $e"; exit 1; }; done
[[ -f "$NUGGETS" ]] || { echo "file di aperture assente: $NUGGETS"; exit 1; }

# --- pepite disponibili e bilanciamento dei colori -------------------------------
NUG=$(grep -c '^\[Event' "$NUGGETS" || true)
TOTAL=$((ROUNDS * GAMES))
echo "Pepite nel file: $NUG    partite per ripetizione: $TOTAL (${ROUNDS}x${GAMES})"
if (( NUG > 0 && ROUNDS > NUG )); then
  echo "  ATTENZIONE: $ROUNDS round su $NUG pepite: cutechess riavvolge il file, quindi le prime"
  echo "  $((ROUNDS - NUG)) pepite vengono giocate due volte e le altre una. Per un campione bilanciato"
  echo "  usa ROUNDS=$NUG (=$((NUG * GAMES)) partite) oppure un file con $ROUNDS pepite."
fi

# --- concorrenza: mai piu' thread dei core fisici, o il tempo perde significato ---
CORES=$( (lscpu -p=Core,Socket 2>/dev/null | grep -v '^#' | sort -u | wc -l) || nproc )
[[ "$CORES" -gt 0 ]] || CORES=$(nproc)
CONCURRENCY=${CONCURRENCY:-$(( CORES / THREADS ))}
(( CONCURRENCY < 1 )) && CONCURRENCY=1
if (( CONCURRENCY * THREADS > CORES )); then
  echo "  ATTENZIONE: ${CONCURRENCY}x${THREADS} = $((CONCURRENCY * THREADS)) thread su $CORES core fisici."
  echo "  A 25+10 l'oversubscription falsa il tempo: abbassa CONCURRENCY o THREADS."
fi
echo "Core fisici: $CORES    concorrenza: $CONCURRENCY x $THREADS thread"

EACH=( -each "tc=$TC" "option.Threads=$THREADS" "option.Hash=$HASH" )
[[ -n "${SYZYGY:-}" ]] && EACH+=( "option.SyzygyPath=$SYZYGY" )
if [[ "${NEUTRAL:-0}" == "1" ]]; then
  EACH+=( "option.Persisted learning=Off" "option.Experience Book=false"
          "option.MCTS by Shashin=false" "option.Variety=Off" )
  echo "Modalita' neutrale: learning, experience book, MCTS e Variety spenti su entrambi."
fi

mkdir -p "$OUT"
for ((r = 1; r <= REPEATS; r++)); do
  PGN="$OUT/${NAME}_run${r}.pgn"
  LOG="$OUT/${NAME}_run${r}.log"
  echo "===== $NAME - ripetizione $r/$REPEATS  ->  $PGN"
  # shellcheck disable=SC2086
  "$CLI" \
    -engine name="$NEW_NAME" cmd="$NEW" proto=uci ${OPTS_NEW:-} \
    -engine name="$BASE_NAME" cmd="$BASE" proto=uci ${OPTS_BASE:-} \
    "${EACH[@]}" \
    -variant standard \
    -openings file="$NUGGETS" format=pgn order=sequential plies="$PLIES" \
    -draw movenumber=40 movecount=8 score=5 \
    -resign movecount=5 score=600 \
    -maxmoves 300 \
    ${SYZYGY:+-tb "$SYZYGY"} \
    -rounds "$ROUNDS" -games "$GAMES" -repeat -recover -wait 0 \
    -concurrency "$CONCURRENCY" \
    -ratinginterval 20 -outcomeinterval 1 \
    -pgnout "$PGN" fi 2>&1 | tee "$LOG"
done

echo
echo "Fatto. Analisi complessiva delle $((TOTAL * REPEATS)) partite:"
echo "  python tools/match_report.py $OUT/${NAME}_run*.pgn"
