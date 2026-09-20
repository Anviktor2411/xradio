@echo off
REM Double-click this to cut a release. It opens here, asks for the version
REM number, runs whatever checks this machine can run, and writes the zip
REM into dist\. Nothing is changed if a check fails.
cd /d "%~dp0"
where py >nul 2>nul && (py -3 tools\release.py %*) || (python tools\release.py %*)
echo.
pause
