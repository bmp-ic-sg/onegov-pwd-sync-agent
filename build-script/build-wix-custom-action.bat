@echo off
setlocal ENABLEDELAYEDEXPANSION
chcp 65001 >nul
title Build WiX Custom Action DLL

:: =============================================================================
::  ONEGOV PASSWORD AGENT - WIX CUSTOM ACTION DLL BUILDER
::
::  Purpose:
::    Compiles ValidateAgent.dll used by the WiX installer for runtime validation.
::
::  Responsibilities:
::    • Clean previous /bin output folder
::    • Load MSVC and Windows SDK environments
::    • Detect the latest installed Windows SDK containing msi.h
::    • Compile ValidateAgent.c into ValidateAgent.dll
::
::  Usage:
::    Run inside Developer Command Prompt or PowerShell (Administrator recommended)
:: =============================================================================


:: ============================================================
:: COLOR INITIALIZATION
:: Defines ANSI color codes for structured log output.
:: ============================================================
for /f "delims=" %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"
set "COLOR_RESET=%ESC%[0m"
set "COLOR_RED=%ESC%[91m"
set "COLOR_GREEN=%ESC%[92m"
set "COLOR_YELLOW=%ESC%[93m"
set "COLOR_CYAN=%ESC%[96m"
set "COLOR_WHITE=%ESC%[97m"

echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo %COLOR_CYAN% BUILDING WiX Custom Action DLL                                %COLOR_RESET%
echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo.


:: ============================================================
:: STEP 1: DEFINE PATHS
:: Purpose:
::   Define key directories for source, output, and workspace references.
:: ============================================================
echo %COLOR_CYAN%[STEP 1]%COLOR_RESET% Define source, output, and workspace paths

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "WORKSPACE=%SCRIPT_DIR%\.."
for %%I in ("%WORKSPACE%") do set "WORKSPACE=%%~fI"

set "SRC=%WORKSPACE%\wix-setup-wizard\custom-action\ValidateAgent.c"
set "DEF=%WORKSPACE%\wix-setup-wizard\custom-action\ValidateAgent.def"
set "OUTDIR=%WORKSPACE%\wix-setup-wizard\bin"
set "CA_OUTDLL=%OUTDIR%\ValidateAgent.dll"
set "CA_OUTPDB=%OUTDIR%\ValidateAgent.pdb"

echo %COLOR_GREEN%[OK]%COLOR_RESET% Paths initialized successfully
echo.


:: ============================================================
:: STEP 2: CLEAN AND PREPARE OUTPUT DIRECTORY
:: Purpose:
::   Remove old binaries and create a clean /bin folder for the new build.
:: ============================================================
echo %COLOR_CYAN%[STEP 2]%COLOR_RESET% Prepare output directory

if exist "%OUTDIR%" (
    echo %COLOR_YELLOW%[CLEAN]%COLOR_RESET% Removing previous output folder...
    rmdir /S /Q "%OUTDIR%" >nul 2>&1
) else (
    echo %COLOR_YELLOW%[INFO]%COLOR_RESET% No previous build folder found, skipping cleanup
)
echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Creating fresh output directory...
mkdir "%OUTDIR%" >nul 2>&1
echo %COLOR_GREEN%[OK]%COLOR_RESET% Output folder ready
echo.


:: ============================================================
:: STEP 3: SETUP MSVC ENVIRONMENT
:: Purpose:
::   Load Visual Studio Build Tools (vcvars64.bat) to enable cl.exe and link.exe.
:: ============================================================
echo %COLOR_CYAN%[STEP 3]%COLOR_RESET% Load Visual Studio Build Tools environment

set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Cannot find vcvars64.bat — please install Visual Studio Build Tools 2022.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1

where cl >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% cl.exe not found in PATH after environment setup
    exit /b 2
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% MSVC environment loaded successfully
)
echo.


:: ============================================================
:: STEP 4: DETECT WINDOWS SDK VERSION
:: Purpose:
::   Automatically detect the latest Windows 10 SDK containing msi.h
::   and configure INCLUDE/LIB paths accordingly.
:: ============================================================
echo %COLOR_CYAN%[STEP 4]%COLOR_RESET% Detect latest Windows SDK version

set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER="
set "FOUND_SDK=0"

if not exist "%SDKROOT%\Include" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Windows Kits Include folder not found: %SDKROOT%\Include
    echo %COLOR_RED%[HINT]%COLOR_RESET% Please install Windows 10 SDK via Visual Studio Installer.
    exit /b 3
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Windows Kits Include folder found
)

echo %COLOR_YELLOW%[SCAN]%COLOR_RESET% Scanning available SDK versions under "%SDKROOT%\Include"...
for /f "tokens=* delims=" %%v in ('dir "%SDKROOT%\Include" /b /ad ^| sort /r') do (
    if exist "%SDKROOT%\Include\%%v\um\msi.h" (
        echo %COLOR_GREEN%[FOUND]%COLOR_RESET% Detected usable Windows SDK version: %%v
        set "SDKVER=%%v"
        set "FOUND_SDK=1"
        goto :_breakSDK
    ) else (
        echo %COLOR_WHITE%   Skipping %%v (no msi.h found)
    )
)

:_breakSDK
if "%FOUND_SDK%"=="0" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% No valid Windows SDK found in "%SDKROOT%\Include"
    echo %COLOR_RED%[HINT]%COLOR_RESET% Please install Windows 10 SDK with 'Desktop Development with C++'.
    exit /b 4
)

echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Using Windows SDK version %SDKVER%
set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\um"
set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\um\x64"
echo %COLOR_GREEN%[OK]%COLOR_RESET% Windows SDK environment configured
echo.


:: ============================================================
:: STEP 5: COMPILE CUSTOM ACTION DLL
:: Purpose:
::   Compile ValidateAgent.c into ValidateAgent.dll using cl.exe and link.exe.
:: ============================================================
echo %COLOR_CYAN%[STEP 5]%COLOR_RESET% Compile ValidateAgent.dll

if not exist "%SRC%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Source file missing: %SRC%
    exit /b 5
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found source → %SRC%
)

if not exist "%DEF%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% DEF file missing: %DEF%
    exit /b 6
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found DEF → %DEF%
)

echo %COLOR_YELLOW%[BUILD]%COLOR_RESET% Compiling custom action DLL...
cl /LD "%SRC%" ^
   /Fe:"%CA_OUTDLL%" ^
   /Fd:"%CA_OUTPDB%" ^
   /W4 /WX- /nologo /Zi /MD ^
   /link /DEF:"%DEF%" msi.lib winhttp.lib advapi32.lib

if errorlevel 1 (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% Custom Action build failed. Exit code %errorlevel%
    exit /b %errorlevel%
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Custom Action built successfully → %CA_OUTDLL%
)
echo.


:: ============================================================
:: STEP 6: BUILD SUMMARY
:: Purpose:
::   Display final result and output paths.
:: ============================================================
echo %COLOR_CYAN%[STEP 6]%COLOR_RESET% Build summary
echo %COLOR_WHITE%===============================================================%COLOR_RESET%
echo %COLOR_GREEN%✅ Build complete!%COLOR_RESET%
echo %COLOR_WHITE%Output directory:%COLOR_RESET% %OUTDIR%
if exist "%CA_OUTDLL%" echo %COLOR_GREEN%DLL:%COLOR_RESET% %CA_OUTDLL%
if exist "%CA_OUTPDB%" echo %COLOR_GREEN%PDB:%COLOR_RESET% %CA_OUTPDB%
echo %COLOR_WHITE%===============================================================%COLOR_RESET%

endlocal
