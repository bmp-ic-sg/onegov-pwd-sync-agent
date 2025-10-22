@echo off
setlocal ENABLEDELAYEDEXPANSION
chcp 65001 >nul
title Build WiX Custom Action DLL

:: =============================================================================
::  OneGov Password Agent - WiX Custom Action DLL Builder
::
::  This script compiles the ValidateAgent.dll used by the WiX installer.
::  It performs the following tasks:
::    • Cleans previous /bin output
::    • Loads MSVC and Windows SDK environments
::    • Detects the latest installed Windows SDK version
::    • Compiles ValidateAgent.c into ValidateAgent.dll
:: =============================================================================


:: -------------------------------------------------
:: ANSI color codes (for rich console output)
:: -------------------------------------------------
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


:: =================================================
:: STEP 1: Define paths
:: =================================================
:: Purpose:
::   Define key directories for source, output, and workspace references.
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

echo %COLOR_GREEN%[OK]%COLOR_RESET% Paths initialized
echo.


:: =================================================
:: STEP 2: Clean and prepare output directory
:: =================================================
:: Purpose:
::   Remove old binaries and create a fresh /bin directory.
echo %COLOR_CYAN%[STEP 2]%COLOR_RESET% Prepare output directory

if exist "%OUTDIR%" (
    :: If old output folder exists → clean it to ensure a fresh build
    echo %COLOR_YELLOW%[CLEAN]%COLOR_RESET% Removing previous output folder...
    rmdir /S /Q "%OUTDIR%" >nul 2>&1
) else (
    :: If it doesn't exist → just log initialization
    echo %COLOR_YELLOW%[INFO]%COLOR_RESET% No previous build folder found, skipping cleanup
)
echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Creating fresh output directory...
mkdir "%OUTDIR%" >nul 2>&1
echo %COLOR_GREEN%[OK]%COLOR_RESET% Output folder ready
echo.


:: =================================================
:: STEP 3: Setup MSVC environment
:: =================================================
:: Purpose:
::   Load Visual Studio Build Tools (vcvars64.bat) so cl.exe and link.exe
::   become available in PATH for compilation.
echo %COLOR_CYAN%[STEP 3]%COLOR_RESET% Load Visual Studio Build Tools environment

set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    :: Missing build tools = cannot compile
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Cannot find vcvars64.bat — ensure Build Tools 2022 is installed.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1

where cl >nul 2>&1
if %ERRORLEVEL% neq 0 (
    :: If cl.exe still missing → environment load failed
    echo %COLOR_RED%[ERROR]%COLOR_RESET% cl.exe not found in PATH after environment setup
    exit /b 2
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% MSVC environment loaded successfully
)
echo.










:: =================================================
:: STEP 4: Detect Windows SDK version
:: =================================================
:: Purpose:
::   Automatically detect the latest installed Windows 10 SDK that contains
::   the required "msi.h" header, and extend INCLUDE/LIB environment paths.

echo %COLOR_CYAN%[STEP 4]%COLOR_RESET% Detect latest Windows SDK version

set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER="
set "FOUND_SDK=0"

:: --- Verify that Windows SDK Include folder exists ---
if not exist "%SDKROOT%\Include" (
    :: ❌ Missing SDK Include folder → cannot continue
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Windows Kits Include folder not found: %SDKROOT%\Include
    echo %COLOR_RED%[HINT]%COLOR_RESET% Please install the Windows 10 SDK via Visual Studio Installer.
    exit /b 3
) else (
    :: ✅ Folder exists → safe to continue scanning
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Windows Kits Include folder found: %SDKROOT%\Include
)

:: --- Scan for latest SDK version containing msi.h ---
echo %COLOR_YELLOW%[SCAN]%COLOR_RESET% Scanning available SDK versions under "%SDKROOT%\Include"...
for /f "tokens=* delims=" %%v in ('dir "%SDKROOT%\Include" /b /ad ^| sort /r') do (
    if exist "%SDKROOT%\Include\%%v\um\msi.h" (
        echo %COLOR_GREEN%[FOUND]%COLOR_RESET% Detected usable Windows SDK version: %%v
        set "SDKVER=%%v"
        set "FOUND_SDK=1"
        rem We only need the latest one, so stop scanning
        goto :_breakSDK
    ) else (
        echo %COLOR_WHITE%   Skipping %%v (no msi.h found)
    )
)

:_breakSDK
if "%FOUND_SDK%"=="0" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% No valid Windows SDK found in "%SDKROOT%\Include"
    echo %COLOR_RED%[HINT]%COLOR_RESET% Please install Windows 10 SDK (select 'Windows SDK' workload).
    exit /b 4
)

:: Add SDK paths for INCLUDE/LIB after detection
echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Using Windows SDK version %SDKVER%
set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\um"
set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\um\x64"
echo %COLOR_GREEN%[OK]%COLOR_RESET% Windows SDK environment configured
echo.


:: =================================================
:: STEP 5: Compile DLL
:: =================================================
:: Purpose:
::   Build ValidateAgent.dll (WiX Custom Action) using cl.exe + link.exe.
echo %COLOR_CYAN%[STEP 5]%COLOR_RESET% Compile ValidateAgent.dll

if not exist "%SRC%" (
    :: C source not found → cannot continue
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Source file missing: %SRC%
    exit /b 5
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found source → %SRC%
)

if not exist "%DEF%" (
    :: DEF file missing → export symbols not defined
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
    :: Non-zero result = compiler or linker error
    echo %COLOR_RED%[FAIL]%COLOR_RESET% Custom Action build failed. Exit code %errorlevel%
    exit /b %errorlevel%
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Custom Action built successfully → %CA_OUTDLL%
)
echo.


:: =================================================
:: STEP 6: Finish summary
:: =================================================
:: Purpose:
::   Display build completion and output path.
echo %COLOR_CYAN%[STEP 6]%COLOR_RESET% Build summary
echo %COLOR_WHITE%===============================================================%COLOR_RESET%
echo %COLOR_GREEN%✅ Build complete!%COLOR_RESET%
echo %COLOR_WHITE%Output directory:%COLOR_RESET% %OUTDIR%
echo %COLOR_WHITE%===============================================================%COLOR_RESET%

endlocal
