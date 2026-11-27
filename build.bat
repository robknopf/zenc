@echo off
setlocal enabledelayedexpansion

rem Compiler configuration (default to gcc)
if "%CC%"=="" set CC=gcc

rem Version — try git, then .version file, then "unknown"
set ZEN_VERSION=unknown
for /f "delims=" %%i in ('git describe --tags --always --dirty 2^>nul') do set ZEN_VERSION=%%i
if "!ZEN_VERSION!"=="unknown" (
    if exist .version (
        set /p ZEN_VERSION=<.version
    )
)

rem Compilation flags
if "%C_STD%"=="" (
    rem (gnu23 where the compiler knows it, as the Makefile does; else gnu11)
    echo int main(void^){return 0;} > zc_std_probe.c
    %CC% -std=gnu23 -fsyntax-only zc_std_probe.c >nul 2>&1 && set C_STD=gnu23 || set C_STD=gnu11
    del zc_std_probe.c
)
set CFLAGS=-std=%C_STD% -Wall -Wextra -Wshadow -Wformat=2 -Wmissing-prototypes ^
 -Wstrict-prototypes -Wnull-dereference -Wundef -Wfloat-equal ^
 -Wmissing-field-initializers -Wsign-compare -Wtype-limits -Wuninitialized ^
 -Wdouble-promotion -Wtautological-compare -Wshift-negative-value ^
 -Wdangling-else -Wreturn-local-addr -Wconversion -Wno-sign-conversion -Wno-float-conversion ^
 -Wswitch-default -Wvla -Wimplicit-fallthrough -Wredundant-decls -Wcast-align ^
 -Wpacked -Wdisabled-optimization -Wduplicated-cond -Wlogical-op -Wformat-signedness ^
 -g ^
 -I./src -I./src/ast -I./src/parser -I./src/codegen -I./plugins -I./src/zen ^
 -I./src/utils -I./src/lexer -I./src/analysis -I./src/lsp -I./src/diagnostics
set CFLAGS=%CFLAGS% -DZEN_VERSION=\"%ZEN_VERSION%\" -DZEN_SHARE_DIR=\".\" -DZC_ALLOW_INTERNAL

rem Feature flags
set CFLAGS=%CFLAGS% -DZC_HAS_LSP=1 -DZC_HAS_REPL=1 -DZC_HAS_PLUGINS=1 -DZC_HAS_ZEN=1
set CFLAGS=%CFLAGS% -DZC_HAS_CPP_BACKEND=1 -DZC_HAS_CUDA_BACKEND=1 -DZC_HAS_OBJC_BACKEND=1
set CFLAGS=%CFLAGS% -DZC_HAS_JSON_BACKEND=1 -DZC_HAS_LISP_BACKEND=1 -DZC_HAS_DOT_BACKEND=1
set CFLAGS=%CFLAGS% -DZC_HAS_ASTDUMP_BACKEND=1

rem zc's own tcc (vendor\tinycc, see vendor\README.md), built below into tcc\ as the Makefile
rem and cmake\ZcTcc.cmake build it: the REPL's JIT and std\reload.zc's Reloader use it.
if "%ZC_HAS_JIT%"=="" set ZC_HAS_JIT=1
if "%ZC_HAS_JIT%"=="1" (
    call :build_tcc
    if errorlevel 1 (
        echo Building tcc failed!
        exit /b 1
    )
    set CFLAGS=%CFLAGS% -DZC_HAS_JIT -Itcc
    set LIBS=tcc\libtcc.a -lws2_32
) else (
    set LIBS=-lws2_32
)

if "%NO_PLUGINS%"=="1" (
    set CFLAGS=%CFLAGS% -DZC_NO_PLUGINS
)

rem Source files — read from src-sources.txt (single source of truth)
set SRCS=
for /f "usebackq delims=" %%i in ("src-sources.txt") do (
    if not "%%i"=="" set "SRCS=!SRCS! %%i"
)

rem Vendored C (vendor\README.md): tree-sitter and its C grammar, with their own flags
echo Building vendored tree-sitter...
%CC% -std=gnu11 -O2 -w -Ivendor/tree-sitter/include -Ivendor/tree-sitter/src -c vendor/tree-sitter/src/lib.c -o zc_vendor_ts.o
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
%CC% -std=gnu11 -O2 -w -c vendor/tree-sitter-c/src/parser.c -o zc_vendor_tsc.o
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

rem Build
echo Building Zen C (%ZEN_VERSION%)...
%CC% %CFLAGS% -Ivendor/tree-sitter/include %SRCS% zc_vendor_ts.o zc_vendor_tsc.o -o zc.exe %LIBS%
if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b %ERRORLEVEL%
)

