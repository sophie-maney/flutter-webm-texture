// Watches upstream for the libraries pinned in third_party/versions.lock.
//
//   dart tool/upstream_watch.dart [--report-only] [--lock FILE] [--repo OWNER/NAME]
//
// For every pin it reports:
//   1. the latest stable upstream release (pre-releases and release candidates
//      are ignored). libyuv has no releases, so its pinned commit is compared
//      with main's LIBYUV_VERSION. A newer tarball is downloaded once so its
//      sha256 can be pasted into versions.lock.
//   2. advisories from OSV for the pinned commit (resolved from the tag with
//      `git ls-remote`) and for the pinned version, and whether a fix is in the
//      latest release.
//
// The Markdown report goes to stdout and to $GITHUB_STEP_SUMMARY. When a pin
// is actionable, one open issue labelled `upstream-watch` is created or updated
// (needs GITHUB_TOKEN and GITHUB_REPOSITORY). A bump is a human decision that
// reruns the Prebuilts workflow, so this never opens a PR. --report-only never
// touches issues.
//
// Exit codes: 0 when every check ran (actionable results are still 0), 1 when
// some check failed (the report still lists what was checked), 2 for usage
// errors. Only dart: libraries are used, plus sha256Hex from
// tool/fetch_prebuilts.dart, so this runs as a plain `dart tool/upstream_watch.dart`.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'fetch_prebuilts.dart' show sha256Hex;

const defaultLockPath = 'third_party/versions.lock';
const issueLabel = 'upstream-watch';
const issueTitle = 'Upstream watch: newer releases or advisories';
const _userAgent = 'webm-texture-upstream-watch';
const _gitilesRepo = 'https://chromium.googlesource.com/libyuv/libyuv';
const _maxTarballBytes = 100 * 1024 * 1024;
const _maxIndexBytes = 20 * 1024 * 1024;
const _maxApiBytes = 4 * 1024 * 1024;

/// A failed upstream lookup. It is reported and the run continues.
class UpstreamException implements Exception {
  UpstreamException(this.message);
  final String message;
  @override
  String toString() => message;
}

// ---------------------------------------------------------------------------
// versions.lock

/// One row of versions.lock.
class Pin {
  const Pin(this.name, this.version, this.sha256, this.url);
  final String name;
  final String version;
  final String sha256;
  final String url;
}

/// Parses the whitespace-separated rows of versions.lock. `#` lines are comments.
List<Pin> parseLock(String text) {
  final pins = <Pin>[];
  for (final raw in const LineSplitter().convert(text)) {
    final line = raw.trim();
    if (line.isEmpty || line.startsWith('#')) continue;
    final f = line.split(RegExp(r'\s+'));
    if (f.length != 4) {
      throw FormatException('versions.lock row needs 4 columns: $line');
    }
    pins.add(Pin(f[0], f[1], f[2], f[3]));
  }
  return pins;
}

// ---------------------------------------------------------------------------
// Version strings

final _stableName = RegExp(r'^(?:[A-Za-z][A-Za-z0-9]*-)?v?(\d+(?:\.\d+)*)$');

/// Numeric parts of a stable release name: `v1.17.0`, `libwebm-1.0.0.32`,
/// `giflib-6.1.3`, `1.6.0`. Returns null for pre-releases (`v1.9.0-rc1`),
/// commit ids and anything else.
List<int>? parseStableVersion(String name) {
  final m = _stableName.firstMatch(name.trim());
  if (m == null) return null;
  return [for (final p in m.group(1)!.split('.')) int.parse(p)];
}

/// Compares dotted versions numerically, treating missing parts as 0
/// (so `1.0` equals `1.0.0`).
int compareVersions(List<int> a, List<int> b) {
  for (var i = 0; i < a.length || i < b.length; i++) {
    final x = i < a.length ? a[i] : 0;
    final y = i < b.length ? b[i] : 0;
    if (x != y) return x.compareTo(y);
  }
  return 0;
}

String formatVersion(List<int> version) => version.join('.');

