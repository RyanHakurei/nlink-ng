import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:dynamic_color/dynamic_color.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'nlink.dart';
import 'nlink_worker.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const NlinkNgApp());
}

class NlinkNgApp extends StatelessWidget {
  const NlinkNgApp({super.key});

  @override
  Widget build(BuildContext context) {
    return DynamicColorBuilder(
      builder: (lightDynamic, darkDynamic) {
        final light = lightDynamic ??
            ColorScheme.fromSeed(seedColor: const Color(0xFF5B2C91));
        final dark = darkDynamic ??
            ColorScheme.fromSeed(
              seedColor: const Color(0xFF5B2C91),
              brightness: Brightness.dark,
            );
        return MaterialApp(
          title: 'nlink-ng',
          theme: ThemeData(colorScheme: light, useMaterial3: true),
          darkTheme: ThemeData(colorScheme: dark, useMaterial3: true),
          home: const HomePage(),
        );
      },
    );
  }
}

class UsbDeviceInfo {
  UsbDeviceInfo({
    required this.deviceId,
    required this.vendorId,
    required this.productId,
    required this.name,
    required this.isNspire,
    required this.model,
    required this.hasPermission,
  });

  final int deviceId;
  final int vendorId;
  final int productId;
  final String name;
  final bool isNspire;
  final String model;
  final bool hasPermission;

  factory UsbDeviceInfo.fromMap(Map<dynamic, dynamic> map) {
    return UsbDeviceInfo(
      deviceId: map['deviceId'] as int,
      vendorId: map['vendorId'] as int,
      productId: map['productId'] as int,
      name: map['name'] as String? ?? 'USB device',
      isNspire: map['isNspire'] as bool? ?? false,
      model: map['model'] as String? ?? '',
      hasPermission: map['hasPermission'] as bool? ?? false,
    );
  }

  String get label =>
      (isNspire && model.isNotEmpty) ? model : name;
}

