@echo off
REM x64 builds begin (GoldDigger Edition)
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

echo Inizio compilazione ShashChess GoldDigger Edition (Native)...

REM Pulizia preventiva
mingw32-make clean

REM Usiamo ENV_CXXFLAGS per forzare la direttiva attraverso i target PGO del Makefile
mingw32-make profile-build ARCH=native COMP=mingw ENV_CXXFLAGS="-DGOLD_DIGGER" -j %Number_Of_Processors%

if not exist shashchess.exe (
    echo Errore: Compilazione fallita!
    pause
    exit /b
)

strip shashchess.exe
REM Rinominiamo l'eseguibile per distinguerlo da quello normale
ren shashchess.exe ShashChess40-native-GoldDigger.exe

mingw32-make clean
echo.
echo Compilazione completata con successo!
pause