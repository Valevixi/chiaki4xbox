@echo on
setlocal EnableExtensions
cd /d "%~dp0"
set MSBUILDDISABLENODEREUSE=1
echo ==== build started %date% %time%

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
echo ==== after vcvars

set "GITDIR=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd"
set "PATH=%PATH%;%GITDIR%"

if defined VCPKG_ROOT goto :havevcpkg
if exist "%VSPATH%\VC\vcpkg\vcpkg.exe" set "VCPKG_ROOT=%VSPATH%\VC\vcpkg"
if defined VCPKG_ROOT goto :havevcpkg
if exist "%~dp0vcpkg\vcpkg.exe" goto :ownvcpkg
git clone https://github.com/microsoft/vcpkg.git "%~dp0vcpkg"
call "%~dp0vcpkg\bootstrap-vcpkg.bat" -disableMetrics
:ownvcpkg
set "VCPKG_ROOT=%~dp0vcpkg"
:havevcpkg
echo VCPKG_ROOT=%VCPKG_ROOT%
where cmake
where git

echo ==== configure
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-uwp
if errorlevel 1 goto :cfgfail

echo ==== build
cmake --build build --config Release -- /m /nr:false /v:minimal /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly /p:GenerateAppxPackageOnBuild=true "/p:AppxPackageDir=%~dp0AppPackages\\"
if errorlevel 1 goto :buildfail
echo ==== BUILD OK
goto :end

:novs
echo ==== ERROR: Visual Studio not found
goto :end
:cfgfail
echo ==== CONFIGURE FAILED
goto :end
:buildfail
echo ==== BUILD FAILED
:end
echo ==== finished %date% %time%
