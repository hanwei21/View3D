@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo VSDEVCMD_NOT_FOUND
  exit /b 1
)

rem CMake and Ninja come from the Visual Studio developer environment.
set "PATH=%SystemRoot%\System32;%SystemRoot%\System32\Wbem;%SystemRoot%\WindowsPowerShell\v1.0\"
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 -vcvars_ver=14.44.35207
if errorlevel 1 exit /b %ERRORLEVEL%

rem Portable NSIS lives in third_party (not on system PATH/registry); cpack finds makensis via PATH.
set "NSISDIR=%ROOT%third_party\nsis\electron-builder-binaries-nsis-3.0.5.0\nsis"
if not exist "%NSISDIR%\makensis.exe" (
  echo NSIS_NOT_FOUND "%NSISDIR%"
  exit /b 1
)
set "PATH=%PATH%;%NSISDIR%"

pushd "%ROOT%"
cmake --preset view3d-release
if errorlevel 1 goto :failed
cmake --build --preset view3d-release
if errorlevel 1 goto :failed
popd

pushd "%ROOT%out\release"
cpack -G NSIS
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" goto :failedrc
echo VIEW3D_PACKAGE_OK
exit /b 0

:failed
set "RC=%ERRORLEVEL%"
popd
:failedrc
echo VIEW3D_PACKAGE_FAILED rc=%RC%
exit /b %RC%