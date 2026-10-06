@echo off
rem Запуск монитора ПК -> ESP32
cd /d "%~dp0"
python pc_monitor.py %*
pause