/// A stable upstream release: its tag or file-stem name and its version.
class Release {
  const Release(this.tag, this.version);
  final String tag;
  final List<int> version;
}

/// The highest stable release among [names]; null if there is none.
Release? latestStable(Iterable<String> names) {
  Release? best;
  for (final n in names) {
    final v = parseStableVersion(n);
    if (v == null) continue;
    if (best == null || compareVersions(v, best.version) > 0) {
      best = Release(n, v);
    }
  }
  return best;
}

/// Tarball file stems (`giflib-6.1.3`) for [project] named in a SourceForge
/// RSS feed or file listing.
List<String> sourceForgeReleaseNames(String text, String project) {
  final re = RegExp('${RegExp.escape(project)}-\\d+(?:\\.\\d+)+\\.tar\\.gz');
  return [
    for (final m in re.allMatches(text))
      m.group(0)!.replaceFirst('.tar.gz', ''),
  ];
}

/// `LIBYUV_VERSION` from libyuv's include/libyuv/version.h.
int parseLibyuvVersion(String versionHeader) {
  final m = RegExp(r'#define\s+LIBYUV_VERSION\s+(\d+)')
      .firstMatch(versionHeader);
  if (m == null) throw const FormatException('no LIBYUV_VERSION in version.h');
  return int.parse(m.group(1)!);
}

/// Gitiles prefixes its JSON responses with `)]}'`.
Map<String, Object?> decodeGitilesJson(String body) {
  final json = jsonDecode(body.replaceFirst(RegExp(r"^\)\]\}'\n?"), ''));
  if (json is! Map<String, Object?>) {
    throw const FormatException('gitiles JSON is not an object');
  }
  return json;
}

// ---------------------------------------------------------------------------
// OSV

/// One advisory that affects a pinned library.
class Advisory {
  Advisory(this.id, this.summary, this.fixed);
  final String id;
  final String summary;

  /// Fixed-in values from the affected ranges: versions or commit ids.
  final List<String> fixed;

  /// `yes`, `no` or `unknown`: whether the latest release contains a fix.
  String fixedInLatest = 'unknown';
}

/// Fixed-in values (`fixed` events) of an OSV vulnerability record.
List<String> osvFixedEvents(Map<String, Object?> vuln) {
  final out = <String>[];
  for (final affected in _list(vuln['affected'])) {
    for (final range in _list(_map(affected)['ranges'])) {
      for (final event in _list(_map(range)['events'])) {
        final fixed = _map(event)['fixed'];
        if (fixed is String && !out.contains(fixed)) out.add(fixed);
      }
    }
  }
  return out;
}

/// An advisory from an OSV record, or null when it has no id.
Advisory? parseOsvVuln(Map<String, Object?> vuln) {
  final id = vuln['id'];
  if (id is! String) return null;
  final summary = vuln['summary'];
  final details = vuln['details'];
  final raw = summary is String && summary.isNotEmpty
      ? summary
      : details is String && details.isNotEmpty
      ? details.split('\n').first
      : '(no summary)';
  final text = raw.replaceAll(RegExp(r'\s+'), ' ').trim();
  return Advisory(
    id,
    text.length > 120 ? '${text.substring(0, 117)}...' : text,
    osvFixedEvents(vuln),
  );
}

List<Object?> _list(Object? v) => v is List ? v.cast<Object?>() : const [];

Map<String, Object?> _map(Object? v) =>
    v is Map ? v.cast<String, Object?>() : const {};

// ---------------------------------------------------------------------------
// HTTP and git

class _Response {
  _Response(this.status, this.bytes);
  final int status;
  final List<int> bytes;
}

Future<_Response> _send(
  String method,
  Uri uri, {
  Map<String, String> headers = const {},
  List<int>? body,
  String? contentType,
  int maxBytes = 1024 * 1024,
}) async {
  final client = HttpClient()
    ..userAgent = _userAgent
    ..connectionTimeout = const Duration(seconds: 30);
  try {
    final req = await client.openUrl(method, uri);
    headers.forEach((k, v) => req.headers.set(k, v));
    if (contentType != null) {
      req.headers.contentType = ContentType.parse(contentType);
    }
    if (body != null) req.add(body);
    final resp = await req.close();
    final out = BytesBuilder(copy: false);
    await for (final chunk in resp) {
      out.add(chunk);
      if (out.length > maxBytes) {
        throw UpstreamException('$uri: response larger than $maxBytes bytes');
      }
    }
    return _Response(resp.statusCode, out.takeBytes());
  } finally {
    client.close(force: true);
  }
}

