// Spike B (T0.8): does a platform texture filled with premultiplied RGBA
// composite correctly over non-opaque backgrounds?
//
// The screen is a grid. Each row has a different background (checkerboard,
// colour gradient, black, white), and each row has three tiles:
//   1. texture  – the platform texture showing the test pattern
//   2. flutter  – the same pattern drawn by Flutter's own canvas
//   3. control  – the bare background
// `tool/sample.py` reads a screenshot plus the SPIKE_B_LAYOUT log line and
// compares tile 1 with tile 2 (and with tile 3 + maths, for the
// premultiply probes that Flutter cannot draw).
//
// All geometry is in physical pixels and the textures are drawn at 1 texel
// per pixel with FilterQuality.none, so sampled blocks are exact.

import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

const _channel = MethodChannel('spike_b');

/// Build-time switches, set with `--dart-define`.
const _forceGl = bool.fromEnvironment('FORCE_GL');
const _lifecycle = String.fromEnvironment('LIFECYCLE', defaultValue: 'manual');
const _redrawOnResume = bool.fromEnvironment('REDRAW_ON_RESUME');

/// iOS only: thread that calls textureFrameAvailable ('bg' or 'main').
const _notify = String.fromEnvironment('IOS_NOTIFY', defaultValue: 'bg');

/// Test pattern geometry (physical pixels).
const int patW = 256;
const int patH = 128;
const int blk = 32;
const int animSize = 128;

/// One uniform block of the test pattern, premultiplied RGBA.
class Block {
  const Block(this.name, this.col, this.row, this.r, this.g, this.b, this.a);
  final String name;
  final int col, row, r, g, b, a;

  /// Premultiplied values with colour > alpha are not valid premultiplied
  /// colours. They are probes: a premultiplied compositor adds them to the
  /// background, a straight-alpha one hides them.
  bool get isProbe => r > a || g > a || b > a;

  Map<String, Object> toJson() => {
    'name': name,
    'x': col * blk,
    'y': row * blk,
    'w': blk,
    'h': blk,
    'rgba': [r, g, b, a],
  };
}

int _pm(int c, int a) => (c * a + 127) ~/ 255;

/// 8 × 4 blocks of 32 px.
final List<Block> pattern = () {
  const steps = [0, 36, 73, 109, 146, 182, 219, 255];
  final out = <Block>[];
  for (var i = 0; i < 8; i++) {
    final a = steps[i];
    out.add(Block('white_a$a', i, 0, a, a, a, a));
    out.add(Block('red_a$a', i, 1, a, 0, 0, a));
    out.add(Block('azure_a$a', i, 2, 0, _pm(128, a), a, a));
  }
  out.addAll(const [
    Block('clear', 0, 3, 0, 0, 0, 0),
    Block('opaque_red', 1, 3, 255, 0, 0, 255),
    Block('opaque_green', 2, 3, 0, 255, 0, 255),
    Block('opaque_blue', 3, 3, 0, 0, 255, 255),
    Block('half_grey', 4, 3, 128, 128, 128, 128),
    Block('half_green', 5, 3, 0, 128, 0, 128),
    Block('probe_white_a0', 6, 3, 255, 255, 255, 0),
    Block('probe_red_a128', 7, 3, 255, 0, 0, 128),
  ]);
  return out;
}();

Uint8List buildPattern() {
  final px = Uint8List(patW * patH * 4);
  for (final b in pattern) {
    for (var y = b.row * blk; y < (b.row + 1) * blk; y++) {
      for (var x = b.col * blk; x < (b.col + 1) * blk; x++) {
        final o = (y * patW + x) * 4;
        px[o] = b.r;
        px[o + 1] = b.g;
        px[o + 2] = b.b;
        px[o + 3] = b.a;
      }
    }
  }
  return px;
}

void main() => runApp(const SpikeApp());

class SpikeApp extends StatelessWidget {
  const SpikeApp({super.key});

  @override
  Widget build(BuildContext context) =>
      const MaterialApp(debugShowCheckedModeBanner: false, home: SpikePage());
}

class SpikePage extends StatefulWidget {
  const SpikePage({super.key});

