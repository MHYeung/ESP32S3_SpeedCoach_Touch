# BLE sensors

The speed coach is a NimBLE **central and observer**. It does not advertise. Two connection slots share one radio:

| Slot | What it connects to | Stage |
|------|---------------------|--------|
| Heart rate | Any strap that advertises the standard Heart Rate Service `0x180D` | Shipped |
| RowPod | C3 motion tracker advertising `9b7e1000-2b2f-4f71-9b86-4bb2e6d54f00` | GAP connect only. Stroke metrics are the next stage |

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is 2. ATT MTU stays 23. Bonding stays off: the heart-rate profile does not need it, and a bond table costs RAM on a board with no PSRAM.

Free internal heap is logged immediately before and after `nimble_port_init`. Do not turn on PSRAM to hide that cost.

## Heart-rate strap

The Sensors page **Heart rate** card scans only for advertisements that carry 16-bit UUID `0x180D` (or Heart Rate service data). A tap connects, writes the address and name to NVS (`hr_addr`, `hr_atyp`, `hr_name`), and runs GATT discovery on the NimBLE host task:

1. Discover service `0x180D`, characteristic Heart Rate Measurement `0x2A37`, then its descriptors.
2. Write `0x0001` to CCCD `0x2902` (notifications). If the characteristic is indicate-only, write `0x0002` instead.
3. Ask for a connection interval of 100–200 ms with slave latency 4. The strap notifies about once a second, so it can skip four connection events. Supervision timeout is 4 s, which is above `(1 + latency) * interval_max * 2`.
4. Read Battery Level `0x180F` / `0x2A19` once, if the strap has it. Missing battery is not a failure.

### Heart Rate Measurement flags

`0x2A37` (Bluetooth GATT Specification Supplement):

| Bits | Meaning |
|------|---------|
| 0 | 0 = BPM is `uint8`, 1 = BPM is `uint16` little-endian |
| 1–2 | Sensor contact: `0`/`1` not supported, `2` supported but off-skin, `3` on-skin |
| 3 | Energy Expended field follows (2 bytes). Ignored |
| 4 | RR intervals follow. Ignored in this stage |

Off-skin (`contact == 2`) is stored as 0 bpm. A sample older than 5 s is also reported as 0. The live **HR** metric then shows `--`.

### Reconnect

A saved strap reconnects by itself after boot and after a drop, unless the user tapped **Disconnect** (that pause clears on **Reconnect**, **Forget**, or reboot). The timer is a NimBLE callout, so it runs on the host task next to GAP events. The first try is at 500 ms; after a failure the delay is 2 s and doubles up to 15 s.

Only one `ble_gap_connect` is outstanding. A scan cancels a pending reconnect and arms it again when the scan ends. Opening Sensors does not start a scan by itself, so a reconnect already in progress is left alone.

### What the athlete sees

- Sensors page: name, bpm, battery, Pair / Reconnect / Disconnect / Forget.
- Status rail: a Bluetooth glyph. Hidden when no strap is saved or the user disconnected it. Accent colour while a sample is fresh. Red when a saved strap is missing or stale.
- Settings row: `HR On / Pod Off`.
- Live data slots: **HR** is the metric after Strokes. Tap a slot until it appears. `0` renders as `--`.
- CSV: `Heart Rate (bpm)` appended to each stroke row (blank when there is no fresh sample). `Avg HR` appended to split and interval rows (mean of the non-zero samples in that split or phase, blank when there were none). Splits header line `HR Sensor,<name>` or `HR Sensor,none`. The activity detail title shows `HR <avg>` when the splits file has samples.

`stroke_task` is still the only writer of `coach_ui_snapshot_t`. BLE callbacks never call LVGL. The Sensors page polls `sensor_hub_get_epoch()` from an LVGL timer.

## RowPod

The **RowPod** card scans for motion service `9b7e1000-2b2f-4f71-9b86-4bb2e6d54f00`. The address stays in `sn_addr` / `sn_atyp` (a pairing from older firmware still loads). The name is `sn_name`.

After connect the coach looks for summary characteristic `9b7e1005-2b2f-4f71-9b86-4bb2e6d54f00` and subscribes. One 20-byte notify arrives per finished stroke (protocol version 1):

| Offset | Type | Field |
|--------|------|--------|
| 0 | u8 | version = 1 |
| 1 | u8 | flags: bit0 zeroed, bit1 feather trusted, bit2 valid |
| 2 | u16 | stroke index |
| 4 | u32 | sample index at the catch |
| 8 | i16 | catch, decidegrees |
| 10 | i16 | finish, decidegrees |
| 12 | u16 | absolute arc, decidegrees |
| 14 | u16 | SPM × 10, catch to catch |
| 16 | u16 | drive time, ms |
| 18 | u8 | battery percent, 255 if unknown |
| 19 | u8 | reserved |

Control `9b7e200c-2b2f-4f71-9b86-4bb2e6d54f00` is write-only. Opcode 1 zeros the oar (hold it square and perpendicular). Opcode 2 plus a byte picks the side: 0 stroke, 1 bow. Opcode 3 clears the in-progress stroke. **Zero oar** and **Stroke/Bow** on the RowPod card send these. Side and zero are stored on the pod.

The pod computes the angles. It does not send the 56 Hz IMU stream to the coach. Capability flag `0x40` on `…2009` means this summary exists. A pod built before that flag still connects; the card stays at GAP and the catch/arc metrics stay `--`.

A saved pod reconnects on its own, same backoff as the strap, unless the user tapped Disconnect. The pod allows one central, so the phone app cannot attach while the coach holds the link. Forget or Disconnect releases it.

The coach asks for a 30–50 ms connection interval after the summary subscribe. The pod does not push its 7.5 ms capture interval unless a motion characteristic is also subscribed.

Live slots **Catch** and **Arc** sit after HR. Stroke CSV columns `Catch (deg)`, `Finish (deg)`, `Arc (deg)` are appended after heart rate and left blank until a stroke has arrived.

## Compatibility

| Strap | BLE heart rate |
|-------|----------------|
| Polar H9, H10 | Yes. H10 can keep two centrals; most other straps allow one, so a watch already connected may have to be disconnected first |
| Polar OH1, Verity Sense | Yes, when their BLE heart-rate profile is on |
| Garmin HRM-Pro, HRM-Pro Plus, HRM-Dual, HRM 600 | Yes |
| Garmin HRM-Run, HRM-Tri, and older ANT+ only straps | No. The ESP32-S3 has no ANT+ radio |

## On-water check

1. Pair a Polar H9 or H10 and a Garmin HRM-Pro or Dual. Compare bpm with the vendor app or a watch.
2. Take the strap off. The HR metric and the Sensors status show no bpm within 5 s, and the rail glyph turns red.
3. Power-cycle the strap during a session. It reconnects without opening the Sensors page. The CSV stroke column fills again.
4. Tap Disconnect. The glyph hides and the coach does not reconnect until Reconnect.
5. Open the new CSV in a spreadsheet. Old columns are in the same places. `Heart Rate (bpm)` is the last stroke column. `Avg HR` is the last split column.
6. Water lock and the PWR key behave as before.
