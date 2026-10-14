# RedisExt Compilation & Build Guide

RedisExt is built as a **32-bit (x86) Windows Dynamic Link Library (`.dll`)** to maintain full compatibility with the Arma 2 dedicated server (`arma2oaserver.exe`).

---

## Prerequisites

1. **Operating System:** Windows 7/10/11 or Windows Server 2016/2019/2022.
2. **C++ Compiler:**
   * Microsoft Visual Studio 2019 or 2022 (Community, Professional, Enterprise)
   * OR Visual Studio Build Tools with the **C++ Desktop Development** workload installed.
3. **Package Manager:** [vcpkg](https://github.com/microsoft/vcpkg) for `hiredis`.

---

## 1. Installing Hiredis Dependency

Clone and set up vcpkg if not already installed:

```cmd
git clone https://github.com/microsoft/vcpkg.git %USERPROFILE%\vcpkg
cd %USERPROFILE%\vcpkg
bootstrap-vcpkg.bat
```

Install the **32-bit (x86)** static or dynamic hiredis package:

```cmd
vcpkg install hiredis:x86-windows
```

---

## 2. Compiling with `build.bat` (Recommended)

The repository includes a ready-to-use build script:

1. Double-click or run `build.bat` from Developer Command Prompt or regular Command Prompt:
   ```cmd
   build.bat
   ```
2. The script will automatically:
   * Detect your MSVC installation (Build Tools / Visual Studio).
   * Initialize the 32-bit (`x86`) compiler environment via `vcvarsall.bat x86`.
   * Find `hiredis` in your vcpkg installation.
   * Compile `src/dllmain.cpp` with release optimizations (`/O2 /EHsc`).
   * Output `RedisExt.dll` into the `bin/` directory.

---

## 3. Compiling Manually via MSVC Command Line

Open **x86 Native Tools Command Prompt for VS 2022** (or VS 2019):

```cmd
cl.exe /LD /EHsc /O2 /W3 /wd4200 /wd4244 ^
    /I "%USERPROFILE%\vcpkg\installed\x86-windows\include" ^
    /Fe:"bin\RedisExt.dll" ^
    "src\dllmain.cpp" ^
    /link /LIBPATH:"%USERPROFILE%\vcpkg\installed\x86-windows\lib" hiredis.lib
```

---

## 4. Compiling with CMake

```cmd
mkdir build
cd build
cmake .. -A Win32 -DCMAKE_TOOLCHAIN_FILE="%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build . --config Release
```

The resulting `RedisExt.dll` will be output directly into `bin/`.

---

## 5. Runtime Deployment

To deploy on your Arma 2 server:
1. Copy `bin/RedisExt.dll` and `bin/hiredis.dll` to the **root directory of your Arma 2 server** (where `arma2oaserver.exe` is located).
2. Copy `bin/RedisExt.ini` into the same directory and configure your Redis connection parameters (IP, Port, Password).
