@echo off
REM Impostazione dei percorsi per i tool di compilazione
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

Title "ShashChess x86-64-bmi2 GoldDigger Build"

REM Pulizia totale prima di iniziare
mingw32-make clean

REM Utilizziamo ENV_CXXFLAGS per assicurarci che il flag -DGOLD_DIGGER 
REM non venga sovrascritto durante i 4 step della profile-build.
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw ENV_CXXFLAGS="-DGOLD_DIGGER" -j %Number_Of_Processors%

REM Controllo se il file è stato generato
if not exist shashchess.exe (
    echo Errore: Compilazione fallita!
    pause
    exit /b
)

REM Ottimizzazione dell'eseguibile (rimozione simboli di debug)
strip shashchess.exe

REM Rinominiamo l'eseguibile con il nome identificativo corretto
ren shashchess.exe "ShashChess40-x86-64-bmi2-GoldDigger.exe"

REM Pulizia finale degli oggetti temporanei
mingw32-make clean

echo.
echo Compilazione completata con successo!
echo Eseguibile creato: ShashChess40-x86-64-bmi2-GoldDigger.exe
pause