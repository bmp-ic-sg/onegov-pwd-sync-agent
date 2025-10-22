@echo off
setlocal ENABLEDELAYEDEXPANSION
chcp 65001 >nul
title Build OneGov Password Agent MSI Package

:: =============================================================================
::  ONEGOV PASSWORD AGENT - MSI BUILDER
::
::  Purpose:
::    Packages all compiled binaries and assets into a WiX-based
::    Windows Installer (.MSI) for distribution.
::
::  Responsibilities:
::    • Validate environment and dependencies
::    • Prepare and verify required assets
::    • Compile .wxs sources via WiX Toolset
::    • Output the final signed MSI installer
::
::  Usage:
::    Run after completing the core + service build script.
:: =============================================================================


:: ============================================================
:: COLOR INITIALIZATION
:: Defines ANSI color codes for structured, colorized logs.
:: ============================================================
for /f "delims=" %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"
set "COLOR_RESET=%ESC%[0m"
set "COLOR_RED=%ESC%[91m"
set "COLOR_GREEN=%ESC%[92m"
set "COLOR_YELLOW=%ESC%[93m"
set "COLOR_CYAN=%ESC%[96m"
set "COLOR_WHITE=%ESC%[97m"

echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo %COLOR_CYAN% BUILDING OneGovPasswordAgent MSI Package                      %COLOR_RESET%
echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo.


:: ============================================================
:: STEP 1: DEFINE PATHS AND ENVIRONMENT
:: Purpose:
::   Establish base directory layout and define WiX build variables.
:: ============================================================
echo %COLOR_CYAN%[STEP 1]%COLOR_RESET% Define base paths and environment

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "ROOT=%SCRIPT_DIR%\.."
set "MSI_DIR=%ROOT%\wix-setup-wizard"
set "ASSET_DIR=%MSI_DIR%\asset"
set "RES_DIR=%ROOT%\resources"
set "BUILD_DIR=%ROOT%\build-output"
set "RELEASE_DIR=%ROOT%\release"

set "MSI_NAME=OneGovPwdAgent-installer.msi"
set "MSI_PATH=%RELEASE_DIR%\%MSI_NAME%"
set "UIEXT=%USERPROFILE%\.wix\extensions\WixToolset.UI.wixext\4.0.4\tools\WixToolset.UI.wixext.dll"

set "TIMER_START=%TIME%"
echo %COLOR_GREEN%[OK]%COLOR_RESET% Base paths defined successfully
echo.


:: ============================================================
:: STEP 2: PREPARE RELEASE DIRECTORY
:: Purpose:
::   Clean old /release folder and re-create a fresh directory.
:: ============================================================
echo %COLOR_CYAN%[STEP 2]%COLOR_RESET% Prepare clean release folder

if exist "%RELEASE_DIR%" (
    echo %COLOR_YELLOW%[CLEAN]%COLOR_RESET% Removing previous release folder...
    rmdir /S /Q "%RELEASE_DIR%" >nul 2>&1
) else (
    echo %COLOR_YELLOW%[INFO]%COLOR_RESET% No previous release folder found, continuing...
)
echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Creating fresh release directory...
mkdir "%RELEASE_DIR%" >nul 2>&1
echo %COLOR_GREEN%[OK]%COLOR_RESET% Release folder ready
echo.


:: ============================================================
:: STEP 3: VERIFY WIX TOOLSET AND PREREQUISITES
:: Purpose:
::   Check WiX CLI presence and ensure all input files exist.
:: ============================================================
echo %COLOR_CYAN%[STEP 3]%COLOR_RESET% Verify WiX installation and required files

where wix >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% WiX toolset not found — install WiX 4.x and ensure it's in PATH.
    exit /b 1
)
if not exist "%MSI_DIR%\Main.wxs" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Missing Main.wxs in installer directory.
    exit /b 2
)
if not exist "%ASSET_DIR%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Missing asset directory: %ASSET_DIR%
    exit /b 3
)
echo %COLOR_GREEN%[OK]%COLOR_RESET% WiX toolset and base folders verified
echo.


:: ============================================================
:: STEP 4: COPY AND VERIFY ASSETS
:: Purpose:
::   Copy required graphics and text resources to WiX folder.
:: ============================================================
echo %COLOR_CYAN%[STEP 4]%COLOR_RESET% Prepare and verify asset files

pushd "%MSI_DIR%" >nul
set "ASSET_MISSING=0"

