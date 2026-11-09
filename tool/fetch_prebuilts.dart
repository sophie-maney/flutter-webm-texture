// Fetches the native prebuilts that CI publishes to a GitHub Release.
//
//   fvm dart run tool/fetch_prebuilts.dart [options]
//
// Reads third_party/versions.lock and third_party/build/, derives the release tag `deps-<hash>`,
// downloads `SHA256SUMS` and the per-platform archives from that release,
// verifies every archive against `SHA256SUMS`, and unpacks them into
// third_party/prebuilt/{android,ios}.
//
// It fails closed: any download error, missing or duplicate checksum entry,
// checksum mismatch, unsafe archive entry or unexpected layout aborts with a
// non-zero exit code and leaves the existing third_party/prebuilt untouched.
//
// Options:
//   --platform android|ios|all  what to fetch (default: all)
//   --repo OWNER/NAME           release repository (env WT_PREBUILT_REPO)
//   --base-url URL              directory holding the release assets instead of
//                               the GitHub release; http(s):// or file:// (env
//                               WT_PREBUILT_BASE_URL). Used by tests and for
//                               locally built archives.
//   --lock FILE                 versions.lock to hash (default: the repo's)
//   --build-dir DIR             build recipe to hash (default: third_party/build)
//   --out DIR                   output directory (default: third_party/prebuilt)
//   --force                     re-fetch even if the marker says it is current
//   --print-tag                 print the release tag and exit
//   -h, --help
//
// Needs `tar` on PATH. Only dart: libraries are used, so the script also runs
// as a plain `dart tool/fetch_prebuilts.dart` without resolving the package;
// the prebuilts workflow calls it with `--print-tag` to name the release.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

/// Repository the release lives in unless `--repo` says otherwise.
const defaultRepo = 'sophie-maney/flutter-webm-texture';

/// Name of the checksum file attached to every deps release.
const checksumsAsset = 'SHA256SUMS';

/// Largest download accepted, per file. The real archives are ~30 MB.
const maxDownloadBytes = 200 * 1024 * 1024;

/// Largest unpacked size accepted, per platform.
const maxUnpackedBytes = 400 * 1024 * 1024;

const _markerName = '.release';

/// What one platform's archive is called and what it must contain.
class PlatformSpec {
  const PlatformSpec(this.name, this.requiredFiles, this.requiredDirs);

  final String name;

  /// Paths (relative to the platform directory) that must exist after
  /// unpacking.
  final List<String> requiredFiles;
  final List<String> requiredDirs;

  String get asset => 'webm_texture_deps-$name.tar.gz';
}

const platforms = <PlatformSpec>[
  PlatformSpec('android', [], ['include']),
  PlatformSpec('ios', ['WebmTextureDeps.xcframework/Info.plist'], []),
];

/// Android ABIs the archive is expected to carry at least one of.
const _androidLib = 'libwebm_texture_deps.a';

class FetchException implements Exception {
  FetchException(this.message);
  final String message;
  @override
  String toString() => message;
}

// ---------------------------------------------------------------------------
// Lock hash

/// The release tag for a `versions.lock` text plus the build recipe.
///
/// [recipe] holds the files in `third_party/build/` (name → bytes). The tag
/// covers them so that changing a build script or flag yields a new release
/// instead of silently reusing stale binaries (docs/decisions/0003).
///
/// Of the lock only the data rows matter: comments, blank lines and column
/// alignment don't change the tag. Recipe files are hashed byte for byte, in
/// name order.
String releaseTag(String lockText, [Map<String, List<int>> recipe = const {}]) {
  final rows = <String>[];
  for (final raw in const LineSplitter().convert(lockText)) {
    final line = raw.trim();
    if (line.isEmpty || line.startsWith('#')) continue;
    rows.add(line.split(RegExp(r'\s+')).join(' '));
  }
  if (rows.isEmpty) throw FetchException('versions.lock has no entries');
  final text = StringBuffer('${rows.join('\n')}\n');
  for (final name in recipe.keys.toList()..sort()) {
    text.write('${sha256Hex(recipe[name]!)}  $name\n');
  }
  final digest = sha256Hex(utf8.encode(text.toString()));
  return 'deps-${digest.substring(0, 16)}';
}

