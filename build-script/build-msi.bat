@echo off
setlocal ENABLEDELAYEDEXPANSION
chcp 65001 >nul
title Build OneGov Password Agent MSI Package

:: =============================================================================
::  ONEGOV PASSWORD AGENT - MSI BUILDER
:: =============================================================================

:: Colors
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
:: ============================================================
echo %COLOR_CYAN%[STEP 1]%COLOR_RESET% Define base paths and environment

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "ROOT=%SCRIPT_DIR%\.."
set "MSI_DIR=%ROOT%\wix-setup-wizard"
set "ASSET_DIR=%MSI_DIR%\asset"
set "RES_DIR=%ROOT%\resources"
set "BUILD_DIR=%ROOT%\build-output"
set "BIN_DIR=%MSI_DIR%\bin"
set "RELEASE_DIR=%ROOT%\release"

:: Build architecture for MSI (x64 required to write to real System32)
set "ARCH=x64"

set "MSI_NAME=OneGovPwdAgent-installer.msi"
set "MSI_PATH=%RELEASE_DIR%\%MSI_NAME%"

set "TIMER_START=%TIME%"
echo %COLOR_GREEN%[OK]%COLOR_RESET% Base paths defined successfully
echo.

:: ============================================================
:: STEP 2: PREPARE RELEASE DIRECTORY
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
:: STEP 3: VERIFY WIX TOOLSET AND REQUIRED FILES
:: ============================================================
echo %COLOR_CYAN%[STEP 3]%COLOR_RESET% Verify WiX installation and required files

where wix >nul 2>&1 || ( echo %COLOR_RED%[ERROR]%COLOR_RESET% WiX 4 CLI not found & exit /b 1 )

if not exist "%MSI_DIR%\Main.wxs"               ( echo %COLOR_RED%[ERROR]%COLOR_RESET% Missing Main.wxs & exit /b 2 )
if not exist "%MSI_DIR%\UIHostConfiguration.wxs" ( echo %COLOR_RED%[ERROR]%COLOR_RESET% Missing UIHostConfiguration.wxs & exit /b 2 )
if not exist "%ASSET_DIR%"                      ( echo %COLOR_RED%[ERROR]%COLOR_RESET% Missing asset dir: %ASSET_DIR% & exit /b 3 )

:: ---- Resolve UI/Util extension DLLs (support wixext4/ or tools/)
set "WIXCACHE=%USERPROFILE%\.wix\extensions"
set "UI_DLL="
set "UTIL_DLL="

:: First try user cache (wixext4 then tools)
if exist "%WIXCACHE%\WixToolset.UI.wixext\4.0.4\wixext4\WixToolset.UI.wixext.dll" set "UI_DLL=%WIXCACHE%\WixToolset.UI.wixext\4.0.4\wixext4\WixToolset.UI.wixext.dll"
if not defined UI_DLL if exist "%WIXCACHE%\WixToolset.UI.wixext\4.0.4\tools\WixToolset.UI.wixext.dll" set "UI_DLL=%WIXCACHE%\WixToolset.UI.wixext\4.0.4\tools\WixToolset.UI.wixext.dll"

if exist "%WIXCACHE%\WixToolset.Util.wixext\4.0.4\wixext4\WixToolset.Util.wixext.dll" set "UTIL_DLL=%WIXCACHE%\WixToolset.Util.wixext\4.0.4\wixext4\WixToolset.Util.wixext.dll"
if not defined UTIL_DLL if exist "%WIXCACHE%\WixToolset.Util.wixext\4.0.4\tools\WixToolset.Util.wixext.dll" set "UTIL_DLL=%WIXCACHE%\WixToolset.Util.wixext\4.0.4\tools\WixToolset.Util.wixext.dll"

:: Fallback to C:\Temp\wixext if cache is empty
if not defined UI_DLL if exist "C:\Temp\wixext\UI\tools\WixToolset.UI.wixext.dll" set "UI_DLL=C:\Temp\wixext\UI\tools\WixToolset.UI.wixext.dll"
if not defined UTIL_DLL if exist "C:\Temp\wixext\Util\tools\WixToolset.Util.wixext.dll" set "UTIL_DLL=C:\Temp\wixext\Util\tools\WixToolset.Util.wixext.dll"

echo Using UI DLL:   "%UI_DLL%"
echo Using Util DLL: "%UTIL_DLL%"

if not exist "%UI_DLL%"  (
  echo %COLOR_RED%[ERROR]%COLOR_RESET% UI extension DLL not found in:
  echo   %WIXCACHE%\WixToolset.UI.wixext\4.0.4\wixext4\WixToolset.UI.wixext.dll
  echo   C:\Temp\wixext\UI\tools\WixToolset.UI.wixext.dll
  exit /b 20
)
if not exist "%UTIL_DLL%" (
  echo %COLOR_RED%[ERROR]%COLOR_RESET% Util extension DLL not found in:
  echo   %WIXCACHE%\WixToolset.Util.wixext\4.0.4\wixext4\WixToolset.Util.wixext.dll
  echo   C:\Temp\wixext\Util\tools\WixToolset.Util.wixext.dll
  exit /b 21
)

