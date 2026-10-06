# -*- coding: utf-8 -*-
"""
Движок монитора ПК -> ESP32 (CYD 2USB).
=======================================
Один общий код для CLI (pc_monitor.py) и GUI-приложения (cyd_app.py).

Возможности:
  * поиск и запуск LibreHardwareMonitor с авто-включением его веб-сервера
    (конфиг runWebServerMenuItem=true, порт 127.0.0.1:8085);
  * чтение датчиков через HTTP-сервер LHM (или WMI, или psutil/nvidia-smi);
  * отправка строки данных в ESP32 раз в секунду;
  * приём команд с экрана ESP32: sleep / reboot / shutdown.
"""
import json
import os
import platform
import re
import socket
import subprocess
import sys
import threading
import time

import psutil

SYS = platform.system()
APP_DIR = os.path.dirname(os.path.abspath(__file__))


# ----------------------------------------------------------------------------
#  LibreHardwareMonitor: поиск, конфиг, запуск
# ----------------------------------------------------------------------------
LHM_PORT = 8085
LHM_URL = 'http://127.0.0.1:%d/data.json' % LHM_PORT


def lhm_candidate_dirs():
    """Где ищем LibreHardwareMonitor.exe (порядок важен)."""
    dirs = []
    # рядом с exe / исходниками
    here = os.path.dirname(os.path.abspath(sys.argv[0] or __file__))
    dirs.append(os.path.join(here, 'LibreHardwareMonitor'))
    dirs.append(os.path.join(APP_DIR, 'LibreHardwareMonitor'))
    # локальный appdata (куда мы распаковываем встроенную копию)
    la = os.environ.get('LOCALAPPDATA')
    if la:
        dirs.append(os.path.join(la, 'CydMonitor', 'LibreHardwareMonitor'))
    # известные пути на этой машине
    for d in (r'D:\LibreHardwareMonitor', r'C:\Program Files\LibreHardwareMonitor',
              r'C:\Program Files (x86)\LibreHardwareMonitor'):
        dirs.append(d)
    return dirs


def find_lhm_exe():
    for d in lhm_candidate_dirs():
        p = os.path.join(d, 'LibreHardwareMonitor.exe')
        if os.path.isfile(p):
            return p
    return None


def http_up(url=None, timeout=2.0):
    """Жив ли веб-сервер LHM (отвечает /data.json)."""
    import urllib.request
    try:
        req = urllib.request.Request(url or LHM_URL, headers={'User-Agent': 'cyd-monitor'})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = r.read()
            return r.status == 200 and len(data) > 100
    except Exception:
        return False


def ensure_lhm_config(exe_path, ip='127.0.0.1', port=LHM_PORT):
    """Прописывает в LibreHardwareMonitor.exe.config автозапуск веб-сервера.

    LHM v0.9.6 читает appSettings из своего exe.config при старте
    (UserOption срабатывает сразу при подписке -> StartHttpListener).
    """
    cfg = os.path.join(os.path.dirname(exe_path), 'LibreHardwareMonitor.exe.config')
    if not os.path.isfile(cfg):
        return False
    with open(cfg, 'r', encoding='utf-8-sig') as f:
        text = f.read()

    keys = {
        'runWebServerMenuItem': 'true',
        'listenerIp': ip,
        'listenerPort': str(port),
        'authenticationEnabled': 'false',
        'startMinMenuItem': 'true',
        'minTrayMenuItem': 'true',
        # ---- ускорение первичного опроса железа ----
        # На некоторых машинах LHM подолгу/намертво зависает, перечисляя
        # сетевые адаптеры / диски / БП / батарею. Эти датчики экрану не нужны
        # (сеть и память мы берём из psutil), поэтому отключаем их опрос.
        'nicMenuItem': 'false',
        'hddMenuItem': 'false',
        'psuMenuItem': 'false',
        'batteryMenuItem': 'false',
        'powerMonitorMenuItem': 'false',
        'logSensorsMenuItem': 'false',
    }

    def set_key(m):
        key = m.group(1)
        if key in keys:
            return '<add key="%s" value="%s" />' % (key, keys[key])
        return m.group(0)

    if '<appSettings>' in text:
        text = re.sub(r'<add key="([^"]+)"[^>]*/>', set_key, text)
        # на случай отсутствующих ключей — добавим недостающие
        for k, v in keys.items():
            if ('key="%s"' % k) not in text:
                text = text.replace('</appSettings>',
                                    '    <add key="%s" value="%s" />\n  </appSettings>' % (k, v))
    else:
        adds = '\n  <appSettings>\n' + \
               ''.join('    <add key="%s" value="%s" />\n' % (k, v) for k, v in keys.items()) + \
               '  </appSettings>\n'
        text = text.replace('</configuration>', adds + '</configuration>')

    with open(cfg, 'w', encoding='utf-8') as f:
        f.write(text)
    return True


def is_elevated():
    """Запущен ли процесс с правами администратора (Windows)."""
    if SYS != 'Windows':
        return True
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def ensure_lhm_installed():
    """Если LHM нигде не найден — распаковать встроенный бандл в %LOCALAPPDATA%.\n
    Возвращает путь к exe или None.
    """
    if find_lhm_exe():
        return find_lhm_exe()
    la = os.environ.get('LOCALAPPDATA')
    if not la:
        return None
    target = os.path.join(la, 'CydMonitor', 'LibreHardwareMonitor')
    exe = os.path.join(target, 'LibreHardwareMonitor.exe')
    if os.path.isfile(exe):
        return exe
    # встроенный архив: рядом с exe (PyInstaller datas) или рядом с engine.py
    import zipfile
    cands = []
    if getattr(sys, 'frozen', False):
        base = os.path.dirname(sys.executable)
        cands.append(os.path.join(base, 'librehardwaremonitor.zip'))
        meipass = getattr(sys, '_MEIPASS', None)
        if meipass:
            cands.append(os.path.join(meipass, 'librehardwaremonitor.zip'))
    cands.append(os.path.join(APP_DIR, '_bundle', 'librehardwaremonitor.zip'))
    zpath = next((c for c in cands if os.path.isfile(c)), None)
    if not zpath:
        return None
    try:
        os.makedirs(target, exist_ok=True)
        with zipfile.ZipFile(zpath) as z:
            z.extractall(target)
    except Exception:
        return None
    return exe if os.path.isfile(exe) else None


def lhm_process_running():
    try:
        out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq LibreHardwareMonitor.exe',
                              '/NH'], capture_output=True, text=True, timeout=10)
        return 'LibreHardwareMonitor.exe' in out.stdout
    except Exception:
        return False


# ----------------------------------------------------------------------------
#  Автозапуск LHM через Планировщик заданий Windows (без окон UAC)
# ----------------------------------------------------------------------------
# Задача с RunLevel=HighestAvailable стартует LHM с правами администратора
# в момент входа в Windows — молча, без единого окна подтверждения.
LHM_TASK = 'CYD-LibreHardwareMonitor'


