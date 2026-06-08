import 'package:flutter/material.dart';
import 'package:nfc_manager/nfc_manager.dart';

import 'src/image_source.dart';
import 'src/nfc_flasher.dart';
import 'src/protocol.dart';

void main() => runApp(const NfcFlasherApp());

class NfcFlasherApp extends StatelessWidget {
  const NfcFlasherApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'STICKER NFC Flasher',
      theme: ThemeData(colorSchemeSeed: Colors.deepPurple, useMaterial3: true),
      home: const FlasherPage(),
    );
  }
}

class FlasherPage extends StatefulWidget {
  const FlasherPage({super.key});
  @override
  State<FlasherPage> createState() => _FlasherPageState();
}

class _FlasherPageState extends State<FlasherPage> {
  final _linkCtrl = TextEditingController();
  final _tokenCtrl = TextEditingController();
  bool _allowUnsigned = false;

  FirmwareImage? _image;
  String _status = 'Enter a release link or a .hex/.bin/.sfu URL.';
  double _progress = 0;
  bool _busy = false;

  @override
  void dispose() {
    _linkCtrl.dispose();
    _tokenCtrl.dispose();
    super.dispose();
  }

  Future<void> _download() async {
    setState(() {
      _busy = true;
      _status = 'Downloading…';
      _image = null;
    });
    try {
      final src = ImageSource(
          githubToken: _tokenCtrl.text.trim().isEmpty
              ? null
              : _tokenCtrl.text.trim());
      final img = await src.load(_linkCtrl.text);
      setState(() {
        _image = img;
        _status = 'Ready: ${img.versionString}, '
            '${img.payload.length} B, ${img.totalDataFrames} frames, '
            '${img.signed ? "signed" : "UNSIGNED"}.';
      });
    } catch (e) {
      setState(() => _status = 'Download error: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<void> _flash() async {
    final img = _image;
    if (img == null) return;
    if (!await NfcManager.instance.isAvailable()) {
      setState(() => _status = 'NFC is not available/enabled on this device.');
      return;
    }
    setState(() {
      _busy = true;
      _progress = 0;
      _status = 'Hold the phone against the STICKER…';
    });
    try {
      final flasher = NfcFlasher(
        allowUnsigned: _allowUnsigned,
        onProgress: (p) => setState(() {
          _progress = p.fraction;
          _status = '${p.phase}: ${(p.fraction * 100).toStringAsFixed(0)}%';
        }),
      );
      await flasher.run(img);
      setState(() => _status = '✅ Update complete — device will reboot.');
    } catch (e) {
      setState(() => _status = '❌ $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final canFlash = _image != null && !_busy;
    return Scaffold(
      appBar: AppBar(title: const Text('STICKER NFC Flasher')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: ListView(
          children: [
            TextField(
              controller: _linkCtrl,
              decoration: const InputDecoration(
                labelText: 'Release link or firmware URL',
                hintText: 'https://github.com/hardwario/sticker-firmware/releases/latest',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _tokenCtrl,
              obscureText: true,
              decoration: const InputDecoration(
                labelText: 'GitHub token (private repos, optional)',
                border: OutlineInputBorder(),
              ),
            ),
            SwitchListTile(
              title: const Text('Allow unsigned image (debug only)'),
              value: _allowUnsigned,
              onChanged: _busy ? null : (v) => setState(() => _allowUnsigned = v),
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                Expanded(
                  child: FilledButton.icon(
                    onPressed: _busy ? null : _download,
                    icon: const Icon(Icons.download),
                    label: const Text('Download'),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: FilledButton.icon(
                    onPressed: canFlash ? _flash : null,
                    icon: const Icon(Icons.nfc),
                    label: const Text('Flash via NFC'),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 24),
            if (_busy) LinearProgressIndicator(value: _progress == 0 ? null : _progress),
            const SizedBox(height: 16),
            Text(_status, style: Theme.of(context).textTheme.bodyLarge),
          ],
        ),
      ),
    );
  }
}
