if exist "%SERVER_FOLDER%" rmdir "%SERVER_FOLDER%" /s /q

xcopy shell\Release "%SERVER_FOLDER%" /E /I /Y
xcopy flashtemplatehost-prefix\src\flashtemplatehost "%SERVER_FOLDER%\" /E /I /Y

copy %1\src\shell\casparcg_auto_restart.bat "%SERVER_FOLDER%\"

echo Copying documentation...
copy %1\CHANGELOG.md "%SERVER_FOLDER%"
copy %1\LICENSE "%SERVER_FOLDER%"
copy %1\README.md "%SERVER_FOLDER%"

rem The MIT license of the OMT libraries asks for its notice to come with them.
if exist omt-src\libomt\LICENSE.txt (
    mkdir "%SERVER_FOLDER%\licenses"
    copy omt-src\libvmx\LICENSE.txt "%SERVER_FOLDER%\licenses\libvmx.txt"
    copy omt-src\libomtnet\LICENSE.txt "%SERVER_FOLDER%\licenses\libomtnet.txt"
    copy omt-src\libomt\LICENSE.txt "%SERVER_FOLDER%\licenses\libomt.txt"
)

if exist "%MEDIA_SCANNER_FOLDER%" (
    xcopy "%MEDIA_SCANNER_FOLDER%" "%SERVER_FOLDER%" /E /I /Y
    rem media-scanner releases ship the exe with a versioned name, but casparcg_auto_restart.bat expects scanner.exe
    for %%F in ("%SERVER_FOLDER%\casparcg-scanner-*.exe") do (
        echo %%~nxF> "%SERVER_FOLDER%\scanner.version.txt"
        ren "%%F" scanner.exe
    )
)