Future<List<int>> _bytes(String url, {int maxBytes = _maxIndexBytes}) async {
  final res = await _send('GET', Uri.parse(url), maxBytes: maxBytes);
  if (res.status != 200) {
    throw UpstreamException('GET $url: HTTP ${res.status}');
  }
  return res.bytes;
}

Future<String> _text(String url, {int maxBytes = _maxIndexBytes}) async =>
    utf8.decode(await _bytes(url, maxBytes: maxBytes));

String? get _token {
  for (final name in const ['GITHUB_TOKEN', 'GH_TOKEN']) {
    final v = Platform.environment[name];
    if (v != null && v.isNotEmpty) return v;
  }
  return null;
}

/// GitHub REST call. Unauthenticated when no token is set (lower rate limit).
Future<Object?> githubApi(String method, String path, {Object? json}) async {
  final token = _token;
  final res = await _send(
    method,
    Uri.parse('https://api.github.com$path'),
    headers: {
      'Accept': 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
      if (token != null) 'Authorization': 'Bearer $token',
    },
    body: json == null ? null : utf8.encode(jsonEncode(json)),
    contentType: json == null ? null : 'application/json',
    maxBytes: _maxApiBytes,
  );
  if (res.status < 200 || res.status >= 300) {
    throw UpstreamException(
      '$method $path: HTTP ${res.status} ${utf8.decode(res.bytes, allowMalformed: true)}',
    );
  }
  return res.bytes.isEmpty ? null : jsonDecode(utf8.decode(res.bytes));
}

Future<Object?> osvPost(Map<String, Object?> body) async {
  final res = await _send(
    'POST',
    Uri.parse('https://api.osv.dev/v1/query'),
    body: utf8.encode(jsonEncode(body)),
    contentType: 'application/json',
    maxBytes: _maxApiBytes,
  );
  if (res.status != 200) {
    throw UpstreamException('OSV query: HTTP ${res.status}');
  }
  return jsonDecode(utf8.decode(res.bytes));
}

/// Tag name to commit id for a remote repository. Annotated tags map to the
/// commit they point at (`^{}`).
Future<Map<String, String>> remoteTags(String repoUrl) async {
  final r = await Process.run('git', ['ls-remote', '--tags', repoUrl]);
  if (r.exitCode != 0) {
    throw UpstreamException('git ls-remote $repoUrl: ${r.stderr}'.trim());
  }
  final tags = <String, String>{};
  for (final line in const LineSplitter().convert(r.stdout as String)) {
    final parts = line.split('\t');
    if (parts.length != 2 || !parts[1].startsWith('refs/tags/')) continue;
    var name = parts[1].substring('refs/tags/'.length);
    final peeled = name.endsWith('^{}');
    if (peeled) name = name.substring(0, name.length - 3);
    if (peeled || !tags.containsKey(name)) tags[name] = parts[0];
  }
  return tags;
}

String? commitForVersion(Map<String, String> tags, List<int> version) {
  for (final e in tags.entries) {
    final v = parseStableVersion(e.key);
    if (v != null && compareVersions(v, version) == 0) return e.value;
  }
  return null;
}

/// `owner/name` for a `https://github.com/owner/name(.git)` URL.
String githubRepo(String url) {
  final segments = Uri.parse(url).pathSegments;
  return '${segments[0]}/${segments[1].replaceFirst(RegExp(r'\.git$'), '')}';
}

// ---------------------------------------------------------------------------
// Checks

/// How to find a library's releases and its tarball.
class Upstream {
  Upstream(
    this.name, {
    required this.tarballUrl,
    this.repoUrl,
    this.sourceForgeProject,
  });
  final String name;
  final String Function(String version) tarballUrl;

