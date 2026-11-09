# Spike A: VP9/VP8 alpha demux and decode (T0.7)

Throwaway host CLI. The findings are in [`docs/spikes/A-alpha.md`](../../docs/spikes/A-alpha.md). Nothing here ships. T1.x reimplements whatever survives in `src/`.

| File | What it is |
|---|---|
| `frame_index.h` | The frame index both demuxers produce: video track plus per-frame byte spans for colour and alpha |
| `demux_libwebm.cc` | Index built on libwebm's `webm_parser` callbacks, with the hardening it needs |
| `demux_mini_ebml.cc` | The alternative: a minimal EBML/WebM reader |
| `alpha_spike.cc` | CLI with `verify`, `experiments`, `stress` and `compare` modes |
| `size_probe.cc` | Driver for comparing each demuxer's code size |
| `build-host-deps.sh` | Builds libvpx, libwebm and libyuv for the host from `versions.lock` |
| `run.sh` | Builds and runs verify + experiments (+ sizes, or + stress with `--asan`) |
| `allframes.sh` | Compares **every** frame, plus extra encoder/muxer variants, against ffmpeg |

```sh
spikes/alpha/build-host-deps.sh     # once; macOS or Linux, needs cmake
spikes/alpha/run.sh                 # release: correctness, pitfalls, timing, code size
spikes/alpha/run.sh --asan 50000    # ASan/UBSan: the same, plus 50k mutated inputs
spikes/alpha/allframes.sh           # every frame vs ffmpeg (needs ffmpeg with libvpx)
```

`run.sh` needs `jq` to flatten `test_assets/manifest.json`.