def _schtasks_cmd(cmd):
    """schtasks с русской локалью: смотрим только код возврата."""
    try:
        r = subprocess.run(cmd, capture_output=True)
        return r.returncode
    except Exception:
        return -1


def lhm_task_exists():
    return _schtasks_cmd(['schtasks', '/Query', '/TN', LHM_TASK]) == 0


def lhm_task_register(exe_path=None):
    """Создаёт (или обновляет) задачу автозапуска LHM при входе в Windows.
    Требует прав администратора — обычно вызывается один раз.
    """
    exe = exe_path or find_lhm_exe()
    if not exe:
        return False
    return _schtasks_cmd(_lhm_create_cmd(exe)) == 0


def lhm_task_run():
    """Мгновенно запускает LHM через задачу (молча, с правами админа)."""
    return _schtasks_cmd(['schtasks', '/Run', '/TN', LHM_TASK]) == 0


def _lhm_create_cmd(exe):
    """Команда регистрации: /TR "\"C:\\path\\app.exe\"" — стандартная
    форма для путей с пробелами (кавычки сохраняются в самой задаче).
    """
    return ('schtasks /Create /TN "%s" /TR "\\\"%s\\\"" '
            '/SC ONLOGON /RL HIGHEST /F') % (LHM_TASK, exe)


def lhm_task_ensure(exe_path=None, on_log=None):
    """Гарантирует наличие задачи автозапуска. Если прав нет — один раз
    показывает UAC (только первая настройка), дальше всё без окон.
    """
    log = on_log or (lambda m: None)
    if lhm_task_exists():
        return True
    exe = exe_path or find_lhm_exe()
    if not exe:
        return False
    if is_elevated():
        ok = lhm_task_register(exe)
        log('Задача автозапуска LHM: %s' % ('OK' if ok else 'ошибка регистрации'))
        return ok
    # один раз попросить права на регистрацию задачи (UAC)
    cmd = _lhm_create_cmd(exe)
    try:
        import ctypes
        r = ctypes.windll.shell32.ShellExecuteW(
            None, 'runas', 'schtasks.exe', cmd, None, 0)
        if r > 32 and lhm_task_exists():
            log('Задача автозапуска LHM зарегистрирована (больше окон не будет).')
            return True
    except Exception:
        pass
    return False


def start_lhm(elevated=False, force_uac=False):
    """Запускает LHM. Приоритет — Планировщик заданий (без UAC-окон).

    Порядок:
      1) если есть задача автозапуска — стартуем через неё (молча, с админ-правами);
      2) если процесс уже администраторский — обычный Popen без UAC;
      3) только если явно попросили (elevated/force_uac) — окно UAC.
    """
    exe = find_lhm_exe()
    if not exe:
        return None
    if lhm_task_exists():
        if lhm_task_run():
            return exe
    if SYS == 'Windows' and is_elevated():
        subprocess.Popen([exe], cwd=os.path.dirname(exe))
        return exe
    if SYS == 'Windows' and (elevated or force_uac):
        import ctypes
        r = ctypes.windll.shell32.ShellExecuteW(
            None, 'runas', exe, '', os.path.dirname(exe), 1)
        if r <= 32:
            return None
        return exe
    if SYS != 'Windows':
        subprocess.Popen([exe], cwd=os.path.dirname(exe))
        return exe
    return None


# ----------------------------------------------------------------------------
#  Поиск COM-порта ESP32
# ----------------------------------------------------------------------------
def pick_port(prefer=None, on_log=None):
    import serial.tools.list_ports as lp
    ports = list(lp.comports())
    log = on_log or (lambda m: None)
    if prefer:
        for p in ports:
            if prefer.lower() in p.device.lower():
                return p.device
    for p in ports:  # CH340 / CH9102 — штатный мост CYD
        desc = (p.description or '').lower()
        if p.vid == 0x1A86 or 'ch340' in desc or 'ch910' in desc:
            return p.device
    if len(ports) == 1:
        return ports[0].device
    log('Доступные порты: ' + ', '.join(p.device for p in ports) or 'нет')
    return None


# ----------------------------------------------------------------------------
#  HTTP-бэкенд (LibreHardwareMonitor v0.9.6, веб-сервер)
# ----------------------------------------------------------------------------
class HttpSensors:
    def __init__(self, port=LHM_PORT):
        self._urls = ['http://127.0.0.1:%d/data.json' % port]
        ips = []
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(('8.8.8.8', 80))
            ips.append(s.getsockname()[0])
            s.close()
        except Exception:
            pass
        try:
            for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
                if ip not in ips and not ip.startswith('127.'):
                    ips.append(ip)
        except Exception:
            pass
        self._urls += ['http://%s:%d/data.json' % (ip, port) for ip in ips]
        self._ok_idx = -1

    @staticmethod
    def available():
        try:
            import urllib.request  # noqa: F401
            return True
        except Exception:
            return False

    def read(self):
        import urllib.request
        err = None
        for i, url in enumerate(self._urls):
            try:
                req = urllib.request.Request(url, headers={'User-Agent': 'cyd-monitor'})
                with urllib.request.urlopen(req, timeout=4) as r:
                    root = json.loads(r.read().decode('utf-8', 'replace'))
                self._ok_idx = i
                break
            except Exception as e:
                err = e
                continue
        else:
            raise err if err else RuntimeError('LHM HTTP unavailable')

        out = []

        def walk(node):
            typ = node.get('Type')
            if typ:
                if typ == 'Frequency':
                    typ = 'Clock'
                rv = node.get('RawValue')
                if rv is None:
                    rv = node.get('Value')
                value = parse_num(rv)
                if value is not None and value == value:
                    out.append({'type': typ, 'name': node.get('Text') or '', 'value': value})
            for ch in node.get('Children') or []:
                walk(ch)

        walk(root)
        return out


def parse_num(v):
    if v is None:
        return None
    if isinstance(v, (int, float)):
        return float(v)
    m = re.match(r'^\s*([-+]?\d+(?:[.,]\d+)?)', str(v))
    if not m:
        return None
    try:
        return float(m.group(1).replace(',', '.'))
    except ValueError:
        return None


# ----------------------------------------------------------------------------
#  WMI-бэкенд (LibreHardwareMonitor / OpenHardwareMonitor, старые версии)
# ----------------------------------------------------------------------------
class WmiSensors:
    def __init__(self, namespace):
        import wmi
        self._w = wmi.WMI(namespace=namespace)

    @staticmethod
    def available():
        try:
            import wmi  # noqa: F401
            return True
        except Exception:
            return False

    def read(self):
        try:
            sensors = self._w.Sensor()
        except Exception:
            return []
        out = []
        for s in sensors:
            v = getattr(s, 'Value', None)
            name = getattr(s, 'Name', '') or ''
            stype = getattr(s, 'SensorType', '') or ''
            if v is not None:
                try:
                    out.append({'type': stype, 'name': name, 'value': float(v)})
                except (TypeError, ValueError):
                    pass
        return out


