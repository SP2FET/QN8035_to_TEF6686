# Recording a ride

The bridge keeps a short ring of records (`GET /api/trace`): every quality sample of what plays
(~100 ms), every AF_Update measurement of an alternative frequency, and every AF step (PI check,
trial, switch, back). A PC on the bridge's Wi-Fi fetches the ring twice a second and writes
everything to disk, so the ESP itself logs nothing.

## Before the ride

1. Flash the build (USB or `pio run -e lolin_s2_mini -t ota`).
2. Join the laptop to `TEF-Bridge` (password `tef6686bridge`). The access point has no internet:
   tell Windows to stay connected ("Connect automatically", and no switch to another network),
   or it drops the link after a while.
3. Check `http://192.168.4.1/` opens. Optional: on the Tuning tab, look at the AF group
   (window, gain, trial length, retry).

## During the ride

```sh
python scripts/ride_log.py --note "A2 east, evening"
```

It writes to `rides/<date-time>/` and shows one live line (frequency, PS, level, USN, WAM, AF
state, trial). AF steps print above it as they happen. Type while driving (or let a passenger):

| Input | Recorded as |
|---|---|
| Enter | a mark ("something happened here") |
| `1` .. `5` + Enter | an audio rating: 1 = unlistenable, 5 = clean |
| any text + Enter | a note, e.g. `hiss on 94.4` |
| `q` + Enter, or Ctrl+C | stop |

Ratings are the most useful input: they let the report line up what you heard with level, USN,
WAM, soft mute and high cut at that moment. Rate when the sound changes, not on a timer.

A lost Wi-Fi connection is retried every few seconds; the ring covers about 10 s of quality
samples, so short drop-outs lose nothing (the live line counts `lost` records otherwise). A device
restart starts a new segment in the files.

Lines starting with `TUNER` (from the trace) and `HEALTH` (from `/api/status`) report the device's
own problems as they happen: the tuner unavailable and ready again, I/O errors and retries,
head-unit commands dropped or replayed, head-unit tunes that timed out, restarts, a core dump,
stack or heap running low, a changed reception tuning. Note what you heard at that moment.

## After the ride

```sh
python scripts/ride_report.py rides/<date-time>
python scripts/ride_report.py rides/<date-time> --from 17:42:00 --to 17:47:00   # zoom in
```

This writes `report.html` (open it in a browser, no internet needed) and `quality.csv`,
`af.csv`, `events.csv` and `health.csv` next to the recording:

- **Charts** on one time axis: frequency with AF steps, level (tuned, trial, AF_Update
  candidates), USN/WAM, score, DSP (soft mute, high cut, stereo and high blend), RDS errors. Marks
  and ratings are vertical lines.
- **Per frequency**: time spent and mean quality.
- **AF steps** in order: checks, trials and their results (trial score vs the tuned station's).
- **AF_Update calibration**: how far AF_Update levels and scores sit from the settled values of
  the same frequency. This is the number to tune `af_window` with.
- **Marks** with the mean quality of the 3 s before each.
- **Device health**: how far each counter rose (tuner I/O errors, retries and recoveries,
  head-unit drops, replays and tune timeouts, AF memory writes, resets), when the tuner was
  unavailable and for how long, restarts with their reason, and the least free heap and stack.
  The last chart shows free heap with the tuner's outages.
- **Reception tuning**: values off their default at the start, and every change during the ride.

Send the whole `rides/<date-time>/` folder (or just `report.html` and the CSVs) for analysis.

## Files

| File | Content |
|---|---|
| `meta.json` | start time, host, arguments, firmware version |
| `trace.jsonl` | the device records (chip units: 0.1 dBµV, 0.1 %, 0.1 kHz, 0.1 dB) plus the PC arrival time `pc` |
| `status.jsonl` | `/api/status` every 2 s (RDS names, AF list with averages) |
| `console.log` | the device log, PC time stamps |
| `marks.jsonl` | marks, ratings, notes |
| `tuning.jsonl` | the reception tuning at the start, after a restart and after every change |
| `coredump-*.bin` | a core dump the device held, fetched once and then erased on the device (CRC-checked; decode with that build's ELF) |
| `health.csv` | (report) the device's health counters at every status poll |

The record format is described in `scripts/ride_log.py` and in `radio_trace_t`
([radio.h](../components/radio/include/radio.h)).
