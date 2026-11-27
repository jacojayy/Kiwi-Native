@echo off
rem Kiwi Native's builder on Windows: .nsp / .xci (or a .nup, or a dump folder) -> .nup -> pack -> (with --push) the phone.
rem Runs kiwi_build.py with the packer's Python (scripts\kiwinative.bat python: set up on first use, see that file).
rem Usage: kiwi\scripts\kiwi-build.bat <input>... [options] [-- pack_android.py options]   (see kiwi\docs\BUILDER.md)
call "%~dp0..\..\scripts\kiwinative.bat" python "%~dp0kiwi_build.py" %*
exit /b %errorlevel%