echo Build success! zc.exe created.

rem Build plugins
if "%NO_PLUGINS%"=="1" (
    echo Plugins disabled by NO_PLUGINS flag.
    goto end
)
echo Building plugins...
if not exist plugins mkdir plugins
for %%f in (plugins\*.zc) do (
    echo Compiling native plugin %%f...
    .\zc.exe build %%f -shared -o %%~dpnf.dll
    if errorlevel 1 (
        echo Plugin build failed for %%f
        exit /b 1
    )
)

:end
goto :eof

rem zc's own tcc into tcc\, as the Makefile and cmake\ZcTcc.cmake build it (keep the three in
rem step): its config.h; tccdefs.h made into C (c2str); libtcc.c compiled as one source;
rem tcc.c linked with it; and libtcc1.a and the runtime's objects compiled by that tcc. Laid
rem out as tcc's lib path is on Windows: include\ (with tcc's Windows headers), lib\
rem (libtcc1.a, the runtime's objects, the DLLs' .def files), and libtcc.a, libtcc.h,
rem zc_tcc.h and tcc.exe at the top. Skipped when tcc\libtcc.a is there (delete tcc\ to
rem rebuild it).
:build_tcc
if exist tcc\libtcc.a exit /b 0
echo Building tcc (vendor\tinycc)...
if "%AR%"=="" set AR=ar
set TS=vendor\tinycc
set TB=obj\tinycc
if exist %TB% rmdir /s /q %TB%
if exist tcc rmdir /s /q tcc
mkdir %TB%\lib tcc\include tcc\lib
%CC% -DC2STR -o %TB%\c2str.exe %TS%\conftest.c || exit /b 1
%TB%\c2str.exe %TS%\include\tccdefs.h %TB%\tccdefs_.h || exit /b 1
set /p TCC_VERSION=<%TS%\VERSION
>%TB%\config.h echo #define TCC_VERSION "%TCC_VERSION%"
>>%TB%\config.h echo #define TCC_TARGET_X86_64 1
>>%TB%\config.h echo #define TCC_TARGET_PE 1
>>%TB%\config.h echo #define CONFIG_TCC_PREDEFS 1
%CC% -O2 -w -fno-strict-aliasing -I%TB% -c %TS%\libtcc.c -o %TB%\libtcc.o || exit /b 1
%AR% rcs %TB%\libtcc.a %TB%\libtcc.o || exit /b 1
%CC% -O2 -w -fno-strict-aliasing -I%TB% -DONE_SOURCE=0 -c %TS%\tcc.c -o %TB%\tcc.o || exit /b 1
%CC% -o tcc\tcc.exe %TB%\tcc.o %TB%\libtcc.a -static || exit /b 1
set TCC_B=-B%TS%\win32 -I%TS%\include -I%TB%
set TCC_LIB1_OBJS=
for %%f in (lib\libtcc1.c lib\stdatomic.c lib\atomic.S lib\builtin.c lib\alloca.S lib\alloca-bt.S lib\tcov.c win32\lib\chkstk.S win32\lib\crt1.c win32\lib\crt1w.c win32\lib\wincrt1.c win32\lib\wincrt1w.c win32\lib\dllcrt1.c win32\lib\dllmain.c win32\lib\winex.c) do (
    tcc\tcc.exe %TCC_B% -c %TS%\%%f -o %TB%\lib\%%~nf.o || exit /b 1
    set "TCC_LIB1_OBJS=!TCC_LIB1_OBJS! %TB%\lib\%%~nf.o"
)
tcc\tcc.exe -ar rcs tcc\lib\libtcc1.a !TCC_LIB1_OBJS! || exit /b 1
for %%f in (lib\runmain.c lib\bt-exe.c lib\bt-log.c lib\bt-dll.c) do (
    tcc\tcc.exe %TCC_B% -c %TS%\%%f -o tcc\lib\%%~nf.o || exit /b 1
)
tcc\tcc.exe %TCC_B% -bt -c %TS%\lib\bcheck.c -o tcc\lib\bcheck.o || exit /b 1
tcc\tcc.exe %TCC_B% -bt -DBCHECK_RUN -c %TS%\lib\bcheck.c -o tcc\lib\bcheck_run.o || exit /b 1
copy /y %TS%\include\*.h tcc\include >nul
copy /y %TS%\tcclib.h tcc\include >nul
xcopy /e /i /q /y %TS%\win32\include tcc\include >nul
copy /y %TS%\win32\lib\*.def tcc\lib >nul
copy /y %TS%\libtcc.h tcc >nul
copy /y %TS%\zc_tcc.h tcc >nul
copy /y %TB%\libtcc.a tcc >nul
echo Built tcc: tcc\
exit /b 0
