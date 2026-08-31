@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake --build out/build/x64-Debug --parallel
echo EXITCODE=%ERRORLEVEL%