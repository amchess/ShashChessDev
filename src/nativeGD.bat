@echo off
REM x64 builds begin (GoldDigger Edition)
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

REM Compilazione con flag GOLD_DIGGER attivato
mingw32-make profile-build ARCH=native COMP=mingw EXTRACXXFLAGS="-DGOLD_DIGGER" -j %Number_Of_Processors%

strip shashchess.exe
REM Rinominiamo l'eseguibile per distinguerlo da quello normale
ren shashchess.exe ShashChess40-native-GoldDigger.exe

mingw32-make clean
pause