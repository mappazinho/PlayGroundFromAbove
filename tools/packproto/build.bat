@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /std:c++17 /EHsc /DUNICODE /D_UNICODE /arch:AVX2 /MT /Fe:packproto.exe packproto.cpp /link Psapi.lib
