#pragma once
// ============================================================
// Протокол ПК <-> ESP32 (по USB-Serial, ASCII, 115200 8N1)
//
// ПК -> ESP32  (раз в секунду, строка датчиков + служебные):
//   MNOW:80|Artist - Title     (громкость %; отрицательная = mute; и трек)
//   TOP:name|cpu|mem|pid;...x5  (топ-5 процессов по CPU)
//
// ПК -> ESP32  (раз в секунду, одна строка, \n):
//   cpu_temp=52.3,gpu_temp=63.1,cpu_load=42.5,gpu_load=88.0,
//   cpu_freq=3450,gpu_freq=1980,cpu_power=63.5,gpu_power=185.2,
//   ram_pct=48.3,ram_total_gb=15.9,fan1=900,fan2=1200,
//   net_dn=12.4,net_up=3.1,v12=11.98,disk_pct=62.5,disk_free_gb=93.2,
//   uptime=25341
//   (любые поля могут отсутствовать)
//
// ПК -> ESP32  (настройка горячих клавиш, в любой момент):
//   HK:0=TaskMgr|ctrl+shift+esc
//   HK:1=Desktop|win+d
//   HKREQ                     (ПК при старте: пришли весь конфиг — экран
//                              источник истины; ответ — строки HKCFG: ниже)
//   AP:0=Explorer             (подпись кнопки вкладки APPS; команда живёт
//                              в ПК-приложении — сюда только подпись)
//   THR:r1on=60,r1off=55,     (пороги реле AUTO и тревог WARN/ALARM;
//        r2on=65,r2off=60,      кнопка «Пороги…» в ПК-приложении)
//        warn=60,crit=75)
//
// ESP32 -> ПК  (команды управления):
//   CMD:sleep | CMD:reboot | CMD:shutdown
//   MEDIA:mute | MEDIA:voldn | MEDIA:volup
//   MEDIA:prev | MEDIA:play | MEDIA:next
//   KEY:0 .. KEY:7            (нажата кнопка горячей клавиши)
//   APP:0 .. APP:7            (нажата кнопка быстрого запуска на вкладке APPS)
//   HKCFG:2=Close|ctrl+w      (комбинация изменена в редакторе НА ЭКРАНЕ;
//                              ПК обновляет свою таблицу и пересылает в GUI)
// ============================================================
#include <Arduino.h>
#include "pc_data.h"

// Разобрать строку key=val,key=val,... и обновить d.
// Возвращает true, если обновлено хотя бы одно поле.
bool protocolParseLine(const String& line, PCData& d);

// Отправить команду управления на ПК
void protocolSendCommand(const char* name);

// Отправить на ПК медиа-клавишу (см. список MEDIA: выше)
void protocolSendMedia(const char* action);

// Отправить на ПК нажатие горячей клавиши с номером 0..HK_MAX-1
void protocolSendKey(int index);

// Уведомить ПК, что комбинация слота index изменена в редакторе на экране
void protocolSendHkCfg(int index);

// Отправить на ПК запрос запуска приложения с номером 0..APP_MAX-1
void protocolSendApp(int index);

// Разобрать служебные строки MNOW: и TOP: (медиа/процессы)
void protocolParseMediaNow(const String& line, PCData& d);
void protocolParseTop(const String& line, PCData& d);

// Завершить процесс на ПК (вкладка PROCS): "KILL:pid"
void protocolSendKill(long pid);
