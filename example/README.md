# webm_texture_example

Demo app for the `webm_texture` plugin. Run it with `fvm flutter run` from this
directory; the stickers come from the corpus in `test_assets/`.

| Page | What it shows |
|---|---|
| Chat | Stickers mixed with text bubbles, in a list where off-screen stickers pause |
| Checkerboard | Alpha correctness over a checker pattern; toggles dark checks and play/pause |
| Stress | 200 standalone stickers in a grid, with the active count; the switch gives each cell its own animation |
| Probe | `WebmStickers.probe` metadata for every corpus file, including the rejected ones |
| Textures (dev) | The raw platform texture glue (T2.2, T2.3) |
| Visibility (dev) | A 200-row list with the active counter (T2.5) |
