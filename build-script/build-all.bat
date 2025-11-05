@echo off
setlocal ENABLEEXTENSIONS ENABLEDELAYEDEXPANSION
chcp 65001 >nul
title Build OneGov Password Sync Agent (Core + Service)

:: =============================================================================
::  ONEGOV PASSWORD SYNC AGENT - BUILD SCRIPT (preamble)
:: =============================================================================

:: ============================================================
:: COLOR INITIALIZATION
:: ============================================================
for /f "delims=" %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"
if not defined ESC (
  rem Fallback: disable colors if ESC couldn't be resolved
  set "COLOR_RESET="
  set "COLOR_RED="
  set "COLOR_GREEN="
  set "COLOR_YELLOW="
  set "COLOR_CYAN="
  set "COLOR_WHITE="
) else (
  set "COLOR_RESET=%ESC%[0m"
  set "COLOR_RED=%ESC%[91m"
  set "COLOR_GREEN=%ESC%[92m"
  set "COLOR_YELLOW=%ESC%[93m"
  set "COLOR_CYAN=%ESC%[96m"
  set "COLOR_WHITE=%ESC%[97m"
)

echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo %COLOR_CYAN% BUILDING OneGovPasswordAgent-core.dll + OneGovPasswordAgent-service.exe %COLOR_RESET%
echo %COLOR_CYAN%===============================================================%COLOR_RESET%
echo.

set "BUILD_START=%DATE% %TIME%"

:: ============================================================
:: STEP 1: DEFINE WORKSPACE AND PATHS
:: ============================================================
echo %COLOR_CYAN%[STEP 1]%COLOR_RESET% Define workspace and file paths

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "WORKSPACE=%SCRIPT_DIR%\.."

:: Agent (core DLL + C# service)
set "CORE_SRC=%WORKSPACE%\agent-core\OneGovPasswordAgent-core.c"
set "CORE_DEF=%WORKSPACE%\agent-core\OneGovPasswordAgent-core.def"
set "SVC_DIR=%WORKSPACE%\agent-service"

:: Resources and outputs
set "RES_DIR=%WORKSPACE%\resources"
set "OUTDIR=%WORKSPACE%\build-output"
set "OUTDLL=%OUTDIR%\OneGovPasswordAgent-core.dll"
set "OUTCS=%OUTDIR%\OneGovPasswordAgent-service.exe"

:: WiX bin destination
set "DST=%WORKSPACE%\wix-setup-wizard\bin"

:: WiX Custom Action
set "CA_SRC=%WORKSPACE%\wix-custom-action\AgentValidation.c"
set "CA_DEF=%WORKSPACE%\wix-custom-action\AgentValidation.def"
set "CA_OUTDLL=%DST%\AgentValidation.dll"
set "CA_OUTPDB=%DST%\AgentValidation.pdb"

:: Ensure output folders exist early
if not exist "%OUTDIR%" (
  mkdir "%OUTDIR%" 2>nul && echo %COLOR_GREEN%[OK]%COLOR_RESET% Created OUTDIR "%OUTDIR%" || (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Cannot create OUTDIR "%OUTDIR%"
    exit /b 2
  )
)
if not exist "%DST%" (
  mkdir "%DST%" 2>nul && echo %COLOR_GREEN%[OK]%COLOR_RESET% Created BIN "%DST%" || (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Cannot create BIN "%DST%"
    exit /b 2
  )
)

echo %COLOR_GREEN%[OK]%COLOR_RESET% Workspace initialized
echo.

:: ============================================================
:: STEP 2: PRE-CLEAN bin artifacts (optional)
:: ============================================================
echo %COLOR_CYAN%[STEP 2]%COLOR_RESET% Pre-clean wix-setup-wizard\bin
if exist "%DST%" (
  set "CLEAN_LIST=OneGovPasswordAgent-core.dll OneGovPasswordAgent-service.exe Newtonsoft.Json.dll ValidateAgent.dll ValidateAgent.ilk ValidateAgent.pdb"
  for %%F in (%CLEAN_LIST%) do (
    if exist "%DST%\%%~F" (
      echo %COLOR_YELLOW%[CLEAN]%COLOR_RESET% Deleting "%%~F" from bin...
      del /f /q "%DST%\%%~F" 1>nul 2>nul || echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Could not delete %%~F
    ) else (
      echo %COLOR_WHITE%[SKIP]%COLOR_RESET% Not found: %%~F
    )
  )
) else (
  echo %COLOR_WHITE%[INFO]%COLOR_RESET% Bin folder not found yet; it will be created later.
)
echo %COLOR_GREEN%[OK]%COLOR_RESET% wix-setup-wizard\bin folder ready
echo.

:: ============================================================
:: STEP 3: PRE-CLEAN OUTPUT FOLDER
:: ============================================================
echo %COLOR_CYAN%[STEP 3]%COLOR_RESET% Pre-clean build-output

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
:: STEP 4: SETUP MSVC BUILD ENVIRONMENT
:: ============================================================
echo %COLOR_CYAN%[STEP 4]%COLOR_RESET% Load Visual Studio Build Tools environment

set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
  echo %COLOR_RED%[ERROR]%COLOR_RESET% Cannot find vcvars64.bat. please install VS Build Tools 2022.
  echo %COLOR_YELLOW%[HINT]%COLOR_RESET% Check your edition BuildTools/Community/Professional/Enterprise.
  exit /b 1
)

