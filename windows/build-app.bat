@echo off
setlocal EnableExtensions
rem =========================================================
rem  P3 exe client build script (x64 only)
rem  Usage: build-app.bat [Release^|Debug]
rem  Output: build\x64\<Config>\app\  (portable folder)
rem =========================================================

rem MSBuild crashes (MSB6001) when HTTPS_PROXY and https_proxy both exist.
set "https_proxy="
set "http_proxy="
set "all_proxy="
set "no_proxy="

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

rem MSBuild crashes (MSB6001): environment table has case-insensitive
rem duplicate keys ("Path"/"PATH"/"path") -> ArgumentException "Path already added".
rem Normalize to a single "Path" before invoking MSBuild.
set "PATH_BACKUP=%Path%"
set "PATH="
set "path="
set "Path=%PATH_BACKUP%"

echo [1/4] Locating Visual Studio...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vs
echo       VS: %VSPATH%

echo [2/4] Loading compiler environment (x64)...
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 goto :no_vcvars

echo [3/4] Building x64 ^| %CONFIG% ...
"%VSPATH%\MSBuild\Current\Bin\MSBuild.exe" "%~dp0LanShareApp.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /m /v:minimal /nologo
if errorlevel 1 goto :build_failed

echo [4/4] Assembling portable folder...
set "APPDIR=%~dp0build\x64\%CONFIG%\app"
if not exist "%APPDIR%" goto :no_output

rem web/index.html: disk copy is preferred by HttpServer, so the page can be
rem edited without rebuilding. Copy it next to the exe.
if not exist "%APPDIR%\web" mkdir "%APPDIR%\web"
copy /Y "%~dp0web\index.html" "%APPDIR%\web\index.html" >nul
if errorlevel 1 goto :copy_failed

rem WebView2Loader.dll must sit next to the exe (Evergreen bootstrap).
copy /Y "%~dp0third_party\webview2\bin\WebView2Loader.dll" "%APPDIR%\WebView2Loader.dll" >nul
if errorlevel 1 goto :copy_failed

echo.
echo [OK] Portable folder: %APPDIR%
echo      Run: LanShare.exe  (data goes to APPDIR\LanShare\)
endlocal
exit /b 0

:no_vs
echo [ERROR] vswhere.exe not found, or no VS with C++ tools.
exit /b 1
:no_vcvars
echo [ERROR] vcvarsall.bat failed.
exit /b 1
:build_failed
echo [ERROR] Build failed, see messages above.
exit /b 1
:no_output
echo [ERROR] Expected output folder not found: %APPDIR%
exit /b 1
:copy_failed
echo [ERROR] Failed to copy runtime files into portable folder.
exit /b 1