class CalcFile {
  CalcFile({
    required this.name,
    required this.isDir,
    required this.size,
  });
  final String name;
  final bool isDir;
  final int size;
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});
  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with WidgetsBindingObserver {
  static const _usb = MethodChannel('nlink/usb');
  NlinkWorker? _nlink;
  List<UsbDeviceInfo> _devices = [];
  Map<String, dynamic>? _info;
  String _path = '/';
  List<CalcFile> _files = [];
  String _status = 'Connect a TI calculator over USB.';
  bool _busy = false;
  bool _connected = false;
  double? _transferProgress;
  Timer? _progressTimer;
  Nlink? _progressLib;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _usb.setMethodCallHandler((call) async {
      if (call.method == 'usbDetached' && _connected) {
        await _disconnect(userMessage: 'Calculator disconnected.');
      }
    });
    () async {
      try {
        _nlink = await NlinkWorker.spawn();
      } catch (e) {
        if (mounted) setState(() => _status = 'Native library missing: $e');
      }
      await _scan();
    }();
  }

  @override
  void dispose() {
    _progressTimer?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed && _connected && !_busy) {
      _reload();
    }
  }

  Future<void> _scan() async {
    setState(() => _busy = true);
    try {
      final raw = await _usb.invokeMethod<List<dynamic>>('listDevices');
      setState(() {
        _devices = (raw ?? [])
            .whereType<Map<dynamic, dynamic>>()
            .map(UsbDeviceInfo.fromMap)
            .toList();
        if (!_connected) {
          final n = _devices.where((d) => d.isNspire).length;
          _status = n == 0
              ? 'No TI calculator on USB.'
              : 'Found $n calculator${n == 1 ? '' : 's'}. Grant USB permission, then Connect.';
        }
      });
    } catch (e) {
      setState(() => _status = 'USB scan failed: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<void> _permit(UsbDeviceInfo d) async {
    try {
      await _usb.invokeMethod('requestPermission', {'deviceId': d.deviceId});
      await _scan();
    } catch (e) {
      _snack('$e');
    }
  }

  Future<void> _connect(UsbDeviceInfo d) async {
    if (_nlink == null) return;
    if (!d.hasPermission) {
      await _permit(d);
      await _scan();
    }
    setState(() {
      _busy = true;
      _status = 'Opening calculator…';
    });
    Object? lastError;
    for (var attempt = 1; attempt <= 4; attempt++) {
      try {
        if (attempt > 1) {
          setState(() => _status = 'Opening calculator… (try $attempt/4)');
          await Future<void>.delayed(Duration(milliseconds: 350 * attempt));
        }
        final raw = await _usb.invokeMethod<Map<dynamic, dynamic>>('open', {
          'deviceId': d.deviceId,
        });
        if (raw == null) throw Exception('open returned null');
        final json = await _nlink!.openAndroid(
          fd: raw['fd'] as int,
          epIn: raw['epIn'] as int,
          epOut: raw['epOut'] as int,
          productId: raw['productId'] as int? ?? d.productId,
        );
        if (!mounted) return;
        setState(() {
          _connected = true;
          _info = jsonDecode(json) as Map<String, dynamic>;
          _path = '/';
        });
        await _reload();
        return;
      } catch (e) {
        lastError = e;
        try {
          await _nlink?.close();
        } catch (_) {}
        try {
          await _usb.invokeMethod('close');
        } catch (_) {}
      }
    }
    if (mounted) setState(() => _status = 'Connect failed: $lastError');
    setState(() => _busy = false);
  }

  Future<void> _disconnect({String userMessage = 'Disconnected.'}) async {
    try {
      await _nlink?.close();
    } catch (_) {}
    try {
      await _usb.invokeMethod('close');
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _connected = false;
      _files = [];
      _info = null;
      _path = '/';
      _status = userMessage;
      _busy = false;
    });
    await _scan();
  }

  String _join(String parent, String name) {
    if (parent == '/' || parent.isEmpty) return '/$name';
    return '$parent/$name';
  }

  Future<void> _reload() async {
    if (_nlink == null || !_connected) return;
    setState(() => _busy = true);
    try {
      final raw = await _nlink!.listDir(_path);
      final list = (jsonDecode(raw) as List<dynamic>)
          .map((e) {
            final m = e as Map<String, dynamic>;
            return CalcFile(
              name: m['path'] as String? ?? '',
              isDir: m['isDir'] as bool? ?? false,
              size: (m['size'] as num?)?.toInt() ?? 0,
            );
          })
          .where((f) => f.name.isNotEmpty && f.name != 'NspireLogs.zip')
          .toList()
        ..sort((a, b) {
          if (a.isDir != b.isDir) return a.isDir ? -1 : 1;
          return a.name.toLowerCase().compareTo(b.name.toLowerCase());
        });
      setState(() {
        _files = list;
        final note = _nspire ? '' : '  ·  not verified on hardware';
        _status = '${_info?['name'] ?? 'Calculator'}  ·  $_path  ·  ${list.length} items$note';
      });
    } catch (e) {
      setState(() => _status = 'List failed: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<void> _openItem(CalcFile f) async {
    if (f.isDir) {
      setState(() => _path = _join(_path, f.name));
      await _reload();
    }
  }

  Future<void> _up() async {
    if (_path == '/' || _path.isEmpty) return;
    final parts = _path.split('/').where((s) => s.isNotEmpty).toList();
    parts.removeLast();
    setState(() => _path = parts.isEmpty ? '/' : '/${parts.join('/')}');
    await _reload();
  }

  void _snack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
  }

  bool get _nspire => (_info?['family'] as String? ?? 'nspire') == 'nspire';

  bool get _silverlink => (_info?['family'] as String? ?? '') == 'silverlink';

  bool get _z80Backup {
    final name = (_info?['name'] as String? ?? '').toLowerCase();
    final family = _info?['family'] as String? ?? '';
    return family == 'silverlink' || (family == 'dusb' && !name.contains('ce'));
  }

  bool get _canRomDump {
    final name = (_info?['name'] as String? ?? '').toLowerCase();
    final family = _info?['family'] as String? ?? '';
    return family == 'dusb' && name.contains('84') && !name.contains('ce') && !name.contains('evo');
  }

  Widget _memoryPanel() {
    final info = _info;
    if (info == null) return const SizedBox.shrink();
    final ramTotal = (info['total_ram'] as num?)?.toDouble() ?? 0;
    final ramFree = (info['free_ram'] as num?)?.toDouble() ?? 0;
    final flashTotal = (info['total_storage'] as num?)?.toDouble() ?? 0;
    final flashFree = (info['free_storage'] as num?)?.toDouble() ?? 0;
    final note = info['ram_note'] as String?;
    final clock = info['clock'] as String?;
    final battery = info['battery'] as String?;
    if (ramTotal <= 0 && flashTotal <= 0 && note == null && clock == null && battery == null) {
      return const SizedBox.shrink();
    }
    final ramUsed = (ramTotal - ramFree).clamp(0, ramTotal);
    final flashUsed = (flashTotal - flashFree).clamp(0, flashTotal);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (battery != null || clock != null)
            Text([
              if (battery != null) 'Battery $battery',
              if (clock != null) 'Clock $clock',
            ].join('  ·  ')),
          if (note != null || ramTotal > 0) ...[
            const SizedBox(height: 8),
            Text(note ?? 'RAM ${_fmtBytes(ramUsed.round())} / ${_fmtBytes(ramTotal.round())}'),
            const SizedBox(height: 4),
            LinearProgressIndicator(
              value: note != null || ramTotal <= 0 ? 0 : (ramUsed / ramTotal).clamp(0.0, 1.0),
            ),
          ],
          if (flashTotal > 0) ...[
            const SizedBox(height: 8),
            Text('Archive ${_fmtBytes(flashUsed.round())} / ${_fmtBytes(flashTotal.round())}'),
            const SizedBox(height: 4),
            LinearProgressIndicator(value: (flashUsed / flashTotal).clamp(0.0, 1.0)),
          ],
        ],
      ),
    );
  }

  Future<void> _backupRam() async {
    final name = _silverlink ? 'backup.8xb' : 'backup.8xg';
    final uri = await _usb.invokeMethod<String>('pickSaveFile', {'name': name});
    if (uri == null || uri.isEmpty) return;
    final cache = await _usb.invokeMethod<String>('cacheDir');
    if (cache == null || cache.isEmpty) return;
    final dest = '$cache/${DateTime.now().millisecondsSinceEpoch}_$name';
    try {
      await _withTransfer('Backing up RAM', () async {
        await _nlink!.backup(dest);
        await _usb.invokeMethod('copyToUri', {'src': dest, 'uri': uri});
      });
      _snack('RAM backup saved');
    } catch (e) {
      _snack('$e');
    }
  }

  Future<void> _romDump() async {
    final go = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('ROM dump'),
        content: const Text(
          'Run the USB ROM dumper on the TI-84 Plus or Silver Edition first. '
          'The screen should say Dumping. This does not work on the CE or the Evo.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Continue')),
        ],
      ),
    );
    if (go != true) return;
    final uri = await _usb.invokeMethod<String>('pickSaveFile', {'name': 'ti84.rom'});
    if (uri == null || uri.isEmpty) return;
    final link = await _usb.invokeMethod<Map<dynamic, dynamic>>('currentLink');
    final cache = await _usb.invokeMethod<String>('cacheDir');
    if (link == null || cache == null) {
      _snack('Connect the calculator first');
      return;
    }
    final dest = '$cache/${DateTime.now().millisecondsSinceEpoch}_ti84.rom';
    try {
      await _withTransfer('Dumping ROM', () async {
        await _nlink!.romDump(
          fd: link['fd'] as int,
          epIn: link['epIn'] as int,
          epOut: link['epOut'] as int,
          dest: dest,
        );
        await _usb.invokeMethod('copyToUri', {'src': dest, 'uri': uri});
      });
      await _disconnect(userMessage: 'ROM dump saved. Restart the calculator, then connect again.');
    } catch (e) {
      _snack('$e');
      await _disconnect(userMessage: 'ROM dump failed. Restart the calculator, then connect again.');
    }
  }

  String _fmtBytes(int n) {
    if (n < 1024) return '$n B';
    if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KB';
    return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
  }

  Future<T> _withTransfer<T>(String label, Future<T> Function() op) async {
    _progressLib ??= Nlink.load();
    _progressTimer?.cancel();
    setState(() {
      _busy = true;
      _transferProgress = 0;
      _status = label;
    });
    _progressTimer = Timer.periodic(const Duration(milliseconds: 120), (_) {
      final p = _progressLib!.progressGet();
      if (!mounted) return;
      setState(() {
        if (p.total > 0) {
          _transferProgress = (p.done / p.total).clamp(0.0, 1.0);
          final pct = ((_transferProgress ?? 0) * 100).round();
          _status = '$label  ·  ${_fmtBytes(p.done)} / ${_fmtBytes(p.total)}  ($pct%)';
        }
      });
    });
    try {
      return await op();
    } finally {
      _progressTimer?.cancel();
      _progressTimer = null;
      if (mounted) {
        setState(() {
          _transferProgress = null;
          _busy = false;
        });
      }
    }
  }

  Future<void> _mkdir() async {
    final name = await _prompt('New folder', 'Name');
    if (name == null || name.isEmpty) return;
    try {
      await _nlink!.mkdir(_join(_path, name));
      await _reload();
    } catch (e) {
      _snack('$e');
    }
  }

  Future<void> _upload() async {
    try {
      final raw = await _usb.invokeMethod<List<dynamic>>('pickFiles');
      final paths = (raw ?? []).whereType<String>().toList();
      if (paths.isEmpty) return;
      for (var i = 0; i < paths.length; i++) {
        final path = paths[i];
        final name = path.split('/').last;
        final label = paths.length == 1
            ? 'Uploading $name'
            : 'Uploading $name (${i + 1}/${paths.length})';
        await _withTransfer(label, () => _nlink!.uploadFile(_path, path));
      }
      await _reload();
      _snack(paths.length == 1 ? 'Upload complete' : 'Uploaded ${paths.length} files');
    } catch (e) {
      _snack('$e');
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _download(CalcFile f) async {
    final remote = _join(_path, f.name);
    final cacheRoot = await _usb.invokeMethod<String>('cacheDir');
    if (cacheRoot == null || cacheRoot.isEmpty) {
      _snack('No cache directory');
      return;
    }
    final dest = '$cacheRoot/dl_${DateTime.now().millisecondsSinceEpoch}';
    try {
      if (f.isDir) {
        final tree = await _usb.invokeMethod<String>('pickSaveTree');
        if (tree == null || tree.isEmpty) return;
        await _withTransfer('Downloading ${f.name}', () async {
          await _nlink!.downloadDir(remote, dest);
          await _usb.invokeMethod('copyDirToTree', {
            'src': dest,
            'uri': tree,
            'name': f.name,
          });
        });
        _snack('Saved folder ${f.name}');
        await _reload();
      } else {
        final uri = await _usb.invokeMethod<String>('pickSaveFile', {'name': f.name});
        if (uri == null || uri.isEmpty) return;
        await _withTransfer('Downloading ${f.name}', () async {
          await _nlink!.downloadFile(remote, f.size, dest);
          final saved = Directory(dest)
              .listSync()
              .whereType<File>()
              .toList();
          if (saved.isEmpty) {
            throw Exception('Download produced no file');
          }
          await _usb.invokeMethod('copyToUri', {
            'src': saved.first.path,
            'uri': uri,
          });
        });
        _snack('Saved ${f.name}');
        await _reload();
      }
    } catch (e) {
      _snack('$e');
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _rename(CalcFile f) async {
    final name = await _prompt('Rename', 'New name', f.name);
    if (name == null || name.isEmpty || name == f.name) return;
    try {
      await _nlink!.move(_join(_path, f.name), _join(_path, name));
      await _reload();
    } catch (e) {
      _snack('$e');
    }
  }

  Future<void> _delete(CalcFile f) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Delete'),
        content: Text('Delete ${f.name}?'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Delete')),
        ],
      ),
    );
    if (ok != true) return;
    try {
      final p = _join(_path, f.name);
      if (f.isDir) {
        await _nlink!.rmdir(p);
      } else {
        await _nlink!.rm(p);
      }
      await _reload();
    } catch (e) {
      _snack('$e');
    }
  }

  Future<void> _liveView() async {
    final worker = _nlink;
    if (worker == null) return;
    setState(() => _busy = true);
    try {
      await showDialog<void>(
        context: context,
        builder: (ctx) => _LiveViewDialog(nlink: worker),
      );
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _screenshot() async {
    setState(() => _busy = true);
    try {
      final shot = await _nlink!.screenshot();
      final bytes = shot.rgba;
      final image = await _decodeRgba(bytes, shot.width, shot.height);
      if (!mounted) return;
      await showDialog<void>(
        context: context,
        builder: (ctx) => AlertDialog(
          title: const Text('Screenshot'),
          content: RawImage(image: image, scale: 0.5),
          actions: [
            TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Close')),
          ],
        ),
      );
      image.dispose();
    } catch (e) {
      _snack('$e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<ui.Image> _decodeRgba(Uint8List rgba, int w, int h) {
    final c = Completer<ui.Image>();
    ui.decodeImageFromPixels(rgba, w, h, ui.PixelFormat.rgba8888, c.complete);
    return c.future;
  }

  Future<void> _exitExam() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Exit exam mode'),
        content: const Text(
          'Uploads Exit Test Mode.tns into Press-to-Test. The calculator usually restarts.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Continue')),
        ],
      ),
    );
    if (ok != true) return;
    try {
      await _nlink!.exitExam();
      _snack('Exam-mode exit sent.');
      await _disconnect();
    } catch (e) {
      _snack('$e');
    }
  }

  Future<String?> _prompt(String title, String label, [String initial = '']) {
    final ctrl = TextEditingController(text: initial);
    return showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(title),
        content: TextField(
          controller: ctrl,
          decoration: InputDecoration(labelText: label),
          autofocus: true,
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, ctrl.text.trim()), child: const Text('OK')),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(_connected ? (_info?['name'] as String? ?? 'nlink-ng') : 'nlink-ng'),
        actions: [
          if (_connected)
            IconButton(onPressed: _busy ? null : _up, icon: const Icon(Icons.arrow_upward), tooltip: 'Up'),
          IconButton(onPressed: _busy ? null : (_connected ? _reload : _scan), icon: const Icon(Icons.refresh)),
          if (_connected)
            PopupMenuButton<String>(
              onSelected: (v) {
                switch (v) {
                  case 'shot':
                    _screenshot();
                  case 'live':
                    _liveView();
                  case 'backup':
                    _backupRam();
                  case 'rom':
                    _romDump();
                  case 'exam':
                    _exitExam();
                  case 'disc':
                    _disconnect();
                }
              },
              itemBuilder: (ctx) => [
                const PopupMenuItem(value: 'shot', child: Text('Screenshot')),
                if (_nspire) const PopupMenuItem(value: 'live', child: Text('Live view')),
                if (_z80Backup) const PopupMenuItem(value: 'backup', child: Text('Backup RAM')),
                if (_canRomDump) const PopupMenuItem(value: 'rom', child: Text('ROM dump')),
                if (_nspire) const PopupMenuItem(value: 'exam', child: Text('Exit exam mode')),
                const PopupMenuItem(value: 'disc', child: Text('Disconnect')),
              ],
            ),
        ],
      ),
      floatingActionButton: _connected
          ? Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (_nspire) ...[
                  FloatingActionButton.small(
                    heroTag: 'mkdir',
                    onPressed: _busy ? null : _mkdir,
                    tooltip: 'New folder',
                    child: const Icon(Icons.create_new_folder),
                  ),
                  const SizedBox(height: 8),
                ],
                FloatingActionButton(
                  heroTag: 'upload',
                  onPressed: _busy ? null : _upload,
                  tooltip: 'Upload',
                  child: const Icon(Icons.upload),
                ),
              ],
            )
          : null,
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (_busy) LinearProgressIndicator(value: _transferProgress),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 8),
            child: Text(_status),
          ),
          if (_connected) _memoryPanel(),
          if (!_connected)
            Expanded(
              child: ListView(
                children: [
                  for (final d in _devices)
                    ListTile(
                      leading: Icon(d.isNspire ? Icons.calculate : Icons.usb),
                      title: Text(d.label),
                      subtitle: Text(
                        '${d.vendorId.toRadixString(16)}:${d.productId.toRadixString(16)}'
                        ' · permission ${d.hasPermission}',
                      ),
                      trailing: d.isNspire
                          ? FilledButton(
                              onPressed: _busy ? null : () => _connect(d),
                              child: const Text('Connect'),
                            )
                          : null,
                    ),
                ],
              ),
            )
          else
            Expanded(
              child: ListView.builder(
                itemCount: _files.length,
                itemBuilder: (ctx, i) {
                  final f = _files[i];
                  return ListTile(
                    leading: Icon(f.isDir ? Icons.folder : Icons.insert_drive_file),
                    title: Text(f.name),
                    subtitle: Text(f.isDir ? 'Folder' : '${f.size} bytes'),
                    onTap: () => _openItem(f),
                    onLongPress: () => showModalBottomSheet<void>(
                      context: context,
                      builder: (ctx) => SafeArea(
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            ListTile(
                              leading: const Icon(Icons.download),
                              title: const Text('Download'),
                              onTap: () {
                                Navigator.pop(ctx);
                                _download(f);
                              },
                            ),
                            ListTile(
                              leading: const Icon(Icons.drive_file_rename_outline),
                              title: const Text('Rename'),
                              onTap: () {
                                Navigator.pop(ctx);
                                _rename(f);
                              },
                            ),
                            ListTile(
                              leading: const Icon(Icons.delete),
                              title: const Text('Delete'),
                              onTap: () {
                                Navigator.pop(ctx);
                                _delete(f);
                              },
                            ),
                          ],
                        ),
                      ),
                    ),
                  );
                },
              ),
            ),
        ],
      ),
    );
  }
}

