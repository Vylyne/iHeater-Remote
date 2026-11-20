# iHeater Remote developer tools

## tools/fake_moonraker/

A local WebSocket server that imitates Moonraker (Klipper) for bench-testing
the Remote's Moonraker link without a real printer. It supports a pairing
token, several heaters, a large object list, proc-stat noise, and the
`restart`, `shutdown`, `freeze` and `drop` scenarios.

Details: [`fake_moonraker/fake_moonraker.README.md`](fake_moonraker/fake_moonraker.README.md).

```bash
pip install websockets
python tools/fake_moonraker/fake_moonraker.py --token ABCD-2345
```

In the Setup page, choose Klipper (Moonraker), set host to the IP of the
computer running the fake and port to `7125`, with TLS off.
