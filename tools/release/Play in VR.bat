@echo off
rem Wave Race 64: Recompiled, in VR (PC VR through OpenXR).
rem Connect the headset first -- Quest Link, Air Link, Virtual Desktop or
rem SteamVR -- so its OpenXR runtime is running, then start this. Without it
rem the game starts on the monitor as usual (WaveRace64Recomp.exe).
set WR64_VR=1
cd /d "%~dp0"
start "" "%~dp0WaveRace64Recomp.exe" %*