  /// Git repository whose tags are the releases.
  final String? repoUrl;

  /// SourceForge project whose RSS feed names the releases.
  final String? sourceForgeProject;
}

final upstreams = <Upstream>[
  Upstream(
    'libvpx',
    repoUrl: 'https://github.com/webmproject/libvpx.git',
    tarballUrl: (v) =>
        'https://github.com/webmproject/libvpx/archive/refs/tags/v$v.tar.gz',
  ),
  Upstream(
    'libwebp',
    repoUrl: 'https://github.com/webmproject/libwebp.git',
    tarballUrl: (v) =>
        'https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$v.tar.gz',
  ),
  Upstream(
    'libwebm',
    repoUrl: 'https://github.com/webmproject/libwebm.git',
    tarballUrl: (v) =>
        'https://github.com/webmproject/libwebm/archive/refs/tags/libwebm-$v.tar.gz',
  ),
  Upstream(
    'lz4',
    repoUrl: 'https://github.com/lz4/lz4.git',
    tarballUrl: (v) =>
        'https://github.com/lz4/lz4/releases/download/v$v/lz4-$v.tar.gz',
  ),
  Upstream(
    'giflib',
    sourceForgeProject: 'giflib',
    tarballUrl: (v) =>
        'https://downloads.sourceforge.net/project/giflib/giflib-${v.split('.').first}.x/giflib-$v.tar.gz',
  ),
];

/// Result for one library, as shown in the report.
class LibraryReport {
  LibraryReport(this.name, this.pinned);
  final String name;
  String pinned;
  String latest = '-';
  String status = 'error';

  /// A newer release (or libyuv LIBYUV_VERSION) exists.
  bool newer = false;
  String? newerUrl;
  String? newerSha256;
  final notes = <String>[];
  final advisories = <Advisory>[];
  final errors = <String>[];

  bool get actionable => newer || advisories.isNotEmpty;
}

Future<LibraryReport> checkUpstream(Pin pin, Upstream u) async {
  final r = LibraryReport(pin.name, pin.version);
  final pinned = parseStableVersion(pin.version);
  if (pinned == null) {
    r.errors.add('pinned version "${pin.version}" is not a stable release');
    return r;
  }

  Release? latest;
  String? pinnedCommit;
  String? latestCommit;
  final repo = u.repoUrl == null ? null : githubRepo(u.repoUrl!);
  try {
    if (u.repoUrl != null) {
      final tags = await remoteTags(u.repoUrl!);
      pinnedCommit = commitForVersion(tags, pinned);
      latest = latestStable(tags.keys);
      if (latest != null) latestCommit = tags[latest.tag];
    } else {
      final project = u.sourceForgeProject!;
      final names = <String>[];
      for (final dir in ['/', '/$project-${pinned.first}.x']) {
        final feed = await _text(
          'https://sourceforge.net/projects/$project/rss?path=$dir',
        );
        names.addAll(sourceForgeReleaseNames(feed, project));
      }
      latest = latestStable(names);
    }
  } catch (e) {
    r.errors.add('release lookup failed: $e');
  }

  if (latest == null) {
    if (r.errors.isEmpty) r.errors.add('no stable release found');
  } else {
    final latestVersion = formatVersion(latest.version);
    r.latest = latestVersion;
    if (compareVersions(latest.version, pinned) > 0) {
      r.newer = true;
      r.status = 'newer release';
      final url = u.tarballUrl(latestVersion);
      try {
        final bytes = await _bytes(url, maxBytes: _maxTarballBytes);
        r.newerUrl = url;
        r.newerSha256 = sha256Hex(bytes);
      } catch (e) {
        r.errors.add('tarball for $latestVersion: $e');
      }
    } else {
      r.status = 'up to date';
    }
  }

  await _addAdvisories(
    r,
    queries: [
      if (pinnedCommit != null) {'commit': pinnedCommit},
      {
        'package': {'name': pin.name, 'ecosystem': 'OSS-Fuzz'},
        'version': pin.version,
      },
      {
        'package': {'purl': 'pkg:generic/${pin.name}@${pin.version}'},
      },
    ],
    latestVersion: latest?.version,
    latestCommit: latestCommit,
    repo: repo,
  );
  return r;
}

