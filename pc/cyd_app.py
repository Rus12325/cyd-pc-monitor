#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CYD PC Monitor — приложение для Windows (собирается в .exe через PyInstaller).
=============================================================================
«Запустил — и всё готово»:
  * само находит / запускает LibreHardwareMonitor с включённым веб-сервером;
  * само находит ESP32 (CH340) по USB;
  * шлёт датчики ПК на экран раз в секунду;
  * живёт в трее, умеет сворачиваться при закрытии и стартовать с Windows.
"""
import json
import os
import queue
import sys
import threading
import time

try:
    import tkinter as tk
    from tkinter import ttk, messagebox
    HAVE_TK = True
except Exception:
    HAVE_TK = False

import engine
from engine import Monitor, ensure_lhm, find_lhm_exe, http_up, pick_port

APP_NAME = 'CYD PC Monitor'


def appdata_dir():
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    d = os.path.join(base, 'CydMonitor')
    try:
        os.makedirs(d, exist_ok=True)
    except Exception:
        pass
    return d


def _crash_log(tp, val=None, tb=None):
    # Один обработчик и для sys.excepthook (tp, val, tb), и для
    # threading.excepthook (один аргумент ExceptHookArgs) — иначе при крахе
    # фонового потока теряется настоящий traceback.
    try:
        import traceback
        if hasattr(tp, 'exc_value'):
            tb = tp.exc_traceback
            val = tp.exc_value
            tp = tp.exc_type
        with open(os.path.join(appdata_dir(), 'app.log'), 'a', encoding='utf-8') as f:
            f.write('\n=== КРАХ %s ===\n' % time.strftime('%Y-%m-%d %H:%M:%S'))
            traceback.print_exception(tp, val, tb, file=f)
    except Exception:
        pass


sys.excepthook = _crash_log
threading.excepthook = _crash_log


def settings_path():
    return os.path.join(appdata_dir(), 'settings.json')


def load_settings():
    try:
        with open(settings_path(), 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception:
        return {}


def save_settings(s):
    try:
        with open(settings_path(), 'w', encoding='utf-8') as f:
            json.dump(s, f, ensure_ascii=False, indent=2)
        return True
    except Exception:
        return False


def autostart_enabled():
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r'Software\Microsoft\Windows\CurrentVersion\Run') as k:
            winreg.QueryValueEx(k, 'CydPcMonitor')
            return True
    except Exception:
        return False


def autostart_set(exe_path, enable):
    import winreg
    key = r'Software\Microsoft\Windows\CurrentVersion\Run'
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_SET_VALUE) as k:
            if enable:
                winreg.SetValueEx(k, 'CydPcMonitor', 0, winreg.REG_SZ, '"%s" -t' % exe_path)
            else:
                try:
                    winreg.DeleteValue(k, 'CydPcMonitor')
                except FileNotFoundError:
                    pass
        return True
    except Exception:
        return False


# ----------------------------------------------------------------------------
#  Иконка (генерируется кодом — не нужен файл)
# ----------------------------------------------------------------------------
def make_icon_image(size=64):
    try:
        from PIL import Image, ImageDraw
    except Exception:
        return None
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = size * 0.05
    # тёмная плашка
    d.rounded_rectangle([m, m, size - m, size - m], radius=size * 0.18,
                        fill=(16, 24, 48, 255))
    # «экран» с графиком
    d.rectangle([size * 0.22, size * 0.24, size * 0.78, size * 0.58],
                fill=(10, 16, 36, 255), outline=(70, 120, 220, 255))
    pts = []
    for i, y in enumerate([0.5, 0.42, 0.46, 0.34, 0.38, 0.28]):
        x = 0.26 + i * (0.50 / 5.0)
        pts.append((size * x, size * (y + 0.24 - 0.24 * 0)))  # заполнено ниже
    # простая ломаная "температуры"
    ys = [0.62, 0.55, 0.58, 0.48, 0.52, 0.40, 0.44, 0.33]
    for i in range(1, len(ys)):
        x0 = size * (0.24 + (i - 1) * (0.52 / (len(ys) - 1)))
        y0 = size * ys[i - 1]
        x1 = size * (0.24 + i * (0.52 / (len(ys) - 1)))
        y1 = size * ys[i]
        d.line([x0, y0, x1, y1], fill=(64, 220, 130, 255), width=max(2, size // 18))
    # «термометр» справа
    cx, cy = size * 0.72, size * 0.40
    r = size * 0.06
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 90, 90, 255))
    d.line([cx, cy + r, cx, size * 0.58], fill=(255, 90, 90, 255), width=max(2, size // 20))
    d.rounded_rectangle([cx - size * 0.02, size * 0.5, cx + size * 0.02, size * 0.6],
                        radius=size * 0.01, fill=(255, 90, 90, 255))
    # буква C
    return img


def make_tray_image():
    img = make_icon_image(64)
    if img is None:
        return None
    try:
        from PIL import Image
        # Windows-иконки трея обычно 32x32
        return img.resize((32, 32), Image.LANCZOS)
    except Exception:
        return img


# ----------------------------------------------------------------------------
#  Приложение
# ----------------------------------------------------------------------------
class CydApp:
    def __init__(self, root):
        self.root = root
        self.settings = load_settings()
        self.monitor = None
        self.port_var = tk.StringVar(value=self.settings.get('port', ''))
        self.state_var = tk.StringVar(value='—')
        self.lhm_var = tk.StringVar(value='проверка…')
        self.icon = None
        self._q = queue.Queue()
        self._last_data = {}
        self._started_by_default = False
        self._busy_lhm = False

        root.title(APP_NAME)
        root.protocol('WM_DELETE_WINDOW', self._on_close)
        self._build_ui()
        self._build_tray()
        root.after(150, self._poll_queue)
        root.after(400, self._bootstrap)

    # ----------------------------- UI -----------------------------
    def _build_ui(self):
        root = self.root
        root.configure(bg='#101826')
        fg = '#e8eef8'
        try:
            root.geometry('560x700')
            root.minsize(520, 620)
            img = make_icon_image(48)
            if img is not None:
                try:
                    from PIL import ImageTk
                    self._tkicon = ImageTk.PhotoImage(img)
                    root.iconphoto(True, self._tkicon)
                except Exception:
                    pass
        except Exception:
            pass

        # --- верхняя панель статусов ---
        top = tk.Frame(root, bg='#101826')
        top.pack(fill='x', padx=12, pady=(10, 4))

        self.dot = tk.Canvas(top, width=18, height=18, bg='#101826', highlightthickness=0)
        self.dot.pack(side='left', padx=(0, 8))
        self._dot_id = self.dot.create_oval(3, 3, 15, 15, fill='#999999', outline='')

        tk.Label(top, textvariable=self.state_var, font=('Segoe UI', 11, 'bold'),
                 bg='#101826', fg=fg).pack(side='left')

        tk.Label(root, textvariable=self.lhm_var, font=('Segoe UI', 9),
                 bg='#101826', fg='#8fa3bf').pack(anchor='w', padx=14, pady=(0, 2))

        # --- значения ---
        grid = tk.Frame(root, bg='#101826')
        grid.pack(fill='x', padx=12)
        self.val_labels = {}
        fields = [
            ('cpu_temp', 'CPU °C'), ('cpu_load', 'CPU %'), ('cpu_freq', 'CPU ГГц'),
            ('gpu_temp', 'GPU °C'), ('gpu_load', 'GPU %'), ('gpu_freq', 'GPU МГц'),
            ('ram_pct', 'RAM %'), ('cpu_power', 'CPU Вт'), ('gpu_power', 'GPU Вт'),
            ('fan1', 'Вент-1'), ('fan2', 'Вент-2'), ('v12', '12V'),
            ('net_dn', 'Сеть ↓ МБ/с'), ('net_up', 'Сеть ↑ МБ/с'), ('uptime', 'Аптайм, с'),
            ('disk_pct', 'Диск C %'), ('disk_free_gb', 'Своб. ГБ'),
            ('c0t', 'Ядро 1 °C'), ('c1t', 'Ядро 2 °C'), ('c2t', 'Ядро 3 °C'),
            ('c3t', 'Ядро 4 °C'), ('sys_temp', 'Материнка °C'),
        ]
        for i, (key, label) in enumerate(fields):
            r, c = divmod(i, 3)
            box = tk.Frame(grid, bg='#182236', highlightthickness=1,
                           highlightbackground='#243450')
            box.grid(row=r, column=c, sticky='nsew', padx=3, pady=3)
            grid.columnconfigure(c, weight=1)
            tk.Label(box, text=label, font=('Segoe UI', 8), bg='#182236',
                     fg='#7f93b5').pack(pady=(4, 0))
            val = tk.Label(box, text='--', font=('Consolas', 13, 'bold'),
                           bg='#182236', fg='#dfe8f5')
            val.pack(pady=(0, 4))
            self.val_labels[key] = val

        # --- кнопки ---
        btns = tk.Frame(root, bg='#101826')
        btns.pack(fill='x', padx=12, pady=6)
        tk.Label(btns, text='Порт ESP32:', bg='#101826', fg='#8fa3bf',
                 font=('Segoe UI', 9)).pack(side='left')
        self.port_combo = ttk.Combobox(btns, textvariable=self.port_var, width=9,
                                       state='readonly')
        self.port_combo.pack(side='left', padx=(4, 6))
        self._refresh_ports()
        ttk.Button(btns, text='⟳', width=3, command=self._refresh_ports).pack(side='left')
        self.start_btn = ttk.Button(btns, text='Пуск', width=8, command=self._toggle)
        self.start_btn.pack(side='left', padx=(10, 4))
        self.lhm_btn = ttk.Button(btns, text='Запустить LHM', command=self._ensure_lhm)
        self.lhm_btn.pack(side='left', padx=4)
        ttk.Button(btns, text='Клавиши KEYS…', command=self._edit_hotkeys).pack(side='left', padx=4)
        ttk.Button(btns, text='Запуск APPS…', command=self._edit_apps).pack(side='left', padx=4)
        ttk.Button(btns, text='Пороги…', command=self._edit_thresholds).pack(side='left', padx=4)
        ttk.Button(btns, text='Выход', command=self._quit).pack(side='right')

        # --- лог ---
        tk.Label(root, text='Журнал:', bg='#101826', fg='#8fa3bf',
                 font=('Segoe UI', 9)).pack(anchor='w', padx=14)
        logf = tk.Frame(root, bg='#101826')
        logf.pack(fill='both', expand=True, padx=12, pady=(2, 8))
        self.log = tk.Text(logf, height=8, bg='#0a0f1a', fg='#9fc0a8',
                           font=('Consolas', 9), state='disabled',
                           insertbackground='#9fc0a8', relief='flat', wrap='word')
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)

        # нижняя строка: автозапуск с Windows
        low = tk.Frame(root, bg='#101826')
        low.pack(fill='x', padx=12, pady=(0, 8))
        self.auto_win = tk.BooleanVar(value=autostart_enabled())
        tk.Checkbutton(low, text='Старт с Windows (в трей)', variable=self.auto_win,
                       bg='#101826', fg='#8fa3bf', selectcolor='#182236',
                       activebackground='#101826', activeforeground='#8fa3bf',
                       command=self._on_autostart, font=('Segoe UI', 8)
                       ).pack(side='left')

        self._set_dot('#999999')
        self._log('Приложение запущено.')

    def _refresh_ports(self):
        try:
            import serial.tools.list_ports as lp
            ports = [p.device for p in lp.comports()]
        except Exception:
            ports = []
        self.port_combo['values'] = ports or ['—']
        cur = self.port_var.get()
        if cur not in ports:
            # предпочитаем CH340/CH9102 (мост CYD), иначе — первый порт
            chosen = pick_port()
            if not chosen and ports:
                chosen = ports[0]
            self.port_var.set(chosen or '')

    def _set_dot(self, color):
        try:
            self.dot.itemconfig(self._dot_id, fill=color)
        except Exception:
            pass

    # ----------------------------- лог -----------------------------
    def _log(self, msg):
        line = '[%s] %s' % (time.strftime('%H:%M:%S'), msg)
        try:
            self.log.configure(state='normal')
            self.log.insert('end', line + '\n')
            self.log.see('end')
            self.log.configure(state='disabled')
        except Exception:
            pass
        # дублируем в файл (для диагностики)
        try:
            with open(os.path.join(appdata_dir(), 'app.log'), 'a', encoding='utf-8') as f:
                f.write(line + '\n')
        except Exception:
            pass

    # ----------------------------- трей -----------------------------
    def _build_tray(self):
        try:
            import pystray
            from pystray import MenuItem as MItem
        except Exception:
            return

        def show():
            self.root.after(0, self._show_window)

        def toggle():
            self.root.after(0, self._toggle)

        def quit_():
            self.root.after(0, self._quit)

        def make_menu():
            return pystray.Menu(
                MItem('Показать окно', lambda: show()),
                MItem('Пуск / Пауза', lambda: toggle()),
                MItem('Клавиши KEYS…', lambda: self.root.after(0, self._edit_hotkeys)),
                MItem('Запуск APPS…', lambda: self.root.after(0, self._edit_apps)),
                MItem('Пороги…', lambda: self.root.after(0, self._edit_thresholds)),
                MItem('Выход', lambda: quit_()),
            )

        try:
            img = make_tray_image()
            self.icon = pystray.Icon('cyd_monitor', img or make_icon_image(64),
                                     APP_NAME, make_menu())
            self.icon.run_detached()
        except Exception as e:
            self._log('Трей недоступен: %s' % e)

    def _show_window(self):
        try:
            self.root.deiconify()
            self.root.lift()
            self.root.focus_force()
        except Exception:
            pass

    def _on_close(self):
        if getattr(self, 'minimize_on_close', True):
            try:
                self.root.withdraw()
                if self.icon is None:
                    # без трея — просто в панель задач не убираемся, показываем окно
                    self.root.deiconify()
                else:
                    self._log('Свёрнут в трей. Выход — через иконку в трее.')
            except Exception:
                pass
        else:
            self._quit()

    # ----------------------------- действия -----------------------------
    def _bootstrap(self):
        self._refresh_ports()
        # 1) LibreHardwareMonitor: если сервер не отвечает — поднимаем его фоном
        #    (через задачу автозапуска Windows — БЕЗ окон UAC), но не ждём:
        #    базовые данные (psutil) поедут на экран немедленно, а полный набор
        #    датчиков подключится сам, как только веб-сервер LHM поднимется.
        if not http_up():
            engine.ensure_lhm_installed()
            self.lhm_var.set('LibreHardwareMonitor: запускаю…')
            self._ensure_lhm(auto=False)
        else:
            self.lhm_var.set('LibreHardwareMonitor: работает (веб-сервер :8085) ✓')
            self._log('LHM веб-сервер уже отвечает на :8085.')
            self.settings['lhm_ok'] = True
            save_settings(self.settings)

        # 2) автозапуск мониторинга — сразу, не дожидаясь LHM
        if self.settings.get('auto_start', True):
            self.root.after(400, self._try_auto_start)

    def _try_auto_start(self):
        if not HAVE_TK:
            return
        port = self.port_var.get()
        if not port or port == '—':
            self._log('COM-порт не найден — проверьте подключение ESP32.')
            self.state_var.set('ESP32 не найден')
            self._set_dot('#c0392b')
            return
        self._start(port)

    def _ensure_lhm(self, auto=True):
        if self._busy_lhm:
            return
        self._busy_lhm = True
        self.lhm_btn.configure(state='disabled')
        self.lhm_var.set('Запускаю LibreHardwareMonitor…')

        def work():
            def log(m):
                self._q.put(('log', m))
            status = ensure_lhm(on_log=log, elevated_start=False, wait_s=240,
                                restart_on_stuck=auto)
            self._q.put(('lhm_done', status))

        threading.Thread(target=work, daemon=True).start()

    def _edit_hotkeys(self):
        """Диалог настройки горячих клавиш (страница KEYS на экране)."""
        cur = self.settings.get('hotkeys') or engine.DEFAULT_HOTKEYS
        if len(cur) < 8:
            cur = list(cur) + list(engine.DEFAULT_HOTKEYS[len(cur):])

        dlg = tk.Toplevel(self.root)
        dlg.title('Горячие клавиши (страница KEYS)')
        dlg.configure(bg='#101826')
        dlg.transient(self.root)
        dlg.grab_set()
        try:
            dlg.geometry('470x360')
        except Exception:
            pass

        tk.Label(dlg, text='Кнопки 1–8: подпись и комбинация (например ctrl+shift+esc).',
                 bg='#101826', fg='#8fa3bf', font=('Segoe UI', 9)).pack(padx=10, pady=(10, 2))
        tk.Label(dlg, text='Медиа-клавиши работают сразу (страница MEDIA); сюда — остальное.',
                 bg='#101826', fg='#5a6c8c', font=('Segoe UI', 8)).pack(padx=10)

        frame = tk.Frame(dlg, bg='#101826')
        frame.pack(fill='both', expand=True, padx=10, pady=6)
        entries = []
        for i in range(8):
            hk = cur[i] if i < len(cur) else {'label': '', 'combo': ''}
            tk.Label(frame, text='%d' % (i + 1), bg='#101826', fg='#7f93b5',
                     font=('Segoe UI', 9, 'bold')).grid(row=i, column=0, padx=(0, 4), pady=1)
            lab_v = tk.StringVar(value=hk.get('label', ''))
            com_v = tk.StringVar(value=hk.get('combo', ''))
            tk.Entry(frame, textvariable=lab_v, width=10).grid(row=i, column=1, padx=2, pady=1)
            tk.Entry(frame, textvariable=com_v, width=30).grid(row=i, column=2, padx=2, pady=1)
            entries.append((lab_v, com_v))

        err = tk.Label(dlg, text='', bg='#101826', fg='#ff9a8c', font=('Segoe UI', 8))
        err.pack(padx=10)

        def apply():
            hotkeys = []
            for lab_v, com_v in entries:
                label = lab_v.get().strip()[:16]
                combo = com_v.get().strip()[:24]
                hotkeys.append({'label': label, 'combo': combo})
            # быстрая проверка комбо (кроме пустых)
            for hk in hotkeys:
                c = hk['combo']
                if not c:
                    continue
                bad = None
                for token in c.split('+'):
                    if not token.strip() or engine._vk_for(token.strip()) is None:
                        bad = token
                        break
                if bad:
                    err.configure(text='Не понимаю клавишу «%s» в комбинации «%s»' % (bad, c))
                    return
            self.settings['hotkeys'] = hotkeys
            save_settings(self.settings)
            if self.monitor and self.monitor.running:
                self.monitor.set_hotkeys(hotkeys)
                self._log('Конфиг горячих клавиш отправлен на экран.')
            else:
                self._log('Горячие клавиши сохранены (применятся при старте).')
            dlg.destroy()

        ttk.Button(dlg, text='Сохранить и отправить', command=apply).pack(pady=(0, 10))

    def _edit_apps(self):
        """Диалог настройки быстрого запуска (вкладка APPS на экране)."""
        cur = self.settings.get('apps') or engine.DEFAULT_APPS
        if len(cur) < 8:
            cur = list(cur) + list(engine.DEFAULT_APPS[len(cur):])

        dlg = tk.Toplevel(self.root)
        dlg.title('Быстрый запуск (вкладка APPS)')
        dlg.configure(bg='#101826')
        dlg.transient(self.root)
        dlg.grab_set()
        try:
            dlg.geometry('560x420')
        except Exception:
            pass

        tk.Label(dlg, text='Кнопки 1–8: подпись на экране и что запускать на ПК.',
                 bg='#101826', fg='#8fa3bf', font=('Segoe UI', 9)).pack(padx=10, pady=(10, 2))
        tk.Label(dlg, text='Команда: путь к .exe/.lnk/URL или строка (например explorer.exe shell:downloads).',
                 bg='#101826', fg='#5a6c8c', font=('Segoe UI', 8)).pack(padx=10)

        frame = tk.Frame(dlg, bg='#101826')
        frame.pack(fill='both', expand=True, padx=10, pady=6)
        entries = []
        for i in range(8):
            ap = cur[i] if i < len(cur) else {'label': '', 'command': ''}
            tk.Label(frame, text='%d' % (i + 1), bg='#101826', fg='#7f93b5',
                     font=('Segoe UI', 9, 'bold')).grid(row=i, column=0, padx=(0, 4), pady=1)
            lab_v = tk.StringVar(value=ap.get('label', ''))
            com_v = tk.StringVar(value=ap.get('command', ''))
            tk.Entry(frame, textvariable=lab_v, width=12).grid(row=i, column=1, padx=2, pady=1)
            tk.Entry(frame, textvariable=com_v, width=42).grid(row=i, column=2, padx=2, pady=1)

            def browse(com=com_v, lab=lab_v, idx=i):
                from tkinter import filedialog
                p = filedialog.askopenfilename(
                    title='Приложение для кнопки %d' % (idx + 1),
                    filetypes=[('Программы/ссылки', '*.exe *.lnk *.url'),
                               ('Все файлы', '*.*')])
                if p:
                    com.set(p)
                    if not lab.get().strip():
                        import os
                        lab.set(os.path.splitext(os.path.basename(p))[0][:16])

            ttk.Button(frame, text='Обзор…', width=7,
                       command=browse).grid(row=i, column=3, padx=(6, 0), pady=1)
            entries.append((lab_v, com_v))

        err = tk.Label(dlg, text='', bg='#101826', fg='#ff9a8c', font=('Segoe UI', 8))
        err.pack(padx=10)

        def apply():
            apps = []
            for lab_v, com_v in entries:
                label = lab_v.get().strip()[:16]
                command = com_v.get().strip()
                apps.append({'label': label, 'command': command})
            self.settings['apps'] = apps
            save_settings(self.settings)
            if self.monitor and self.monitor.running:
                self.monitor.set_apps(apps)
                self._log('Список быстрого запуска отправлен на экран.')
            else:
                self._log('Быстрый запуск сохранён (применится при старте).')
            dlg.destroy()

        ttk.Button(dlg, text='Сохранить и отправить', command=apply).pack(pady=(0, 10))

    def _edit_thresholds(self):
        """Диалог настройки порогов авто-реле и тревог (страница CTRL).
        Уходит на экран строкой THR:… и хранится во флеше ESP32 — без
        перепрошивки."""
        cur = dict(engine.DEFAULT_THRESHOLDS)
        if isinstance(self.settings.get('thresholds'), dict):
            for k in cur:
                if k in self.settings['thresholds']:
                    try:
                        cur[k] = int(round(float(self.settings['thresholds'][k])))
                    except (TypeError, ValueError):
                        pass

        dlg = tk.Toplevel(self.root)
        dlg.title('Пороги AUTO-реле и тревог')
        dlg.configure(bg='#101826')
        dlg.transient(self.root)
        dlg.grab_set()
        try:
            dlg.geometry('400x360')
        except Exception:
            pass

        tk.Label(dlg, text='Реле AUTO: вкл при t ≥ порогу, выкл при t ≤ (гистерезис).',
                 bg='#101826', fg='#8fa3bf', font=('Segoe UI', 9)).pack(padx=12, pady=(10, 2))
        tk.Label(dlg, text='Тревоги/цвета: WARN — жёлтый, ALARM — красный + зуммер.',
                 bg='#101826', fg='#5a6c8c', font=('Segoe UI', 8)).pack(padx=12)

        frame = tk.Frame(dlg, bg='#101826')
        frame.pack(fill='both', expand=True, padx=12, pady=6)
        rows = [
            ('r1on',  'R1 CASE  вкл ≥  °C'),
            ('r1off', 'R1 CASE  выкл ≤ °C'),
            ('r2on',  'R2 GPU   вкл ≥  °C'),
            ('r2off', 'R2 GPU   выкл ≤ °C'),
            ('warn',  'WARN (жёлтый) ≥ °C'),
            ('crit',  'ALARM (красный) ≥ °C'),
        ]
        vars = {}
        for i, (key, label) in enumerate(rows):
            tk.Label(frame, text=label, bg='#101826', fg='#cfdae8',
                     font=('Segoe UI', 9)).grid(row=i, column=0, sticky='w', pady=2)
            v = tk.IntVar(value=cur[key])
            vars[key] = v
            tk.Spinbox(frame, from_=20, to=110, textvariable=v, width=6,
                       justify='center').grid(row=i, column=1, padx=6, pady=2)

        err = tk.Label(dlg, text='', bg='#101826', fg='#ff9a8c', font=('Segoe UI', 8))
        err.pack(padx=12)

        def apply():
            try:
                t = {k: v.get() for k, v in vars.items()}
            except Exception:
                err.configure(text='Введите целые числа (20..110).')
                return
            for k, v in t.items():
                if not (20 <= v <= 110):
                    err.configure(text='Порог %s вне диапазона 20..110 °C.' % k)
                    return
            # Гистерезис: off должен быть < on, warn < crit (как на прошивке)
            if not (t['r1off'] < t['r1on']):
                err.configure(text='У R1: «выкл при ≤» должен быть меньше «вкл при ≥».')
                return
            if not (t['r2off'] < t['r2on']):
                err.configure(text='У R2: «выкл при ≤» должен быть меньше «вкл при ≥».')
                return
            if not (t['warn'] < t['crit']):
                err.configure(text='WARN должен быть меньше ALARM.')
                return
            self.settings['thresholds'] = t
            save_settings(self.settings)
            if self.monitor and self.monitor.running:
                self.monitor.set_thresholds(t)
                self._log('Пороги отправлены на экран: R1>=%d/<=%d R2>=%d/<=%d W%d A%d'
                          % (t['r1on'], t['r1off'], t['r2on'], t['r2off'],
                             t['warn'], t['crit']))
            else:
                self._log('Пороги сохранены (применятся при старте монитора).')
            dlg.destroy()

        ttk.Button(dlg, text='Сохранить и отправить', command=apply).pack(pady=(0, 10))

    def _toggle(self):
        if self.monitor and self.monitor.running:
            self._stop()
        else:
            port = self.port_var.get()
            if not port:
                messagebox.showwarning(APP_NAME, 'Не выбран COM-порт ESP32.')
                self._refresh_ports()
                return
            self._start(port)

    def _start(self, port):
        self.settings['port'] = port
        self.settings['auto_start'] = True
        save_settings(self.settings)
        self.state_var.set('Подключение к %s…' % port)
        self._set_dot('#f39c12')
        self.start_btn.configure(text='Стоп')

        self.monitor = Monitor(
            port=port,
            on_log=lambda m: self._q.put(('log', m)),
            on_data=lambda f: self._q.put(('data', dict(f))),
            hotkeys=self.settings.get('hotkeys'),
            # событие в очереди должно оставаться парой (kind, payload),
            # иначе распаковка в _poll_queue падает и цикл очереди умирает
            on_hotkey_change=lambda i, l, c: self._q.put(('hkcfg', (i, l, c))),
            apps=self.settings.get('apps'),
            thresholds=self.settings.get('thresholds'),
            # пересечение порогов warn/crit -> всплывающее уведомление
            on_notify=lambda level, text: self._q.put(('alert', (level, text))),
        )
        self._log('Запуск монитора на %s…' % port)
        self.monitor.start()

    def _stop(self):
        if self.monitor:
            self.monitor.stop()
        self.state_var.set('Остановлен')
        self._set_dot('#7f8c8d')
        self.start_btn.configure(text='Пуск')

    def _quit(self):
        try:
            if self.icon:
                self.icon.stop()
        except Exception:
            pass
        if self.monitor:
            self.monitor.stop()
        try:
            self.root.destroy()
        except Exception:
            pass
        os._exit(0)

    def _on_autostart(self):
        enable = self.auto_win.get()
        exe = os.path.abspath(sys.argv[0])
        if autostart_set(exe, enable):
            self._log('Автозапуск с Windows: ' + ('включён' if enable else 'выключен'))
        else:
            messagebox.showerror(APP_NAME, 'Не удалось изменить автозапуск.')

    # ----------------------------- очередь -> UI -----------------------------
    def _poll_queue(self):
        try:
            while True:
                kind, payload = self._q.get_nowait()
                # Любой сбой обработчика события не должен «убивать» цикл
                # очереди (иначе журнал и автостарт замирают без следа).
                try:
                    if kind == 'log':
                        self._log(payload)
                    elif kind == 'data':
                        self._apply_data(payload)
                    elif kind == 'hkcfg':
                        self._on_hkcfg(*payload)
                    elif kind == 'alert':
                        self._show_alert(*payload)
                    elif kind == 'lhm_done':
                        self.lhm_var.set(payload)
                        self._busy_lhm = False
                        self.lhm_btn.configure(state='normal')
                        running = bool(self.monitor and self.monitor.running)
                        if 'работает' in payload and not running:
                            self._log('LHM готов — запускаю монитор…')
                            self.root.after(200, self._try_auto_start)
                except Exception as e:
                    self._log('Ошибка события %s: %s' % (kind, e))
        except queue.Empty:
            pass
        self.root.after(120, self._poll_queue)

    def _show_alert(self, level, text):
        """Пересечение порога warn/crit.
        Всплывающие окна Windows отключены (раздражали) — событие только
        пишется в журнал; индикация остаётся на самой плате (RGB + зуммер)."""
        self._log(('⚠ ТРЕВОГА: ' if level == 2 else 'Внимание: ') + text)
        if level >= 1:
            try:
                self.state_var.set('⚠ ' + text)
            except Exception:
                pass

    def _on_hkcfg(self, i, label, combo):
        """Комбинация изменена в редакторе на экране ESP32 — синхронизируем
        локальные настройки, чтобы при следующем старте ПК не «затёр» её
        своим устаревшим конфигом."""
        if not (0 <= i < 8):
            return
        hk = self.settings.get('hotkeys')
        if not isinstance(hk, list):
            hk = [dict(x) for x in engine.DEFAULT_HOTKEYS]
            self.settings['hotkeys'] = hk
        while len(hk) <= i:
            hk.append({'label': '', 'combo': ''})
        hk[i] = {'label': label, 'combo': combo}
        save_settings(self.settings)
        # Лог уже пишет engine (там же видно — единичное изменение или приём
        # всего конфига при старте), здесь только персистим в settings.json.

    def _apply_data(self, f):
        self._last_data = f
        if f.get('online') is None:
            pass
        fmt = {
            'cpu_temp': '%.0f°', 'cpu_load': '%.0f%%', 'cpu_freq': '%.2f',
            'gpu_temp': '%.0f°', 'gpu_load': '%.0f%%', 'gpu_freq': '%.0f',
            'ram_pct': '%.0f%%', 'cpu_power': '%.0f', 'gpu_power': '%.0f',
            'fan1': '%.0f', 'fan2': '%.0f', 'v12': '%.2f',
            'net_dn': '%.1f', 'net_up': '%.1f', 'uptime': '%.0f',
            'disk_pct': '%.0f%%', 'disk_free_gb': '%.0f',
            'c0t': '%.0f°', 'c1t': '%.0f°', 'c2t': '%.0f°', 'c3t': '%.0f°',
            'sys_temp': '%.0f°',
        }
        for key, lab in self.val_labels.items():
            v = f.get(key)
            if v is None:
                lab.configure(text='--')
            else:
                try:
                    lab.configure(text=fmt[key] % v)
                except Exception:
                    lab.configure(text=str(v))
        # статус
        cpu = f.get('cpu_temp')
        self.state_var.set('ESP32 на связи — данные идут')
        self._set_dot('#2ecc71')
        if not self.monitor or not self.monitor.running:
            self.state_var.set('Остановлен')
            self._set_dot('#7f8c8d')


# ----------------------------------------------------------------------------
#  Запуск
# ----------------------------------------------------------------------------
def run_gui():
    if not HAVE_TK:
        print('Нет tkinter — используйте консольную версию: python pc_monitor.py')
        return 1
    root = tk.Tk()
    CydApp(root)
    root.mainloop()
    return 0


def run_headless():
    """Без GUI (для отладки / -t)."""
    print('Headless-режим, жду…')

    def log(m):
        print('  ', time.strftime('%H:%M:%S'), m, flush=True)

    if not http_up():
        print('LHM не отвечает — пробую запустить.')
        print(ensure_lhm(on_log=log, elevated_start=True, wait_s=150))
    port = pick_port(on_log=log)
    if not port:
        print('Порт не найден')
        return 1
    mon = Monitor(port=port, on_log=log, on_data=lambda f: None)
    mon.start()
    try:
        while mon.running:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    mon.stop()
    return 0


def main():
    args = sys.argv[1:]
    if '--headless' in args:
        return run_headless()
    if not HAVE_TK:
        return run_headless()
    if '-t' in args or '--tray' in args:
        # старт в трее (без окна) — для автозапуска с Windows
        return run_gui_start_tray()
    return run_gui()


def run_gui_start_tray():
    if not HAVE_TK:
        return run_headless()
    root = tk.Tk()
    app = CydApp(root)
    root.withdraw()
    app.minimize_on_close = True
    if app.icon is None:
        # нет трея — просто свернуть в панель задач
        root.deiconify()
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
