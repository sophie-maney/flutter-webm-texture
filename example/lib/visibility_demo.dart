// A 200-item list of `WebmSticker`s with a debug overlay that counts the
// stickers reporting active (T2.5). Only the rows on screen should count.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:webm_texture/webm_texture.dart';

/// Playable corpus files, cycled through the list.
const _assets = [
  'vp9_alpha_512.webm',
  'vp8_alpha_256.webm',
  'vp9_alpha_vfr.webm',
  'vp9_odd_301x199.webm',
  'vp9_opaque_256.webm',
  'vp9_with_audio.webm',
  'anim_disposal.gif',
];

/// Number of rows.
const visibilityItemCount = 200;

class VisibilityDemoPage extends StatefulWidget {
  const VisibilityDemoPage({super.key});

  @override
  State<VisibilityDemoPage> createState() => _VisibilityDemoPageState();
}

class _VisibilityDemoPageState extends State<VisibilityDemoPage> {
  List<StickerSource>? _sources;
  Object? _loadError;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final sources = [
        for (final name in _assets)
          StickerSource.bytes(
            (await rootBundle.load('test_assets/$name')).buffer.asUint8List(),
            key: name,
          ),
      ];
      if (mounted) setState(() => _sources = sources);
    } catch (e) {
      if (mounted) setState(() => _loadError = e);
    }
  }

  @override
  Widget build(BuildContext context) {
    final sources = _sources;
    return Scaffold(
      appBar: AppBar(title: const Text('Visibility')),
      body: Stack(
        children: [
          if (sources == null)
            Center(child: Text(_loadError?.toString() ?? 'Loading…'))
          else
            ListView.builder(
              key: const Key('stickerList'),
              itemCount: visibilityItemCount,
              itemExtent: 96,
              itemBuilder: (context, i) => _Row(i, sources[i % sources.length]),
            ),
          const Positioned(top: 8, right: 8, child: _ActiveCounter()),
        ],
      ),
    );
  }
}

class _Row extends StatelessWidget {
  const _Row(this.index, this.source);

  final int index;
  final StickerSource source;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
    child: Row(
      children: [
        WebmSticker(
          source: source,
          width: 80,
          height: 80,
          placeholder: const ColoredBox(color: Color(0x14000000)),
          errorBuilder: (_, e) => Center(
            child: Text(
              e is StickerException ? e.name : '$e',
              style: const TextStyle(fontSize: 10, color: Colors.red),
            ),
          ),
        ),
        const SizedBox(width: 16),
        Text('#$index · ${_assets[index % _assets.length]}'),
      ],
    ),
  );
}

/// Shows [WebmStickers.debugActiveCount].
class _ActiveCounter extends StatelessWidget {
  const _ActiveCounter();

  @override
  Widget build(BuildContext context) => IgnorePointer(
    child: DecoratedBox(
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.75),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        child: ValueListenableBuilder<int>(
          valueListenable: WebmStickers.debugActiveCount,
          builder: (_, n, _) => Text(
            'active $n',
            key: const Key('activeCount'),
            style: const TextStyle(color: Colors.white, fontSize: 14),
          ),
        ),
      ),
    ),
  );
}