Future<LibraryReport> checkLibyuv(Pin pin) async {
  final r = LibraryReport('libyuv', _short(pin.version));
  try {
    final mainCommit = decodeGitilesJson(
      await _text('$_gitilesRepo/+/refs/heads/main?format=JSON'),
    )['commit'];
    if (mainCommit is! String) throw const FormatException('no main commit');
    final pinnedVer = await _libyuvVersion(pin.version);
    final mainVer = await _libyuvVersion(mainCommit);
    r.pinned = '${_short(pin.version)} (LIBYUV_VERSION $pinnedVer)';
    r.latest = 'main ${_short(mainCommit)} (LIBYUV_VERSION $mainVer)';
    if (mainVer > pinnedVer) {
      r.newer = true;
      r.status = 'newer LIBYUV_VERSION';
      r.notes.add(
        'libyuv has no tarball release: pin commit $mainCommit in versions.lock '
        'and recompute its `git archive` sha256 (ADR 0002).',
      );
    } else if (mainCommit != pin.version) {
      r.status = 'newer commit, same LIBYUV_VERSION';
    } else {
      r.status = 'up to date';
    }
    await _addAdvisories(
      r,
      queries: [
        {'commit': pin.version},
      ],
      latestCommit: mainCommit,
      repo: null,
    );
  } catch (e) {
    r.errors.add('libyuv lookup failed: $e');
  }
  return r;
}

Future<int> _libyuvVersion(String commit) async {
  final encoded = await _text(
    '$_gitilesRepo/+/$commit/include/libyuv/version.h?format=TEXT',
  );
  final header = utf8.decode(
    base64.decode(encoded.replaceAll(RegExp(r'\s'), '')),
  );
  return parseLibyuvVersion(header);
}

String _short(String commit) =>
    commit.length > 8 ? commit.substring(0, 8) : commit;

/// Queries OSV with each query, merges the advisories and works out for each
/// whether the latest release contains its fix. Errors go to [LibraryReport.errors].
Future<void> _addAdvisories(
  LibraryReport r, {
  required List<Map<String, Object?>> queries,
  List<int>? latestVersion,
  String? latestCommit,
  String? repo,
}) async {
  final found = <String, Advisory>{};
  try {
    for (final q in queries) {
      var body = Map<String, Object?>.of(q);
      for (var page = 0; page < 10; page++) {
        final resp = _map(await osvPost(body));
        for (final v in _list(resp['vulns'])) {
          final adv = parseOsvVuln(_map(v));
          if (adv != null) found.putIfAbsent(adv.id, () => adv);
        }
        final next = resp['next_page_token'];
        if (next is! String || next.isEmpty) break;
        body = {...q, 'page_token': next};
      }
    }
  } catch (e) {
    r.errors.add('OSV query failed: $e');
  }
  for (final adv in found.values) {
    if (adv.fixed.isEmpty) {
      adv.fixedInLatest = 'no';
    } else {
      final verdicts = <String>[];
      for (final fix in adv.fixed) {
        verdicts.add(
          await _fixedInLatest(
            fix,
            latestVersion: latestVersion,
            latestCommit: latestCommit,
            repo: repo,
          ),
        );
      }
      adv.fixedInLatest = verdicts.contains('yes')
          ? 'yes'
          : verdicts.every((v) => v == 'no')
          ? 'no'
          : 'unknown';
    }
    r.advisories.add(adv);
  }
}

