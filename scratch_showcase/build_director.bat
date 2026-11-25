@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /W3 /std:c++20 /DNDEBUG /DNOMINMAX showcase_director.cpp /LD /Fe:ShowcaseDirector.asi user32.lib d3d11.lib dxgi.lib || exit /b 1
echo BUILT ShowcaseDirector.asi OK