echo %COLOR_YELLOW%[INFO]%COLOR_RESET% Using: %VCVARS%
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
:: STEP 5: DETECT WINDOWS SDK VERSION (safe with parentheses)
:: ============================================================
echo %COLOR_CYAN%[STEP 5]%COLOR_RESET% Detect Windows SDK (msi.h)

set "FOUND_SDK=0"
set "SDKINCLUDE="
set "SDKLIB="
set "SDKVER="

:: 4.1 Prefer env from vcvars (Win10/11 SDK)
if defined WindowsSdkDir if defined WindowsSDKVersion (
    set "SDKDIRRAW=!WindowsSdkDir!"
    set "SDKVERRAW=!WindowsSDKVersion!"
    if "!SDKDIRRAW:~-1!"=="\" set "SDKDIRRAW=!SDKDIRRAW:~0,-1!"
    if "!SDKVERRAW:~-1!"=="\" set "SDKVERRAW=!SDKVERRAW:~0,-1!"

    set "SDKINCLUDE=!SDKDIRRAW!\Include\!SDKVERRAW!"
    set "SDKLIB=!SDKDIRRAW!\Lib\!SDKVERRAW!"
    set "SDKVER=!SDKVERRAW!"

    echo !COLOR_YELLOW![INFO]!COLOR_RESET! Env WindowsSdkDir=!SDKDIRRAW!
    echo !COLOR_YELLOW![INFO]!COLOR_RESET! Env WindowsSDKVersion=!SDKVERRAW!
    echo !COLOR_WHITE![CHECK]!COLOR_RESET! Looking for "!SDKINCLUDE!\um\msi.h"

    if exist "!SDKINCLUDE!\um\msi.h" (
        set "FOUND_SDK=1"
        echo !COLOR_GREEN![FOUND]!COLOR_RESET! Using SDK from env: !SDKVER!
    ) else (
        echo !COLOR_YELLOW![SKIP]!COLOR_RESET! Env SDK missing msi.h at "!SDKINCLUDE!\um\msi.h"
    )
)