Future<String> _fixedInLatest(
  String fixed, {
  List<int>? latestVersion,
  String? latestCommit,
  String? repo,
}) async {
  final fixedVersion = parseStableVersion(fixed);
  if (fixedVersion != null) {
    if (latestVersion == null) return 'unknown';
    return compareVersions(latestVersion, fixedVersion) >= 0 ? 'yes' : 'no';
  }
  if (repo == null ||
      latestCommit == null ||
      !RegExp(r'^[0-9a-f]{7,40}$').hasMatch(fixed)) {
    return 'unknown';
  }
  try {
    // base = the fix, head = latest tag: "ahead" or "identical" means the fix is included.
    final cmp = _map(
      await githubApi('GET', '/repos/$repo/compare/$fixed...$latestCommit'),
    );
    return switch (cmp['status']) {
      'ahead' || 'identical' => 'yes',
      'behind' || 'diverged' => 'no',
      _ => 'unknown',
    };
  } catch (_) {
    return 'unknown';
  }
}

// ---------------------------------------------------------------------------
// Report and issue

String _cell(String s) => s.replaceAll('|', r'\|').replaceAll('\n', ' ');

String renderReport(
  List<LibraryReport> reports, {
  required DateTime checkedAt,
  required String lockPath,
}) {
  final b = StringBuffer()
    ..writeln('# Upstream watch')
    ..writeln()
    ..writeln('Checked ${checkedAt.toIso8601String()} against `$lockPath`.')
    ..writeln()
    ..writeln('| Library | Pinned | Latest stable | Status | Advisories |')
    ..writeln('|---|---|---|---|---|');
  for (final r in reports) {
    b.writeln(
      '| ${_cell(r.name)} | ${_cell(r.pinned)} | ${_cell(r.latest)} '
      '| ${_cell(r.status)} | ${r.advisories.length} |',
    );
  }
  final actionable = reports.where((r) => r.actionable).length;
  b
    ..writeln()
    ..writeln(
      actionable == 0
          ? 'Nothing actionable.'
          : 'Actionable: $actionable. Bumping a pin means editing its row in '
                '`versions.lock` and then running the Prebuilts workflow by hand.',
    );

  final newer = reports.where((r) => r.newer).toList();
  if (newer.isNotEmpty) {
    b
      ..writeln()
      ..writeln('## Newer releases');
    for (final r in newer) {
      b.writeln('- **${r.name} ${r.latest}**');
      if (r.newerUrl != null) {
        b
          ..writeln('  - URL: `${r.newerUrl}`')
          ..writeln('  - sha256: `${r.newerSha256}`')
          ..writeln(
            '  - versions.lock row: `${r.name} ${r.latest} ${r.newerSha256} ${r.newerUrl}`',
          );
      }
      for (final note in r.notes) {
        b.writeln('  - $note');
      }
    }
  }

  final withAdvisories = reports.where((r) => r.advisories.isNotEmpty).toList();
  if (withAdvisories.isNotEmpty) {
    b
      ..writeln()
      ..writeln('## Advisories (OSV)')
      ..writeln()
      ..writeln(
        '| Library | Advisory | Summary | Fixed in | Fixed in latest release |',
      )
      ..writeln('|---|---|---|---|---|');
    for (final r in withAdvisories) {
      for (final a in r.advisories) {
        final fixed = a.fixed.isEmpty ? 'none listed' : a.fixed.join(', ');
        b.writeln(
          '| ${_cell(r.name)} | [${_cell(a.id)}](https://osv.dev/vulnerability/${a.id}) '
          '| ${_cell(a.summary)} | ${_cell(fixed)} | ${a.fixedInLatest} |',
        );
      }
    }
  }

  final failed = reports.where((r) => r.errors.isNotEmpty).toList();
  if (failed.isNotEmpty) {
    b
      ..writeln()
      ..writeln('## Errors')
      ..writeln();
    for (final r in failed) {
      for (final e in r.errors) {
        b.writeln('- ${r.name}: $e');
      }
    }
  }
  return b.toString();
}

Future<String> upsertIssue(String repo, String body) async {
  final open = await githubApi(
    'GET',
    '/repos/$repo/issues?state=open&labels=$issueLabel&per_page=20',
  );
  Map<String, Object?>? existing;
  for (final i in _list(open)) {
    final m = _map(i);
    if (m['pull_request'] == null) {
      existing = m;
      break;
    }
  }
  if (existing == null) {
    await githubApi(
      'POST',
      '/repos/$repo/issues',
      json: {
        'title': issueTitle,
        'body': body,
        'labels': [issueLabel],
      },
    );
    return 'created';
  }
  final number = existing['number'];
  await githubApi(
    'PATCH',
    '/repos/$repo/issues/$number',
    json: {'title': issueTitle, 'body': body},
  );
  return 'updated #$number';
}