set "UIEXT_SWITCH=-ext \"%UI_DLL%\" -ext \"%UTIL_DLL%\""
echo %COLOR_GREEN%[OK]%COLOR_RESET% WiX CLI and local extensions ready
echo.



:: ============================================================
:: STEP 4: COPY AND VERIFY ASSETS
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
    copy /Y "%ASSET_DIR%\license.rtf" . >nul
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Copied license.rtf
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
:: STEP 5: VERIFY BIN PAYLOAD FOR .WXS REFERENCES
:: ============================================================
echo %COLOR_CYAN%[STEP 5]%COLOR_RESET% Verify payload under wix-setup-wizard\bin

set "BIN_MISSING=0"
if exist "%BIN_DIR%\OneGovPwdAgent-core.dll" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found bin\OneGovPwdAgent-core.dll
) else (
    echo %COLOR_RED%[MISSING]%COLOR_RESET% bin\OneGovPwdAgent-core.dll not found. Run build-agent.bat to populate bin.
    set "BIN_MISSING=1"
)
if exist "%BIN_DIR%\OneGovPwdAgent-service.exe" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found bin\OneGovPwdAgent-service.exe
) else (
    echo %COLOR_RED%[MISSING]%COLOR_RESET% bin\OneGovPwdAgent-service.exe not found. Run build-agent.bat to populate bin.
    set "BIN_MISSING=1"
)
if exist "%BIN_DIR%\Newtonsoft.Json.dll" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found bin\Newtonsoft.Json.dll
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% bin\Newtonsoft.Json.dll not found. If your service needs it, ensure it’s copied to bin.
)
if exist "%BIN_DIR%\ValidateAgent.dll" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found bin\ValidateAgent.dll
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% bin\ValidateAgent.dll not found. If your service needs it, ensure it’s copied to bin.
)

if %BIN_MISSING%==1 (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Required payload missing in bin. Aborting.
    exit /b 10
)
echo.

:: ============================================================
:: STEP 6: BUILD MSI PACKAGE (x64)
:: ============================================================
echo %COLOR_CYAN%[STEP 6]%COLOR_RESET% Build MSI using WiX Toolset (arch=%ARCH%)

:: Use wixext4 only (you confirmed both DLLs are here)
set "UI_DLL=C:\Users\bacht\.wix\extensions\WixToolset.UI.wixext\4.0.4\wixext4\WixToolset.UI.wixext.dll"
set "UTIL_DLL=C:\Users\bacht\.wix\extensions\WixToolset.Util.wixext\4.0.4\wixext4\WixToolset.Util.wixext.dll"

echo Extensions:
echo   UI  : %UI_DLL%
echo   Util: %UTIL_DLL%

if not exist "%UI_DLL%" (
  echo %COLOR_RED%[ERROR]%COLOR_RESET% Missing UI DLL at the path above
  echo --- Parent folder listing ---
  for %%D in ("%UI_DLL%") do set "UI_DIR=%%~dpD"
  echo UI_DIR=%UI_DIR%
  dir /b "%UI_DIR%" 2>&1
  exit /b 61
)

if not exist "%UTIL_DLL%" (
  echo %COLOR_RED%[ERROR]%COLOR_RESET% Missing Util DLL at the path above
  echo --- Parent folder listing ---
  for %%D in ("%UTIL_DLL%") do set "UTIL_DIR=%%~dpD"
  echo UTIL_DIR=%UTIL_DIR%
  dir /b "%UTIL_DIR%" 2>&1
  exit /b 62
)

pushd "%MSI_DIR%" >nul
echo %COLOR_YELLOW%[BUILD]%COLOR_RESET% Running WiX build process...

wix build ".\Main.wxs" ".\UIHostConfiguration.wxs" ^
  -ext "%UI_DLL%" -ext "%UTIL_DLL%" ^
  -loc ".\asset\en-us.wxl" ^
  -arch %ARCH% ^
  -o "%MSI_PATH%"

set "MSI_RESULT=%ERRORLEVEL%"
popd >nul

if %MSI_RESULT% neq 0 (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% WiX build failed. Exit code %MSI_RESULT%
    echo %COLOR_YELLOW%[HINT]%COLOR_RESET% Verify DLLs exist exactly at the paths above and match WiX 4.0.4.
    exit /b %MSI_RESULT%
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% WiX build succeeded → %MSI_PATH%
)
echo.




:: ============================================================
:: STEP 7: CLEANUP TEMPORARY ASSETS
:: ============================================================
echo %COLOR_CYAN%[STEP 7]%COLOR_RESET% Cleanup temporary asset copies

pushd "%MSI_DIR%" >nul
del /Q app.ico banner.bmp dialog.bmp readme.txt license.rtf >nul 2>&1
popd >nul
echo %COLOR_GREEN%[OK]%COLOR_RESET% Temporary assets cleaned up successfully
echo.

:: ============================================================
:: STEP 8: BUILD SUMMARY
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
