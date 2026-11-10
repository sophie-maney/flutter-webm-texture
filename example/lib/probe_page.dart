// Probe: pick a corpus file and read its metadata through
// `WebmStickers.probe`, without decoding pixels.
import 'package:flutter/material.dart';
import 'package:webm_texture/webm_texture.dart';

import 'corpus.dart';

class ProbePage extends StatefulWidget {
  const ProbePage({super.key});

  @override
  State<ProbePage> createState() => _ProbePageState();
}

class _ProbePageState extends State<ProbePage> {
  String _name = allCorpus.first;
  Future<StickerInfo>? _info;

  @override
  void initState() {
    super.initState();
    _select(_name);
  }

  void _select(String name) {
    setState(() {
      _name = name;
      _info = WebmStickers.probe(corpusSource(name));
    });
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Probe')),
    body: ListView(
      padding: const EdgeInsets.all(16),
      children: [
        DropdownButton<String>(
          key: const Key('probeFile'),
          isExpanded: true,
          value: _name,
          items: [
            for (final n in allCorpus)
              DropdownMenuItem(value: n, child: Text(n)),
          ],
          onChanged: (n) => _select(n!),
        ),
        const SizedBox(height: 16),
        FutureBuilder<StickerInfo>(
          future: _info,
          builder: (context, snapshot) {
            if (snapshot.connectionState != ConnectionState.done) {
              return const Center(child: CircularProgressIndicator());
            }
            final error = snapshot.error;
            if (error != null) {
              return Text(
                error is StickerException ? error.name : '$error',
                key: const Key('probeError'),
                style: TextStyle(color: Theme.of(context).colorScheme.error),
              );
            }
            return _InfoTable(snapshot.requireData);
          },
        ),
      ],
    ),
  );
}

class _InfoTable extends StatelessWidget {
  const _InfoTable(this.info);

  final StickerInfo info;

  @override
  Widget build(BuildContext context) {
    final rows = {
      'format': info.format.name,
      'codec': info.codec.name,
      'size': '${info.width} × ${info.height}',
      'frames': '${info.frameCount}',
      'duration': '${info.duration.inMilliseconds} ms',
      'fps': info.fps.toStringAsFixed(2),
      'alpha': '${info.hasAlpha}',
      'audio': '${info.hasAudio}',
      'truncated': '${info.truncated}',
    };
    return Table(
      key: const Key('probeInfo'),
      columnWidths: const {0: IntrinsicColumnWidth()},
      children: [
        for (final e in rows.entries)
          TableRow(
            children: [
              Padding(
                padding: const EdgeInsets.only(right: 24, bottom: 8),
                child: Text(e.key),
              ),
              Text(e.value),
            ],
          ),
      ],
    );
  }
}