# ----------------------------------------------------------------------------
#  Маппинг датчиков в поля протокола
# ----------------------------------------------------------------------------
def pick(sensors, stype, kws, exclude=()):
    """Выбирает датчик, в имени которого есть один из kws (в порядке приоритета).
    Ни одно ключевое слово не совпало -> None (не первый попавшийся!).
    """
    best, best_score = None, 0
    for s in sensors:
        if s['type'] != stype:
            continue
        n = s['name'].lower()
        if any(e in n for e in exclude):
            continue
        for i, k in enumerate(kws):
            if k in n:
                score = 100 - i
                if score > best_score:
                    best_score, best = score, s
                break
    return best['value'] if best else None


def pick_many(sensors, stype, kws, count, exclude=()):
    used, res = [], []
    for k in kws:
        for s in sensors:
            if s['type'] != stype or s in used:
                continue
            n = s['name'].lower()
            if any(e in n for e in exclude):
                continue
            if k in n:
                used.append(s)
                res.append(s['value'])
                break
        if len(res) >= count:
            break
    return res


def _core_fields(sensors):
    """Температуры и загрузка по ядрам CPU (c0t..c7t / c0l..c7l)."""
    f = {}
    temps, loads = [], {}
    for s in sensors:
        n = (s['name'] or '').strip().lower()
        if s['type'] == 'Temperature':
            m = re.match(r'^cpu core #(\d+)$', n)
            if m:
                temps.append((int(m.group(1)), s['value']))
        elif s['type'] == 'Load':
            m = re.match(r'^cpu core #(\d+) thread #(\d+)$', n)
            if m:
                loads.setdefault(int(m.group(1)), []).append(s['value'])
    for n, (idx, v) in enumerate(sorted(temps)[:8]):
        f['c%dt' % n] = round(v, 1)
    for n, (idx, vs) in enumerate(sorted(loads.items())[:8]):
        f['c%dl' % n] = round(sum(vs) / float(len(vs)), 1)  # ядро = среднее по потокам
    return f


def read_wmi(backend):
    f = {}
    try:
        sens = backend.read()
    except Exception:
        return f
    if not sens:
        return f
    f.update(_core_fields(sens))
    f['cpu_temp'] = pick(sens, 'Temperature',
        ['cpu package', 'cpu', 'core', 'processor', 'tctl', 'tdie'],
        ('gpu', 'memory', 'motherboard', 'ssd', 'hdd', 'nvme', 'chipset'))
    f['gpu_temp'] = pick(sens, 'Temperature', ['gpu core', 'gpu', 'video'], ('cpu',))
    f['cpu_load'] = pick(sens, 'Load', ['cpu total', 'cpu'], ('gpu',))
    f['gpu_load'] = pick(sens, 'Load', ['gpu core', 'gpu'], ('cpu',))
    f['cpu_power'] = pick(sens, 'Power', ['cpu package power', 'cpu package', 'cpu', 'processor'])
    f['gpu_power'] = pick(sens, 'Power', ['gpu power', 'gpu'])
    f['gpu_freq'] = pick(sens, 'Clock', ['gpu core', 'gpu', 'video'],
                          ('cpu', 'memory', 'shader', 'processor'))
    # Температура материнки (Temperature #N на узле Motherboard) — как "SYS".
    # Настоящая температура SSD без прав админа недоступна (SMART WMI требует
    # админа), а узел материнки читается свободно.
    f['sys_temp'] = pick(sens, 'Temperature', ['temperature #2', 'temperature #1', 'temperature #3'],
                          ('cpu', 'gpu', 'core', 'ram', 'memory', 'ssd', 'hdd', 'nvme'))
    v12 = pick(sens, 'Voltage', ['+12v', '12v'])
    if v12 is not None and not (5.0 <= v12 <= 18.0):
        v12 = None
    f['v12'] = v12
    for i, v in enumerate(_pick_live_fans(sens)[:2]):
        f['fan%d' % (i + 1)] = v
    return {k: v for k, v in f.items() if v is not None}


def _pick_live_fans(sensors, count=2):
    """До двух реально крутящихся вентиляторов (RPM > 0).

    Многие платы показывают «фантомные» Fan #2..#N со значением 0 — такие
    отбрасываем, иначе на экране висит бессмысленный 0 RPM. Приоритет: сначала
    CPU-вентилятор, затем корпусные, затем GPU (если он реально есть).
    """
    cands = [s for s in sensors
             if s['type'] == 'Fan' and s['value'] and s['value'] > 0]

    def prio(s):
        n = (s['name'] or '').lower()
        if 'cpu' in n:
            return 0
        if any(k in n for k in ('system', 'case', 'chassis', 'rear', 'front', 'top')):
            return 1
        return 2  # gpu / прочие

    cands.sort(key=lambda s: (prio(s), -s['value']))
    return [s['value'] for s in cands[:count]]


def read_nvidia():
    f = {}
    try:
        out = subprocess.run(
            ['nvidia-smi', '--query-gpu=temperature.gpu,utilization.gpu,power.draw,clocks.gr',
             '--format=csv,noheader,nounits'],
            capture_output=True, text=True, timeout=5)
        if out.returncode != 0:
            return f
        parts = [p.strip() for p in out.stdout.strip().split(',')]
        vals = []
        for p in parts:
            try:
                vals.append(float(p))
            except ValueError:
                vals.append(None)
        if len(vals) >= 4:
            f['gpu_temp'], f['gpu_load'], f['gpu_power'], f['gpu_freq'] = vals[:4]
            f = {k: v for k, v in f.items() if v is not None}
    except Exception:
        pass
    return f


def read_linux_cpu_temp():
    try:
        temps = psutil.sensors_temperatures()
    except Exception:
        return None
    best = None
    for key, lst in temps.items():
        key = key.lower()
        if 'coretemp' in key or 'k10temp' in key:
            for e in lst:
                if e.current is not None:
                    name = (e.label or '').lower()
                    if 'tctl' in name or 'tdie' in name or 'package' in name or 'core' in name or not name:
                        best = max(best or 0, e.current)
        elif best is None and lst:
            for e in lst:
                if e.current is not None:
                    best = max(best or 0, e.current)
    return best


# ----------------------------------------------------------------------------
#  Виртуальные клавиши (Windows SendInput/keybd_event) для медиа и горячих клавиш
# ----------------------------------------------------------------------------
# Медиа-клавиши
MEDIA_VK = {
    'mute': 0xAD, 'voldn': 0xAE, 'volup': 0xAF,
    'prev': 0xB1, 'next': 0xB0, 'play': 0xB3,
}
# Клавиши-модификаторы и спецклавиши для разбора комбо вида "ctrl+shift+esc"
KEY_VK = {
    'ctrl': 0x11, 'control': 0x11, 'alt': 0x12, 'shift': 0x10, 'win': 0x5B,
    'esc': 0x1B, 'tab': 0x09, 'space': 0x20, 'enter': 0x0D, 'backspace': 0x08,
    'delete': 0x2E, 'del': 0x2E, 'insert': 0x2D, 'home': 0x24, 'end': 0x23,
    'pgup': 0x21, 'pgdn': 0x22, 'up': 0x26, 'down': 0x28, 'left': 0x25, 'right': 0x27,
    'printscreen': 0x2C, 'prtsc': 0x2C, 'pause': 0x13, 'capslock': 0x14,
}
# Быстрый запуск приложений по умолчанию (вкладка APPS): label + command.
# Command — путь к exe/lnk/url/документу или команда (shell).
DEFAULT_APPS = [
    {'label': 'Explorer',  'command': 'explorer.exe'},
    {'label': 'Terminal',  'command': 'cmd.exe'},
    {'label': 'Notepad',   'command': 'notepad.exe'},
    {'label': 'Calc',      'command': 'calc.exe'},
    {'label': 'TaskMgr',   'command': 'taskmgr.exe'},
    {'label': 'Paint',     'command': 'mspaint.exe'},
    {'label': 'Browser',   'command': 'https://www.google.com'},
    {'label': 'Downloads', 'command': 'explorer.exe shell:downloads'},
]