/// Reads the regular files directly inside [dir] as a recipe map.
Map<String, List<int>> readRecipe(Directory dir) {
  if (!dir.existsSync()) throw FetchException('${dir.path} not found');
  return {
    for (final f in dir.listSync(followLinks: false).whereType<File>())
      f.uri.pathSegments.last: f.readAsBytesSync(),
  };
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Hand-rolled so the tool needs no package; the unit
// tests check it against the standard vectors.

const _k = <int>[
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, //
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const _mask = 0xffffffff;

int _rotr(int x, int n) => ((x >> n) | (x << (32 - n))) & _mask;

/// Lower-case hex SHA-256 of [data].
String sha256Hex(List<int> data) {
  final h = Uint32List.fromList(const [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, //
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]);
  final length = data.length;
  final paddedLength = ((length + 9 + 63) ~/ 64) * 64;
  final msg = Uint8List(paddedLength)
    ..setRange(0, length, data)
    ..[length] = 0x80;
  ByteData.sublistView(msg).setUint64(paddedLength - 8, length * 8);
  final view = ByteData.sublistView(msg);
  final w = Uint32List(64);
  for (var off = 0; off < paddedLength; off += 64) {
    for (var i = 0; i < 16; i++) {
      w[i] = view.getUint32(off + i * 4);
    }
    for (var i = 16; i < 64; i++) {
      final s0 = _rotr(w[i - 15], 7) ^ _rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      final s1 = _rotr(w[i - 2], 17) ^ _rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & _mask;
    }
    var a = h[0], b = h[1], c = h[2], d = h[3];
    var e = h[4], f = h[5], g = h[6], hh = h[7];
    for (var i = 0; i < 64; i++) {
      final s1 = _rotr(e, 6) ^ _rotr(e, 11) ^ _rotr(e, 25);
      final ch = (e & f) ^ (~e & _mask & g);
      final t1 = (hh + s1 + ch + _k[i] + w[i]) & _mask;
      final s0 = _rotr(a, 2) ^ _rotr(a, 13) ^ _rotr(a, 22);
      final maj = (a & b) ^ (a & c) ^ (b & c);
      final t2 = (s0 + maj) & _mask;
      hh = g;
      g = f;
      f = e;
      e = (d + t1) & _mask;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) & _mask;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  return h.map((v) => v.toRadixString(16).padLeft(8, '0')).join();
}

// ---------------------------------------------------------------------------
// SHA256SUMS

/// Parses `sha256sum`-style text (`<hex>  <name>` or `<hex> *<name>`) into a
/// name → digest map. A repeated name is an error: with two entries there is
/// no telling which one was meant.
Map<String, String> parseChecksums(String text) {
  final sums = <String, String>{};
  final line = RegExp(r'^([0-9a-fA-F]{64}) [ *](\S.*)$');
  for (final raw in const LineSplitter().convert(text)) {
    if (raw.trim().isEmpty) continue;
    final m = line.firstMatch(raw);
    if (m == null) throw FetchException('malformed $checksumsAsset line: $raw');
    final name = m.group(2)!;
    if (sums.containsKey(name)) {
      throw FetchException('$checksumsAsset lists $name more than once');
    }
    sums[name] = m.group(1)!.toLowerCase();
  }
  return sums;
}

// ---------------------------------------------------------------------------
// Download

Future<Uint8List> _download(Uri uri) async {
  if (uri.scheme == 'file') {
    final file = File.fromUri(uri);
    if (!file.existsSync()) throw FetchException('not found: $uri');
    if (file.lengthSync() > maxDownloadBytes) {
      throw FetchException('$uri is larger than $maxDownloadBytes bytes');
    }
    return file.readAsBytes();
  }
  if (uri.scheme != 'https' && uri.scheme != 'http') {
    throw FetchException('unsupported URL scheme in $uri');
  }
  final client = HttpClient()
    ..connectionTimeout = const Duration(seconds: 30)
    ..autoUncompress = false;
  try {
    final request = await client.getUrl(uri);
    final response = await request.close();
    if (response.statusCode != 200) {
      await response.drain<void>();
      throw FetchException('GET $uri failed: HTTP ${response.statusCode}');
    }
    if (response.contentLength > maxDownloadBytes) {
      throw FetchException('$uri is larger than $maxDownloadBytes bytes');
    }
    final out = BytesBuilder(copy: false);
    await for (final chunk in response) {
      out.add(chunk);
      if (out.length > maxDownloadBytes) {
        throw FetchException('$uri is larger than $maxDownloadBytes bytes');
      }
    }
    return out.takeBytes();
  } on SocketException catch (e) {
    throw FetchException('GET $uri failed: ${e.message}');
  } on HandshakeException catch (e) {
    throw FetchException('GET $uri failed: TLS error: ${e.message}');
  } finally {
    client.close(force: true);
  }
}

// ---------------------------------------------------------------------------
// Unpack

Future<ProcessResult> _tar(List<String> args) async {
  try {
    return await Process.run('tar', args, stdoutEncoding: utf8);
  } on ProcessException {
    throw FetchException("'tar' is required but not on PATH");
  }
}

/// Rejects entries that could write outside the target directory or that are
/// anything but plain files and directories (links, devices, ...).
Future<void> _checkArchive(File archive) async {
  final names = await _tar(['-tzf', archive.path]);
  if (names.exitCode != 0) {
    throw FetchException(
      '${archive.uri.pathSegments.last} is not a valid '
      'tar.gz: ${names.stderr}',
    );
  }
  for (final name in const LineSplitter().convert(names.stdout as String)) {
    final segments = name.split('/');
    if (name.startsWith('/') ||
        name.contains('\\') ||
        segments.contains('..') ||
        RegExp(r'^[A-Za-z]:').hasMatch(name)) {
      throw FetchException('unsafe path in archive: $name');
    }
  }
  final listing = await _tar(['-tzvf', archive.path]);
  for (final line in const LineSplitter().convert(listing.stdout as String)) {
    // Both GNU and BSD tar start a verbose line with the entry type char.
    if (line.isNotEmpty && line[0] != '-' && line[0] != 'd') {
      throw FetchException('archive has a link or special file: $line');
    }
  }
}

int _dirSize(Directory dir) {
  var total = 0;
  for (final e in dir.listSync(recursive: true, followLinks: false)) {
    if (e is File) total += e.lengthSync();
  }
  return total;
}

void _checkLayout(PlatformSpec p, Directory dir) {
  for (final f in p.requiredFiles) {
    if (!File('${dir.path}/$f').existsSync()) {
      throw FetchException('${p.name} archive is missing $f');
    }
  }
  for (final d in p.requiredDirs) {
    if (!Directory('${dir.path}/$d').existsSync()) {
      throw FetchException('${p.name} archive is missing $d/');
    }
  }
  if (p.name == 'android') {
    final abis = dir
        .listSync(followLinks: false)
        .whereType<Directory>()
        .where((d) => File('${d.path}/$_androidLib').existsSync());
    if (abis.isEmpty) {
      throw FetchException('android archive has no <abi>/$_androidLib');
    }
  }
}

// ---------------------------------------------------------------------------
// Driver

class Options {
  Options({
    required this.platforms,
    required this.repo,
    required this.baseUrl,
    required this.lockFile,
    required this.buildDir,
    required this.outDir,
    required this.force,
    required this.printTag,
  });

  final List<PlatformSpec> platforms;
  final String repo;
  final String? baseUrl;
  final File lockFile;
  final Directory buildDir;
  final Directory outDir;
  final bool force;
  final bool printTag;
}

class UsageException implements Exception {
  UsageException(this.message);
  final String message;
}

/// The repository root: the parent of this script's `tool/` directory.
Directory _repoRoot() => File.fromUri(Platform.script).parent.parent;

Options parseArgs(List<String> args, {Map<String, String>? environment}) {
  final env = environment ?? Platform.environment;
  var platformName = 'all';
  String? repo = env['WT_PREBUILT_REPO'];
  String? baseUrl = env['WT_PREBUILT_BASE_URL'];
  String? lock;
  String? buildDir;
  String? out;
  var force = false;
  var printTag = false;

  String value(int i, String flag) {
    if (i + 1 >= args.length) throw UsageException('$flag needs a value');
    return args[i + 1];
  }

  for (var i = 0; i < args.length; i++) {
    final a = args[i];
    switch (a) {
      case '--platform':
        platformName = value(i++, a);
      case '--repo':
        repo = value(i++, a);
      case '--base-url':
        baseUrl = value(i++, a);
      case '--lock':
        lock = value(i++, a);
      case '--build-dir':
        buildDir = value(i++, a);
      case '--out':
        out = value(i++, a);
      case '--force':
        force = true;
      case '--print-tag':
        printTag = true;
      default:
        throw UsageException('unknown argument $a');
    }
  }

  final selected = switch (platformName) {
    'all' => platforms,
    'android' || 'ios' => platforms.where((p) => p.name == platformName),
    _ => throw UsageException("--platform must be android, ios or all"),
  };
  final effectiveRepo = repo ?? defaultRepo;
  if (!RegExp(r'^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$').hasMatch(effectiveRepo)) {
    throw UsageException('--repo must look like OWNER/NAME');
  }
  final root = _repoRoot();
  return Options(
    platforms: selected.toList(),
    repo: effectiveRepo,
    baseUrl: baseUrl,
    lockFile: File(lock ?? '${root.path}/third_party/versions.lock'),
    buildDir: Directory(buildDir ?? '${root.path}/third_party/build'),
    outDir: Directory(out ?? '${root.path}/third_party/prebuilt'),
    force: force,
    printTag: printTag,
  );
}

/// Whether [p] is already installed at [tag] with an intact layout.
bool _isCurrent(PlatformSpec p, Options o, String tag) {
  final dir = Directory('${o.outDir.path}/${p.name}');
  final marker = File('${dir.path}/$_markerName');
  if (!marker.existsSync() || marker.readAsStringSync().trim() != tag) {
    return false;
  }
  try {
    _checkLayout(p, dir);
    return true;
  } on FetchException {
    return false; // marker present but files damaged: refetch
  }
}

Future<void> _fetchPlatform(
  PlatformSpec p,
  Options o,
  String tag,
  Uri base,
  Map<String, String> sums,
) async {
  final target = Directory('${o.outDir.path}/${p.name}');
  final expected = sums[p.asset];
  if (expected == null) {
    throw FetchException('$checksumsAsset has no entry for ${p.asset}');
  }
  stdout.writeln('${p.name}: downloading ${p.asset}');
  final bytes = await _download(base.resolve('./${p.asset}'));
  final actual = sha256Hex(bytes);
  if (actual != expected) {
    throw FetchException(
      'sha256 mismatch for ${p.asset}: '
      'expected $expected, got $actual',
    );
  }

  o.outDir.createSync(recursive: true);
  final work = o.outDir.createTempSync('.${p.name}.fetch-');
  try {
    final archive = File('${work.path}/${p.asset}')..writeAsBytesSync(bytes);
    await _checkArchive(archive);
    final unpacked = Directory('${work.path}/unpacked')..createSync();
    final result = await _tar(['-xzf', archive.path, '-C', unpacked.path]);
    if (result.exitCode != 0) {
      throw FetchException('unpacking ${p.asset} failed: ${result.stderr}');
    }
    if (_dirSize(unpacked) > maxUnpackedBytes) {
      throw FetchException(
        '${p.asset} unpacks to more than '
        '$maxUnpackedBytes bytes',
      );
    }
    _checkLayout(p, unpacked);
    File('${unpacked.path}/$_markerName').writeAsStringSync('$tag\n');

    // Swap in last, so a failure above never disturbs an existing install.
    if (target.existsSync()) target.deleteSync(recursive: true);
    unpacked.renameSync(target.path);
  } finally {
    work.deleteSync(recursive: true);
  }
  stdout.writeln('${p.name}: ready at ${target.path}');
}

/// Runs the tool; returns the process exit code.
Future<int> run(List<String> args, {Map<String, String>? environment}) async {
  if (args.contains('-h') || args.contains('--help')) {
    stdout.writeln(_usage());
    return 0;
  }
  try {
    final o = parseArgs(args, environment: environment);
    if (!o.lockFile.existsSync()) {
      throw FetchException('${o.lockFile.path} not found');
    }
    final tag = releaseTag(
      o.lockFile.readAsStringSync(),
      readRecipe(o.buildDir),
    );
    if (o.printTag) {
      stdout.writeln(tag);
      return 0;
    }
    final baseText =
        o.baseUrl ?? 'https://github.com/${o.repo}/releases/download/$tag';
    final base = Uri.parse(baseText.endsWith('/') ? baseText : '$baseText/');
    stdout.writeln('release $tag (${o.baseUrl ?? o.repo})');

    final pending = <PlatformSpec>[];
    for (final p in o.platforms) {
      if (!o.force && _isCurrent(p, o, tag)) {
        stdout.writeln('${p.name}: already at $tag');
      } else {
        pending.add(p);
      }
    }
    if (pending.isEmpty) return 0;

    // Parse the checksums before downloading any archive: nothing is unpacked
    // unless the digest listed for it is known.
    final sums = parseChecksums(
      utf8.decode(await _download(base.resolve('./$checksumsAsset'))),
    );
    for (final p in pending) {
      await _fetchPlatform(p, o, tag, base, sums);
    }
    return 0;
  } on UsageException catch (e) {
    stderr.writeln('error: ${e.message}\n\n${_usage()}');
    return 64;
  } on FetchException catch (e) {
    stderr.writeln('error: $e');
    return 1;
  } on FileSystemException catch (e) {
    stderr.writeln('error: ${e.message} ${e.path ?? ''}');
    return 1;
  }
}

String _usage() =>
    '''
Usage: fvm dart run tool/fetch_prebuilts.dart [options]
  --platform android|ios|all  what to fetch (default: all)
  --repo OWNER/NAME           release repository (default: $defaultRepo)
  --base-url URL              asset directory instead of the GitHub release
  --lock FILE                 versions.lock to hash
  --build-dir DIR             build recipe to hash (default: third_party/build)
  --out DIR                   output directory (default: third_party/prebuilt)
  --force                     re-fetch even if up to date
  --print-tag                 print the release tag and exit''';

Future<void> main(List<String> args) async {
  exit(await run(args));
}
