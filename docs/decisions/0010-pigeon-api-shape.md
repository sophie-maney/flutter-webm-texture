# 0010: Shape of the Pigeon API

- **Status:** Accepted (T2.1)
- **Date:** 2026-11-10

## Context

`PLAN.md` sketched `createTexture(animId, w, h) → textureId`. That assumed Dart picks the animation id and the platform only allocates a texture. The platform glue (T2.2, T2.3) also owns the decoder handle and the source bytes' lifetime, so one call has to create both. Tier detection (T3.4) and late failures need channels too.

## Decision

`pigeons/messages.dart` defines:

- `createAnimation(sourceKey, AnimationSourceMessage{bytes?, path?}, bucketW, bucketH) → {animId, textureId}` (async). The platform assigns `animId`. Pigeon has no unions, so the source is a class with two nullable fields; exactly one is set. `sourceKey` is the content hash or caller key naming the cache entry.
- `setActive(animId, active)`, `dispose(animId)` (idempotent).
- `deviceSignals() → {ramMb, cores, lowRam, perfClass, lowPower, thermal}`, with `perfClass` and `thermal` as enums that have an `unknown` member for platforms that don't report them.
- An `@EventChannelApi` `streamErrors()` that emits `{animId, code, message}` for failures after `createAnimation` returned (lost surface, mid-playback decode error). `code` is a `wt_status` name or a platform code such as `SURFACE_LOST`.

All of it uses `*Message` suffixes so the public Dart types in `lib/` stay free of generated names. Output lands in `lib/src/gen/messages.g.dart`, `Messages.g.kt` and `Messages.g.swift`, committed. The Android and iOS plugins implement the host API with stubs that throw `UNIMPLEMENTED` until T2.2, T2.3 and T3.4 fill them in.

## Consequences

- Media data still never crosses Pigeon except the encoded bytes handed to `createAnimation`; frames go straight to the texture from native code.
- `pigeon` is a new dev dependency, already named in `PLAN.md`.
