# fake_moonraker.py

Локальный заглушечный WebSocket-сервер, имитирующий Moonraker для проверки
интеграции iHeater Link без реального Klipper/принтера.

## Назначение

Проверка цепочки:

```
fake_moonraker → ESP32 (LinkIntegrationsManager → onVirtualChamberUpdate)
              → auto_heat (filament-type lookup в menu)
              → RmtOutputAdapter (RMT pulse) → STM32 iHeater
              → mirrorRmtToStatus → MQTT status → портал
```

## Что делает

- Слушает `ws://0.0.0.0:7125/websocket`.
- Отвечает на JSON-RPC: `server.connection.identify`, `server.info`,
  `printer.objects.list`, `printer.objects.subscribe`.
- После `printer.objects.subscribe` шлёт `notify_status_update` с объектом
  `heater_bed.target` всем подписчикам.
- Каждые 10 секунд переключает setpoint стола между `0.0` и `100.0` °C,
  провоцируя локальный авто-контроль на ON/OFF цикл.

## Запуск

```bash
pip3 install --user websockets
python3 tools/fake_moonraker.py
```

В логе появится:

```
listening on 0.0.0.0:7125 (path=/websocket)
+ client 192.168.0.x
← server.connection.identify id=1
← printer.objects.subscribe id=2
→ notify heater_bed.target=0.0
→ notify heater_bed.target=100.0
```

## Настройка локальной интеграции

Откройте `http://<iHeater-IP>/setup`, выберите **Klipper (Moonraker)** и укажите:

| Поле     | Значение            |
| -------- | ------------------- |
| host     | `<IP вашего мака>`  |
| port     | `7125`              |
| auto target / bed threshold | значения для проверки авто-режима |

IP мака:

```bash
ipconfig getifaddr en0
```

ESP32 и мак должны быть в одной WiFi-сети.

Выбор Moonraker локально выключает Bambu; источник управления всегда один.

## Ожидаемый Serial-вывод на ESP

```
[INFO ] MQTT: ← .../commands/link_integration ... {"type":"moonraker", "enabled":true, ...}
[CMD  ] link_integration: switched active to moonraker
[REMOTE] moonraker auto target=60.0C
[DEBUG] MQTT: → .../status (...): {"unitsInfo":[{"mode":"Drying","target":{"temperature":70 ...}}]}
```

## Диагностика

- ESP не подключается → проверьте firewall на маке (System Settings →
  Network → Firewall → разрешить входящие подключения для `python3`).
- Подключение установилось, но `heater_bed.target` не приходит → ESP не шлёт
  `printer.objects.subscribe`. Проверьте host и порт в локальном `/setup`.
- Pylance ругается «websockets не разрешается» — ложноположительное:
  библиотека стоит в `~/Library/Python/3.9/`, runtime её видит.
