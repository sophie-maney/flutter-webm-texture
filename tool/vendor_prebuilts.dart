// Prepares the package for `pub publish`: the release job's vendoring step.
//
//   fvm dart run tool/vendor_prebuilts.dart vendor [--skip-fetch]
//   fvm dart run tool/vendor_prebuilts.dart archive --out build/release/webm_texture.tar.gz
//
// `vendor` runs tool/fetch_prebuilts.dart (which verifies every archive against
// the release's SHA256SUMS and fails closed), then checks that the prebuilts
// are complete, that every symlink the iOS package relies on resolves, that
// nothing shipped is a shared library with the wrong page alignment, and that
// each platform's prebuilts are under the size limit. It prints per-platform
// sizes. `--skip-fetch` checks what is already in third_party/prebuilt.
//
// `archive` runs `flutter pub publish --dry-run`, which must pass with no
// warnings, then stages exactly the files it lists (pub follows symlinks, so
// the staging copy is made of regular files, as the consumer will get them)
// and writes a .tar.gz. It fails when the archive is too big for pub.dev.
//
// There is no `dart pub publish --to-archive`, so the file list comes from the
// dry run's tree. Set FLUTTER to change how flutter is invoked (default
// `fvm flutter`).
//
// ADR 0024 explains the layout and the limits.
import 'dart:convert';
import 'dart:io';

/// pub.dev rejects archives over 100 MiB compressed.
const pubLimitBytes = 100 * 1024 * 1024;

/// We fail well below the pub.dev limit; the real archive is ~7 MB.
const maxArchiveBytes = 40 * 1024 * 1024;

/// Per-platform ceiling for the unpacked prebuilts (14 MB and 12 MB today).
const maxPlatformBytes = 40 * 1024 * 1024;

/// Ceiling for the unpacked package.
const maxUnpackedBytes = 120 * 1024 * 1024;

const androidAbis = ['arm64-v8a', 'armeabi-v7a', 'x86_64'];

Future<void> main(List<String> args) async {
  if (args.isEmpty || args.first == '-h' || args.first == '--help') {
    stdout.writeln(
      'usage: vendor_prebuilts.dart vendor [--skip-fetch] | archive --out FILE',
    );
    exit(args.isEmpty ? 64 : 0);
  }
  final root = Directory.current.path;
  if (!File('$root/pubspec.yaml').existsSync() ||
      !Directory('$root/tool').existsSync()) {
    _fail('run this from the repository root');
  }
  switch (args.first) {
    case 'vendor':
      await _vendor(root, skipFetch: args.contains('--skip-fetch'));
    case 'archive':
      final i = args.indexOf('--out');
      if (i < 0 || i + 1 >= args.length) _fail('archive needs --out FILE');
      await _archive(root, args[i + 1]);
    default:
      _fail('unknown command ${args.first}');
  }
}

Never _fail(String message) {
  stderr.writeln('vendor_prebuilts: $message');
  exit(1);
}

Future<void> _vendor(String root, {required bool skipFetch}) async {
  if (!skipFetch) {
    final r = await Process.start(Platform.resolvedExecutable, [
      'run',
      'tool/fetch_prebuilts.dart',
    ], mode: ProcessStartMode.inheritStdio);
    if (await r.exitCode != 0) _fail('fetch_prebuilts failed');
  }

  final prebuilt = '$root/third_party/prebuilt';
  final required = [
    '$prebuilt/android/include/vpx/vpx_decoder.h',
    for (final abi in androidAbis)
      '$prebuilt/android/$abi/libwebm_texture_deps.a',
    '$prebuilt/ios/WebmTextureDeps.xcframework/Info.plist',
    for (final slice in ['ios-arm64', 'ios-arm64_x86_64-simulator'])
      '$prebuilt/ios/WebmTextureDeps.xcframework/$slice/libWebmTextureDeps.a',
  ];
  for (final p in required) {
    if (!File(p).existsSync()) _fail('missing prebuilt: $p (fetch first)');
  }

  // Every symlink under the package that SwiftPM or the example needs must
  // resolve, or pub would silently leave the file out.
  var links = 0;
  for (final e in Directory(
    root,
  ).listSync(recursive: true, followLinks: false)) {
    if (e is! Link) continue;
    final rel = e.path.substring(root.length + 1);
    if (rel.startsWith('build/') ||
        rel.startsWith('.dart_tool/') ||
        rel.contains('/.dart_tool/') ||
        rel.contains('/build/') ||
        rel.startsWith('.fvm/')) {
      continue;
    }
    links++;
    if (!FileSystemEntity.isDirectorySync(e.path) &&
        !FileSystemEntity.isFileSync(e.path)) {
      _fail('dangling symlink: $rel -> ${e.targetSync()}');
    }
  }

  // Shipped shared libraries need 16 KB-aligned LOAD segments (AGENTS.md).
  // The prebuilts are static archives and the .so is built by the consumer's
  // Gradle, so none is expected; guard against one sneaking in.
  final sos = Directory(prebuilt)
      .listSync(recursive: true)
      .whereType<File>()
      .where((f) => f.path.endsWith('.so'))
      .toList();
  for (final so in sos) {
    await _checkAlignment(so.path);
  }

  var failed = false;
  stdout.writeln('Prebuilts (limit ${_mb(maxPlatformBytes)} per platform):');
  for (final platform in ['android', 'ios']) {
    final bytes = _dirSize('$prebuilt/$platform');
    stdout.writeln('  $platform: ${_mb(bytes)}');
    if (bytes > maxPlatformBytes) {
      stderr.writeln('  $platform exceeds ${_mb(maxPlatformBytes)}');
      failed = true;
    }
  }
  for (final abi in androidAbis) {
    stdout.writeln(
      '    android/$abi: '
      '${_mb(File('$prebuilt/android/$abi/libwebm_texture_deps.a').lengthSync())}',
    );
  }
  stdout.writeln(
    'Symlinks resolving: $links; shared libraries checked: ${sos.length}',
  );
  if (failed) _fail('size limit exceeded');
}

