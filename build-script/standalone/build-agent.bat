@echo off
setlocal ENABLEDELAYEDEXPANSION
chcp 65001 >nul
title Build OneGov Password Sync Agent (Core + Service)

:: =============================================================================
::  ONEGOV PASSWORD SYNC AGENT - BUILD SCRIPT
::
::  Purpose:
::    Automates the complete build pipeline for the OneGov Password Sync Agent.
::
::  Components:
::    • Native C core DLL (LSA Notification Package)
::    • Managed C# Service Executable (Sync Listener / Relay)
::
::  Key Features:
::    - Cleans previous build outputs
::    - Loads MSVC build environment for cl.exe
::    - Compiles native DLL + managed EXE
::    - Validates and copies runtime dependencies
::    - Mirrors selected artifacts to wix-setup-wizard\bin
::    - Provides detailed step-by-step logs with ANSI colors
::
::  Usage:
::    Run this script from PowerShell or Developer Command Prompt.
:: =============================================================================


:: ============================================================
:: COLOR INITIALIZATION
:: ============================================================
for /f "delims=" %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"
set "COLOR_RESET=%ESC%[0m"
set "COLOR_RED=%ESC%[91m"
set "COLOR_GREEN=%ESC%[92m"
set "COLOR_YELLOW=%ESC%[93m"
set "COLOR_CYAN=%ESC%[96m"
set "COLOR_WHITE=%ESC%[97m"

echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo %COLOR_CYAN% BUILDING OneGovPasswordAgent-core.dll + OneGovPasswordAgent-service.exe %COLOR_RESET%
echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo.


:: ============================================================
:: STEP 1: DEFINE WORKSPACE AND PATHS
:: ============================================================
echo %COLOR_CYAN%[STEP 1]%COLOR_RESET% Define workspace and file paths

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "WORKSPACE=%SCRIPT_DIR%\.."
set "SRC=%WORKSPACE%\agent-core\OneGovPasswordAgent-core.c"
set "DEF=%WORKSPACE%\agent-core\OneGovPasswordAgent-core.def"
set "CS_DIR=%WORKSPACE%\agent-service"
set "RES_DIR=%WORKSPACE%\resources"
set "OUTDIR=%WORKSPACE%\build-output"
set "OUTDLL=%OUTDIR%\OneGovPasswordAgent-core.dll"
set "OUTCS=%OUTDIR%\OneGovPasswordAgent-service.exe"
set "DST=%WORKSPACE%\wix-setup-wizard\bin"

echo %COLOR_GREEN%[OK]%COLOR_RESET% Workspace initialized
echo.


:: ============================================================
:: STEP 1.1: PRE-CLEAN bin artifacts (optional)
:: ============================================================
echo %COLOR_CYAN%[STEP 1.1]%COLOR_RESET% Pre-clean wix-setup-wizard\bin
if exist "%DST%" (
  for %%F in ("OneGovPasswordAgent-core.dll" "OneGovPasswordAgent-service.exe" "Newtonsoft.Json.dll") do (
    if exist "%DST%\%%~F" (
      echo %COLOR_YELLOW%[CLEAN]%COLOR_RESET% Deleting "%%~F" from bin...
      del /f /q "%DST%\%%~F" || echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Could not delete %%~F
    ) else (
      echo %COLOR_WHITE%[SKIP]%COLOR_RESET% Not found: %%~F
    )
  )
) else (
  echo %COLOR_WHITE%[INFO]%COLOR_RESET% Bin folder not found yet; it will be created in STEP 7.
)
echo.


:: ============================================================
:: STEP 2: CLEAN AND PREPARE OUTPUT FOLDER
:: ============================================================
echo %COLOR_CYAN%[STEP 2]%COLOR_RESET% Prepare output folder

if exist "%OUTDIR%" (
    echo %COLOR_YELLOW%[CLEAN]%COLOR_RESET% Removing previous build-output folder...
    rmdir /S /Q "%OUTDIR%" >nul 2>&1
) else (
    echo %COLOR_YELLOW%[INFO]%COLOR_RESET% No previous build folder found, creating fresh one...
)
mkdir "%OUTDIR%" >nul 2>&1
echo %COLOR_GREEN%[OK]%COLOR_RESET% Output folder ready
echo.


:: ============================================================
:: STEP 3: SETUP MSVC BUILD ENVIRONMENT
:: ============================================================
echo %COLOR_CYAN%[STEP 3]%COLOR_RESET% Load Visual Studio Build Tools environment

