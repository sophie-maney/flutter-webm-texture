# 0018: Device tiers are decided in Dart from platform signals

- **Status:** Accepted (T3.4)
- **Date:** 2026-11-11

## Context

PLAN.md names the inputs to tiers (RAM, cores, Android `isLowRamDevice` and media performance class, Low Power Mode, thermal state) and the three tiers' limits, but not the thresholds, where the rules live, or how runtime changes reach the scheduler. [ADR 0010](0010-pigeon-api-shape.md) already gave the platforms a `deviceSignals()` call, and [ADR 0017](0017-scheduler.md) left "a Pigeon call into `wt_sched_set_options`" to this task.

## Decision

1. **The platforms only report facts; the rules are Dart** (`lib/src/sticker_tier.dart`, `StickerTiers`), so they are unit-tested on the host and identical on both platforms.
2. **Hardware tier** (`StickerTiers.detect`): `low` for `isLowRamDevice`, a `low` performance class, under 2560 MiB of RAM (2 GB devices report about 1.9 GB) or under 4 cores. Otherwise `mid` for a `mid` performance class, under 3584 MiB (3 GB devices report about 2.8 GB, 4 GB ones 3.7 GB) or under 6 cores. Otherwise `high`. Android reports performance class 30 and 31 as `mid`, 33 and up as `high`, and `unknown` when the device claims none; the platform never reports `low`. iOS reports `unknown` and `lowRam` false. Cores are `ProcessInfo.processorCount`, not `activeProcessorCount`, which can drop in Low Power Mode and would count it twice.
3. **Runtime downgrade** (`StickerTiers.pressure`): Low Power Mode (iOS) or battery saver (Android) takes one tier off; `serious` thermal state takes one, `critical` two. They add up and stop at `low`. Android maps `LIGHT` and `MODERATE` to `fair` and `SEVERE` to `serious` and `CRITICAL` or worse to `critical`; `fair` takes nothing off. Pressure ending restores the tier.
4. **Overrides.** `StickerConfig(tier:, maxFps:, maxConcurrent:)`. An explicit tier is used as given and ignores pressure; `maxFps` (1..240) and `maxConcurrent` (0 = unlimited) replace that tier's value either way.
5. **Limits** (`StickerTiers.limits`), as PLAN.md: `high` 60 fps and unlimited, `mid` 30 fps and 30, `low` 15 fps and 8. Workers (PLAN.md: "N = tier"): `high` half the cores clamped to 1–4, `mid` at most 2, `low` 1. Stickers over `maxConcurrent` freeze on the frame they show, which is the first frame if they never played ([ADR 0017](0017-scheduler.md) §5).
6. **Plumbing.** New Pigeon call `setSchedulerOptions(workerCount, maxFps, maxConcurrent)` → `wt_sched_set_options` (Android JNI `nativeSetSchedulerOptions`, iOS `+[WTPlayer setSchedulerOptions…]`; both create the process-wide scheduler if needed). A second event, `streamDeviceSignals` on `WebmTextureEvents`, carries a fresh reading whenever `ACTION_POWER_SAVE_MODE_CHANGED` or the thermal status listener (API 29+), or `NSProcessInfoPowerStateDidChange` or `thermalStateDidChangeNotification`, fires. Dart also rereads the signals when the app resumes. `StickerTierController` (`lib/src/sticker_tier.dart`) applies a result only when the limits change.
7. **Startup.** `WebmStickers.configure` and the registry's first `createAnimation` start the controller; the registry waits for it, so the first animation already plays under the tier. A platform that can't report signals leaves the scheduler on its defaults, and the tier reads `high`.
8. **Visibility.** `WebmStickers.debugTier` (a `ValueListenable<StickerTier>`, like `debugActiveCount`) shows the tier in force. The example app's home page shows it in its title bar.

## Consequences

- Thresholds are heuristics from reported memory sizes and have no benchmark behind them yet; T3.5 supplies the first numbers, and changing them is a one-file edit with the tests as the matrix.
- Android emulators report few cores and land on `low`; the iOS Simulator reports the Mac and lands on `high`.
- The iOS Simulator has no Low Power Mode or thermal control, so the downgrade is checked on Android (`adb shell cmd power set-mode 1`, `cmd thermalservice override-status 3`) and by unit tests. On a device, toggle Low Power Mode in Settings and watch the title bar.