Future<void> _checkAlignment(String path) async {
  for (final tool in ['llvm-readelf', 'readelf']) {
    final r = await Process.run(tool, [
      '-l',
      '-W',
      path,
    ]).catchError((_) => ProcessResult(0, 127, '', ''));
    if (r.exitCode != 0) continue;
    for (final line in LineSplitter.split(r.stdout as String)) {
      if (!line.trimLeft().startsWith('LOAD')) continue;
      final align = line.trim().split(RegExp(r'\s+')).last;
      if (int.parse(align) % 0x4000 != 0) {
        _fail('$path: LOAD alignment $align is not a multiple of 0x4000');
      }
    }
    return;
  }
  _fail('$path is a shared library but no readelf is available to check it');
}

Future<void> _archive(String root, String out) async {
  final flutter = (Platform.environment['FLUTTER'] ?? 'fvm flutter').split(' ');
  final r = await Process.run(flutter.first, [
    ...flutter.skip(1),
    'pub',
    'publish',
    '--dry-run',
  ], workingDirectory: root);
  final log = '${r.stdout}${r.stderr}';
  stdout.write(log);
  if (r.exitCode != 0) _fail('pub publish --dry-run failed (${r.exitCode})');
  if (log.contains('potential issue') || log.contains('warning')) {
    _fail('pub publish --dry-run reported warnings');
  }

  final files = _parseTree(log);
  if (files.isEmpty) _fail('could not read the file list from the dry run');
  for (final needed in [
    'pubspec.yaml',
    'third_party/prebuilt/android/arm64-v8a/libwebm_texture_deps.a',
    'ios/webm_texture/Frameworks/WebmTextureDeps.xcframework/Info.plist',
    'ios/webm_texture/Sources/wt_core/api/wt_api.cc',
    'src/CMakeLists.txt',
  ]) {
    if (!files.contains(needed)) _fail('the archive would lack $needed');
  }

  final stage = Directory.systemTemp.createTempSync('wt_archive_');
  try {
    var bytes = 0;
    for (final f in files) {
      final src = File('$root/$f');
      if (!src.existsSync()) _fail('listed but unreadable: $f');
      final dst = File('${stage.path}/$f')..parent.createSync(recursive: true);
      src.copySync(dst.path); // follows symlinks
      bytes += dst.lengthSync();
    }
    final outFile = File(out)..parent.createSync(recursive: true);
    final tar = await Process.run('tar', [
      '-czf',
      outFile.absolute.path,
      '-C',
      stage.path,
      '.',
    ]);
    if (tar.exitCode != 0) _fail('tar failed: ${tar.stderr}');
    final packed = outFile.lengthSync();
    stdout.writeln(
      '\nArchive: ${outFile.path}\n'
      '  files: ${files.length}\n'
      '  unpacked: ${_mb(bytes)} (limit ${_mb(maxUnpackedBytes)})\n'
      '  compressed: ${_mb(packed)} (our limit ${_mb(maxArchiveBytes)}, '
      'pub.dev ${_mb(pubLimitBytes)})',
    );
    for (final top in [
      'android',
      'ios',
      'src',
      'third_party',
      'lib',
      'example',
    ]) {
      stdout.writeln(
        '    $top/: ${_mb(_dirSize('${stage.path}/$top'))} unpacked',
      );
    }
    if (bytes > maxUnpackedBytes || packed > maxArchiveBytes) {
      _fail('archive too large');
    }
  } finally {
    stage.deleteSync(recursive: true);
  }
}

/// Reads the file paths out of the dry run's box-drawing tree.
Set<String> _parseTree(String log) {
  final files = <String>{};
  final stack = <String>[];
  var inTree = false;
  for (final line in LineSplitter.split(log)) {
    if (line.startsWith('Publishing ')) {
      inTree = true;
      continue;
    }
    if (!inTree) continue;
    final m = RegExp(
      r'^((?:│   |    )*)[├└]── (.+?)( \((?:<1|\d+) [KMG]?B\))?$',
    ).firstMatch(line);
    if (m == null) {
      if (line.trim().isEmpty || line.startsWith('Total compressed')) break;
      continue;
    }
    final depth = m.group(1)!.length ~/ 4;
    stack.length = depth;
    final name = m.group(2)!;
    stack.add(name);
    if (m.group(3) != null) files.add(stack.join('/'));
  }
  return files;
}

int _dirSize(String path) {
  final d = Directory(path);
  if (!d.existsSync()) return 0;
  var total = 0;
  for (final e in d.listSync(recursive: true, followLinks: true)) {
    if (e is File) total += e.lengthSync();
  }
  return total;
}

String _mb(int bytes) => '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MiB';
