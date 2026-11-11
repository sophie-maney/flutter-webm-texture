// Atlas (dev): 64 animations drawn through atlas pages (T4.2). It drives the
// Pigeon atlas API directly, with a hand-rolled clipped and translated
// `Texture` per tile; T4.3's `StickerAtlasScope` replaces it. A button moves
// every animation between the atlas and textures of their own, and the
// header shows how many platform textures are in use and what each page
// costs.
// ignore_for_file: implementation_imports
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:webm_texture/src/gen/messages.g.dart';

/// Clips that play today, cycled over the tiles.
const atlasDemoClips = [
  'vp9_alpha_vfr.webm',
  'vp8_alpha_256.webm',
  'vp9_alpha_512.webm',
  'vp9_with_audio.webm',
  'vp9_opaque_256.webm',
];

/// Draws one atlas slot: page [page]'s texture laid out at the page's size,
/// moved so the slot's content rect lands on this widget, and clipped to it.
/// The widget's size sets the scale: at [slot]'s width and height in
/// physical pixels, the page is sampled 1:1.
class AtlasSlotView extends StatelessWidget {
  const AtlasSlotView({super.key, required this.page, required this.slot});

  final AtlasPageMessage page;
  final AtlasSlotMessage slot;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, box) {
      // Logical pixels per page pixel.
      final scale = box.maxWidth / slot.width;
      return ClipRect(
        child: OverflowBox(
          alignment: Alignment.topLeft,
          minWidth: 0,
          minHeight: 0,
          maxWidth: double.infinity,
          maxHeight: double.infinity,
          child: Transform.translate(
            offset: Offset(-slot.x * scale, -slot.y * scale),
            child: SizedBox(
              width: page.width * scale,
              height: page.height * scale,
              child: Texture(textureId: page.textureId),
            ),
          ),
        ),
      );
    },
  );
}

/// Releases textures a layout retired once a frame without them was drawn
/// (the engine unregisters textures after the raster work queued before).
void releaseAfterFrame(WebmTextureHostApi api, List<int> ids) {
  if (ids.isEmpty) return;
  SchedulerBinding.instance.addPostFrameCallback((_) {
    unawaited(api.releaseTextures(ids));
  });
  SchedulerBinding.instance.scheduleFrame();
}

class AtlasDemoPage extends StatefulWidget {
  const AtlasDemoPage({super.key, this.count = 64, this.bucket = 128});

  /// Animations, each with a decoder of its own.
  final int count;

  /// Their size in physical pixels.
  final int bucket;

  @override
  State<AtlasDemoPage> createState() => _AtlasDemoPageState();
}

class _AtlasDemoPageState extends State<AtlasDemoPage> {
  final _api = WebmTextureHostApi();
  final _anims = <int>[];
  final _slots = <int, AtlasSlotMessage>{};
  final _pages = <int, AtlasPageMessage>{};
  final _own = <int, int>{};
  int? _atlasId;
  bool _atlasMode = true;
  bool _busy = true;
  AtlasStatsMessage? _stats;
  String? _error;
  Timer? _poll;

  @override
  void initState() {
    super.initState();
    unawaited(_start());
  }

  @override
  void dispose() {
    _poll?.cancel();
    unawaited(_teardown());
    super.dispose();
  }

  Future<void> _start() async {
    try {
      final bytes = <String, Uint8List>{};
      for (final clip in atlasDemoClips) {
        bytes[clip] = (await rootBundle.load('test_assets/$clip')).buffer
            .asUint8List();
      }
      for (var i = 0; i < widget.count; i++) {
        final clip = atlasDemoClips[i % atlasDemoClips.length];
        final h = await _api.createAnimation(
          clip,
          AnimationSourceMessage(bytes: bytes[clip]),
          widget.bucket,
          widget.bucket,
          true,
        );
        _anims.add(h.animId);
        _own[h.animId] = h.textureId;
      }
      final atlasId = await _api.createAtlas(
        AtlasOptionsMessage(width: 2048, maxHeight: 2048, maxPages: 4),
      );
      if (!mounted) {
        await _api.disposeAtlas(atlasId);
        return;
      }
      _atlasId = atlasId;
      _apply(await _api.attachToAtlas(atlasId, _anims));
      for (final id in _anims) {
        await _api.setActive(id, true);
      }
      _poll = Timer.periodic(const Duration(seconds: 1), (_) => _refresh());
      await _refresh();
    } on PlatformException catch (e) {
      _error = '${e.code}: ${e.message}';
    }
    if (mounted) setState(() => _busy = false);
  }

