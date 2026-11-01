@echo off
rem ---------------------------------------------------------------------------
rem  Builds the game modules as QVMs with the q3lcc/q3asm tools of the solution:
rem
rem    <output dir>\oe\vm\oegame.qvm     Orcs & Elves     (src\cgame_oe  + src\cgame_common)
rem    <output dir>\oe2\vm\oegame.qvm    Orcs & Elves II  (src\cgame_oe2 + src\cgame_common)
rem    <output dir>\base\vm\ui.qvm       the menus without fs_game (src\ui)
rem
rem    build_qvm.cmd <output dir> [rebuild|clean]
rem
rem  <output dir> is the solution's build folder (..\build\). The tools are taken
rem  from <output dir>\tools. oe_game.c goes first: the interpreter starts at
rem  vmMain, which must be the first function of the QVM.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "OUT=%~1"
if "%OUT%"=="" (
	echo usage: build_qvm.cmd ^<output dir^> [rebuild^|clean]
	exit /b 1
)
if not "%OUT:~-1%"=="\" set "OUT=%OUT%\"
set "MODE=%~2"

set "SRC=%~dp0..\src"
set "TMPD=%~dp0..\build\obj\qvm"
set "LCC=%OUT%tools\q3lcc.exe"
set "ASM=%OUT%tools\q3asm.exe"

if /i "%MODE%"=="clean" (
	if exist "%TMPD%" rd /s /q "%TMPD%"
	if exist "%OUT%oe\vm\oegame.qvm" del /q "%OUT%oe\vm\oegame.qvm"
	if exist "%OUT%oe2\vm\oegame.qvm" del /q "%OUT%oe2\vm\oegame.qvm"
	if exist "%OUT%base\vm\ui.qvm" del /q "%OUT%base\vm\ui.qvm"
	exit /b 0
)
rem (every build recompiles all QVM sources)
if exist "%TMPD%" rd /s /q "%TMPD%"

if not exist "%LCC%" (
	echo build_qvm.cmd : error : "%LCC%" not found, build the q3lcc project first
	exit /b 1
)
if not exist "%ASM%" (
	echo build_qvm.cmd : error : "%ASM%" not found, build the q3asm project first
	exit /b 1
)

call :game oe || exit /b 1
call :game oe2 || exit /b 1
call :ui || exit /b 1
exit /b 0

rem ---- :ui: <output dir>\base\vm\ui.qvm, the menus without fs_game (src\ui) -------
:ui
set "OBJ=%TMPD%\ui"
set "VMDIR=%OUT%base\vm"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%VMDIR%" mkdir "%VMDIR%"
set "FILES="
pushd "%SRC%\ui"
rem ui_main.c first (vmMain); ui_syscalls.c is the DLL's, the QVM has ui_syscalls.asm
call :cc ui_main.c || (popd & exit /b 1)
call :cc ui_menu.c || (popd & exit /b 1)
call :cc ui_parse.c || (popd & exit /b 1)
call :cc ..\cgame_common\vm_libc.c || (popd & exit /b 1)
popd
echo q3asm base\vm\ui.qvm
"%ASM%" -o "%VMDIR%\ui.qvm" !FILES! "%SRC%\ui\ui_syscalls.asm"
if errorlevel 1 (
	echo build_qvm.cmd : error : q3asm failed for ui
	exit /b 1
)
exit /b 0

rem ---- :game <oe|oe2> --------------------------------------------------------
:game
set "G=%~1"
set "OBJ=%TMPD%\%G%"
set "VMDIR=%OUT%%G%\vm"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%VMDIR%" mkdir "%VMDIR%"
set "FILES="
rem q3cpp resolves -I relative to the source file, so compile from the game folder
pushd "%SRC%\cgame_%G%"
call :cc ..\cgame_common\oe_game.c || (popd & exit /b 1)
for %%f in (*.c) do (
	call :cc %%f || (popd & exit /b 1)
)
call :cc ..\cgame_common\brew_shim.c || (popd & exit /b 1)
call :cc ..\cgame_common\oe_hw.c || (popd & exit /b 1)
call :cc ..\cgame_common\oe_options.c || (popd & exit /b 1)
call :cc ..\cgame_common\oe_export.c || (popd & exit /b 1)
call :cc ..\cgame_common\moddata.c || (popd & exit /b 1)
call :cc ..\cgame_common\vm_libc.c || (popd & exit /b 1)
popd
echo q3asm %G%\vm\oegame.qvm
"%ASM%" -o "%VMDIR%\oegame.qvm" !FILES! "%SRC%\cgame_common\oe_syscalls.asm"
if errorlevel 1 (
	echo build_qvm.cmd : error : q3asm failed for %G%
	exit /b 1
)
exit /b 0

rem ---- :cc <source> ----------------------------------------------------------
:cc
set "ASMF=%OBJ%\%~n1.asm"
"%LCC%" -Wf-unsigned_char=1 -DQ3_VM -Dinline= -I../cgame_common/vmlibc -I../cgame_common -o "%ASMF%" "%~1" 2> "%OBJ%\%~n1.err"
if errorlevel 1 (
	type "%OBJ%\%~n1.err"
	echo build_qvm.cmd : error : q3lcc failed on %~1
	exit /b 1
)
findstr /v /c:"warning" "%OBJ%\%~n1.err" > nul && type "%OBJ%\%~n1.err"
set "FILES=!FILES! "%ASMF%""
exit /b 0
