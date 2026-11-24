@echo off
echo Building chiaki-ng for Xbox... everything is logged to build_output.txt
echo The first run downloads and compiles libraries and can take 30+ minutes.
cmd /c ""%~dp0build-inner.bat"" > "%~dp0build_output.txt" 2>&1
echo.
echo Done. Results are in build_output.txt.
pause
