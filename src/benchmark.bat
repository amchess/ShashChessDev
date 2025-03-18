@echo off
setlocal enabledelayedexpansion

:: Specifica il motore da usare
set ENGINE=ShashChess38-x86-64-bmi2.exe

:: Specifica il numero di ripetizioni del benchmark
set NUM_REPEATS=5

:: Specifica il file di output
set OUTPUT_FILE=benchmark_log.txt

:: Specifica eventuali opzioni per il motore (esempio: impostazioni UCI)
set OPTIONS_FILE=uci_options.txt

:: Disattiva Internet (disabilita la scheda di rete)
echo Disattivazione Internet...
netsh interface set interface "Wi-Fi" admin=disable
netsh interface set interface "Ethernet" admin=disable

:: Imposta il piano di alimentazione su "Prestazioni elevate"
echo Attivazione modalità prestazioni elevate...
powercfg -setactive SCHEME_MIN

:: Disabilita la sospensione automatica
echo Disattivazione sospensione automatica...
powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0

:: Chiude programmi non necessari
echo Chiusura processi inutili...
taskkill /IM OneDrive.exe /F
taskkill /IM Skype.exe /F
taskkill /IM Teams.exe /F
taskkill /IM Discord.exe /F
taskkill /IM WhatsApp.exe /F

:: Cancella il file di output precedente, se esiste
if exist %OUTPUT_FILE% del %OUTPUT_FILE%

:: Scrive l'intestazione del log
echo Esecuzione benchmark ShashChess - %DATE% %TIME% > %OUTPUT_FILE%
echo ======================================== >> %OUTPUT_FILE%
echo Motore: %ENGINE% >> %OUTPUT_FILE%
echo Numero di ripetizioni: %NUM_REPEATS% >> %OUTPUT_FILE%
echo. >> %OUTPUT_FILE%

:: Imposta eventuali opzioni UCI se il file esiste
if exist %OPTIONS_FILE% (
    echo Caricamento impostazioni UCI da %OPTIONS_FILE%...
    for /f "tokens=*" %%o in (%OPTIONS_FILE%) do (
        echo setoption name %%o >> %OUTPUT_FILE%
        echo %%o | %ENGINE%
    )
)

:: Esegue il benchmark per NUM_REPEATS volte
for /L %%i in (1,1,%NUM_REPEATS%) do (
    echo Esecuzione %%i...
    echo Esecuzione %%i: >> %OUTPUT_FILE%

    :: Esegue il motore con il comando bench e cattura l'output
    %ENGINE% bench 1>> %OUTPUT_FILE% 2>>&1

    echo. >> %OUTPUT_FILE%
)

:: Analizza i risultati e calcola le medie
echo ======================================== >> %OUTPUT_FILE%
echo Media dei risultati su %NUM_REPEATS% esecuzioni: >> %OUTPUT_FILE%

set /a TOTAL_TIME=0
set /a TOTAL_NODES=0
set /a TOTAL_NPS=0

for /f "tokens=3" %%a in ('findstr "Total time (ms)" %OUTPUT_FILE%') do set /a TOTAL_TIME+=%%a
for /f "tokens=3" %%b in ('findstr "Nodes searched" %OUTPUT_FILE%') do set /a TOTAL_NODES+=%%b
for /f "tokens=3" %%c in ('findstr "Nodes/second" %OUTPUT_FILE%') do set /a TOTAL_NPS+=%%c

set /a AVG_TIME=%TOTAL_TIME% / %NUM_REPEATS%
set /a AVG_NODES=%TOTAL_NODES% / %NUM_REPEATS%
set /a AVG_NPS=%TOTAL_NPS% / %NUM_REPEATS%

echo Tempo medio: %AVG_TIME% ms >> %OUTPUT_FILE%
echo Nodi medi: %AVG_NODES% >> %OUTPUT_FILE%
echo Nodi al secondo medi: %AVG_NPS% >> %OUTPUT_FILE%
echo. >> %OUTPUT_FILE%

:: Ripristina impostazioni originali
echo Ripristino impostazioni originali...

:: Riattiva Internet
netsh interface set interface "Wi-Fi" admin=enable
netsh interface set interface "Ethernet" admin=enable

:: Ripristina il piano di alimentazione predefinito
powercfg -setactive SCHEME_BALANCED

:: Riattiva la sospensione automatica (30 min monitor, 45 min standby)
powercfg /change monitor-timeout-ac 30
powercfg /change standby-timeout-ac 45

echo Benchmark completato!
type %OUTPUT_FILE%

endlocal