# Пороги по умолчанию (страница CTRL / тревоги). Синхронизированы с
# config.h на прошивке; отправляются на ESP32 при старте и при изменении.
#   r1on/r1off — авто-включение/выключение реле 1 (по CPU)
#   r2on/r2off — реле 2 (по GPU); warn/crit — зоны жёлтого и красного+зуммер
DEFAULT_THRESHOLDS = {
    'r1on': 60, 'r1off': 55,
    'r2on': 65, 'r2off': 60,
    'warn': 60, 'crit': 75,
}


def _thr_line(thr):
    """Собрать строку THR:... из словаря порогов (для отправки на ESP32)."""
    t = dict(DEFAULT_THRESHOLDS)
    if isinstance(thr, dict):
        for k in t:
            if k in thr:
                try:
                    t[k] = int(round(float(thr[k])))
                except (TypeError, ValueError):
                    pass
    # гистерезис: off < on и warn < crit — как на прошивке
    if t['r1off'] >= t['r1on']: t['r1off'] = max(20, t['r1on'] - 1)
    if t['r2off'] >= t['r2on']: t['r2off'] = max(20, t['r2on'] - 1)
    if t['warn'] >= t['crit']:  t['warn']  = max(20, t['crit'] - 1)
    return 'THR:' + ','.join('%s=%d' % (k, v) for k, v in t.items())


# Горячие клавиши по умолчанию (label, combo)
DEFAULT_HOTKEYS = [
    {'label': 'TaskMgr', 'combo': 'ctrl+shift+esc'},
    {'label': 'Desktop', 'combo': 'win+d'},
    {'label': 'Refresh', 'combo': 'f5'},
    {'label': 'Close', 'combo': 'ctrl+w'},
    {'label': 'Snap L', 'combo': 'win+left'},
    {'label': 'Snap R', 'combo': 'win+right'},
    {'label': 'PrScr', 'combo': 'printscreen'},
    {'label': 'Run', 'combo': 'win+r'},
]


def _vk_for(token):
    """Виртуальный код клавиши по имени токена комбо ('f5', 'a', 'ctrl'...)."""
    t = token.lower()
    if t in KEY_VK:
        return KEY_VK[t]
    if len(t) == 1 and t.isalpha():
        return ord(t.upper())          # 'a'..'z'
    if t.isdigit() and len(t) == 1:
        return 0x30 + int(t)           # '0'..'9'
    if t[0] == 'f' and t[1:].isdigit():
        n = int(t[1:])
        if 1 <= n <= 24:
            return 0x6F + n            # F1..F24
    return None


def _send_keys(keys):
    """Нажать последовательность VK-кодов через keybd_event (Windows)."""
    if SYS != 'Windows':
        return False
    try:
        import ctypes
        u = ctypes.windll.user32
        mods = [k for k in keys if k in (0x10, 0x11, 0x12, 0x5B)]
        rest = [k for k in keys if k not in (0x10, 0x11, 0x12, 0x5B)]
        for m in mods:
            u.keybd_event(m, 0, 0, 0)
        for k in rest:
            u.keybd_event(k, 0, 0, 0)
            u.keybd_event(k, 0, 2, 0)
        for m in reversed(mods):
            u.keybd_event(m, 0, 2, 0)
        return True
    except Exception:
        return False


def exec_media(action, no_commands=False):
    """Медиа-клавиша с экрана ESP32: mute/volup/voldn/play/next/prev."""
    if no_commands:
        return '[пропущен] media:' + str(action)
    vk = MEDIA_VK.get((action or '').strip().lower())
    if not vk:
        return '[неизвестное медиа-действие] ' + str(action)
    ok = _send_keys([vk])
    return ('-> media ' + str(action)) if ok else '[media недоступна (не Windows)]'


def exec_hotkey(combo, no_commands=False):
    """Комбинация горячей клавиши со страницы KEYS (например 'ctrl+shift+esc')."""
    if no_commands:
        return '[пропущен] key:' + str(combo)
    keys = []
    for token in (combo or '').split('+'):
        token = token.strip()
        if not token:
            continue
        vk = _vk_for(token)
        if vk is None:
            return '[непонятная клавиша в комбо] ' + token
        keys.append(vk)
    if not keys:
        return '[пустая комбинация]'
    ok = _send_keys(keys)
    return ('-> key ' + combo) if ok else '[клавиши недоступны (не Windows)]'


# ----------------------------------------------------------------------------
#  Команды с экрана ESP32
# ----------------------------------------------------------------------------
def _launch_command(cmd):
    """Запустить файл/URL/команду примерно как двойной клик мышью."""
    import os
    cmd = (cmd or '').strip()
    if not cmd:
        return False
    # URL / ссылка — открыть ассоциированной программой (браузер)
    if cmd.lower().startswith(('http://', 'https://', 'www.')):
        full = cmd if not cmd.lower().startswith('www.') else 'https://' + cmd
        os.startfile(full)
        return True
    # Путь в кавычках или одиночный токен — os.startfile (как двойной клик)
    if cmd.startswith('"') and cmd.count('"') == 2:
        path = cmd[1:-1].strip()
        if os.path.exists(path):
            os.startfile(path)
            return True
    if ' ' not in cmd and os.path.exists(cmd):
        os.startfile(cmd)
        return True
    if cmd.lower().startswith('start '):   # явная обёртка cmd
        os.startfile(cmd[6:].strip())
        return True
    # иначе — командная строка (shell=True): аргументы, системные утилиты
    import subprocess
    subprocess.Popen(cmd, shell=True)
    return True


def exec_app(cmd, no_commands=False):
    """Запуск приложения со вкладки APPS экрана ESP32."""
    if no_commands:
        return '[пропущен] app:' + str(cmd)
    try:
        if _launch_command(cmd):
            return '-> запуск: ' + str(cmd)
        return '[пустая команда]'
    except Exception as e:
        return 'Ошибка запуска: %s' % e


# ----------------------------------------------------------------------------
#  Громкость Windows и «сейчас играет» (для страницы MEDIA)
# ----------------------------------------------------------------------------
_VOL_EP = None


