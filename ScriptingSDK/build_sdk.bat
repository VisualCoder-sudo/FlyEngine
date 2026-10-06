@echo off
setlocal enabledelayedexpansion

:: Builds the FlyScript SDK + sample scripts into FlyScript.dll
:: Usage: build_sdk.bat [project_path]
:: If project_path is given, the result is copied to <project_path>\Scripts\,
:: which is where CoreCLRHost looks for it.

set "SDK_DIR=%~dp0FlyScript"

:: ---------------------------------------------------------------- dotnet
:: Hardcoding a path here meant the script only ran on the machine it was
:: written on. Honour DOTNET_ROOT, then a dotnet already on PATH, then the
:: standard per-user install location.
set "DOTNET="

if defined DOTNET_ROOT if exist "%DOTNET_ROOT%\dotnet.exe" set "DOTNET=%DOTNET_ROOT%\dotnet.exe"

if not defined DOTNET (
    for /f "delims=" %%d in ('where dotnet 2^>nul') do (
        if not defined DOTNET set "DOTNET=%%d"
    )
)

if not defined DOTNET if exist "%USERPROFILE%\.dotnet\dotnet.exe" set "DOTNET=%USERPROFILE%\.dotnet\dotnet.exe"

if not defined DOTNET (
    echo [build_sdk] error: no dotnet found.
    echo   Install the .NET 8 SDK, or set DOTNET_ROOT to its location.
    exit /b 1
)

if not exist "%SDK_DIR%\FlyScript.csproj" (
    echo [build_sdk] error: FlyScript.csproj not found at %SDK_DIR%
    exit /b 1
)

set "RID=%FLYSDK_RID%"
if not defined RID set "RID=win-x64"
set "OUT_DIR=%SDK_DIR%\bin\Release\net8.0\publish"

echo.
echo === Building FlyScript SDK + sample scripts ===
echo   dotnet : %DOTNET%
echo   rid    : %RID%
echo   out    : %OUT_DIR%

:: Nothing here needs the MSVC toolchain: this is a managed assembly. The old
:: version shelled out to a hardcoded vcvars64.bat first, which meant a machine
:: without Visual Studio could not build the C# side at all.

"%DOTNET%" publish "%SDK_DIR%\FlyScript.csproj" -c Release -o "%OUT_DIR%" --nologo -r "%RID%" --self-contained false
if errorlevel 1 (
    echo [build_sdk] dotnet publish failed
    exit /b 1
)

set "DLL=%OUT_DIR%\FlyScript.dll"
if not exist "%DLL%" (
    echo [build_sdk] error: FlyScript.dll not found at %DLL%
    exit /b 1
)

echo [build_sdk] built %DLL%

if not "%~1"=="" (
    set "SCRIPTS_DIR=%~1\Scripts"
    if not exist "!SCRIPTS_DIR!" mkdir "!SCRIPTS_DIR!"
    copy /Y "%DLL%" "!SCRIPTS_DIR!\FlyScript.dll" >nul
    echo [build_sdk] copied to !SCRIPTS_DIR!\FlyScript.dll
)

echo.
echo === Build complete ===
exit /b 0
