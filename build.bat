@echo off
REM ---------------------------------------------------------------------------
REM  WinWhisper build helper.
REM  Finds Visual Studio Build Tools, then builds WinWhisper.vcxproj (x64).
REM
REM    build.bat            -> Release|x64
REM    build.bat Debug      -> Debug|x64
REM ---------------------------------------------------------------------------
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build] vswhere.exe not found - is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
    echo [build] Visual Studio installation not found.
    exit /b 1
)

set "MSBUILD=%VSPATH%\MSBuild\Current\Bin\MSBuild.exe"
echo [build] using %MSBUILD%
echo [build] configuration: %CONFIG%^|x64

"%MSBUILD%" "%~dp0WinWhisper.vcxproj" -p:Configuration=%CONFIG% -p:Platform=x64 -m -v:minimal -nologo
if errorlevel 1 exit /b 1

echo.
echo [build] done: %~dp0%CONFIG%\x64\WinWhisper.exe
endlocal