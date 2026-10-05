@echo off
setlocal DisableDelayedExpansion
pushd "%~dp0"
set "POIMA_LAUNCH_ROOT=%~dp0"
set "poima_desktop=build\desktop-win-x64\Poima.Editor.exe"
rem Read only a validated ASCII build-relative path, never arbitrary batch text.
for /f "usebackq delims=" %%P in (`powershell.exe -NoProfile -NonInteractive -Command "$p = Join-Path $env:POIMA_LAUNCH_ROOT 'build\desktop-current.txt'; if (Test-Path -LiteralPath $p -PathType Leaf) { $v = [IO.File]::ReadAllText($p); if ($v -cmatch '\Abuild[\\/][A-Za-z0-9 _./\\-]+[\\/]Poima\.Editor\.exe\r?\n\z' -and $v -notmatch '(^|[\\/])\.\.?([\\/]|$)' -and $v -notmatch '[. ]([\\/]|$)') { $v = $v.TrimEnd([char]13, [char]10); if (Test-Path -LiteralPath (Join-Path $env:POIMA_LAUNCH_ROOT $v) -PathType Leaf) { [Console]::WriteLine($v) } } }"`) do set "poima_desktop=%%P"
if not exist "%poima_desktop%" (
  echo Build the desktop prototype with python3 scripts/build_desktop.py from WSL first.
  echo See docs\DESKTOP_EDITOR.md.
  pause
  popd
  exit /b 1
)
if "%~1"=="" (
  if not exist "projects\desktop-sandbox" mkdir "projects\desktop-sandbox"
  start "" /wait "%poima_desktop%" "projects\desktop-sandbox\world.json" --endpoint poima-desktop
) else (
  start "" /wait "%poima_desktop%" "%~1" --endpoint poima-desktop
)
if errorlevel 1 pause
popd
endlocal
