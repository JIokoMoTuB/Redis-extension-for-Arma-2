@echo off
setlocal enabledelayedexpansion

echo =======================================================
echo Building RedisExt.dll for Arma 2 (x86 32-bit)
echo =======================================================

:: 1. Search for Visual Studio installation via vswhere or standard locations
set "VS_PATH="
set "VCVARS="

if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_PATH=%%i"
    )
)

if defined VS_PATH (
    if exist "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" (
        set "VCVARS=!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat"
    )
)

:: Fallback check standard directories
if not defined VCVARS (
    for %%p in (
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    ) do (
        if exist %%p if not defined VCVARS set "VCVARS=%%~p"
    )
)

if not defined VCVARS (
    echo [ERROR] Visual Studio / MSVC build tools not found!
    echo Please install Visual Studio 2019/2022 with C++ Desktop Development tools.
    exit /b 1
)

echo [INFO] Found MSVC environment: "!VCVARS!"
call "!VCVARS!" x86
if errorlevel 1 (
    echo [ERROR] Failed to initialize x86 MSVC environment.
    exit /b 1
)

:: 2. Locate hiredis include and library directories
set "HIREDIS_INC="
set "HIREDIS_LIB="

if defined VCPKG_ROOT (
    if exist "%VCPKG_ROOT%\installed\x86-windows\include\hiredis\hiredis.h" (
        set "HIREDIS_INC=%VCPKG_ROOT%\installed\x86-windows\include"
        set "HIREDIS_LIB=%VCPKG_ROOT%\installed\x86-windows\lib"
    )
)

if not defined HIREDIS_INC (
    if exist "%USERPROFILE%\vcpkg\installed\x86-windows\include\hiredis\hiredis.h" (
        set "HIREDIS_INC=%USERPROFILE%\vcpkg\installed\x86-windows\include"
        set "HIREDIS_LIB=%USERPROFILE%\vcpkg\installed\x86-windows\lib"
    )
)

if not defined HIREDIS_INC (
    if exist "%~dp0include\hiredis\hiredis.h" (
        set "HIREDIS_INC=%~dp0include"
        set "HIREDIS_LIB=%~dp0lib"
    )
)

if not defined HIREDIS_INC (
    echo [ERROR] hiredis headers/libraries not found!
    echo Please install hiredis via vcpkg:
    echo   vcpkg install hiredis:x86-windows
    exit /b 1
)

echo [INFO] hiredis Include: "!HIREDIS_INC!"
echo [INFO] hiredis Lib:     "!HIREDIS_LIB!"

:: 3. Prepare bin output folder
if not exist "%~dp0bin" mkdir "%~dp0bin"

:: 4. Compile RedisExt.dll
echo [INFO] Compiling src\dllmain.cpp...
cl.exe /nologo /LD /EHsc /O2 /W3 /wd4200 /wd4244 ^
    /I "!HIREDIS_INC!" ^
    /Fe:"%~dp0bin\RedisExt.dll" ^
    "%~dp0src\dllmain.cpp" ^
    /link /LIBPATH:"!HIREDIS_LIB!" hiredis.lib /DYNAMICBASE /NXCOMPAT

if errorlevel 1 (
    echo [ERROR] Compilation failed!
    exit /b 1
)

:: 5. Copy hiredis runtime DLL if present
if exist "!HIREDIS_LIB!\..\bin\hiredis.dll" (
    copy /y "!HIREDIS_LIB!\..\bin\hiredis.dll" "%~dp0bin\hiredis.dll" >nul
)

:: Clean up temporary link files
del /q "%~dp0bin\RedisExt.lib" "%~dp0bin\RedisExt.exp" dllmain.obj 2>nul

echo =======================================================
echo [SUCCESS] Build completed successfully:
echo Output: %~dp0bin\RedisExt.dll
echo =======================================================
exit /b 0
