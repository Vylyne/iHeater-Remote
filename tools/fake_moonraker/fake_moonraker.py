#!/usr/bin/env python3
"""
Fake Moonraker WebSocket server for bench-testing iHeater Remote.

Listens on ws://0.0.0.0:<port>/websocket and answers the JSON-RPC calls the
Remote makes: server.connection.identify, printer.objects.list,
printer.objects.query and printer.objects.subscribe. It simulates one or
more `heater_virtual` objects, each carrying a `remote_token`, and sends
`notify_proc_stat_update` noise every second like a real Moonraker.

Commands on stdin (type then Enter):
  t <C>      set the paired heater's target and notify
  restart    notify_klippy_disconnected, target 0, notify_klippy_ready in 2 s
  shutdown   target 0, notify_status_update, notify_klippy_shutdown;
             Klipper stays shut down until restart; list/query/subscribe still
             answer, with webhooks.state "shutdown", as real Klipper does
  freeze     stop answering everything, pings included, until "thaw"
  thaw       resume after "freeze"
  drop       close all sockets
"""
import argparse
import asyncio
import json
import logging
import sys

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("fake-moonraker")

try:
    import websockets
except ImportError:
    print("pip install websockets", file=sys.stderr)
    sys.exit(1)

TOKEN_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

clients = set()
frozen = False
klippy_state = "ready"  # "ready", "startup" (restart) or "shutdown" (sticky until restart)
heaters = {}  # object name -> {"token": str, "target": float}
PAIRED = "heater_virtual chamber"  # the heater that gets --token


def normalize_token(raw: str) -> str:
    return raw.upper().replace("-", "")


def build_heaters(token: str, count: int):
    """First heater gets --token (or none); extra heaters get other tokens."""
    heaters.clear()
    heaters[PAIRED] = {"token": normalize_token(token) if token else "", "target": 0.0}
    for i in range(1, count):
        other = "".join(TOKEN_ALPHABET[(i * 7 + j * 3) % 32] for j in range(8))
        heaters[f"heater_virtual extra{i}"] = {"token": other, "target": 0.0}


def status_for(requested: dict) -> dict:
    """Build a status object for the requested objects/fields."""
    status = {}
    for name, fields in requested.items():
        if name == "webhooks":
            status[name] = {"state": klippy_state}
        elif name in heaters:
            h = heaters[name]
            out = {}
            for f in fields or ["target", "remote_token"]:
                if f == "target":
                    out[f] = h["target"]
                elif f == "remote_token":
                    out[f] = h["token"]
            status[name] = out
    return status


async def send_all(msg: dict):
    if frozen or not clients:
        return
    payload = json.dumps(msg)
    await asyncio.gather(*[c.send(payload) for c in list(clients)], return_exceptions=True)


async def notify(method: str, params=None):
    await send_all({"jsonrpc": "2.0", "method": method, "params": params if params is not None else []})


async def notify_target():
    h = heaters[PAIRED]
    log.info(f"-> notify {PAIRED} target={h['target']}")
    await notify("notify_status_update", [{PAIRED: {"target": h["target"]}}, 123456.0])


async def noise():
    while True:
        await asyncio.sleep(1)
        await notify(
            "notify_proc_stat_update",
            [{"moonraker_stats": {"time": 1.0, "cpu_usage": 1.2, "memory": 40000},
              "cpu_temp": 41.0, "network": {"wlan0": {"rx_bytes": 1, "tx_bytes": 1}}}],
        )


def make_object_list(padding: int):
    objs = ["webhooks", "configfile", "heater_bed", "extruder", "gcode_move", "toolhead"]
    objs += [f"gcode_macro PAD_{i}" for i in range(padding)]
    objs += list(heaters.keys())
    return objs


