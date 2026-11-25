@echo off
setlocal EnableExtensions
rem =========================================================
rem  TM-LAN-Share command line build script
rem  Usage: build.bat [x64^|x86] [Release^|Debug]
rem  Output: build\Platform\Config\lanshare.dll
rem =========================================================

rem MSBuild crashes (MSB6001) when both HTTPS_PROXY and https_proxy exist,
rem because its process environment table is case-insensitive.
set "https_proxy="
set "http_proxy="
set "all_proxy="
set "no_proxy="

set "PLATFORM=%~1"
set "CONFIG=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"
if "%CONFIG%"=="" set "CONFIG=Release"
if /I "%PLATFORM%"=="x86" set "PLATFORM=Win32"
if /I "%PLATFORM%"=="win32" set "PLATFORM=Win32"
if /I "%PLATFORM%"=="amd64" set "PLATFORM=x64"

echo [1/3] Locating Visual Studio...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vs
echo       VS: %VSPATH%

echo [2/3] Loading compiler environment...
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %PLATFORM% >nul
if errorlevel 1 goto :no_vcvars

echo [3/3] Building %PLATFORM% ^| %CONFIG% ...
rem ---------------------------------------------------------------
rem rc.exe does not track files referenced by RCDATA: after editing
rem web\index.html, MSBuild thinks web.rc is unchanged and skips the
rem resource step, so the DLL keeps serving the OLD page with no
rem compile error at all.
rem Touch web.rc unconditionally to force a resource rebuild (<1s).
rem NOTE: keep this file pure ASCII. cmd reads .bat in the OEM code
rem page (GBK here); UTF-8 Chinese in comments gets split into bogus
rem commands. Same class of bug as "PowerShell .ps1 must be UTF-8 BOM".
rem ---------------------------------------------------------------
rem copy /Y 到自己身上不会更新时间戳（会报"源和目标相同"），必须写临时文件再改名。
copy /Y "%~dp0web.rc" "%~dp0web.rc.tmp" >nul 2>&1
move /Y "%~dp0web.rc.tmp" "%~dp0web.rc" >nul 2>&1

"%VSPATH%\MSBuild\Current\Bin\MSBuild.exe" "%~dp0TrafficMonitorLanShare.vcxproj" /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m /v:minimal /nologo
if errorlevel 1 goto :build_failed

set "OUT=%~dp0build\%PLATFORM%\%CONFIG%\lanshare.dll"
if not exist "%OUT%" goto :no_output
echo.
echo [OK] %OUT%
endlocal
exit /b 0

:no_vs
echo [ERROR] vswhere.exe not found, or no VS with "Desktop development with C++".
exit /b 1
:no_vcvars
echo [ERROR] vcvarsall.bat failed.
exit /b 1
:build_failed
echo [ERROR] Build failed, see messages above.
exit /b 1
:no_output
echo [ERROR] Expected output not found: %OUT%
exit /b 1
