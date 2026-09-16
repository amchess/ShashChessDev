@echo off
setlocal enableextensions enabledelayedexpansion

REM ==========================================================================
REM  universal.bat  -  ShashChess 42  -  binario UNIVERSAL x86-64 (MinGW/MSYS2)
REM  Da lanciare DENTRO la cartella "src" del sorgente.
REM
REM  ARCH=x86-64-universal compila 8 slice
REM  (x86-64, sse41-popcnt, avx2, bmi2, avxvnni, avx512, vnni512, avx512icl)
REM  e le linka in un unico .exe che sceglie a runtime quella giusta.
REM ==========================================================================

REM --- toolchain: identica al batch nativo ----------------------------------
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

REM --- Intel SDE: serve SOLO per il PGO -------------------------------------
REM  profile-build esegue il bench di OGNI slice, comprese AVX-512/VNNI/ICL:
REM  senza emulatore la CPU si ferma con "illegal instruction".
REM  Percorso SENZA spazi. Lasciare vuoto ( SET "SDE_DIR=" ) per build no-PGO.
SET "SDE_DIR=C:\tools\sde"

REM --- NON passare ENV_CXXFLAGS sulla riga di comando di make ---------------
REM  Sulla riga di comando sovrascriverebbe i flag per-slice
REM  (-DShashChess=..., -DUNIVERSAL_BINARY, --defsym=main=..., --embed-dir=...)
REM  iniettati dal driver universal nel sub-make => link rotto.
REM  Se servono flag extra: SET "CXXFLAGS=-Dxxx" come variabile d'ambiente.

set "TARGET=build"
set "RUN_PREFIX="
if exist "%SDE_DIR%\sde.exe" (
    set "SDE_FWD=%SDE_DIR:\=/%"
    set "RUN_PREFIX=!SDE_FWD!/sde.exe -future --"
    set "TARGET=profile-build"
    echo [universal] Intel SDE trovato: build CON PGO ^(profile-build^)
) else (
    echo [universal] Intel SDE assente in "%SDE_DIR%": build SENZA PGO ^(build^)
)

mingw32-make clean

mingw32-make %TARGET% ARCH=x86-64-universal COMP=mingw -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 goto :fallita
if not exist shashchess.exe goto :fallita

strip shashchess.exe
ren shashchess.exe ShashChess19-universal.exe

REM --- verifica: stampa la slice selezionata sulla CPU corrente -------------
ShashChess19-universal.exe compiler

mingw32-make clean
echo.
echo [universal] OK: ShashChess19-universal.exe
pause
exit /b 0

:fallita
echo.
echo *** BUILD UNIVERSAL FALLITA ***
pause
exit /b 1