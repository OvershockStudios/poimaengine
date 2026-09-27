@echo off
setlocal
pushd "%~dp0"
if not exist "build\windows-runtime\poima.exe" (
  echo Build the Windows editor first. See docs\EDITOR.md.
  pause
  popd
  exit /b 1
)
if "%~1"=="" (
  if not exist "projects\sandbox" mkdir "projects\sandbox"
  "build\windows-runtime\poima.exe" editor "projects\sandbox\world.json" --endpoint sandbox --width 1440 --height 900
) else (
  "build\windows-runtime\poima.exe" editor "%~1" --endpoint editor --width 1440 --height 900
)
if errorlevel 1 pause
popd
endlocal