def read_volume():
    """Громкость Windows: 0..100, отрицательное = mute, None = недоступно."""
    global _VOL_EP
    if SYS != 'Windows':
        return None
    try:
        if _VOL_EP is None:
            from pycaw.pycaw import AudioUtilities
            _VOL_EP = AudioUtilities.GetSpeakers().EndpointVolume
        v = _VOL_EP.GetMasterVolumeLevelScalar() * 100.0
        if _VOL_EP.GetMute():
            v = -v
        return v
    except Exception:
        return None


# Известные десктопные плееры (окно которых содержит название трека)
_PLAYER_NAMES = {
    'spotify.exe', 'foobar2000.exe', 'winamp.exe', 'wacup.exe', 'aimp.exe',
    'vlc.exe', 'itunes.exe', 'musicbee.exe', 'mediamonkey.exe', 'mpc-hc.exe',
    'mpc-be.exe', 'wmplayer.exe', 'qmmp.exe', 'audacious.exe', 'deadbeef.exe',
    'fmedia.exe', 'smplayer.exe', 'potplayermini64.exe', 'potplayer64.exe',
}


def _clean_media_title(title, app):
    """Убрать из заголовка окна имя плеера/суффиксы (самый правый маркер)."""
    s = (title or '').strip()
    appbase = (app or '').replace('.exe', '').lower()
    low = s.lower()
    best = -1
    for marker in (' [', ' - ', ' – ', ' — ', ' | ', ' · '):
        idx = low.rfind(marker)
        if idx > 0:
            tail = s[idx + len(marker):].strip().rstrip(']')
            if appbase in tail.lower() or len(tail) <= len(appbase) + 4:
                if idx > best:
                    best = idx
    if best >= 0:
        s = s[:best].strip()
    s = s.strip(' -–—|·[]()')
    if s.lower() in ('spotify', 'spotify premium', appbase, 'vlc media player',
                     'itunes', 'aimp', 'winamp', 'foobar2000', 'media player'):
        return ''
    return s


def read_now_playing():
    """Название текущего трека (заголовки окон известных плееров)."""
    if SYS != 'Windows':
        return ''
    try:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        found = []  # (pid, title) в порядке z-порядка (сверху вниз)
        Proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def cb(hwnd, _lp):
            if not user32.IsWindowVisible(hwnd):
                return True
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            ln = user32.GetWindowTextLengthW(hwnd)
            if ln <= 0:
                return True
            buf = ctypes.create_unicode_buffer(ln + 1)
            user32.GetWindowTextW(hwnd, buf, ln + 1)
            t = buf.value.strip()
            if t:
                found.append((pid.value, t))
            return True

        user32.EnumWindows(Proc(cb), 0)
        if not found:
            return ''
        pids = {p for p, _ in found}
        names = {}
        for pr in psutil.process_iter(['pid', 'name']):
            if pr.info['pid'] in pids:
                names[pr.info['pid']] = (pr.info['name'] or '').lower()
        for pid, title in found:
            app = names.get(pid, '')
            if app in _PLAYER_NAMES:
                clean = _clean_media_title(title, app)
                if clean:
                    return clean
    except Exception:
        pass
    return ''


# ----------------------------------------------------------------------------
#  Топ-5 процессов по CPU (страница PROCS)
# ----------------------------------------------------------------------------
class _ProcTracker:
    """Держит объекты psutil между циклами, чтобы cpu_percent() считал дельту."""

    def __init__(self):
        self._procs = {}

    def top(self, n=5):
        seen = {}
        for pr in psutil.process_iter(['pid', 'name']):
            pid = pr.info['pid']
            obj = self._procs.get(pid) or pr
            self._procs[pid] = obj
            try:
                # ровно один вызов за цикл: дельта к прошлому циклу (~1 c)
                seen[pid] = (obj, obj.cpu_percent(None) or 0.0)
            except Exception:
                pass
        for pid in [k for k in self._procs if k not in seen]:
            try:
                self._procs.pop(pid, None)
            except Exception:
                pass
        rows = []
        for pid, (obj, cpu) in seen.items():
            if cpu is None:
                cpu = -1.0
            try:
                mem = obj.memory_info().rss / 1048576.0
            except Exception:
                mem = 0.0
            try:
                name = obj.name()
            except Exception:
                name = '?'
            rows.append((cpu, name, mem, pid))
        rows.sort(reverse=True, key=lambda r: r[0])
        out = []
        for cpu, name, mem, pid in rows:
            low = (name or '').lower()
            if pid == os.getpid() or pid <= 0:
                continue       # не показываем самих себя и служебные
            if low in ('system idle process', 'system', 'registry', 'memory compression'):
                continue
            if cpu < 0:
                continue
            out.append((name, cpu, mem, pid))
            if len(out) >= n:
                break
        return out


def exec_kill(pid, no_commands=False):
    """Завершить процесс по pid (страница PROCS, после подтверждения)."""
    if no_commands:
        return '[пропущен] kill:' + str(pid)
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return '[неверный pid] %s' % pid
    if pid <= 4 or pid == os.getpid():
        return '[защищённый pid] %d' % pid
    try:
        r = subprocess.run(['taskkill', '/PID', str(pid), '/F'],
                           capture_output=True, timeout=10)
        if r.returncode == 0:
            return '-> процесс %d завершён' % pid
        msg = r.stdout.decode('cp1251', 'ignore').strip()
        return 'taskkill %d: %s' % (pid, msg or ('код %d' % r.returncode))
    except Exception as e:
        return 'Ошибка kill: %s' % e


def exec_command(name, no_commands=False):
    if no_commands:
        return '[CMD пропущен (--no-commands)] ' + name
    name = name.strip().lower()
    try:
        if name == 'sleep':
            if SYS == 'Windows':
                subprocess.run(['rundll32.exe', 'powrprof.dll,SetSuspendState', '0,1,0'])
            else:
                subprocess.run(['systemctl', 'suspend'])
            return '-> sleep'
        elif name in ('reboot', 'restart'):
            if SYS == 'Windows':
                subprocess.run(['shutdown', '/r', '/t', '0'])
            else:
                subprocess.run(['systemctl', 'reboot'])
            return '-> reboot'
        elif name == 'shutdown':
            if SYS == 'Windows':
                subprocess.run(['shutdown', '/s', '/t', '5'])
            else:
                subprocess.run(['systemctl', 'poweroff'])
            return '-> shutdown'
        else:
            return '[неизвестная команда] ' + name
    except Exception as e:
        return 'Ошибка: %s' % e


