import 'dart:async';
import 'dart:convert';
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
  String _status = 'Connect a TI-Nspire over USB-OTG.';
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
              ? 'No TI-Nspire on USB.'
              : 'Found $n Nspire. Grant USB permission, then Connect.';
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
          isCx2: raw['isCx2'] as bool? ?? true,
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
        _status = '${_info?['name'] ?? 'Nspire'}  ·  $_path  ·  ${list.length} items';
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
          await _usb.invokeMethod('copyToUri', {
            'src': '$dest/${f.name}',
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
                  case 'exam':
                    _exitExam();
                  case 'disc':
                    _disconnect();
                }
              },
              itemBuilder: (ctx) => const [
                PopupMenuItem(value: 'shot', child: Text('Screenshot')),
                PopupMenuItem(value: 'exam', child: Text('Exit exam mode')),
                PopupMenuItem(value: 'disc', child: Text('Disconnect')),
              ],
            ),
        ],
      ),
      floatingActionButton: _connected
          ? Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                FloatingActionButton.small(
                  heroTag: 'mkdir',
                  onPressed: _busy ? null : _mkdir,
                  tooltip: 'New folder',
                  child: const Icon(Icons.create_new_folder),
                ),
                const SizedBox(height: 8),
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
            padding: const EdgeInsets.all(16),
            child: Text(_status),
          ),
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


