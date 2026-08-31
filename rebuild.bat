@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\Maksym\Downloads\flyengine
cmake -B out/build/x64-Debug -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/x64-Debug --parallel