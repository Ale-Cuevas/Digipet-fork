# Battery telemetry

Logging starts automatically on battery. No SD card or wireless connection is
needed. The logger never wakes the chip, adds no battery polling, and does not
change brightness, CPU speed, or radio policy. Battery readings follow the
existing 30-second poll; a sample is logged every minute while awake. State
changes are checked once a second. Blocking Wi-Fi and BLE scans have explicit
start/end events. Scan events carry the last battery reading, not a new reading.

Pending records live in RTC memory, retained across deep sleep. A batch is
checkpointed to a separate `powerlog` NVS namespace after 16 records, after five
minutes within an awake session when another event arrives, or when external
power is detected. There are 16 rotating batch slots (at most 256 saved records,
plus 16 pending records). Partial checkpoints reduce the retained record count.
Oldest batches are overwritten. Existing pet/settings keys are not changed.

There are no writes while the chip is asleep; the sleep-entry event can finish
a batch and trigger a checkpoint before sleep. A complete battery loss or other reset
can lose the pending batch, including recent sleep/wake events. Reconnect before
the battery runs flat for the most complete log. Storage failures retain the
pending batch, throttle retries, and drop new events if the batch is full.
Actual logging energy overhead has not yet been measured.

Reconnect USB and wake with BOOT if necessary, then run:

```sh
python3 scripts/read_power_log.py /dev/ttyACM1 /tmp/digipet-power.csv
```

Requires Python `pyserial`. The `POWERLOG` serial command exports CSV without
clearing records or resetting the device. USB power is normally detected within
30 seconds; samples before detection may still be labelled battery powered.
The latest firmware preserves the existing USB-powered screen-off/BLE behavior.

CSV fields: sequence; boot number (increments on each deep-sleep wake too);
uptime in milliseconds within that boot; Unix epoch (0 if clock invalid); event;
battery millivolts; battery percent; display brightness (0–255); page enum;
Wi-Fi mode enum; `FamiliarBattleState` enum; flags.

Events: 0 boot, 1 wake, 2 sample, 3 entering deep sleep, 4 power connected,
5 power removed, 6 Wi-Fi scan start, 7 Wi-Fi scan end, 8 BLE scan start,
9 BLE scan end, 10 page/brightness/battle-state change.

Flags: 1 external power, 2 charging, 4 screen off, 8 valid battery reading.
Wi-Fi mode and battle state are software states, not measured radio duty cycle.
Compare timestamps within a boot for scan durations. Sleep intervals require
valid epoch timestamps; clock adjustments can make an interval unreliable.
No current measurement is available, and voltage/percentage cannot identify
individual component power consumption. No SSIDs, peer identifiers, or secrets
are recorded.
