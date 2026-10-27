# Firmware review fixes, 27 October 2026

Version 0.6.1 addresses the stability/functionality review of 0.6.0 (`d806a4b`).
Priorities retain the original review's meaning: P1 high, P2 medium, P3 low.
The original detailed review and reproduction probes remain in the ignored
`.pio/review-20261027/` directory. This record keeps the repair mapping in Git.

| ID | Priority | Impact addressed and implemented fix |
|---|---|---|
| FW-01 | P1 | Failed AF Jump/End could strand a muted candidate. Require successful exits, otherwise invalidate readiness and restore the original station through boot recovery. |
| FW-02 | P1 | Lost controls and selective failures could remain hidden. Failed writes trigger recovery with requested settings retained; error history clears only after a clean interval. |
| FW-03 | P1 | Head-unit tune completion could hang without quality callbacks. An independent periodic timer services bounded operation deadlines. |
| FW-04 | P1 | Full ISR queues could lose accepted controls. Count losses and reconcile current model controls; revision filtering skips superseded queued messages, and operation generations reject stale completions. |
| FW-05 | P1 | Elapsed uptime alone could confirm broken OTA firmware. Require ready expected tuner state, fresh settled measurements and a running car-build slave before confirmation; otherwise roll back. |
| FW-06 | P2 | Old buffered RDS could be attributed to a new station. Drain RDS after preset retunes and boot before learning or publishing new groups. |
| FW-07 | P2 | Cached lists could renew trust forever and retain deleted AFs. Track actual received AF codes and announced complete lists; replace stored lists on completion, including an empty list. |
| FW-08 | P2 | REG-off regional PI changes could abandon a trial. Apply REG-aware programme identity consistently to following and memory. |
| FW-09 | P2 | Standby/wrong-band operation could pass health checks. Verify expected active state with a bounded band-transition allowance. |
| FW-10 | P2 | Seeking could suppress health checks. Service checks before seek work and bound the overall seek duration. |
| FW-11 | P2 | Warm restore could lose mono/de-emphasis. Reapply both controls and preserve pending tune generations. |
| FW-12 | P2 | RTC snapshots could mix radio/model generations. Compare model revisions and operation state around the radio read, retry briefly, retain the previous snapshot when unstable. |
| FW-13 | P2 | Whole tuning tables could overwrite queued payloads. Each queue item owns its immutable table. |
| FW-14 | P2 | Queued bandwidth could be applied to another band. Carry the validated band and discard obsolete band commands; offline band changes also normalize bandwidth. |
| FW-15 | P2 | Incompatible AF blobs could reappear on the second boot. Erase incompatible channel keys before committing the new schema marker. |
| FW-16 | P3 | RDS sync loss could stay invisible to the head unit. Emit a sync-only event and refresh the emulated status/FIFO. |
| FW-17 | P2 | Preset storage could capture temporary AF candidates. Accept only ready, settled settings outside seek/check/trial. |
| FW-18 | P2 | Save could persist an older debounced value. Flush and serialize browser edits, await the firmware command barrier, save, then reload authoritative saved values. Revert/default actions also drain older edits. |
| FW-19 | P3 | Paused browser logs could grow without bound. Cap pending lines at 2,000 and partial lines at 4,096 characters, with bounded rendering batches. |
| FW-20 | P3 | Duplicate IDs broke AF score/search indicators. Assign distinct element IDs. |
| FW-21 | P2 | Ten-kHz MW tuning could wrap onto the wrong raster. Use 530..1710 kHz and aligned directional stepping; expose the raster to the page. |
| RISK-01 | P1 | Flash could suspend a no-stretch receive service. Keep automatic AF persistence enabled; move receive service/model/tables into internal RAM, remove flash arithmetic helpers and require the SDK IRAM-safe ISR. Linked placement is audited; actual flash-erase timing remains unqualified. |
| RISK-02 | P1 | The SDK's synchronous NACK bus-busy spin bypassed transfer timeouts. Compile a build-local bounded source copy using exact checked anchors; leave the installed SDK untouched and fail configuration on source drift. |
| RISK-03 | P2 | Slow OTA clients could monopolize HTTP indefinitely. Enforce total and progress receive deadlines, in addition to socket timeout limits. |

## Validation

- 149 native tests pass, including captured head-unit register replay, RDS list replacement,
  REG-aware AF memory and model reconciliation/generations.
- 34 script/integration tests pass. Real radio/bridge/store sources are compiled with mocked
  SDK/tuner services: 15 radio fault/order scenarios, four bridge scenarios, and three AF NVS
  scenarios. These include automatic AF-list writes and reload after reboot. Node executes
  actual page helpers/Save and checks HTML IDs and log bounds. The actual snapshot function
  is exercised with racing, unsettled, pending-operation and stable model/radio pairs.
  The SDK patch's actual loop is
  exercised with a permanently busy bus and tick wrap; source drift is rejected.
- Both car/Wi-Fi and isolated stand-alone/DFS/station-memory configurations build with the
  pinned ESP-IDF 5.4.0 / PlatformIO espressif32 6.10.0 toolchain.
- `scripts/check_qn_iram.py` audits normal receive direct calls and literal references in the
  final car ELF. SDK fatal assertion formatting is explicitly excluded; indirect callbacks
  require separate entry-point checks and SDK registration validation.
- `git diff --check` passes.

Host fault injection and target builds do not establish hardware behavior. No device was
flashed. Remaining qualification includes command-queue overload under real bus traffic,
RTC snapshot/reset stress, AF Check/End/audio timing, RDS draining on the fitted module,
flash-write/erase FIFO response timing, held-bus recovery and real OTA rollback.
RISK-01 remains a hardware qualification item despite the placement repair.

Prepared images and matching ELF are archived locally under `builds/`, named by the first
16 hex characters of the ELF SHA-256. The commit body records that build ID, following the
existing repository workflow; the manifest records source provenance and artifact hashes.

Car build: `b2d0f5cc50764c86`, version `0.6.1`; app image 987,200 bytes,
linker-reported application usage 986,805 bytes / 1,703,936 and static RAM
73,872 bytes / 327,680. Files: `builds/b2d0f5cc50764c86/firmware.bin`,
bootloader/partition/initial-OTA images and `manifest.json`, plus the matching
`builds/b2d0f5cc50764c86.elf`. The BIN descriptor's version and ELF ID were checked against
the archive. Stand-alone application usage: 968,933 bytes, static RAM 74,020 bytes.
These static size reports do not measure runtime heap or stack margins.