set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Cannot find vcvars64.bat — please install VS Build Tools 2022.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
where cl >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% cl.exe not found in PATH after vcvars64.bat
    exit /b 2
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% MSVC environment loaded successfully
)
echo.


:: ============================================================
:: STEP 4: COMPILE NATIVE CORE DLL
:: ============================================================
echo %COLOR_CYAN%[STEP 4]%COLOR_RESET% Compile core native DLL

if not exist "%SRC%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Source not found: %SRC%
    exit /b 3
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found C source file → %SRC%
)

if not exist "%DEF%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% DEF not found: %DEF%
    exit /b 4
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found DEF file → %DEF%
)

echo %COLOR_YELLOW%[BUILD]%COLOR_RESET% Compiling %SRC%
cl /LD "%SRC%" /Fe:"%OUTDLL%" /link /DEF:"%DEF%" advapi32.lib
if errorlevel 1 (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% Core DLL build failed with code %errorlevel%
    exit /b %errorlevel%
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Core DLL compiled successfully → %OUTDLL%
)
echo.


:: ============================================================
:: STEP 5: COMPILE C# SYNCAGENT SERVICE
:: ============================================================
echo %COLOR_CYAN%[STEP 5]%COLOR_RESET% Compile C# SyncAgent Service

if not exist "%CS_DIR%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% C# source directory not found: %CS_DIR%
    exit /b 5
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found C# source directory → %CS_DIR%
)
pushd "%CS_DIR%" >nul

echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% Searching for .cs source files...
set "CS_FILES="
for %%F in (*.cs) do set "CS_FILES=!CS_FILES! %%F"

if "!CS_FILES!"=="" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% No .cs files found in %CS_DIR%
    popd
    exit /b 10
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found C# files: !CS_FILES!
)
echo.


:: --- Check Newtonsoft.Json dependency
echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% Validating Newtonsoft.Json.dll presence...
if exist "Newtonsoft.Json.dll" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Newtonsoft.Json.dll already in project folder
) else if exist "%RES_DIR%\Newtonsoft.Json.dll" (
    echo %COLOR_YELLOW%[INFO]%COLOR_RESET% Copying Newtonsoft.Json.dll from resources...
    copy /Y "%RES_DIR%\Newtonsoft.Json.dll" "%CS_DIR%\" >nul
    if exist "Newtonsoft.Json.dll" (
        echo %COLOR_GREEN%[OK]%COLOR_RESET% Newtonsoft.Json.dll successfully copied
    ) else (
        echo %COLOR_RED%[FAIL]%COLOR_RESET% Copy attempt failed — dependency unresolved.
        popd
        exit /b 11
    )
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Newtonsoft.Json.dll not found — continuing without reference.
)
echo.


:: --- Detect compiler (CSC preferred)
echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% Detecting available C# compiler...
set "CS_RESULT=0"
where csc >nul 2>&1

if %ERRORLEVEL%==0 (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% CSC compiler detected
    if exist "Newtonsoft.Json.dll" (
        echo %COLOR_YELLOW%[INFO]%COLOR_RESET% Compiling with Newtonsoft.Json reference
        csc /nologo /t:exe /out:"%OUTCS%" /r:Newtonsoft.Json.dll !CS_FILES!
    ) else (
        echo %COLOR_YELLOW%[INFO]%COLOR_RESET% Compiling without external references
        csc /nologo /t:exe /out:"%OUTCS%" !CS_FILES!
    )
    set "CS_RESULT=!ERRORLEVEL!"
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% CSC compiler not found — using dotnet build fallback
    dotnet build -c Release -o "%OUTDIR%"
    set "CS_RESULT=!ERRORLEVEL!"
)

if "%CS_RESULT%"=="0" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% C# SyncAgent compiled successfully → %OUTCS%
) else (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% C# build failed. Exit code %CS_RESULT%
    popd >nul
    exit /b %CS_RESULT%
)
popd >nul
echo.


:: ============================================================
:: STEP 6: COPY RUNTIME RESOURCES
:: ============================================================
echo %COLOR_CYAN%[STEP 6]%COLOR_RESET% Copy runtime resources
echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% Validating and copying resource files...

for %%R in (OneGovPasswordAgent.ini Newtonsoft.Json.dll license.rtf) do (
    if exist "%RES_DIR%\%%R" (
        copy /Y "%RES_DIR%\%%R" "%OUTDIR%\" >nul
        if exist "%OUTDIR%\%%R" (
            echo %COLOR_GREEN%[OK]%COLOR_RESET% Copied %%R → %OUTDIR%
        ) else (
            echo %COLOR_RED%[FAIL]%COLOR_RESET% Copy failed for %%R
            exit /b 30
        )
    ) else (
        echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Missing resource: %%R → skipped
    )
)
echo %COLOR_GREEN%[OK]%COLOR_RESET% Resource validation complete
echo.