// ---------------------------------------------------------------------------
// Entry point

const _usage =
    '''
Usage: dart tool/upstream_watch.dart [options]

  --report-only      print the report; never create or update issues
  --lock FILE        versions.lock to read (default: $defaultLockPath)
  --repo OWNER/NAME  repository for the issue (default: \$GITHUB_REPOSITORY)
  -h, --help         show this help

Env: GITHUB_TOKEN (or GH_TOKEN) for GitHub and issue calls; GITHUB_STEP_SUMMARY
receives the report when set.
''';

Future<void> main(List<String> args) async {
  exitCode = await run(args);
}

/// Runs the watch and returns the process exit code.
Future<int> run(List<String> args) async {
  var reportOnly = false;
  var lockPath = defaultLockPath;
  String? repo;
  for (var i = 0; i < args.length; i++) {
    final a = args[i];
    if (a == '--report-only') {
      reportOnly = true;
    } else if (a == '-h' || a == '--help') {
      stdout.write(_usage);
      return 0;
    } else if (a == '--lock' || a == '--repo') {
      if (i + 1 >= args.length) {
        stderr.writeln('$a needs a value\n$_usage');
        return 2;
      }
      if (a == '--lock') {
        lockPath = args[++i];
      } else {
        repo = args[++i];
      }
    } else {
      stderr.writeln('unknown option: $a\n$_usage');
      return 2;
    }
  }
  repo ??= Platform.environment['GITHUB_REPOSITORY'];

  final List<Pin> pins;
  try {
    pins = parseLock(await File(lockPath).readAsString());
  } on FileSystemException catch (e) {
    stderr.writeln('cannot read $lockPath: ${e.message}');
    return 2;
  }

  final reports = <LibraryReport>[];
  for (final pin in pins) {
    stderr.writeln('checking ${pin.name} ${pin.version} ...');
    if (pin.name == 'libyuv') {
      reports.add(await checkLibyuv(pin));
      continue;
    }
    final u = upstreams.where((u) => u.name == pin.name).firstOrNull;
    if (u == null) {
      final r = LibraryReport(pin.name, pin.version)
        ..errors.add('no upstream configured in tool/upstream_watch.dart');
      reports.add(r);
      continue;
    }
    reports.add(await checkUpstream(pin, u));
  }

  final md = renderReport(
    reports,
    checkedAt: DateTime.now().toUtc(),
    lockPath: lockPath,
  );
  stdout.write(md);
  final summary = Platform.environment['GITHUB_STEP_SUMMARY'];
  if (summary != null && summary.isNotEmpty) {
    await File(summary).writeAsString(md, mode: FileMode.append);
  }

  var failed = reports.any((r) => r.errors.isNotEmpty);
  if (reportOnly) {
    stdout.writeln('\n--report-only: issues not touched.');
  } else if (reports.any((r) => r.actionable)) {
    final token = _token;
    if (repo == null || token == null) {
      stderr.writeln('issue update needs GITHUB_REPOSITORY and GITHUB_TOKEN');
      failed = true;
    } else {
      try {
        final runUrl = Platform.environment['GITHUB_RUN_ID'] == null
            ? ''
            : '\n\nRun: ${Platform.environment['GITHUB_SERVER_URL']}/$repo/actions/runs/${Platform.environment['GITHUB_RUN_ID']}';
        final result = await upsertIssue(repo, '$md$runUrl');
        stdout.writeln('\nIssue $result (label `$issueLabel`).');
      } catch (e) {
        stderr.writeln('issue update failed: $e');
        failed = true;
      }
    }
  } else {
    stdout.writeln('\nNothing actionable; issue left as it is.');
  }
  return failed ? 1 : 0;
}
