# Manual override of the Klipper target — design

Status: approved (2026-11-23).

## Why

While the Remote follows Klipper, a manual press (web or BOOT button)
currently unbinds the printer. `applyMode(..., releasePrinter=true)` saves
the source as Manual and stops the Moonraker link
(`src/standalone/StandaloneApp.cpp`). A quick tweak silently breaks the
pairing. The owner wants a manual press to be a temporary override that
lasts until Klipper's target changes.

## Goals, non-goals, anti-goals

- **Goal:** a manual press while following Klipper overrides the target and
  keeps the pairing. Klipper takes over again when its target changes.
- **Non-goals:**
  - telling Klipper or Mainsail about the override;
  - changing Klipper, Moonraker or the report format.
- **Anti-goals:**
  - a Moonraker reconnect ending an override, since it repeats the same
    target;
  - an override that outlives a Remote restart;
  - weakening the 10-minute link-lost heat cut.

## Approaches

1. **An override with a baseline, inside the Remote (chosen).**
2. Telling Klipper about the override. Dropped: the owner said the
   dashboard is enough.
3. Keeping the unbind and adding a rebind button. Dropped: it doesn't meet
   the ask.

## Design

- **Starting:** while the source is Klipper, a web or button press (Off
  included) starts an override instead of unbinding. The Remote records
  Klipper's target at that moment as the baseline.
- **Klipper's target during an override:** one equal to the baseline is
  ignored, as Moonraker re-sends it on every reconnect. Any other value
  ends the override and is applied at once, 0 included.
- **Back to Klipper:** a dashboard button ends the override and applies
  Klipper's current target.
- **Timer:** if the manual timer runs out during an override, the override
  ends and Klipper's current target applies. It does not just turn off.
- **Restart:** the override ends, and Klipper's target applies.
- **Link lost:** the existing 10-minute heat cut still applies during an
  override. Setup's "Manual controls only" remains for heating without the
  printer host.
- **Unpaired:** if the Moonraker link unpairs (target forced to 0), any
  override ends.
- **Dashboard:** shows "Manual override, Klipper wants N °C" and a "Back to
  Klipper" button. The status JSON gains the override flag and Klipper's
  target.
- **Manual source:** "Manual controls only" in Setup keeps today's
  behaviour.

## Left to the builder

- Whether baseline equality uses a small tolerance, since Moonraker sends
  floats.
- How "Back to Klipper" is exposed, for example
  `POST /api/mode` with a new field or a new route. Either way it's an
  action under the web-auth design.
- Exact dashboard wording and placement.

## Testing

- **Native** (`lib/remote-policy`), covering the override decision:
  - start;
  - a same-target repeat is ignored;
  - a changed target ends it;
  - a change to 0 ends it;
  - Back to Klipper;
  - timer expiry returns to Klipper;
  - unpairing ends it;
  - a press in Manual source isn't an override.
- **Bench:**
  - override at idle, then a print start takes over;
  - override mid-print, then the print end turns heat off;
  - a Moonraker restart during an override keeps it;
  - Back to Klipper.
