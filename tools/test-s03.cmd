@echo off
setlocal
cd /d "%~dp0.."
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "S03_VS=%%i"
if not defined S03_VS exit /b 1
call "%S03_VS%\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64 >nul
if errorlevel 1 exit /b 1
if not exist artifacts\S03\tests mkdir artifacts\S03\tests
cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /I src\WeldScope.App tests\sample_session_tests.cpp /Foartifacts\S03\tests\sample_session_tests.obj /Feartifacts\S03\tests\sample_session_tests.exe
if errorlevel 1 exit /b 1
artifacts\S03\tests\sample_session_tests.exe
