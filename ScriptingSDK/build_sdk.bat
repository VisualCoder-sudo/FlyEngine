@echo off
setlocal enabledelayedexpansion

:: Build the FlyScript SDK + sample scripts into FlyScript.dll
:: Usage: build_sdk.bat [project_path]
:: If project_path is given, copies FlyScript.dll to <project>/Scripts/

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [build_sdk] vcvars64.bat failed
    exit /b 1
)

set "PATH=C:\Users\Maksym\.dotnet;%PATH%"
set "DOTNET=C:\Users\Maksym\.dotnet\dotnet.exe"
set "SDK_DIR=%~dp0..\ScriptingSDK\FlyScript"

if not exist "%SDK_DIR%\FlyScript.csproj" (
    echo [build_sdk] FlyScript.csproj not found at %SDK_DIR%
    exit /b 1
)

echo.
echo === Building FlyScript SDK + sample scripts ===
"%DOTNET%" publish "%SDK_DIR%\FlyScript.csproj" -c Release -o "%SDK_DIR%\bin\Release\net8.0\publish" --nologo -r win-x64 --self-contained false
if errorlevel 1 (
    echo [build_sdk] dotnet publish failed
    exit /b 1
)

:: The publish output has FlyScript.dll
set "OUTPUT_DLL=%SDK_DIR%\bin\Release\net8.0\publish\FlyScript.dll"
if not exist "%OUTPUT_DLL%" (
    echo [build_sdk] FlyScript.dll not found at %OUTPUT_DLL%
    exit /b 1
)

echo [build_sdk] Built FlyScript.dll successfully

:: If project path provided, copy to project's Scripts/ folder
if "%~1" NEQ "" (
    set "PROJECT_PATH=%~1"
    set "SCRIPTS_DIR=%PROJECT_PATH%\Scripts"
    if not exist "%SCRIPTS_DIR%" mkdir "%SCRIPTS_DIR%"
    copy /Y "%OUTPUT_DLL%" "%SCRIPTS_DIR%\FlyScript.dll" >nul
    echo [build_sdk] Copied FlyScript.dll to %SCRIPTS_DIR%
)

echo.
echo === Build complete ===
exit /b 0