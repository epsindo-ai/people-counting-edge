# Store people counter

DeepStream 7.1 app (stock `deepstream-test5-app`) that counts people crossing a line
and sends the counts to MQTT.

```
./download.sh   # once: get PeopleNet from NGC
./start.sh      # build message library, run
```

## Files

| File | What |
|---|---|
| `config/app.txt` | pipeline: cameras, model, tracker, outputs |
| `config/analytics.txt` | counting lines per camera |
| `config/msgconv.txt` | store / camera names, message settings |
| `config/pgie_peoplenet.txt` | PeopleNet model settings |
| `msgconv/` | small library that builds the MQTT message |
| `mqtt/` | NVIDIA's MQTT adapter + 2 fixes for broker outages (see its Makefile) |

Adding the second camera: see the checklist at the top of `config/app.txt`.

## MQTT

Topic: `stores/<store>/events` (all cameras of a store, one topic).

A message is sent per camera:
- `reason: "crossing"`: right away when someone crosses a line
- `reason: "heartbeat"`: every `heartbeat-sec` (default 1 s), even when empty

No heartbeat for a few seconds means the camera or the app is down.

If the broker goes down, the app keeps counting, skips messages while it is down,
and reconnects (retries every 1 s for up to 1 hour). Totals in the next message
include crossings from the gap.

```json
{
  "version": "1.0",
  "messageid": "efe414fb-2736-4977-be14-29fea71618a2",
  "@timestamp": "2026-10-16T02:59:31.085Z",
  "reason": "crossing",
  "store":  { "id": "mystore", "name": "My Store" },
  "sensor": { "id": "cam0", "description": "Entrance", "place": "FrontDoor", "source_id": 0 },
  "counts": {
    "in_view": 1,
    "entries": 0,
    "exits": 1,
    "total_entries": 0,
    "total_exits": 1,
    "since": "2026-10-16T02:59:29.181Z"
  },
  "events":  [ { "type": "exit", "object_id": 0, "line": "Exit" } ],
  "objects": [ { "id": 0, "confidence_pct": 90,
                 "bbox": { "left": 892, "top": 34, "right": 1014, "bottom": 460 } } ],
  "frame": { "number": 65, "width": 1920, "height": 1080 }
}
```

| Field | Meaning |
|---|---|
| `counts.in_view` | people in the camera view now |
| `counts.entries` / `exits` | new crossings since this camera's last message |
| `counts.total_entries` / `total_exits` | all crossings since `since` |
| `counts.since` | when totals started. Changes on app or stream restart (totals go back to 0) |
| `events` | who crossed in this frame (tracker id, line name) |
| `objects` | people in view; bbox in camera pixels. Off with `include-objects=0` |

Dashboard tips:
- Use the `total_*` values, not the sum of `entries`. A lost message then doesn't matter.
- Store occupancy ≈ sum over entrance cameras of `total_entries - total_exits`.
  When `since` changes, the totals restarted from 0: keep your own offset.
