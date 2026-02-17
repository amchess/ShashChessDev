#!/bin/bash

# Rendi lo script resiliente agli errori
set -e

# Numero di core disponibili per velocizzare la compilazione
CORES=$(nproc)

echo "Inizio compilazione ShashChess GoldDigger Edition su Linux..."

# Compilazione con flag GOLD_DIGGER attivato
# Usiamo ARCH=x86-64-bmi2 o ARCH=native a seconda delle tue preferenze
make profile-build ARCH=native COMP=gcc EXTRACXXFLAGS="-DGOLD_DIGGER" -j $CORES

# Pulizia dei simboli per ridurre la dimensione del file
strip shashchess

# Rinominiamo l'eseguibile per distinguerlo
mv shashchess ShashChess40-linux-GoldDigger

# Pulizia dei file oggetto intermedi
make clean

echo "Compilazione completata: ShashChess40-linux-GoldDigger"