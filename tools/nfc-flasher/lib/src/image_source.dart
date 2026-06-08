// Resolve a user-supplied link into a FirmwareImage.
//
// Supported inputs:
//   * Direct URL to a .sfu / .hex / .bin file.
//   * GitHub release page  (…/releases/tag/<tag>  or  …/releases/latest).
//   * GitHub asset download URL (…/releases/download/<tag>/<file>).
//
// Asset preference for GitHub releases: .sfu > .bin > .hex.

import 'dart:convert';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import 'protocol.dart';

class ImageSource {
  /// Optional GitHub token for private repositories.
  final String? githubToken;

  ImageSource({this.githubToken});

  Future<FirmwareImage> load(String link) async {
    link = link.trim();
    final uri = Uri.parse(link);

    final bytes = await _fetchBytes(uri);
    return _decode(uri.path, bytes);
  }

  Future<Uint8List> _fetchBytes(Uri uri) async {
    final isGithubReleasePage = uri.host == 'github.com' &&
        uri.pathSegments.contains('releases') &&
        !uri.pathSegments.contains('download');

    if (isGithubReleasePage) {
      final assetUrl = await _resolveGithubAsset(uri);
      return _download(Uri.parse(assetUrl), accept: 'application/octet-stream');
    }
    return _download(uri);
  }

  Future<Uint8List> _download(Uri uri, {String? accept}) async {
    final headers = <String, String>{};
    if (accept != null) headers['Accept'] = accept;
    if (githubToken != null && uri.host.contains('github')) {
      headers['Authorization'] = 'Bearer $githubToken';
    }
    final resp = await http.get(uri, headers: headers);
    if (resp.statusCode != 200) {
      throw HttpException('Download failed (${resp.statusCode}) for $uri');
    }
    return resp.bodyBytes;
  }

  /// Query the GitHub API for a release and pick the best firmware asset.
  Future<String> _resolveGithubAsset(Uri uri) async {
    // /<owner>/<repo>/releases/(tag/<tag> | latest)
    final seg = uri.pathSegments;
    final owner = seg[0];
    final repo = seg[1];
    final relIndex = seg.indexOf('releases');
    final String apiPath;
    if (relIndex + 1 < seg.length && seg[relIndex + 1] == 'tag') {
      apiPath = 'tags/${seg[relIndex + 2]}';
    } else {
      apiPath = 'latest';
    }
    final apiUri =
        Uri.parse('https://api.github.com/repos/$owner/$repo/releases/$apiPath');

    final headers = <String, String>{'Accept': 'application/vnd.github+json'};
    if (githubToken != null) headers['Authorization'] = 'Bearer $githubToken';
    final resp = await http.get(apiUri, headers: headers);
    if (resp.statusCode != 200) {
      throw HttpException('GitHub API ${resp.statusCode} for $apiUri');
    }
    final release = jsonDecode(resp.body) as Map<String, dynamic>;
    final assets = (release['assets'] as List).cast<Map<String, dynamic>>();
    if (assets.isEmpty) {
      throw const FormatException('Release has no assets');
    }

    String? pick(String ext) => assets
        .firstWhere(
          (a) => (a['name'] as String).toLowerCase().endsWith(ext),
          orElse: () => const {},
        )['browser_download_url'] as String?;

    final url = pick('.sfu') ?? pick('.bin') ?? pick('.hex');
    if (url == null) {
      throw const FormatException('No .sfu/.bin/.hex asset in release');
    }
    return url;
  }

  FirmwareImage _decode(String path, Uint8List bytes) {
    final lower = path.toLowerCase();
    if (lower.endsWith('.sfu')) {
      return FirmwareImage.parseSfu(bytes);
    }
    if (lower.endsWith('.hex')) {
      return FirmwareImage.unsigned(parseIntelHex(bytes));
    }
    // .bin or unknown — treat as raw payload.
    return FirmwareImage.unsigned(bytes);
  }
}

/// Parse an Intel HEX file into a contiguous binary starting at the lowest
/// address. Gaps are zero-filled. Handles record types 00/01/02/04.
Uint8List parseIntelHex(Uint8List raw) {
  final text = ascii.decode(raw, allowInvalid: true);
  final lines = const LineSplitter().convert(text);

  final segments = <int, List<int>>{}; // addr -> byte (sparse)
  int upper = 0; // upper 16 bits (ext linear/segment address)
  int? minAddr;
  int? maxAddr;

  for (var line in lines) {
    line = line.trim();
    if (line.isEmpty || !line.startsWith(':')) continue;
    final bytes = <int>[];
    for (int i = 1; i + 1 < line.length; i += 2) {
      bytes.add(int.parse(line.substring(i, i + 2), radix: 16));
    }
    if (bytes.length < 5) continue;
    final len = bytes[0];
    final addr = (bytes[1] << 8) | bytes[2];
    final type = bytes[3];

    switch (type) {
      case 0x00: // data
        final base = (upper << 16) + addr;
        for (int i = 0; i < len; i++) {
          final a = base + i;
          segments[a] = bytes[4 + i];
          minAddr = (minAddr == null || a < minAddr!) ? a : minAddr;
          maxAddr = (maxAddr == null || a > maxAddr!) ? a : maxAddr;
        }
        break;
      case 0x01: // EOF
        break;
      case 0x02: // extended segment address (<<4)
        upper = ((bytes[4] << 8) | bytes[5]) >> 12;
        break;
      case 0x04: // extended linear address (upper 16 bits)
        upper = (bytes[4] << 8) | bytes[5];
        break;
    }
  }

  if (minAddr == null || maxAddr == null) {
    throw const FormatException('Empty Intel HEX');
  }
  final out = Uint8List(maxAddr! - minAddr! + 1);
  segments.forEach((a, v) => out[a - minAddr!] = v);
  return out;
}

class HttpException implements Exception {
  final String message;
  const HttpException(this.message);
  @override
  String toString() => message;
}
