@echo off
rem ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
setlocal

if defined VSCMD_VER goto :have_vs

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
	for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR if exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" goto :init_vs

for %%d in (
	"C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
	"C:\Program Files\Microsoft Visual Studio\2022\Professional"
	"C:\Program Files\Microsoft Visual Studio\2022\Community"
	"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
) do (
	if exist "%%~d\VC\Auxiliary\Build\vcvars32.bat" (
		set "VSDIR=%%~d"
		goto :init_vs
	)
)

echo ERROR: Visual Studio 2022 C++ x86 tools not found.
exit /b 1

:init_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1

:have_vs
cd /d "%~dp0"
if not exist build\obj mkdir build\obj

set "MD_FXC=fxc.exe"
if exist "%WindowsSdkVerBinPath%x64\fxc.exe" set "MD_FXC=%WindowsSdkVerBinPath%x64\fxc.exe"
"%MD_FXC%" /nologo /WX /O3 /T ps_2_0 /E main /Vn kPlayerMaterialPS /Fh build\obj\player_material_bc.h src\gfx\shaders\player_material.hlsl || exit /b 1
"%MD_FXC%" /nologo /WX /O3 /T ps_4_0 /E main /Vn kRoadPuddlesPS /Fh build\obj\road_puddles_bc.h src\gfx\shaders\road_puddles.hlsl || exit /b 1
"%MD_FXC%" /nologo /WX /O3 /T ps_5_0 /E main /Vn kAoReusePS /Fh build\obj\ao_reuse_bc.h src\gfx\shaders\ao_reuse.hlsl || exit /b 1

cl /nologo /MP /c /O2 /MT /EHsc /W4 /WX- /std:c++20 /utf-8 /DNDEBUG /DNOMINMAX /Isrc /Ibuild\obj /Fobuild\obj\ ^
	src\main.cpp src\core\*.cpp src\gfx\*.cpp || exit /b 1

link /nologo /DLL "/OUT:build\ModernDisplay.asi" build\obj\*.obj ^
	user32.lib kernel32.lib ole32.lib windowscodecs.lib d3d11.lib dxgi.lib dxguid.lib || exit /b 1

echo OK: "build\ModernDisplay.asi"