  @override
  State<SpikePage> createState() => _SpikePageState();
}

class _SpikePageState extends State<SpikePage> with WidgetsBindingObserver {
  int? _staticId;
  int? _animId;
  Map<Object?, Object?> _info = const {};
  Map<Object?, Object?> _stats = const {};
  final List<String> _lifecycleLog = [];
  Timer? _poll;
  String? _error;
  bool _layoutLogged = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _init();
  }

  Future<void> _init() async {
    try {
      final info = await _channel.invokeMapMethod<Object?, Object?>('info');
      final s = await _channel.invokeMapMethod<Object?, Object?>('create', {
        'kind': 'static',
        'width': patW,
        'height': patH,
        'rgba': buildPattern(),
        'forceGl': _forceGl,
        'lifecycle': _lifecycle,
        'notify': _notify,
      });
      final a = await _channel.invokeMapMethod<Object?, Object?>('create', {
        'kind': 'anim',
        'width': animSize,
        'height': animSize,
        'forceGl': _forceGl,
        'lifecycle': _lifecycle,
        'notify': _notify,
      });
      setState(() {
        _info = {...?info, 'staticPath': s?['path'], 'animPath': a?['path']};
        _staticId = s?['textureId'] as int?;
        _animId = a?['textureId'] as int?;
      });
      _poll = Timer.periodic(const Duration(milliseconds: 500), (_) async {
        final st = await _channel.invokeMapMethod<Object?, Object?>('stats');
        if (mounted) setState(() => _stats = st ?? const {});
      });
    } on PlatformException catch (e) {
      setState(() => _error = '${e.code}: ${e.message}');
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    final line = '${DateTime.now().toIso8601String()} ${state.name}';
    debugPrint('SPIKE_B_LIFECYCLE $line');
    if (_redrawOnResume && state == AppLifecycleState.resumed) {
      _channel.invokeMethod<void>('redraw');
    }
    setState(() {
      _lifecycleLog.add(state.name);
      if (_lifecycleLog.length > 12) _lifecycleLog.removeAt(0);
    });
  }

  @override
  void dispose() {
    _poll?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final view = View.of(context);
    final dpr = view.devicePixelRatio;
    final pad = view.padding; // physical px
    double l(num px) => px / dpr;

    const gap = 16;
    final x0 = 32;
    final y0 = (pad.top.ceil() + 24);
    final rowsY = [for (var i = 0; i < 4; i++) y0 + i * (patH + gap)];
    final colsX = [for (var i = 0; i < 3; i++) x0 + i * (patW + gap)];
    final animY = rowsY.last + patH + gap;
    final textY = animY + animSize + gap;

    final backgrounds = <String, Widget Function()>{
      'checker': () => const CustomPaint(painter: CheckerPainter()),
      'gradient': () => const DecoratedBox(
        decoration: BoxDecoration(
          gradient: LinearGradient(
            colors: [Color(0xFFFF00FF), Color(0xFF00FFFF), Color(0xFFFFFF00)],
          ),
        ),
      ),
      'black': () => const ColoredBox(color: Color(0xFF000000)),
      'white': () => const ColoredBox(color: Color(0xFFFFFFFF)),
    };

    final layout = <String, Object>{
      'dpr': dpr,
      'physicalSize': [view.physicalSize.width, view.physicalSize.height],
      'pattern': {'w': patW, 'h': patH},
      'blocks': [for (final b in pattern) b.toJson()],
      'rows': [
        for (final (i, name) in backgrounds.keys.indexed)
          {
            'background': name,
            'texture': [colsX[0], rowsY[i], patW, patH],
            'flutter': [colsX[1], rowsY[i], patW, patH],
            'control': [colsX[2], rowsY[i], patW, patH],
          },
      ],
      'anim': [x0, animY, animSize, animSize],
      'info': _info.map((k, v) => MapEntry('$k', '$v')),
    };
    if (_staticId != null && !_layoutLogged) {
      _layoutLogged = true;
      // logcat and os_log truncate long lines, so print in chunks;
      // tool/sample.py reassembles them.
      final json = jsonEncode(layout);
      const chunk = 600;
      final n = (json.length + chunk - 1) ~/ chunk;
      for (var i = 0; i < n; i++) {
        final end = (i + 1) * chunk;
        debugPrint(
          'SPIKE_B_LAYOUT ${i + 1}/$n '
          '${json.substring(i * chunk, end < json.length ? end : json.length)}',
        );
      }
    }

    Widget at(int x, int y, int w, int h, Widget child) => Positioned(
      left: l(x),
      top: l(y),
      width: l(w),
      height: l(h),
      child: child,
    );

    final children = <Widget>[];
    for (final (i, bg) in backgrounds.values.indexed) {
      for (var c = 0; c < 3; c++) {
        Widget tile = bg();
        if (c == 0 && _staticId != null) {
          tile = Stack(
            fit: StackFit.expand,
            children: [
              tile,
              Texture(textureId: _staticId!, filterQuality: FilterQuality.none),
            ],
          );
        } else if (c == 1) {
          tile = Stack(
            fit: StackFit.expand,
            children: [
              tile,
              CustomPaint(painter: PatternPainter(dpr)),
            ],
          );
        }
        children.add(at(colsX[c], rowsY[i], patW, patH, tile));
      }
    }
    children.add(
      at(
        x0,
        animY,
        animSize,
        animSize,
        Stack(
          fit: StackFit.expand,
          children: [
            const CustomPaint(painter: CheckerPainter()),
            if (_animId != null)
              Texture(textureId: _animId!, filterQuality: FilterQuality.none),
          ],
        ),
      ),
    );

    final status = StringBuffer()
      ..writeln(_info.entries.map((e) => '${e.key}=${e.value}').join('  '))
      ..writeln(
        'dpr=$dpr forceGl=$_forceGl lifecycle=$_lifecycle '
        'redrawOnResume=$_redrawOnResume notify=$_notify',
      )
      ..writeln('stats: $_stats')
      ..writeln('app: ${_lifecycleLog.join(' > ')}');
    if (_error != null) status.writeln('ERROR $_error');

    children.add(
      Positioned(
        left: l(x0),
        top: l(textY),
        right: l(x0),
        child: Text(
          status.toString(),
          style: const TextStyle(fontSize: 11, color: Colors.white),
        ),
      ),
    );

    return Material(
      color: const Color(0xFF404040),
      child: Stack(children: children),
    );
  }
}

