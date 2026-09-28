@echo off
setlocal
title ISO Package Manager - build executable
cd /d "%~dp0"

set "EXE_NAME=ISO Package Manager"
set "ENTRY=ipm_launcher.py"

echo ============================================================
echo  Building "%EXE_NAME%.exe"  (single file, console launcher)
echo ============================================================
echo.

echo [1/4] Checking Python and tkinter ...
where python >nul 2>nul
if errorlevel 1 (
    echo   [x] python is not on PATH. Install Python 3.10+ and retry.
    pause
    exit /b 1
)
python -c "import tkinter" >nul 2>nul
if errorlevel 1 (
    echo   [x] tkinter is missing from this Python install ^(needed for the UI^).
    pause
    exit /b 1
)
python -c "import sys;print('   python', sys.version.split()[0])"

echo.
echo [2/4] Checking PyInstaller ...
python -m PyInstaller --version >nul 2>nul
if errorlevel 1 (
    echo   installing PyInstaller ...
    python -m pip install --disable-pip-version-check pyinstaller
    if errorlevel 1 (
        echo   [x] pip install pyinstaller failed.
        pause
        exit /b 1
    )
)
for /f "delims=" %%v in ('python -m PyInstaller --version') do echo   PyInstaller %%v

set "EXTRA="
python -c "import webview" >nul 2>nul
if not errorlevel 1 (
    echo   pywebview detected - bundling the in-app browser backend
    set "EXTRA=--collect-all webview --hidden-import webview.platforms.edgechromium --hidden-import webview.platforms.winforms"
) else (
    echo   pywebview not installed - the in-app browser falls back to the default browser
)

REM Carry a CA bundle inside the .exe so HTTPS does not depend on whatever
REM certificate store the machine running it happens to have.
python -c "import certifi" >nul 2>nul
if errorlevel 1 (
    echo   certifi missing - installing it so the .exe carries a CA bundle
    python -m pip install --disable-pip-version-check --quiet certifi >nul 2>nul
)
set "CERTS="
python -c "import certifi" >nul 2>nul
if not errorlevel 1 (
    echo   bundling the certifi CA bundle
    set "CERTS=--collect-data certifi"
)

echo.
echo [3/4] Building (this takes a minute) ...
python -m PyInstaller --noconfirm --clean --onefile --console ^
    --name "%EXE_NAME%" ^
    --distpath dist --workpath build --specpath build ^
    --hidden-import ipm_cli --hidden-import ipm_ia --hidden-import ipm_search ^
    --hidden-import ipm_windows --hidden-import ipm_winops --hidden-import ipm_http ^
    --hidden-import ipm_models --hidden-import ipm_utils ^
    --hidden-import main --hidden-import ipm_themes --hidden-import ipm_shop ^
    --add-data "%~dp0themes;themes" ^
    %CERTS% %EXTRA% "%ENTRY%"
if errorlevel 1 (
    echo   [x] build failed - scroll up for the PyInstaller error.
    pause
    exit /b 1
)

echo.
echo [4/4] Done.
dir /b "dist\%EXE_NAME%.exe"
echo.
echo   run:  "dist\%EXE_NAME%.exe"                    (asks: UI or Terminal)
echo         "dist\%EXE_NAME%.exe" --cli search ubuntu
echo         "dist\%EXE_NAME%.exe" --gui
echo.
pause
endlocal
