import 'package:flutter_test/flutter_test.dart';
import 'package:spike_b/main.dart';

void main() {
  test('pattern covers the texture with 32 uniform blocks', () {
    expect(pattern, hasLength(32));
    final px = buildPattern();
    expect(px, hasLength(patW * patH * 4));
  });

  test('only the probe blocks are invalid premultiplied colours', () {
    final probes = pattern.where((b) => b.isProbe).map((b) => b.name);
    expect(probes, ['probe_white_a0', 'probe_red_a128']);
  });
}