class _LiveViewDialog extends StatefulWidget {
  const _LiveViewDialog({required this.nlink});

  final NlinkWorker nlink;

  @override
  State<_LiveViewDialog> createState() => _LiveViewDialogState();
}

class _LiveViewDialogState extends State<_LiveViewDialog> {
  ui.Image? _image;
  String _status = 'Waiting for a frame…';
  bool _run = true;

  @override
  void initState() {
    super.initState();
    _pump();
  }

  @override
  void dispose() {
    _run = false;
    _image?.dispose();
    _image = null;
    super.dispose();
  }

  Future<void> _pump() async {
    while (_run && mounted) {
      final started = DateTime.now();
      try {
        final shot = await widget.nlink.screenshot();
        if (!_run || !mounted) return;
        final next = await _decodeFrame(shot.rgba, shot.width, shot.height);
        if (!_run || !mounted) {
          next.dispose();
          return;
        }
        final previous = _image;
        setState(() {
          _image = next;
          _status = 'Receiving';
        });
        previous?.dispose();
      } catch (e) {
        if (!_run || !mounted) return;
        setState(() => _status = '$e');
        await Future<void>.delayed(const Duration(milliseconds: 400));
        continue;
      }
      final spent = DateTime.now().difference(started);
      final rest = const Duration(milliseconds: 300) - spent;
      if (_run && mounted && rest > Duration.zero) {
        await Future<void>.delayed(rest);
      }
    }
  }

  Future<ui.Image> _decodeFrame(Uint8List rgba, int width, int height) {
    final done = Completer<ui.Image>();
    ui.decodeImageFromPixels(rgba, width, height, ui.PixelFormat.rgba8888, done.complete);
    return done.future;
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Live view'),
      content: SizedBox(
        width: 320,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text(
              'Shows the calculator screen over the normal link. The keypad stays usable.',
            ),
            const SizedBox(height: 12),
            if (_image != null)
              RawImage(image: _image, scale: 0.5)
            else
              const SizedBox(width: 160, height: 120),
            const SizedBox(height: 8),
            Text(_status),
          ],
        ),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Close')),
      ],
    );
  }
}


