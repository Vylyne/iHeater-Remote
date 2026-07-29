#!/usr/bin/env python3
"""
Fake Moonraker WS-server для проверки iHeater Link Moonraker integration.
Слушает ws://0.0.0.0:7125/websocket, отвечает на JSON-RPC и шлёт
notify_status_update с setpoint стола heater_bed.

Сценарий:
  1) Запустить:  python3 /tmp/fake_moonraker.py
  2) В локальном /setup выбрать Moonraker: host=<mac IP>, port=7125.
  3) ESP подключается → printer.objects.subscribe → SDK status callback
     → bed-setpoint auto-control → s_output (RMT pulse).
  4) Логи в Serial:
       [REMOTE] moonraker auto target=60.0C ...
"""
import asyncio, json, logging, sys

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("fake-moonraker")

try:
    import websockets
except ImportError:
    print("pip3 install websockets", file=sys.stderr)
    sys.exit(1)

# Текущие значения виртуальной камеры и setpoint стола (°C).
chamber_target = 0.0
bed_target = 0.0
clients = set()


async def broadcast_status(target_temp: float, bed_setpoint: float):
    """Шлём notify_status_update всем подписчикам."""
    msg = {
        "jsonrpc": "2.0",
        "method": "notify_status_update",
        "params": [
            {
                "gcode_macro VIRTUAL_CHAMBER": {
                    "target":      target_temp,
                    "temperature": -1,
                    "has_sensor":  0,
                },
                "heater_bed": {"temperature": 25.0, "target": bed_setpoint},
            },
            # eventtime
            123456.0,
        ],
    }
    payload = json.dumps(msg)
    if not clients:
        return
    log.info(f"→ notify heater_bed.target={bed_setpoint}")
    await asyncio.gather(
        *[c.send(payload) for c in clients], return_exceptions=True
    )


async def heartbeat():
    """Переключает setpoint стола: ниже/выше авто-порога каждые 10 секунд."""
    global chamber_target, bed_target
    pattern = [0.0, 100.0]
    idx = 0
    while True:
        bed_target = pattern[idx % len(pattern)]
        await broadcast_status(chamber_target, bed_target)
        idx += 1
        await asyncio.sleep(10)


async def handle(ws):
    clients.add(ws)
    peer = ws.remote_address
    log.info(f"+ client {peer}")
    try:
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except Exception:
                log.warning(f"bad JSON: {raw[:120]}")
                continue

            method = msg.get("method", "?")
            mid = msg.get("id")
            log.info(f"← {method} id={mid}")

            # JSON-RPC reply (только если есть id).
            result = {}
            if method == "server.connection.identify":
                result = {"connection_id": 42}
            elif method == "server.info":
                result = {
                    "klippy_connected": True,
                    "klippy_state": "ready",
                    "moonraker_version": "fake-1.0.0",
                }
            elif method == "printer.objects.list":
                result = {"objects": ["gcode_macro VIRTUAL_CHAMBER", "heater_bed"]}
            elif method == "printer.objects.subscribe":
                # Сразу отдаём текущее состояние + статус.
                result = {
                    "eventtime": 123456.0,
                    "status": {
                        "gcode_macro VIRTUAL_CHAMBER": {
                            "target":      chamber_target,
                            "temperature": -1,
                            "has_sensor":  0,
                        },
                        "heater_bed": {"temperature": 25.0, "target": bed_target},
                    },
                }
            else:
                result = {"ok": True}

            if mid is not None:
                resp = {"jsonrpc": "2.0", "id": mid, "result": result}
                await ws.send(json.dumps(resp))

            # После subscribe — сразу пушим notify, чтобы ESP отработал.
            if method == "printer.objects.subscribe":
                await broadcast_status(chamber_target, bed_target)
    except websockets.ConnectionClosed:
        pass
    finally:
        clients.discard(ws)
        log.info(f"- client {peer}")


async def main():
    log.info("listening on 0.0.0.0:7125 (path=/websocket)")
    log.info("press 't' + Enter in another shell? — see heartbeat (auto 10s flip 0↔70)")
    async with websockets.serve(handle, "0.0.0.0", 7125):
        await heartbeat()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