:: 4.2 Fallback scan only if not found
if "!FOUND_SDK!"=="0" (
    set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
    echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% SDKROOT=!SDKROOT!

    if not exist "!SDKROOT!\Include" (
        echo %COLOR_RED%[ERROR]%COLOR_RESET% Windows Kits Include folder not found: !SDKROOT!\Include
        echo %COLOR_RED%[HINT]%COLOR_RESET% Install Windows 10/11 SDK via Visual Studio Installer.
        exit /b 3
    )

    echo %COLOR_YELLOW%[SCAN]%COLOR_RESET% dir "!SDKROOT!\Include" /b /ad ^| sort /r

    for /f "usebackq tokens=* delims=" %%v in (`dir "!SDKROOT!\Include" /b /ad ^| sort /r`) do (
        echo %COLOR_WHITE%[TRY]%COLOR_RESET% Checking "%%v" ON "!SDKROOT!\Include\%%v\um\msi.h"
        if exist "!SDKROOT!\Include\%%v\um\msi.h" (
            set "SDKINCLUDE=!SDKROOT!\Include\%%v"
            set "SDKLIB=!SDKROOT!\Lib\%%v"
            set "SDKVER=%%v"
            set "FOUND_SDK=1"
            echo %COLOR_GREEN%[FOUND]%COLOR_RESET% SDK version: %%v
            goto :_sdk_found
        ) else (
            echo %COLOR_WHITE%   Skipping %%v (no msi.h)
        )
    )
) 

:_sdk_found
if "!FOUND_SDK!"=="0" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% No valid Windows SDK with msi.h found.
    echo %COLOR_RED%[HINT]%COLOR_RESET% Ensure 'Windows SDK' and 'MSI' components are installed.
    exit /b 4
)

echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Using Windows SDK version !SDKVER!
echo %COLOR_WHITE%[PATH]%COLOR_RESET% SDKINCLUDE=!SDKINCLUDE!
echo %COLOR_WHITE%[PATH]%COLOR_RESET% SDKLIB=!SDKLIB!

:: Include full trees: shared + ucrt + um
if defined INCLUDE (set "INCLUDE=!INCLUDE!;") else (set "INCLUDE=")
if defined LIB (set "LIB=!LIB!;") else (set "LIB=")

set "INCLUDE=!INCLUDE!!SDKINCLUDE!\shared;!SDKINCLUDE!\ucrt;!SDKINCLUDE!\um"
set "LIB=!LIB!!SDKLIB!\ucrt\x64;!SDKLIB!\um\x64"

echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% msi.lib using "!SDKLIB!\um\x64\msi.lib"
if not exist "!SDKLIB!\um\x64\msi.lib" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% msi.lib not found at "!SDKLIB!\um\x64\msi.lib"
    echo %COLOR_RED%[HINT]%COLOR_RESET% Repair Windows SDK: include "MSI" libraries.
    exit /b 4
)

echo %COLOR_GREEN%[OK]%COLOR_RESET% Windows SDK environment configured

echo.

:: ============================================================
:: STEP 6: COMPILE CUSTOM ACTION DLL
:: Purpose:
::   Compile AgentValidation.c into AgentValidation.dll using cl.exe and link.exe.
:: ============================================================
echo %COLOR_CYAN%[STEP 6]%COLOR_RESET% Compile AgentValidation.dll
set "CA_OK=ERROR"

if not exist "%CA_SRC%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% Source file missing: %CA_SRC%
    exit /b 5
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found source: %CA_SRC%
)

if not exist "%CA_DEF%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% CA_DEF file missing: %CA_DEF%
    exit /b 6
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found CA_DEF: %CA_DEF%
)

echo %COLOR_YELLOW%[BUILD]%COLOR_RESET% Compiling custom action DLL...
cl /LD "%CA_SRC%" ^
   /Fe:"%CA_OUTDLL%" ^
   /Fd:"%CA_OUTPDB%" ^
   /W4 /WX- /nologo /Zi /MD ^
   /link /CA_DEF:"%CA_DEF%" msi.lib winhttp.lib advapi32.lib

if errorlevel 1 (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% Custom Action build failed. Exit code %errorlevel%
    exit /b %errorlevel%
) else (
    set "CA_OK=OK"
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Custom Action built successfully: %CA_OUTDLL%
)
echo.