/// 16-px checkerboard aligned to the tile origin, in physical pixels.
class CheckerPainter extends CustomPainter {
  const CheckerPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final dpr =
        WidgetsBinding.instance.platformDispatcher.views.first.devicePixelRatio;
    final cell = 16 / dpr;
    final light = Paint()..color = const Color(0xFFC8C8C8);
    final dark = Paint()..color = const Color(0xFF5A5A5A);
    for (var y = 0; y * cell < size.height; y++) {
      for (var x = 0; x * cell < size.width; x++) {
        canvas.drawRect(
          Rect.fromLTWH(x * cell, y * cell, cell, cell),
          (x + y).isEven ? light : dark,
        );
      }
    }
  }

  @override
  bool shouldRepaint(CheckerPainter oldDelegate) => false;
}

/// Draws the test pattern with Flutter's canvas: the "expected" tile.
/// Probe blocks (invalid premultiplied colours) have no straight-alpha
/// equivalent and are left empty.
class PatternPainter extends CustomPainter {
  const PatternPainter(this.dpr);
  final double dpr;

  @override
  void paint(Canvas canvas, Size size) {
    for (final b in pattern) {
      if (b.a == 0 || b.isProbe) continue;
      int un(int c) => ((c * 255 + b.a ~/ 2) ~/ b.a).clamp(0, 255);
      canvas.drawRect(
        Rect.fromLTWH(
          b.col * blk / dpr,
          b.row * blk / dpr,
          blk / dpr,
          blk / dpr,
        ),
        Paint()..color = Color.fromARGB(b.a, un(b.r), un(b.g), un(b.b)),
      );
    }
  }

  @override
  bool shouldRepaint(PatternPainter oldDelegate) => oldDelegate.dpr != dpr;
}
