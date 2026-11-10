# 0012: `WebmSticker` visibility and playback control

- **Status:** Accepted (T2.5)
- **Date:** 2026-11-10

## Context

`PLAN.md` sketched a widget that measures viewport intersection "during paint/layout", shares one animation between every widget showing the same content, and takes `loop` and a controller with `play`/`pause`/`seekToFrame`. Building it ran into four problems.

1. Scrolling a `ListView` moves each item's repaint boundary without repainting what is inside it, so a render object's `paint` isn't called while it scrolls on or off screen.
2. A shared animation has a single frame position. If one widget pauses or seeks it, every other widget showing it pauses or seeks as well.
3. The Pigeon API ([ADR 0010](0010-pigeon-api-shape.md)) has no loop flag and no seek, and both platform glues always loop.
4. A debug counter that notifies listeners synchronously throws "setState() called during build", because leases are released while slivers garbage-collect children inside the build scope. This showed up on the simulator.

## Decision

- **Visibility.** `RenderStickerVisibility` (a `RenderProxyBox`) is checked once after every frame by a single post-frame callback that serves all instances. Nothing is scheduled when no frames are drawn. The check walks the ancestors to the root view:
  - Any ancestor with `paintsChild` false hides the sticker (`Offstage`, a sliver outside its viewport, a kept-alive list item).
  - Each ancestor's `applyPaintTransform` is applied, and its `describeApproximatePaintClip` is intersected (viewports, `ClipRect`), and so is the view.
  - A non-empty result means visible. Occlusion by opaque widgets drawn on top isn't detected.
- **Active** means all of these hold: the widget is playing (`controller.isPlaying`, else `autoplay`), it is visible, `TickerMode` is enabled, and the app lifecycle is `resumed` or `inactive`. `hidden`, `paused` and `detached` stop playback.
- **Sharing.** `StickerLease.setActive` is per lease. The registry calls the platform `setActive` only when an animation's count of active leases goes between 0 and 1. A widget with a controller, or with `autoplay: false`, gets a private animation through the registry's `owner` (stickers on the same controller share theirs). Every other widget uses the shared one.
- **Controller** supports `play`/`pause` only. **`loop` and `seekToFrame` are deferred to T2.9**, which adds them to Pigeon and both glues. Until then every sticker loops.
- **Bucket aspect.** The widget probes the source over FFI on a background isolate before calling `createAnimation`, because the bucket must have the source's aspect ratio. Results are cached per content key (LRU of 256), and failures aren't cached. A probe error reaches `errorBuilder` without a platform call.
- **Content key** until T2.6: a file's path, a bytes source's `key`, or else one key per `Uint8List` instance. Equal copies therefore aren't shared, but a hash collision can't swap stickers either.
- **Placeholder** stays up until `createAnimation` returns. iOS renders the first frame before returning. Android's render thread draws right after attach, so the texture can be empty for a frame.
- **Errors.** `createAnimation` failures become `StickerException`s, which map a `PlatformException`'s `wt_status` name back to its code. Events on `streamErrors` don't put the widget into its error state, because playback continues after them ([ADR 0011](0011-android-texture-glue.md)).
- **`WebmStickers.debugActiveCount`** is a public `ValueListenable<int>` giving the number of widgets that want to animate. It is published in a microtask, so listeners may call `setState`.

## Consequences

- A visibility check costs one ancestor walk per mounted sticker per frame. That is a few dozen render objects for each of the roughly 10–30 stickers inside a list's cache extent.
- A sticker hidden under another opaque widget still counts as visible.
- Paused or controlled stickers cost a decoder each instead of sharing one.
- T3.3 can take its visible-area priority from `RenderStickerVisibility.visibleRect`.
