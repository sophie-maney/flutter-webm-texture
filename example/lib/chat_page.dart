// Chat: stickers mixed with text bubbles, the way a messenger shows them.
import 'package:flutter/material.dart';
import 'package:webm_texture/webm_texture.dart';

import 'corpus.dart';

/// One message: text, a sticker, or both.
class _Message {
  const _Message(this.mine, {this.text, this.sticker});

  final bool mine;
  final String? text;
  final String? sticker;
}

const _messages = [
  _Message(false, text: 'Did you see the new sticker pack?'),
  _Message(true, text: 'Not yet, send one!'),
  _Message(false, sticker: 'vp9_alpha_512.webm'),
  _Message(true, text: 'Ha. Here is one back'),
  _Message(true, sticker: 'vp8_alpha_256.webm'),
  _Message(false, text: 'This one is opaque, so it keeps its square'),
  _Message(false, sticker: 'vp9_opaque_256.webm'),
  _Message(true, text: 'Odd sizes work too'),
  _Message(true, sticker: 'vp9_odd_301x199.webm'),
  _Message(false, text: 'Variable frame rate:'),
  _Message(false, sticker: 'vp9_alpha_vfr.webm'),
  _Message(true, text: 'And one with an audio track that stays silent'),
  _Message(true, sticker: 'vp9_with_audio.webm'),
  _Message(false, text: 'Same sticker again, sharing one animation'),
  _Message(false, sticker: 'vp9_alpha_512.webm'),
  _Message(true, text: 'Scroll up and down: off-screen ones pause'),
  _Message(false, sticker: 'vp8_alpha_256.webm'),
  _Message(false, text: 'Sticker'),
  _Message(true, sticker: 'vp9_alpha_vfr.webm'),
  _Message(true, text: 'Nice'),
];

class ChatPage extends StatelessWidget {
  const ChatPage({super.key});

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Chat')),
    body: ListView.builder(
      key: const Key('chatList'),
      padding: const EdgeInsets.all(12),
      itemCount: _messages.length * 3,
      itemBuilder: (context, i) => _Bubble(_messages[i % _messages.length]),
    ),
  );
}

class _Bubble extends StatelessWidget {
  const _Bubble(this.message);

  final _Message message;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final sticker = message.sticker;
    final text = message.text;
    return Align(
      alignment: message.mine ? Alignment.centerRight : Alignment.centerLeft,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: sticker != null
            // Stickers sit directly on the page, without a bubble.
            ? WebmSticker(
                source: corpusSource(sticker),
                height: 160,
                placeholder: const SizedBox(width: 160, height: 160),
                errorBuilder: (_, e) => SizedBox(
                  width: 160,
                  height: 160,
                  child: Center(child: Text('$e')),
                ),
              )
            : DecoratedBox(
                decoration: BoxDecoration(
                  color: message.mine
                      ? scheme.primaryContainer
                      : scheme.surfaceContainerHighest,
                  borderRadius: BorderRadius.circular(16),
                ),
                child: Padding(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 14,
                    vertical: 10,
                  ),
                  child: ConstrainedBox(
                    constraints: BoxConstraints(
                      maxWidth: MediaQuery.sizeOf(context).width * 0.7,
                    ),
                    child: Text(text!),
                  ),
                ),
              ),
      ),
    );
  }
}
