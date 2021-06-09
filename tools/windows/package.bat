if exist "%SERVER_FOLDER%" rmdir "%SERVER_FOLDER%" /s /q
mkdir "%SERVER_FOLDER%"

del shell\*_debug.dll
del shell\*-d-2.dll

copy shell\*.dll "%SERVER_FOLDER%\server"
copy shell\Release\casparcg.exe "%SERVER_FOLDER%\server"
copy shell\casparcg.config "%SERVER_FOLDER%\server"
copy shell\*.ttf "%SERVER_FOLDER%\server"
copy shell\*.pak "%SERVER_FOLDER%\server"
copy shell\*.bin "%SERVER_FOLDER%\server"
copy shell\*.dat "%SERVER_FOLDER%\server"
xcopy shell\locales "%SERVER_FOLDER%\server\locales" /E /I /Y
xcopy shell\swiftshader "%SERVER_FOLDER%\server\swiftshader" /E /I /Y

copy %1\src\shell\casparcg_auto_restart.bat "%SERVER_FOLDER%\server"
xcopy %1\resources\windows\flash-template-host-files "%SERVER_FOLDER%" /E /I /Y

echo Copying documentation...
copy %1\CHANGELOG.md "%SERVER_FOLDER%"
copy %1\LICENSE.md "%SERVER_FOLDER%"
copy %1\README.md "%SERVER_FOLDER%"
