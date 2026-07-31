@echo off
setlocal
set "PLUGINS=%USERPROFILE%\Documents\GWToolboxpp\%COMPUTERNAME%\plugins"
if not exist "%PLUGINS%" mkdir "%PLUGINS%"
explorer "%PLUGINS%"
echo.
echo Drop GWDash.dll into this folder.
echo Drop GWDash.core.dll into the GWDash\ subfolder.
echo Then enable the plugin under Toolbox -^> Settings -^> Plugins.
endlocal
