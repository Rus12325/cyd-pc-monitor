@echo off
rem Сборка CYD PC Monitor в один .exe (БЕЗ обязательного админа: приложение
rem работает без UAC, а LHM стартует отдельно — задачей планировщика с админ-правами)
cd /d "%~dp0"
python -m PyInstaller --noconfirm --clean ^
  --onefile --windowed ^
  --name "CYD-PC-Monitor" ^
  --icon app_icon.ico ^
  --version-file version_info.txt ^
  --add-data "_bundle\librehardwaremonitor.zip;." ^
  --hidden-import pystray ^
  --hidden-import serial ^
  --hidden-import pycaw.pycaw ^
  --hidden-import pycaw.utils ^
  --collect-all comtypes ^
  --collect-all pycaw ^
  cyd_app.py
echo.
echo === Готово: dist\CYD-PC-Monitor.exe ===