async def handle(ws):
    clients.add(ws)
    peer = ws.remote_address
    log.info(f"+ client {peer}")
    try:
        async for raw in ws:
            if frozen:
                continue
            try:
                msg = json.loads(raw)
            except Exception:
                log.warning(f"bad JSON: {raw[:120]}")
                continue

            method = msg.get("method", "?")
            mid = msg.get("id")
            params = msg.get("params") or {}
            log.info(f"<- {method} id={mid}")
            if mid is None:
                continue

            reply = {"jsonrpc": "2.0", "id": mid}
            if method == "server.connection.identify":
                reply["result"] = {"connection_id": 42}
            elif method in ("printer.objects.list", "printer.objects.query",
                            "printer.objects.subscribe") and klippy_state == "startup":
                # Only the restart window errors. In "shutdown" Klipper still
                # answers, and webhooks.state reads "shutdown".
                reply["error"] = {"code": 503, "message": "Klippy Host not ready"}
            elif method == "printer.objects.list":
                reply["result"] = {"objects": make_object_list(ARGS.objects)}
            elif method == "printer.objects.query":
                reply["result"] = {"eventtime": 123456.0,
                                   "status": status_for(params.get("objects", {}))}
            elif method == "printer.objects.subscribe":
                reply["result"] = {"eventtime": 123456.0,
                                   "status": status_for(params.get("objects", {}))}
            else:
                reply["result"] = {"ok": True}
            await ws.send(json.dumps(reply))
    except websockets.ConnectionClosed:
        pass
    finally:
        clients.discard(ws)
        log.info(f"- client {peer}")


def set_reading(paused: bool):
    """Stop (or resume) reading from every socket so pings go unanswered."""
    for c in list(clients):
        transport = getattr(c, "transport", None)
        if transport is None:
            log.warning("freeze/thaw: this websockets version exposes no transport; pongs keep flowing")
            continue
        try:
            if paused:
                transport.pause_reading()
            else:
                transport.resume_reading()
        except Exception as exc:  # transport already closed
            log.warning(f"freeze/thaw failed on a socket: {exc}")


async def restart_sequence():
    global klippy_state
    klippy_state = "startup"
    heaters[PAIRED]["target"] = 0.0
    await notify("notify_klippy_disconnected")
    log.info("restart: disconnected, ready in 2 s")
    await asyncio.sleep(2)
    klippy_state = "ready"
    await notify("notify_klippy_ready")
    log.info("restart: ready")


async def command_loop():
    global frozen, klippy_state
    loop = asyncio.get_running_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            await asyncio.sleep(3600)  # stdin closed; keep serving
            continue
        parts = line.split()
        if not parts:
            continue
        cmd = parts[0].lower()
        if cmd == "t" and len(parts) == 2:
            try:
                heaters[PAIRED]["target"] = float(parts[1])
            except ValueError:
                log.warning("usage: t <C>")
                continue
            await notify_target()
        elif cmd == "restart":
            asyncio.create_task(restart_sequence())
        elif cmd == "shutdown":
            heaters[PAIRED]["target"] = 0.0
            klippy_state = "shutdown"  # stays until "restart", as in real Klipper
            await notify_target()
            await notify("notify_klippy_shutdown")
            log.info("shutdown sent; Klipper stays shut down until restart")
        elif cmd == "freeze":
            frozen = True
            set_reading(True)
            log.info("frozen: no replies, no pongs")
        elif cmd == "thaw":
            frozen = False
            set_reading(False)
            log.info("thawed")
        elif cmd == "drop":
            log.info("dropping all sockets")
            await asyncio.gather(*[c.close() for c in list(clients)], return_exceptions=True)
        else:
            log.warning("commands: t <C>, restart, shutdown, freeze, thaw, drop")


async def main():
    build_heaters(ARGS.token, ARGS.heaters)
    log.info(f"listening on 0.0.0.0:{ARGS.port} (path=/websocket)")
    log.info(f"paired heater: {PAIRED!r} token={ARGS.token or '(none, unpaired)'}")
    # ping_interval=None: the Remote does the pinging, not this server.
    async with websockets.serve(handle, "0.0.0.0", ARGS.port, ping_interval=None):
        await asyncio.gather(noise(), command_loop())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=7125)
    parser.add_argument("--token", default="",
                        help="remote_token of the paired heater, XXXX-XXXX "
                             "(default: none, the unpaired case)")
    parser.add_argument("--heaters", type=int, default=1,
                        help="number of heater_virtual objects; extras get other tokens")
    parser.add_argument("--objects", type=int, default=150,
                        help="padding objects in printer.objects.list")
    ARGS = parser.parse_args()
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
