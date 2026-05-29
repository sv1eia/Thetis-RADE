@echo off
REM Build rade.lib from radae_c with the Thetis MSVC toolchain (v145).
REM Run after the vendored opus_dnn is built (thetis_rade_opuslib.bat).
REM When radae_c lives under "Project Files\lib\radae_c", OpusDir defaults
REM to the sibling ..\..\opus_dnn; override with a 2nd arg if needed.
set MSBUILD="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
%MSBUILD% "%~dp0radae_c.vcxproj" -p:Configuration=Release -p:Platform=x64 %* -v:minimal -nologo -m