:: ============================================================
:: STEP 7: COPY selected files → wix-setup-wizard\bin
::   Only: OneGovPasswordAgent-core.dll, OneGovPasswordAgent-service.exe, Newtonsoft.Json.dll
:: ============================================================
echo %COLOR_CYAN%[STEP 7]%COLOR_RESET% Copy selected files → wix-setup-wizard\bin

if not exist "%DST%" (
    echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Creating destination folder: "%DST%"
    mkdir "%DST%" || ( echo %COLOR_RED%[ERROR]%COLOR_RESET% Failed to create "%DST%". & exit /b 40 )
)

set "COPIED_OK=1"

:: --- File 1: core DLL
if exist "%OUTDLL%" (
    echo %COLOR_YELLOW%[COPY]%COLOR_RESET% %OUTDLL% → %DST%
    copy /Y "%OUTDLL%" "%DST%\" >nul || set "COPIED_OK=0"
    if exist "%DST%\OneGovPasswordAgent-core.dll" (
        echo %COLOR_GREEN%[OK]%COLOR_RESET% Copied OneGovPasswordAgent-core.dll
    ) else (
        echo %COLOR_RED%[FAIL]%COLOR_RESET% Copy failed: OneGovPasswordAgent-core.dll
        set "COPIED_OK=0"
    )
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Missing source: %OUTDLL%
    set "COPIED_OK=0"
)

:: --- File 2: service EXE
if exist "%OUTCS%" (
    echo %COLOR_YELLOW%[COPY]%COLOR_RESET% %OUTCS% → %DST%
    copy /Y "%OUTCS%" "%DST%\" >nul || set "COPIED_OK=0"
    if exist "%DST%\OneGovPasswordAgent-service.exe" (
        echo %COLOR_GREEN%[OK]%COLOR_RESET% Copied OneGovPasswordAgent-service.exe
    ) else (
        echo %COLOR_RED%[FAIL]%COLOR_RESET% Copy failed: OneGovPasswordAgent-service.exe
        set "COPIED_OK=0"
    )
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Missing source: %OUTCS%
    set "COPIED_OK=0"
)

:: --- File 3: Newtonsoft.Json.dll (prefer build-output, fallback to agent-service)
set "JSON_SRC=%OUTDIR%\Newtonsoft.Json.dll"
if not exist "%JSON_SRC%" if exist "%CS_DIR%\Newtonsoft.Json.dll" set "JSON_SRC=%CS_DIR%\Newtonsoft.Json.dll"

if exist "%JSON_SRC%" (
    echo %COLOR_YELLOW%[COPY]%COLOR_RESET% %JSON_SRC% → %DST%
    copy /Y "%JSON_SRC%" "%DST%\" >nul || set "COPIED_OK=0"
    if exist "%DST%\Newtonsoft.Json.dll" (
        echo %COLOR_GREEN%[OK]%COLOR_RESET% Copied Newtonsoft.Json.dll
    ) else (
        echo %COLOR_RED%[FAIL]%COLOR_RESET% Copy failed: Newtonsoft.Json.dll
        set "COPIED_OK=0"
    )
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Missing source: Newtonsoft.Json.dll
    set "COPIED_OK=0"
)

if "%COPIED_OK%"=="1" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Selected files copied successfully.
) else (
    echo %COLOR_RED%[WARN]%COLOR_RESET% Some selected files were not copied. See messages above.
)
echo.


:: ============================================================
:: STEP 8: BUILD SUMMARY
:: ============================================================
echo %COLOR_CYAN%[STEP 8]%COLOR_RESET% Build summary
echo %COLOR_WHITE%===============================================================%COLOR_RESET%
echo %COLOR_GREEN%✅ Build complete!%COLOR_RESET%
echo %COLOR_WHITE%Output directory:%COLOR_RESET% %OUTDIR%
if exist "%OUTDLL%" echo %COLOR_GREEN%DLL:%COLOR_RESET% %OUTDLL%
if exist "%OUTCS%" echo %COLOR_GREEN%C# :%COLOR_RESET% %OUTCS%
if exist "%OUTDIR%\OneGovPasswordAgent.ini" echo %COLOR_GREEN%INI:%COLOR_RESET% %OUTDIR%\OneGovPasswordAgent.ini
echo %COLOR_WHITE%Mirrored to:%COLOR_RESET% %DST%
echo %COLOR_WHITE%===============================================================%COLOR_RESET%

endlocal