:: ============================================================
:: STEP 7: COMPILE NATIVE CORE DLL
:: ============================================================
echo %COLOR_CYAN%[STEP 7]%COLOR_RESET% Compile core native DLL
set "CORE_OK=ERROR"

if not defined CORE_SRC (
  echo %COLOR_RED%[ERROR]%COLOR_RESET% Source not found: %CORE_SRC%
  exit /b 3
) else (
  echo %COLOR_YELLOW%[INFO]%COLOR_RESET% Found C source file: %CORE_SRC%
)

if not defined CORE_DEF (
  echo %COLOR_RED%[ERROR]%COLOR_RESET% DEF not found: %CORE_DEF%
  exit /b 3
) else (
  echo %COLOR_YELLOW%[INFO]%COLOR_RESET% Found DEF file: %CORE_DEF%
)


echo %COLOR_YELLOW%[BUILD]%COLOR_RESET% Compiling %SRC%
cl /LD "%CORE_SRC%" /Fe:"%OUTDLL%" /link /DEF:"%CORE_DEF%" advapi32.lib
if errorlevel 1 (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% Core DLL build failed with code %errorlevel%
    exit /b %errorlevel%
) else (
    set "CORE_OK=OK"
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Core DLL compiled successfully: %OUTDLL%
)
echo.

:: ============================================================
:: STEP 8: COMPILE C# SYNCAGENT SERVICE
:: ============================================================
echo %COLOR_CYAN%[STEP 8]%COLOR_RESET% Compile C# SyncAgent Service
set "SVC_OK=ERROR"

if not exist "%SVC_DIR%" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% C# source directory not found: %SVC_DIR%
    exit /b 5
) else (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% Found C# source directory: %SVC_DIR%
)
pushd "%SVC_DIR%" >nul

echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% Searching for .cs source files...
set "CS_FILES="
for %%F in (*.cs) do set "CS_FILES=!CS_FILES! %%F"

if "!CS_FILES!"=="" (
    echo %COLOR_RED%[ERROR]%COLOR_RESET% No .cs files found in %SVC_DIR%
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
    copy /Y "%RES_DIR%\Newtonsoft.Json.dll" "%SVC_DIR%\" >nul
    if exist "Newtonsoft.Json.dll" (
        echo %COLOR_GREEN%[OK]%COLOR_RESET% Newtonsoft.Json.dll successfully copied
    ) else (
        echo %COLOR_RED%[FAIL]%COLOR_RESET% Copy attempt failed. Dependency unresolved.
        popd
        exit /b 11
    )
) else (
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Newtonsoft.Json.dll not found. Continuing without reference.
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
    echo %COLOR_YELLOW%[WARN]%COLOR_RESET% CSC compiler not found. Using dotnet build fallback
    dotnet build -c Release -o "%OUTDIR%"
    set "CS_RESULT=!ERRORLEVEL!"
)

if "%CS_RESULT%"=="0" (
    echo %COLOR_GREEN%[OK]%COLOR_RESET% C# SyncAgent compiled successfully: %OUTCS%
    set "SVC_OK=OK"
) else (
    echo %COLOR_RED%[FAIL]%COLOR_RESET% C# build failed. Exit code %CS_RESULT%
    popd >nul
    exit /b %CS_RESULT%
)
popd >nul
echo.

:: ============================================================
:: STEP 9: COPY RUNTIME RESOURCES
:: ============================================================
echo %COLOR_CYAN%[STEP 9]%COLOR_RESET% Copy runtime resources
echo %COLOR_YELLOW%[CHECK]%COLOR_RESET% Validating and copying resource files...

