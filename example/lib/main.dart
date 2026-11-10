import 'package:flutter/material.dart';

import 'checkerboard_page.dart';
import 'chat_page.dart';
import 'probe_page.dart';
import 'stress_page.dart';
import 'texture_demo.dart';
import 'visibility_demo.dart';

void main() {
  runApp(const ExampleApp());
}

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'webm_texture',
    theme: ThemeData(colorSchemeSeed: Colors.indigo),
    home: const _HomePage(),
  );
}

class _Entry {
  const _Entry(this.key, this.title, this.subtitle, this.builder);

  final String key;
  final String title;
  final String subtitle;
  final WidgetBuilder builder;
}

final _entries = [
  _Entry(
    'openChat',
    'Chat',
    'Stickers mixed with text',
    (_) => const ChatPage(),
  ),
  _Entry(
    'openCheckerboard',
    'Checkerboard',
    'Alpha correctness over a checker pattern',
    (_) => const CheckerboardPage(),
  ),
  _Entry(
    'openStress',
    'Stress',
    '200 standalone stickers in a grid',
    (_) => const StressPage(),
  ),
  _Entry(
    'openProbe',
    'Probe',
    'File info without decoding',
    (_) => const ProbePage(),
  ),
  _Entry(
    'openTextures',
    'Textures (dev)',
    'Raw platform texture glue',
    (_) => const TextureDemoPage(),
  ),
  _Entry(
    'openVisibility',
    'Visibility (dev)',
    '200-row list with an active counter',
    (_) => const VisibilityDemoPage(),
  ),
];

class _HomePage extends StatelessWidget {
  const _HomePage();

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('webm_texture')),
    body: ListView(
      children: [
        for (final e in _entries)
          ListTile(
            key: Key(e.key),
            title: Text(e.title),
            subtitle: Text(e.subtitle),
            trailing: const Icon(Icons.chevron_right),
            onTap: () =>
                Navigator.of(context)
                    .push(MaterialPageRoute<void>(builder: e.builder)),
          ),
      ],
    ),
  );
}
