@echo off
setlocal
if exist "%ProgramFiles%\Thorlabs\Kinesis\Thorlabs.MotionControl.Benchtop.StepperMotor.dll" set "PATH=%ProgramFiles%\Thorlabs\Kinesis;%PATH%"
start "" /D "%~dp0." "%~dp0frontend.exe"
