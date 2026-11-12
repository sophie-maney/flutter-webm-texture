// Unit tests for the pure helpers in tool/upstream_watch.dart. No network.
import 'package:flutter_test/flutter_test.dart';

import '../tool/upstream_watch.dart';

void main() {
  group('parseStableVersion', () {
    test('accepts the release names the pins and tags use', () {
      expect(parseStableVersion('v1.17.0'), [1, 17, 0]);
      expect(parseStableVersion('1.6.0'), [1, 6, 0]);
      expect(parseStableVersion('libwebm-1.0.0.32'), [1, 0, 0, 32]);
      expect(parseStableVersion('giflib-6.1.3'), [6, 1, 3]);
    });

    test('rejects pre-releases, commit ids and junk', () {
      expect(parseStableVersion('v1.9.0-rc1'), isNull);
      expect(parseStableVersion('giflib-6.2.0-rc1'), isNull);
      expect(
        parseStableVersion('9e8c1ef826ea1f6cc624855555c36c4d38e72818'),
        isNull,
      );
      expect(parseStableVersion('main'), isNull);
    });
  });

  test('compareVersions compares numerically and pads with zeros', () {
    expect(compareVersions([1, 10, 0], [1, 9, 4]), greaterThan(0));
    expect(compareVersions([1, 0], [1, 0, 0]), 0);
    expect(compareVersions([1, 0, 0, 31], [1, 0, 0, 32]), lessThan(0));
  });

  test('latestStable picks the highest stable name', () {
    final r = latestStable([
      'v1.9.4',
      'v1.10.0',
      'v1.10.1-rc1',
      'v1.9.0',
      'main',
    ]);
    expect(r?.tag, 'v1.10.0');
    expect(formatVersion(r!.version), '1.10.0');
    expect(latestStable(['main', 'v2.0.0-beta']), isNull);
  });

  test('sourceForgeReleaseNames finds tarballs and skips pre-releases', () {
    const feed = '''
      <item><link>/giflib-6.x/giflib-6.1.2.tar.gz</link></item>
      <item><link>/giflib-6.x/giflib-6.1.3.tar.gz</link></item>
      <item><link>/giflib-6.x/giflib-6.2.0-rc1.tar.gz</link></item>
      <item><link>/lz4-1.11.1.tar.gz</link></item>
    ''';
    final names = sourceForgeReleaseNames(feed, 'giflib');
    expect(names, ['giflib-6.1.2', 'giflib-6.1.3']);
    expect(formatVersion(latestStable(names)!.version), '6.1.3');
  });

  test('parseLibyuvVersion reads LIBYUV_VERSION from version.h', () {
    const header =
        '#ifndef INCLUDE_LIBYUV_VERSION_H_\n'
        '#define INCLUDE_LIBYUV_VERSION_H_\n'
        '#define LIBYUV_VERSION 1982\n';
    expect(parseLibyuvVersion(header), 1982);
    expect(
      () => parseLibyuvVersion('#define OTHER 1'),
      throwsA(isA<FormatException>()),
    );
  });

  test('decodeGitilesJson strips the XSSI prefix', () {
    final json = decodeGitilesJson(")]}'\n{\"commit\": \"abc123\"}");
    expect(json['commit'], 'abc123');
  });

  test('parseLock skips comments and blank lines and rejects short rows', () {
    const text =
        '# header\n\n'
        'libvpx   1.17.0  aaaa  https://example.test/a.tar.gz\n';
    final pins = parseLock(text);
    expect(pins, hasLength(1));
    expect(pins.single.name, 'libvpx');
    expect(pins.single.version, '1.17.0');
    expect(
      () => parseLock('libvpx 1.17.0 aaaa'),
      throwsA(isA<FormatException>()),
    );
  });

  test(
    'parseOsvVuln reads the summary, falls back to details and lists fixes',
    () {
      final adv = parseOsvVuln({
        'id': 'OSV-2020-1035',
        'details': 'first line\nsecond line',
        'affected': [
          {
            'ranges': [
              {
                'type': 'GIT',
                'events': [
                  {'introduced': '0'},
                  {'fixed': 'abc1234'},
                ],
              },
            ],
          },
          {
            'ranges': [
              {
                'events': [
                  {'fixed': 'abc1234'},
                  {'fixed': '1.8.1'},
                ],
              },
            ],
          },
        ],
      });
      expect(adv, isNotNull);
      expect(adv!.id, 'OSV-2020-1035');
      expect(adv.summary, 'first line');
      expect(adv.fixed, ['abc1234', '1.8.1']);
      expect(parseOsvVuln({'summary': 'no id'}), isNull);
    },
  );
}
