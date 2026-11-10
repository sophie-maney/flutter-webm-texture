// The bundled test corpus, shared by the example pages.
import 'package:webm_texture/webm_texture.dart';

/// Corpus files that play today, with a short label for the UI.
const playableCorpus = [
  'vp9_alpha_512.webm',
  'vp8_alpha_256.webm',
  'vp9_alpha_vfr.webm',
  'vp9_odd_301x199.webm',
  'vp9_opaque_256.webm',
  'vp9_with_audio.webm',
];

/// Every corpus file, including the ones the core rejects, for the Probe
/// page.
const allCorpus = [
  ...playableCorpus,
  'vp9_single_frame.webm',
  'vp9_2048.webm',
  'truncated.webm',
  'garbage.bin',
  'anim.webp',
  'anim_disposal.gif',
];

/// The corpus asset [name] as a sticker source. `key` is the file name, so
/// identical files share an animation.
StickerSource corpusSource(String name) =>
    StickerSource.asset('test_assets/$name');
