# RedisExt

<p align="center">
  <b>Высокопроизводительное расширение (x86 DLL) для Arma 2: Operation Arrowhead, DayZ и Origins Mod 1.8.3</b><br>
  <i>High-performance x86 C++ Redis extension for Arma 2: Operation Arrowhead, DayZ, and Origins Mod 1.8.3</i>
</p>

---

## 🇷🇺 Описание на русском

**RedisExt** — это серверное C++ расширение (32-bit DLL), разработанное для полной замены устаревших баз данных MySQL в Arma 2, DayZ и DayZ Origins Mod 1.8.3. Расширение обеспечивает мгновенный отклик при синхронизации персонажей, техники, построек и глобального инвентаря благодаря работе напрямую с оперативной памятью в **Redis**.

### Ключевые возможности

* ⚡ **Сверхбыстрая работа:** Задержки чтения и записи сокращены до долей миллисекунды.
* 📦 **Автоматический чанкинг данных:** Обход встроенного лимита Arma 2 `callExtension` в 4096 байт через протокол передачи частями (`CHILD:FETCH`), что позволяет передавать любые объемы данных (списки сотен единиц техники или домов).
* 🏰 **Полная поддержка Origins Mod 1.8.3:**
  * Управление всеми уровнями построек (`Uroven 1, 2, 3...`) с контролем лимитов на игрока.
  * Поддержка неограниченных построек (заборы, вышки, ворота, стены).
  * Корректное безвозвратное удаление домов (`CHILD:504`).
  * Механика временных кулдаунов на улучшение через TTL в Redis (`CHILD:510`).
* 👤 **Сохранение реальных никнеймов игроков:** Хранение точных отображаемых имен (`Nick:<UID>`) с безопасным экранированием спецсимволов для интеграции с веб-панелями и мониторингами.
* 🚗 **Управление техникой и объектами:** Спавн, синхронизация позиции, груза, топлива, повреждений отдельных узлов и полное уничтожение.
* 🔨 **Мульти-базы:** Динамическое переключение баз данных Redis (`CHILD:999`) на лету для мульти-серверных или мульти-карточных кластеров (Черноруссия, Тавиана и т.д.).
* 📡 **Встроенный телеметрический броадкаст:** Опциональная фоновая отправка состояния сервера на внешний API мониторинга.

---

## 🇬🇧 English Description

**RedisExt** is a high-performance 32-bit C++ server extension for Arma 2, DayZ, and DayZ Origins Mod 1.8.3. It completely replaces legacy MySQL / SQLite database engines with an in-memory **Redis** back-end, eliminating server stutter and disk I/O bottlenecks during peak traffic.

### Key Features

* ⚡ **Ultra-low Latency:** Sub-millisecond reads and writes directly to Redis in-memory storage.
* 📦 **Automatic Payload Chunking:** Transparently overcomes the Arma 2 4096-byte `callExtension` return limit via a multi-chunk streaming protocol (`CHILD:FETCH`).
* 🏰 **Full Origins Mod 1.8.3 Base Building Support:**
  * Per-player limit control on major house tiers (Uroven 1, 2, 3...).
  * Unlimited building limits for utility structures (fences, watchtowers, gates, walls).
  * Clean permanent structure deletion (`CHILD:504`).
  * Upgrade timer locks using native Redis expiration TTL (`CHILD:510`).
* 👤 **Real Nickname Preservation:** Dedicated UTF-8/escaped storage (`Nick:<UID>`) preserving exact player names for web dashboards.
* 🚗 **Vehicles & Dynamic Objects:** Complete persistence of position, hitpoint damage arrays, cargo gear, fuel fractions, and destruction events.
* 🔨 **Dynamic Database Switching:** Instant selection of Redis DB index (`CHILD:999`) without restarting the server.
* 📡 **Integrated Health Broadcast:** Configurable background thread reporting server heartbeat to an external telemetry API.

---

## Структура репозитория / Repository Structure

```
RedisExt/
├── bin/                       # Готовые скомпилированные бинарники / Compiled binaries
│   ├── RedisExt.dll           # 32-bit (x86) DLL для сервера Arma 2
│   ├── hiredis.dll            # Runtime библиотека hiredis
│   └── RedisExt.ini           # Конфигурационный файл
├── config/                    # Шаблоны конфигураций / Configuration templates
│   └── RedisExt.ini
├── docs/                      # Документация / Documentation
│   ├── API_REFERENCE.md       # Спецификация всех методов CHILD (101 - 510, 999)
│   ├── BUILD_GUIDE.md         # Руководство по компиляции
│   └── REDIS_SCHEMA.md        # Схема ключей, хэшей и TTL в Redis
├── examples/                  # Примеры кода / SQF examples
│   └── sqf_examples.sqf       # Полные примеры вызова всех методов из SQF
├── src/                       # Исходный код / Source code
│   └── dllmain.cpp            # Чистый исходный код расширения
├── build.bat                  # Автоматический скрипт сборки (MSVC x86)
├── CMakeLists.txt             # Скрипт сборки CMake
├── .gitignore
└── README.md
```

---

## Быстрый старт / Quick Start

### 1. Установка / Installation
1. Скопируйте файлы из папки `bin/` (`RedisExt.dll`, `hiredis.dll`, `RedisExt.ini`) в корневой каталог вашего сервера Arma 2 (туда, где лежит `arma2oaserver.exe`).
2. Убедитесь, что сервер **Redis** запущен и доступен по сети.

### 2. Настройка `RedisExt.ini` / Configuration
Откройте `RedisExt.ini` и укажите параметры подключения:

```ini
[Redis]
IP=127.0.0.1
Port=6379
Password=
DB=0

[General]
LogLevel=1

[Broadcast]
ServerDevelopBroadCast=0
IntervalMinutes=3
ApiUrl=https://originsmod.1sl.su/api_server_broadcast
GamePort=2302
BuildTag=Origins183
TimeoutSeconds=10
```

### 3. Использование в SQF / SQF Usage

```sqf
// Простой вызов / Simple call
_res = "RedisExt" callExtension "CHILD:101:76561198000000000:1:PlayerName:";
_parsed = call compile _res;

// Пример создания дома / Example house creation
_buildRes = "RedisExt" callExtension format [
    "CHILD:501:%1:%2:%3:%4:%5:",
    "Uroven1DrevenaBudka",
    str [getDir player, getPosATL player],
    getPlayerUID player,
    name player,
    round (time)
];
```

Подробные примеры со всеми обработчиками чанков смотрите в файле [examples/sqf_examples.sqf](examples/sqf_examples.sqf).

---

## Компиляция / Compilation

Для самостоятельной сборки потребуется **Visual Studio 2019/2022** с компонентами C++ и установленная библиотека **hiredis:x86-windows** (через vcpkg).

```cmd
:: Установка hiredis через vcpkg
vcpkg install hiredis:x86-windows

:: Запуск автоматической сборки
build.bat
```

Подробнее о шагах сборки смотрите в [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md).

---

## Лицензия / License

Распространяется свободно для сообщества разработчиков Arma 2, DayZ и Origins Mod под лицензией **MIT License**.
Open-source under the **MIT License**.
