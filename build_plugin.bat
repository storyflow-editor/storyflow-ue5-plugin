@echo off
setlocal

set PLUGIN=%~dp0StoryFlowPlugin.uplugin
set OUTPUT=%~dp0..\..\..\Builds
set ENGINES=C:\Program Files\Epic Games

echo ============================================
echo  StoryFlow Plugin - Multi-Version Builder
echo ============================================
echo.
echo Plugin: %PLUGIN%
echo Output: %OUTPUT%
echo.

for %%V in (5.3 5.4 5.5 5.6 5.7) do (
    echo --------------------------------------------
    echo  Building for UE %%V
    echo --------------------------------------------

    if exist "%OUTPUT%\StoryFlowPlugin_%%V" (
        echo Cleaning previous build...
        rmdir /s /q "%OUTPUT%\StoryFlowPlugin_%%V"
    )

    "%ENGINES%\UE_%%V\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
        -Plugin="%PLUGIN%" ^
        -Package="%OUTPUT%\StoryFlowPlugin_%%V" ^
        -TargetPlatforms=Win64 ^
        -Rocket

    if errorlevel 1 (
        echo [FAILED] UE %%V build failed!
        echo.
    ) else (
        echo [OK] UE %%V build succeeded.
        echo.
    )
)

echo ============================================
echo  All builds finished.
echo ============================================

endlocal
pause
