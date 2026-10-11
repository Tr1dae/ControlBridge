@echo off
rem Builds the Phase 0 probes and the offline self-test into bin\ (low priority, one core).
rem   bin\control\winmm.dll                  Control (2019) probe, a winmm.dll proxy
rem   bin\resonant\cb_probe_resonant.dll     CONTROL Resonant probe, a crloader mod
rem   bin\selftest.exe                       offline test of the probe core
setlocal
cd /d "%~dp0"
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSDIR=
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with x64 C++ tools found & exit /b 1)
echo Visual Studio: %VSDIR%
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

for %%d in (bin bin\control bin\resonant obj obj\minhook obj\control obj\resonant obj\selftest) do if not exist %%d mkdir %%d
set CL_COMMON=/nologo /O2 /MT /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /Ithird_party\minhook\include
set CXX=%CL_COMMON% /EHsc /std:c++20 /permissive-
rem start /wait /b does not pass the exit code on: each step is checked by its output file instead
set RUN=start "" /low /affinity 1 /wait /b

if exist obj\minhook\hde64.obj del /q obj\minhook\*.obj
%RUN% cl %CL_COMMON% /c third_party\minhook\src\buffer.c third_party\minhook\src\hook.c third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c /Foobj\minhook\ /Fdobj\minhook\
if not exist obj\minhook\hde64.obj goto fail
set MH=obj\minhook\buffer.obj obj\minhook\hook.obj obj\minhook\trampoline.obj obj\minhook\hde64.obj

if exist bin\control\winmm.dll del bin\control\winmm.dll
%RUN% cl %CXX% /LD src\probe_control\probe_control.cpp %MH% /Foobj\control\ /Fdobj\control\ /Febin\control\winmm.dll /link /DEBUG /OPT:REF psapi.lib user32.lib
if not exist bin\control\winmm.dll goto fail
if exist bin\resonant\cb_probe_resonant.dll del bin\resonant\cb_probe_resonant.dll
%RUN% cl %CXX% /LD src\probe_resonant\probe_resonant.cpp %MH% /Foobj\resonant\ /Fdobj\resonant\ /Febin\resonant\cb_probe_resonant.dll /link /DEBUG /OPT:REF psapi.lib user32.lib
if not exist bin\resonant\cb_probe_resonant.dll goto fail
if exist bin\selftest.exe del bin\selftest.exe
%RUN% cl %CXX% tests\selftest.cpp %MH% /Foobj\selftest\ /Fdobj\selftest\ /Febin\selftest.exe /link /DEBUG psapi.lib user32.lib
if not exist bin\selftest.exe goto fail
if not exist obj\linktest mkdir obj\linktest
if exist bin\link_test.exe del bin\link_test.exe
%RUN% cl %CXX% tests\link_test.cpp /Foobj\linktest\ /Fdobj\linktest\ /Febin\link_test.exe /link /DEBUG
if not exist bin\link_test.exe goto fail
echo BUILD OK
exit /b 0
:fail
echo BUILD FAILED
exit /b 1