  Future<void> _teardown() async {
    final atlasId = _atlasId;
    if (atlasId != null) await _api.disposeAtlas(atlasId);
    for (final id in _anims) {
      await _api.dispose(id);
    }
  }

  void _apply(AtlasLayoutMessage layout) {
    for (final s in layout.slots) {
      if (s.error == null) {
        _slots[s.animId] = s;
        _own.remove(s.animId);
      }
    }
    for (final h in layout.handles) {
      _slots.remove(h.animId);
      if (h.textureId >= 0) _own[h.animId] = h.textureId;
    }
    _pages
      ..clear()
      ..addEntries(layout.pages.map((p) => MapEntry(p.pageId, p)));
    if (mounted) setState(() {});
    releaseAfterFrame(_api, layout.retiredTextureIds);
  }

  Future<void> _toggle() async {
    final atlasId = _atlasId;
    if (atlasId == null || _busy) return;
    setState(() => _busy = true);
    _apply(
      _atlasMode
          ? await _api.detachFromAtlas(_anims)
          : await _api.attachToAtlas(atlasId, _anims),
    );
    _atlasMode = !_atlasMode;
    await _refresh();
    if (mounted) setState(() => _busy = false);
  }

  Future<void> _refresh() async {
    final atlasId = _atlasId;
    if (atlasId == null || !mounted) return;
    final stats = await _api.atlasStats(atlasId);
    if (!mounted) return;
    setState(() => _stats = stats);
    debugPrint(
      'ATLAS_STATS ${jsonEncode({
        'animations': _anims.length,
        'mode': _atlasMode ? 'atlas' : 'standalone',
        'pageTextures': stats.textures,
        'pluginTextures': stats.pluginTextures,
        'pages': [
          for (final p in stats.pages) {'page': p.pageId, 'size': '${p.width}x${p.height}', 'slots': p.liveSlots, 'buffers': p.buffers, 'bufferBytes': p.bufferBytes, 'cpuBytes': p.cpuBytes, 'presents': p.presents},
        ],
      })}',
    );
  }

  String _mb(int bytes) => (bytes / (1 << 20)).toStringAsFixed(1);

  String _summary() {
    final stats = _stats;
    if (stats == null) return 'Starting ${widget.count} animations...';
    final drawn = _atlasMode ? stats.textures : _own.length;
    final lines = [
      '${_anims.length} animations in ${_atlasMode ? 'atlas' : 'standalone'} '
          'mode: $drawn platform texture${drawn == 1 ? '' : 's'} drawn '
          '(${stats.pluginTextures} held by the plugin)',
      for (final p in stats.pages)
        if (p.textureId >= 0)
          'page ${p.pageId}: ${p.width}x${p.height}, ${p.liveSlots} slots, '
              '${p.buffers} buffers = ${_mb(p.bufferBytes)} MB, '
              'CPU ${_mb(p.cpuBytes)} MB, ${p.presents} presents',
    ];
    return lines.join('\n');
  }

  Widget _tile(int animId) {
    final slot = _slots[animId];
    final page = slot == null ? null : _pages[slot.pageId];
    if (_atlasMode && slot != null && page != null) {
      return AtlasSlotView(page: page, slot: slot);
    }
    final own = _own[animId];
    if (own != null) return Texture(textureId: own);
    return const SizedBox.shrink();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(
      title: const Text('Atlas (dev)'),
      actions: [
        TextButton(
          key: const Key('atlasToggle'),
          onPressed: _busy ? null : _toggle,
          child: Text(_atlasMode ? 'Standalone' : 'Atlas'),
        ),
      ],
    ),
    body: Column(
      children: [
        Padding(
          padding: const EdgeInsets.all(8),
          child: Text(
            _error ?? _summary(),
            key: const Key('atlasInfo'),
            style: const TextStyle(fontSize: 12),
          ),
        ),
        Expanded(
          child: GridView.count(
            crossAxisCount: 8,
            padding: const EdgeInsets.all(4),
            children: [
              for (final id in _anims)
                ColoredBox(color: const Color(0xFFEFEFEF), child: _tile(id)),
            ],
          ),
        ),
      ],
    ),
  );
}