for %%R in (OneGovPasswordAgent.ini Newtonsoft.Json.dll license.rtf) do (
    if exist "%RES_DIR%\%%R" (
        copy /Y "%RES_DIR%\%%R" "%OUTDIR%\" >nul
        if exist "%OUTDIR%\%%R" (
            echo %COLOR_GREEN%[OK]%COLOR_RESET% Copied %%R : %OUTDIR%
        ) else (
            echo %COLOR_RED%[FAIL]%COLOR_RESET% Copy failed for %%R
            exit /b 30
        )
    ) else (
        echo %COLOR_YELLOW%[WARN]%COLOR_RESET% Missing resource: %%R : skipped
    )
)
echo %COLOR_GREEN%[OK]%COLOR_RESET% Resource validation complete
echo.

:: ============================================================
:: STEP 10: COPY build-output to wix-setup-wizard\bin
::   Only: OneGovPasswordAgent-core.dll, OneGovPasswordAgent-service.exe, Newtonsoft.Json.dll
:: ============================================================
echo %COLOR_CYAN%[STEP 10]%COLOR_RESET% Copy build-output files to wix-setup-wizard\bin
set "COPY_OK=ERROR"

if not exist "%DST%" (
    echo %COLOR_YELLOW%[INIT]%COLOR_RESET% Creating destination folder: "%DST%"
    mkdir "%DST%" || ( echo %COLOR_RED%[ERROR]%COLOR_RESET% Failed to create "%DST%". & exit /b 40 )
)

set "COPIED_OK=1"

:: --- File 1: core DLL
if exist "%OUTDLL%" (
    echo %COLOR_YELLOW%[COPY]%COLOR_RESET% OneGovPasswordAgent-core.dll to %DST%
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
    echo %COLOR_YELLOW%[COPY]%COLOR_RESET% OneGovPasswordAgent-service.exe to %DST%
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
    echo %COLOR_YELLOW%[COPY]%COLOR_RESET% Newtonsoft.Json.dll to %DST%
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
    set "COPY_OK=OK"
) else (
    echo %COLOR_RED%[WARN]%COLOR_RESET% Some selected files were not copied. See messages above.
)
echo.

:: ---------- BUILD SUMMARY ----------
set "BUILD_END=%DATE% %TIME%"
echo %COLOR_CYAN%======================== BUILD SUMMARY ========================%COLOR_RESET%
echo Start: %BUILD_START%
echo End  : %BUILD_END%
echo.
echo Toolchain
echo   VC vars : %VCVARS%
echo   SDK ver : %SDKVER%
echo.
echo Artifacts
set "SZ_CA=0" & set "SZ_CORE=0" & set "SZ_CS=0"
if exist "%CA_OUTDLL%" for %%I in ("%CA_OUTDLL%") do set "SZ_CA=%%~zI"
if exist "%OUTDLL%"   for %%I in ("%OUTDLL%")   do set "SZ_CORE=%%~zI"
if exist "%OUTCS%"    for %%I in ("%OUTCS%")    do set "SZ_CS=%%~zI"
echo   AgentValidation.dll : !SZ_CA! bytes  [%CA_OK%]
echo   Core DLL            : !SZ_CORE! bytes  [%CORE_OK%]
echo   Service EXE         : !SZ_CS! bytes  [%SVC_OK%]
echo.
echo Copies to bin: [%COPY_OK%]
echo Status:
set "EXITCODE=0"
if "%CA_OK%" NEQ "OK" set "EXITCODE=20"
if "%CORE_OK%" NEQ "OK" set "EXITCODE=21"
if "%SVC_OK%" NEQ "OK" set "EXITCODE=22"
if "%COPY_OK%" NEQ "OK" set "EXITCODE=23"
if "%EXITCODE%"=="0" (
  echo %COLOR_GREEN%[SUCCESS]%COLOR_RESET% Build completed successfully.
) else (
  echo %COLOR_RED%[FAILED]%COLOR_RESET% Build completed with errors. Exit=%EXITCODE%
)
echo %COLOR_CYAN%================================================================%COLOR_RESET%

exit /b %EXITCODE%