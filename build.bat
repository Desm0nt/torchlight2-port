@echo off
rem Torchlight II Mobile (DM_GAME=tl2, org.torchlight2.port): build, sign and optionally install the APK (wraps tools\build_apk.ps1; see README.md "Building").
rem   build.bat [-Install <adb serial>] [-Validation] [-Android10]
setlocal
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_apk.ps1" %*
exit /b %ERRORLEVEL%