for %%F in (app.ico banner.bmp dialog.bmp readme.txt) do (
    if exist "%ASSET_DIR%\%%F" (
        copy /Y "%ASSET_DIR%\%%F" . >nul
        echo %COLOR_GREEN%[OK]%COLOR_RESET% Copied %%F
    ) else (
        echo %COLOR_RED%[MISSING]%COLOR_RESET% %%F not found in %ASSET_DIR%
        set "ASSET_MISSING=1"
    )
)

if exist "%ASSET_DIR%\license.rtf" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found license.rtf
) else (
    echo %COLOR_RED%[MISSING]%COLOR_RESET% license.rtf not found in %ASSET_DIR%
    set "ASSET_MISSING=1"
)
popd >nul

if %ASSET_MISSING%==1 (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Some assets missing — WiX build may fail.
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% All required assets verified
)
echo.


:: ============================================================
:: STEP 5: VERIFY COMPILED BINARIES
:: Purpose:
::   Confirm the compiled DLL and EXE exist before packaging.
:: ============================================================
echo %COLOR_CYAN%[STEP 5]%COLOR_RESET% Verify compiled DLL and EXE binaries

set "BIN_MISSING=0"
if exist "%BUILD_DIR%\OneGovPwdAgent-core.dll" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found OneGovPwdAgent-core.dll
) else (
    echo %COLOR_RED%[MISSING]%COLOR_RESET% OneGovPwdAgent-core.dll not found in %BUILD_DIR%
    set "BIN_MISSING=1"
)
if exist "%BUILD_DIR%\OneGovPwdAgent-service.exe" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found OneGovPwdAgent-service.exe
) else (
    echo %COLOR_RED%[MISSING]%COLOR_RESET% OneGovPwdAgent-service.exe not found in %BUILD_DIR%
    set "BIN_MISSING=1"
)
if %BIN_MISSING%==1 (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Some binaries missing — MSI may be incomplete.
)
echo.


:: ============================================================
:: STEP 6: BUILD MSI PACKAGE
:: Purpose:
::   Run WiX compiler to generate final installer package.
:: ============================================================
echo %COLOR_CYAN%[STEP 6]%COLOR_RESET% Build MSI using WiX Toolset

pushd "%MSI_DIR%" >nul
echo %COLOR_YELLOW%[BUILD]%COLOR_RESET% Running WiX build process...
wix build .\Main.wxs .\UIHostConfiguration.wxs -ext "%UIEXT%" -loc .\asset\en-us.wxl -o "%MSI_PATH%"
set "MSI_RESULT=%ERRORLEVEL%"
popd >nul

if %MSI_RESULT% neq 0 (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% WiX build failed. Exit code %MSI_RESULT%
    echo %COLOR_YELLOW%[HINT]%COLOR_RESET% Check .wxs syntax, asset paths, or missing binary references.
    exit /b %MSI_RESULT%
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% WiX build succeeded → %MSI_PATH%
)
echo.


:: ============================================================
:: STEP 7: CLEANUP TEMPORARY ASSETS
:: Purpose:
::   Remove temporarily copied files from the WiX project folder.
:: ============================================================
echo %COLOR_CYAN%[STEP 7]%COLOR_RESET% Cleanup temporary asset copies

pushd "%MSI_DIR%" >nul
del /Q app.ico banner.bmp dialog.bmp readme.txt license.rtf >nul 2>&1
popd >nul
echo %COLOR_GREEN%[OK]%COLOR_RESET% Temporary assets cleaned up successfully
echo.


:: ============================================================
:: STEP 8: BUILD SUMMARY
:: Purpose:
::   Print final timestamps and output summary.
:: ============================================================
set "TIMER_END=%TIME%"
echo %COLOR_CYAN%[STEP 8]%COLOR_RESET% Build summary
echo %COLOR_WHITE%===============================================================%COLOR_RESET%
echo %COLOR_GREEN%✅ MSI Build complete!%COLOR_RESET%
echo %COLOR_WHITE%Output :%COLOR_RESET% %MSI_PATH%
echo %COLOR_WHITE%Start  :%COLOR_RESET% %TIMER_START%
echo %COLOR_WHITE%End    :%COLOR_RESET% %TIMER_END%
echo %COLOR_WHITE%-------------------------------------------%COLOR_RESET%
echo %COLOR_GREEN%All done successfully!%COLOR_RESET%
echo %COLOR_WHITE%===============================================================%COLOR_RESET%

endlocal
