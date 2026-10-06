#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PC Monitor -> ESP32 (Cheap Yellow Display) — консольная версия.
================================================================
Читает датчики ПК и шлёт их ESP32 по USB-Serial (115200 8N1) раз в секунду.
Выполняет команды с экрана ESP32: sleep / reboot / shutdown.

Полный набор датчиков (температуры, обороты, мощности) даёт LibreHardwareMonitor:
приложение автоматически найдёт его (D:\\LibreHardwareMonitor, рядом с exe,
%LOCALAPPDATA%\\CydMonitor\\...) и включит его веб-сервер (порт 8085).

Запуск:
  python pc_monitor.py                # авто-поиск COM-порта (CH340)
  python pc_monitor.py --port COM3    # указать порт
  python pc_monitor.py --no-commands  # не выполнять команды с ESP32
  python pc_monitor.py --lhm          # предварительно поднять LHM (UAC) и ждать
"""
import argparse
import sys

from engine import Monitor, ensure_lhm, pick_port

try:
    import serial  # noqa: F401  (проверка наличия)
except ImportError:
    print("Нет pyserial. Установите: pip install -r pc/requirements.txt")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description='PC Monitor -> ESP32')
    ap.add_argument('--port', default=None, help='COM-порт ESP32 (авто-поиск, если не указан)')
    ap.add_argument('--interval', type=float, default=1.0, help='период отправки, сек (default 1.0)')
    ap.add_argument('--no-commands', action='store_true', help='не выполнять команды с ESP32')
    ap.add_argument('--debug', action='store_true', help='печатать строки, отправляемые в ESP32')
    ap.add_argument('--lhm', action='store_true',
                    help='гарантировать работающий LibreHardwareMonitor перед стартом')
    args = ap.parse_args()

    if args.lhm:
        ensure_lhm(on_log=lambda m: print('  ', m), elevated_start=True)

    port = pick_port(args.port)
    if not port:
        print("Порт не найден. Укажите: --port COM3")
        sys.exit(1)
    print("Порт:", port)

    mon = Monitor(port=port, interval=args.interval, no_commands=args.no_commands,
                  on_log=lambda m: print(' ', m),
                  on_data=(lambda f: print('>>', ','.join('%s=%s' % (k, v) for k, v in f.items())))
                  if args.debug else None)

    print("Пошла передача. Ctrl+C — выход.")
    mon.start()
    try:
        while mon.running:
            import time
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nВыход.")
    finally:
        mon.stop()


if __name__ == '__main__':
    main()