# ----------------------------------------------------------------------------
#  Основной поток монитора
# ----------------------------------------------------------------------------
class Monitor:
    """Поток: каждую секунду читает датчики и шлёт их в ESP32 по COM."""

    def __init__(self, port=None, interval=1.0, no_commands=False,
                 on_log=None, on_data=None, prefer_backend=None, hotkeys=None,
                 on_hotkey_change=None, apps=None, thresholds=None,
                 on_notify=None):
        self.port = port
        self.interval = interval
        self.no_commands = no_commands
        self.on_log = on_log or (lambda msg: None)
        self.on_data = on_data or (lambda fields: None)
        self.prefer_backend = prefer_backend  # 'lhm' | 'wmi' | None
        self.on_hotkey_change = on_hotkey_change  # (idx, label, combo) — с экрана
        self.hotkeys = list(hotkeys) if hotkeys else list(DEFAULT_HOTKEYS)
        self.apps = list(apps) if apps else list(DEFAULT_APPS)
        # Пороги (см. DEFAULT_THRESHOLDS). Только целые значения.
        self.thresholds = dict(DEFAULT_THRESHOLDS)
        if isinstance(thresholds, dict):
            for k in self.thresholds:
                if k in thresholds:
                    try:
                        self.thresholds[k] = int(round(float(thresholds[k])))
                    except (TypeError, ValueError):
                        pass
        # Уведомления о пересечении warn/crit: on_notify(level, text)
        # level: 1 = WARN, 2 = ALARM, 0 = вернулось в норму
        self.on_notify = on_notify or (lambda level, text: None)
        self._alarm_lvl = {}      # ('cpu_temp'|'gpu_temp') -> 0/1/2
        self._stop = threading.Event()
        self.thread = None
        self.backend_name = None
        self.last_fields = {}
        self._ser = None
        self._ser_lock = threading.Lock()

    # -- управление --
    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self._stop.clear()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self._stop.set()

    @property
    def running(self):
        return bool(self.thread and self.thread.is_alive())

    # -- бэкенды --
    def _make_backend(self):
        if SYS != 'Windows' and self.prefer_backend == 'lhm':
            pass
        if self.prefer_backend in (None, 'lhm') and HttpSensors.available():
            try:
                b = HttpSensors()
                b.read()
                self.backend_name = 'LHM HTTP :8085'
                return b
            except Exception:
                pass
        if self.prefer_backend in (None, 'wmi') and WmiSensors.available():
            for ns in ('root\\LibreHardwareMonitor', 'root\\OpenHardwareMonitor'):
                try:
                    b = WmiSensors(ns)
                    b.read()
                    self.backend_name = 'WMI ' + ns
                    return b
                except Exception:
                    pass
        self.backend_name = 'psutil (базовый)'
        return None

    # -- отправка произвольной строки на ESP32 (из GUI, например конфиг клавиш) --
    def send_line(self, text):
        if not self._ser:
            return False
        try:
            with self._ser_lock:
                self._ser.write((text + '\n').encode('ascii', 'replace'))
            return True
        except Exception:
            return False

    # -- сменить конфигурацию горячих клавиш (отправить на ESP32 + сохранить) --
    def set_hotkeys(self, hotkeys):
        self.hotkeys = list(hotkeys) if hotkeys else list(DEFAULT_HOTKEYS)
        for i, hk in enumerate(self.hotkeys[:8]):
            label = (hk.get('label') or '').replace('|', ' ').strip()[:16]
            combo = (hk.get('combo') or '').replace('|', ' ').strip()[:24]
            self.send_line('HK:%d=%s|%s' % (i, label, combo))
        return True

    # -- сменить список быстрого запуска (подписи уходят на экран; команды
    #    исполняются здесь, поэтому на ESP32 отправляем только подписи) --
    def set_apps(self, apps):
        self.apps = list(apps) if apps else list(DEFAULT_APPS)
        for i, ap in enumerate(self.apps[:8]):
            label = (ap.get('label') or '').replace('|', ' ').strip()[:16]
            if label:
                self.send_line('AP:%d=%s' % (i, label))
        return True

    # -- сменить пороги реле/тревог (отправить на ESP32 + запомнить) --
    def set_thresholds(self, thr):
        if isinstance(thr, dict):
            for k in self.thresholds:
                if k in thr:
                    try:
                        self.thresholds[k] = int(round(float(thr[k])))
                    except (TypeError, ValueError):
                        pass
        # Сброс зон: следующие срабатывания тревог считаем заново
        self._alarm_lvl = {}
        ok = self.send_line(_thr_line(self.thresholds))
        return ok

    def _check_alarms(self, f):
        """Следит за cpu/gpu температурой против порогов warn/crit.
        Вызывает on_notify(level, text): 2 = ALARM, 1 = WARN, 0 = норма.
        Уведомляем один раз на вход в более высокую зону (0->1, 1->2,
        0->2) и на полный возврат в норму (2/1->0). Пока температура
        остаётся выше warn, зона НЕ понижается (критичный нагрев 75->74
        ->75 не спамит) — напоминание уходит только когда снова ушло
        ниже warn (или сброс порогов через set_thresholds)."""
        try:
            warnC = float(self.thresholds.get('warn', 60))
            critC = float(self.thresholds.get('crit', 75))
            for key, name in (('cpu_temp', 'CPU'), ('gpu_temp', 'GPU')):
                v = f.get(key)
                if v is None:
                    continue
                try:
                    t = float(v)
                except (TypeError, ValueError):
                    continue
                if t != t:  # NaN
                    continue
                lvl = 2 if t >= critC else (1 if t >= warnC else 0)
                cur = self._alarm_lvl.get(key, 0)
                if cur == 0:
                    # сейчас в норме: уведомляем только при ВХОДЕ в зону
                    if lvl == 2:
                        self._alarm_lvl[key] = 2
                        self.on_notify(2, '%s %.0f°C — КРИТИЧНО (>=%d°C)!' % (name, t, critC))
                    elif lvl == 1:
                        self._alarm_lvl[key] = 1
                        self.on_notify(1, '%s %.0f°C — внимание (>=%d°C).' % (name, t, warnC))
                else:
                    # уже в зоне: выше — эскалация (warn->crit), ниже warn — норма
                    if lvl >= cur:
                        if lvl > cur:  # warn->crit: эскалация
                            self._alarm_lvl[key] = 2
                            self.on_notify(2, '%s %.0f°C — КРИТИЧНО (>=%d°C)!' % (name, t, critC))
                    elif lvl == 0:
                        self._alarm_lvl[key] = 0
                        self.on_notify(0, '%s %.0f°C — в норме.' % (name, t))
                    # crit->warn (lvl<cur, lvl>0): зону НЕ понижаем, молчим
        except Exception:
            pass

    def _run(self):
        import serial
        log = self.on_log
        try:
            ser = serial.Serial(self.port, 115200, timeout=0.3, write_timeout=2)
        except Exception as e:
            log('ОШИБКА COM: %s' % e)
            return
        self._ser = ser
        log('COM %s открыт, жду ресет ESP32…' % self.port)
        time.sleep(1.5)

        # Синхронизация горячих клавиш (страница KEYS). Новая прошивка хранит
        # конфиг у себя (там же редактор на борту) — ПК при старте ПРОСИТ его
        # (HKREQ) и принимает экран за источник истины, чтобы правки, сделанные
        # на экране, пока ПК был выключен, не затёрлись. Если плата не ответила
        # (старая прошивка / нет связи) — отправляем свой конфиг, как раньше.
        try:
            ser.reset_input_buffer()
        except Exception:
            pass
        adopt = {}
        last_req = 0.0
        t_end = time.time() + 4.0
        while time.time() < t_end and len(adopt) < 8:
            if time.time() - last_req > 1.0:   # повторяем запрос, пока плата грузится
                try:
                    with self._ser_lock:
                        ser.write(b'HKREQ\n')
                    last_req = time.time()
                except Exception:
                    break
            try:
                while ser.in_waiting:
                    raw = ser.readline().decode('ascii', 'ignore').strip()
                    if raw.startswith('HKCFG:'):
                        body = raw[6:]
                        eq = body.find('=')
                        bar = body.find('|')
                        if eq >= 0 and bar >= 0:
                            idx = int(body[:eq].strip())
                            if 0 <= idx < 8:
                                adopt[idx] = (body[eq + 1:bar].strip()[:16].replace('|', ' '),
                                              body[bar + 1:].strip()[:24])
            except Exception:
                pass
            time.sleep(0.05)
        if adopt:
            n = 0
            for idx in range(8):
                if idx in adopt:
                    label, combo = adopt[idx]
                    self.hotkeys[idx] = {'label': label, 'combo': combo}
                    if self.on_hotkey_change:
                        try:
                            self.on_hotkey_change(idx, label, combo)
                        except Exception:
                            pass
                    n += 1
            log('KEYS: конфиг принят с экрана (%d слотов).' % n)
        else:
            try:
                for i, hk in enumerate(self.hotkeys[:8]):
                    label = (hk.get('label') or '').replace('|', ' ').strip()[:16]
                    combo = (hk.get('combo') or '').replace('|', ' ').strip()[:24]
                    with self._ser_lock:
                        ser.write(('HK:%d=%s|%s\n' % (i, label, combo)).encode('ascii', 'replace'))
            except Exception:
                pass

        # Подписи вкладки APPS (команды живут здесь, ПК — источник истины)
        try:
            for i, ap in enumerate(self.apps[:8]):
                label = (ap.get('label') or '').replace('|', ' ').strip()[:16]
                if not label:
                    continue
                with self._ser_lock:
                    ser.write(('AP:%d=%s\n' % (i, label)).encode('ascii', 'replace'))
        except Exception:
            pass

        # Пороги реле/тревог (ПК — источник истины; плата сохраняет в NVS)
        try:
            with self._ser_lock:
                ser.write((_thr_line(self.thresholds) + '\n').encode('ascii', 'replace'))
            log('Пороги на экране: R1>=%d/<=%d R2>=%d/<=%d warn%d crit%d' % (
                self.thresholds['r1on'], self.thresholds['r1off'],
                self.thresholds['r2on'], self.thresholds['r2off'],
                self.thresholds['warn'], self.thresholds['crit']))
        except Exception:
            pass

        backend = self._make_backend()
        last_backend_try = time.time()
        log('Бэкенд: ' + self.backend_name)
        v0 = read_volume()
        if v0 is not None:
            log('Звук: громкость %d%%%s' % (abs(round(v0)), ' (mute)' if v0 < 0 else ''))
        else:
            log('Звук: pycaw недоступен — громкость без данных.')

        psutil.cpu_percent(interval=None)
        prev_net = psutil.net_io_counters()
        last_t = time.time()
        last_lhm_run = time.time()
        proc_tracker = _ProcTracker()

        while not self._stop.is_set():
            # команды с экрана
            try:
                while ser.in_waiting:
                    try:
                        line = ser.readline().decode('ascii', 'ignore').strip()
                    except Exception:
                        break
                    if line.startswith('CMD:'):
                        log('Команда с экрана: ' + exec_command(line[4:], self.no_commands))
                    elif line.startswith('MEDIA:'):
                        log('Медиа с экрана: ' + exec_media(line[6:], self.no_commands))
                    elif line.startswith('KEY:'):
                        try:
                            idx = int(line[4:].strip())
                        except ValueError:
                            idx = -1
                        if 0 <= idx < len(self.hotkeys):
                            combo = (self.hotkeys[idx].get('combo') or '').strip()
                            log('Горячая клавиша: ' + exec_hotkey(combo, self.no_commands))
                    elif line.startswith('APP:'):
                        # нажата кнопка быстрого запуска на вкладке APPS
                        try:
                            idx = int(line[4:].strip())
                        except ValueError:
                            idx = -1
                        if 0 <= idx < len(self.apps):
                            cmd = (self.apps[idx].get('command') or '').strip()
                            log('Запуск с экрана: ' + exec_app(cmd, self.no_commands))
                    elif line.startswith('KILL:'):
                        # завершение процесса со вкладки PROCS (после подтверждения)
                        log('Процесс с экрана: ' + exec_kill(line[5:], self.no_commands))
                    elif line.startswith('HKCFG:'):
                        # комбинация изменена в редакторе НА ЭКРАНЕ — обновляем
                        # свою таблицу, чтобы KEY:n выполнял новое комбо
                        try:
                            body = line[6:]
                            eq = body.find('=')
                            bar = body.find('|')
                            if eq < 0 or bar < 0:
                                continue
                            idx = int(body[:eq].strip())
                            label = body[eq + 1:bar].strip().replace('|', ' ')[:16]
                            combo = body[bar + 1:].strip()[:24]
                            ok = True
                            for token in combo.split('+'):
                                if not token.strip() or _vk_for(token.strip()) is None:
                                    ok = False
                                    break
                            if 0 <= idx < 8 and ok:
                                self.hotkeys[idx] = {'label': label, 'combo': combo}
                                log('KEYS: слот %d изменён на экране: %s = %s'
                                    % (idx + 1, label or '-', combo or '-'))
                                if self.on_hotkey_change:
                                    try:
                                        self.on_hotkey_change(idx, label, combo)
                                    except Exception:
                                        pass
                        except Exception:
                            pass
            except Exception:
                pass

            # поддержание полного бэкенда: LHM может появиться или пропасть
            # в любой момент (старт/перезапуск) — проверяем и переподключаемся
            if SYS == 'Windows' and time.time() - last_backend_try >= 5:
                last_backend_try = time.time()
                if backend is None:
                    backend = self._make_backend()
                    if backend is not None:
                        log('Подключён полный бэкенд: ' + self.backend_name)
                else:
                    try:
                        backend.read()  # проверка живости сервера
                    except Exception:
                        log('Связь с LHM потеряна — переподключаюсь…')
                        backend = None
                # LHM совсем умер (нет процесса) — молча запускаем через задачу.
                # Безопасно повторять: если задача уже стартует/работает — no-op.
                if backend is None and not lhm_process_running():
                    if time.time() - last_lhm_run >= 30:
                        last_lhm_run = time.time()
                        if lhm_task_run():
                            log('LHM не запущен — поднимаю через задачу (молча).')
                        elif lhm_task_exists():
                            log('Не удалось запустить LHM задачей — нажмите «Запустить LHM».')

            # базовые поля (psutil)
            f = {}
            f['cpu_load'] = psutil.cpu_percent(interval=None)
            cf = psutil.cpu_freq()
            if cf:
                f['cpu_freq'] = cf.current
            vm = psutil.virtual_memory()
            f['ram_pct'] = vm.percent
            f['ram_total_gb'] = round(vm.total / 1e9, 1)

            # системный диск (C:) — занятость и свободное место
            try:
                sysdrv = os.environ.get('SystemDrive', 'C:') or 'C:'
                dd = psutil.disk_usage(sysdrv + os.sep)
                f['disk_pct'] = round(dd.percent, 1)
                f['disk_free_gb'] = round(dd.free / 1e9, 1)
            except Exception:
                pass

            now = time.time()
            dt = max(now - last_t, 0.05)
            last_t = now
            net = psutil.net_io_counters()
            f['net_dn'] = round((net.bytes_recv - prev_net.bytes_recv) / dt / 1e6, 2)
            f['net_up'] = round((net.bytes_sent - prev_net.bytes_sent) / dt / 1e6, 2)
            prev_net = net

            if backend:
                f.update(read_wmi(backend))
            if 'gpu_temp' not in f:
                f.update(read_nvidia())
            if 'cpu_temp' not in f and SYS == 'Linux':
                t = read_linux_cpu_temp()
                if t is not None:
                    f['cpu_temp'] = t

            f['uptime'] = int(time.time() - psutil.boot_time())
            self.last_fields = f

            # Пересечение порогов warn/crit -> уведомление (один раз на зону)
            self._check_alarms(f)

            line = ','.join('%s=%s' % (k, v) for k, v in f.items())
            try:
                ser.write((line + '\n').encode('ascii'))
                self.on_data(dict(f))
            except Exception as e:
                log('Ошибка записи в COM: %s' % e)
                time.sleep(1.0)
                continue

            # Медиа: громкость + «сейчас играет»
            try:
                vol = read_volume()
                vol_s = '' if vol is None else '%.0f' % vol
                tr = read_now_playing().replace('|', ' ').replace('\n', ' ')[:48]
                ser.write(('MNOW:%s|%s\n' % (vol_s, tr)).encode('ascii', 'replace'))
            except Exception:
                pass
            # Топ-5 процессов
            try:
                top = proc_tracker.top(5)
                parts = []
                for i in range(5):
                    if i < len(top):
                        nm, cp, mm, pid = top[i]
                        nm = nm.replace(';', ' ').replace('|', ' ')[:20] or '?'
                    else:
                        nm, cp, mm, pid = '', -1, 0, 0
                    parts.append('%s|%d|%d|%d' % (nm, int(cp), int(mm), pid))
                ser.write(('TOP:' + ';'.join(parts) + '\n').encode('ascii', 'replace'))
            except Exception:
                pass

            time.sleep(max(0.05, self.interval - dt))

        try:
            ser.close()
        except Exception:
            pass
        self._ser = None
        log('Поток монитора остановлен.')


