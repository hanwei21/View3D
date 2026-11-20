@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo VSDEVCMD_NOT_FOUND
  exit /b 1
)

rem CMake and Ninja come from the Visual Studio developer environment.
set "PATH=%SystemRoot%\System32;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0\"
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 -vcvars_ver=14.44.35207
if errorlevel 1 exit /b %ERRORLEVEL%

pushd "%ROOT%"
cmake --preset view3d-release
if errorlevel 1 goto :failed
cmake --build --preset view3d-release
if errorlevel 1 goto :failed
ctest --preset view3d-release --output-on-failure
if errorlevel 1 goto :failed
popd
echo VIEW3D_BUILD_OK
exit /b 0

:failed
set "RC=%ERRORLEVEL%"
popd
echo VIEW3D_BUILD_FAILED rc=%RC%
exit /b %RC%
