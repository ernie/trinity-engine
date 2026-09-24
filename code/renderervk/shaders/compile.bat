@echo off
setlocal
cd /d "%~dp0"
set "cl=%VULKAN_SDK%\Bin\glslangValidator.exe"
set "bh=%~dp0bin2hex.exe"
set "tmpf=%~dp0spirv\data.spv"
set "outf=+%~dp0spirv\shader_data.c"
if not exist spirv mkdir spirv
if exist spirv\shader_data.c del /Q spirv\shader_data.c
rem Columns are stage, array name, source file, variants, then up to five
rem defines. Variants are "+"-separated, or "-" for none.
for /f "tokens=1,2,3,*" %%a in (shaders.list) do (
	call :compile %%a %%b %%c %%d
	if errorlevel 1 exit /b 1
)
if exist "%tmpf%" del /Q "%tmpf%"
echo shader_data.c regenerated
exit /b 0

:compile
call :toomany %*
if errorlevel 1 exit /b 1
set "variants=%4"
set "variants=%variants:+= %"
call :variant %1 %2 %3 %5 %6 %7 %8 %9
if errorlevel 1 exit /b 1
for %%v in (%variants%) do (
	call :extra %%v %1 %2 %3 %5 %6 %7 %8 %9
	if errorlevel 1 exit /b 1
)
exit /b 0

rem Batch addressing stops at %9, so a sixth define needs an explicit check.
:toomany
shift
if not "%9"=="" (
	echo shaders.list: "%1" has more than five defines 1>&2
	exit /b 1
)
exit /b 0

rem Each variant appends a second module named "<array name>_<variant>".
:extra
if "%1"=="-" exit /b 0
set "vdef="
if "%1"=="mv" set "vdef=-DMULTIVIEW"
if "%1"=="array" set "vdef=-DARRAY_SOURCE"
if not defined vdef (
	echo unknown shader variant "%1" 1>&2
	exit /b 1
)
call :variant %2 %3_%1 %4 %5 %6 %7 %8 %9 %vdef%
exit /b 0

:variant
"%cl%" -S %1 -V -o "%tmpf%" %3 %4 %5 %6 %7 %8 %9
if errorlevel 1 exit /b 1
"%bh%" "%tmpf%" "%outf%" %2
if errorlevel 1 exit /b 1
exit /b 0