# ----------------------------------------------------------------------------
#  Проверка и запуск LHM (для GUI и автономного режима)
# ----------------------------------------------------------------------------
def _wait_http(wait_s, log):
    """Ждёт появления веб-сервера LHM. True если поднялся."""
    t0 = time.time()
    while time.time() - t0 < wait_s:
        if http_up():
            log('Веб-сервер LHM отвечает (%0.1f с).' % (time.time() - t0))
            return True
        time.sleep(2)
    return False


def _kill_lhm():
    """Завершает LHM. Если не хватает прав — один раз через UAC (окно скрыто).
    Вызывается только по явному действию пользователя (кнопка «Запустить LHM»),
    никогда при автозапуске.
    """
    try:
        r = subprocess.run(['taskkill', '/IM', 'LibreHardwareMonitor.exe', '/F'],
                           capture_output=True, timeout=10)
        if r.returncode == 0:
            return True
    except Exception:
        pass
    if SYS == 'Windows' and not is_elevated():
        try:
            import ctypes
            rr = ctypes.windll.shell32.ShellExecuteW(
                None, 'runas', 'taskkill.exe',
                '/IM LibreHardwareMonitor.exe /F', None, 0)
            return rr > 32
        except Exception:
            return False
    return False


def ensure_lhm(on_log=None, elevated_start=False, wait_s=240, restart_on_stuck=False):
    """Гарантирует работающий веб-сервер LHM. Возвращает статус-строку.

    Приоритет запуска — Планировщик заданий (задача CYD-LibreHardwareMonitor,
    RunLevel=HighestAvailable): LHM стартует с правами администратора
    БЕЗ окон UAC, в том числе при входе в Windows.

    Важно: уже запущенный LHM по умолчанию не убиваем — он может просто
    ещё "греться" (первичный опрос железа на некоторых машинах занимает
    несколько минут) — ждём его сервер. Перезапуск «зависшего» LHM делается
    только если restart_on_stuck=True (явная кнопка пользователя).
    """
    log = on_log or (lambda m: None)
    if http_up():
        return 'LHM: веб-сервер уже работает (:8085)'

    exe = ensure_lhm_installed()
    if not exe:
        log('LibreHardwareMonitor не найден — полный набор датчиков недоступен.')
        return 'LHM: не установлен'

    ensure_lhm_config(exe)
    log('Конфиг LHM: веб-сервер включён (127.0.0.1:%d).' % LHM_PORT)

    started_here = False
    if lhm_task_exists():
        if not lhm_process_running():
            if lhm_task_run():
                started_here = True
                log('LHM запущен через задачу автозапуска (молча, с админ-правами).')
            else:
                log('Задача LHM есть, но запустить не вышло — обычный запуск.')
        else:
            log('LHM уже стартует — жду его веб-сервер, процесс не трогаю…')
    else:
        # задачи нет: пробуем зарегистрировать (один раз, через UAC)
        lhm_task_ensure(exe, log)
        if lhm_task_exists():
            if lhm_task_run():
                started_here = True
                log('LHM запущен через задачу автозапуска.')
        elif lhm_process_running():
            log('LHM уже запущен — жду веб-сервер…')
        else:
            started = start_lhm(elevated=elevated_start, force_uac=elevated_start)
            if not started:
                log('Не удалось запустить LibreHardwareMonitor (нет прав?).')
                return 'LHM: не запущен (нужны права администратора)'
            started_here = True
            log('LibreHardwareMonitor запущен, жду веб-сервер… (до %d с)' % wait_s)

    if _wait_http(wait_s, log):
        return 'LHM: работает (:8085)'

    # процесс висит без сервера дольше разумного — перезапуск только по явному
    # запросу (кнопка «Запустить LHM») или если LHM запущен нами в этой же
    # сессии и не поднял сервер. При автозапуске (boot) ничего не убиваем:
    # поток монитора сам переподключится, как только сервер появится.
    if (started_here or restart_on_stuck) and lhm_process_running():
        log('LHM не поднял сервер — перезапускаю через задачу.')
        if _kill_lhm():
            time.sleep(2)
            if lhm_task_run() and _wait_http(min(wait_s, 120), log):
                return 'LHM: работает (:8085)'
        else:
            log('Не удалось завершить зависший LHM (нужно подтверждение прав).')
    return 'LHM: веб-сервер не поднялся за %d с (полные датчики подключатся автоматически, когда LHM будет готов)' % wait_s
