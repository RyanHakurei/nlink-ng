import 'dart:isolate';
import 'dart:typed_data';

import 'nlink.dart';

/// Runs all libnlink FFI on a background isolate so USB I/O cannot freeze the UI
/// (alerts, IPAWS, etc.).
class NlinkWorker {
  NlinkWorker._(this._commands);
  final SendPort _commands;

  static Future<NlinkWorker> spawn() async {
    final ready = ReceivePort();
    await Isolate.spawn(_isolateMain, ready.sendPort, debugName: 'nlink-usb');
    final commands = await ready.first as SendPort;
    return NlinkWorker._(commands);
  }

  Future<dynamic> _rpc(String op, [Map<String, dynamic>? args]) async {
    final reply = ReceivePort();
    _commands.send({
      'op': op,
      'args': args ?? const <String, dynamic>{},
      'reply': reply.sendPort,
    });
    final result = await reply.first;
    if (result is Map && result['error'] != null) {
      throw NlinkException(result['error'] as String);
    }
    return (result as Map)['ok'];
  }

  Future<String> openAndroid({
    required int fd,
    required int epIn,
    required int epOut,
    required int productId,
  }) async {
    return await _rpc('open', {
          'fd': fd,
          'epIn': epIn,
          'epOut': epOut,
          'productId': productId,
        })
        as String;
  }

  Future<void> close() async {
    await _rpc('close');
  }

  Future<String> info() async => await _rpc('info') as String;

  Future<String> listDir(String path) async =>
      await _rpc('listDir', {'path': path}) as String;

  Future<void> mkdir(String path) async {
    await _rpc('mkdir', {'path': path});
  }

  Future<void> rm(String path) async {
    await _rpc('rm', {'path': path});
  }

  Future<void> rmdir(String path) async {
    await _rpc('rmdir', {'path': path});
  }

  Future<void> move(String src, String dest) async {
    await _rpc('move', {'src': src, 'dest': dest});
  }

  Future<void> downloadFile(String remote, int size, String destDir) async {
    await _rpc('downloadFile', {
      'remote': remote,
      'size': size,
      'destDir': destDir,
    });
  }

  Future<void> downloadDir(String remote, String destDir) async {
    await _rpc('downloadDir', {'remote': remote, 'destDir': destDir});
  }

  Future<void> uploadFile(String destDir, String src) async {
    await _rpc('uploadFile', {'destDir': destDir, 'src': src});
  }

  Future<void> backup(String dest) async {
    await _rpc('backup', {'dest': dest});
  }

  Future<void> romDump({
    required int fd,
    required int epIn,
    required int epOut,
    required String dest,
  }) async {
    await _rpc('romDump', {'fd': fd, 'epIn': epIn, 'epOut': epOut, 'dest': dest});
  }

  Future<void> exitExam() async {
    await _rpc('exitExam');
  }

  Future<({int width, int height, Uint8List rgba})> screenshot() async {
    final map = await _rpc('screenshot') as Map;
    return (
      width: map['width'] as int,
      height: map['height'] as int,
      rgba: map['rgba'] as Uint8List,
    );
  }

  Future<({int width, int height, Uint8List rgba})> viewFrame() async {
    final map = await _rpc('viewFrame') as Map;
    return (
      width: map['width'] as int,
      height: map['height'] as int,
      rgba: map['rgba'] as Uint8List,
    );
  }
}

void _isolateMain(SendPort ready) {
  final nlink = Nlink.load();
  final inbox = ReceivePort();
  ready.send(inbox.sendPort);
  inbox.listen((raw) {
    final msg = Map<String, dynamic>.from(raw as Map);
    final reply = msg['reply'] as SendPort;
    final op = msg['op'] as String;
    final args = Map<String, dynamic>.from(msg['args'] as Map);
    try {
      Object? ok;
      switch (op) {
        case 'open':
          ok = nlink.openAndroid(
            fd: args['fd'] as int,
            epIn: args['epIn'] as int,
            epOut: args['epOut'] as int,
            productId: args['productId'] as int,
          );
        case 'close':
          nlink.close();
          ok = true;
        case 'info':
          ok = nlink.info();
        case 'listDir':
          ok = nlink.listDir(args['path'] as String);
        case 'mkdir':
          nlink.mkdir(args['path'] as String);
          ok = true;
        case 'rm':
          nlink.rm(args['path'] as String);
          ok = true;
        case 'rmdir':
          nlink.rmdir(args['path'] as String);
          ok = true;
        case 'move':
          nlink.move(args['src'] as String, args['dest'] as String);
          ok = true;
        case 'downloadFile':
          nlink.downloadFile(
            args['remote'] as String,
            args['size'] as int,
            args['destDir'] as String,
          );
          ok = true;
        case 'downloadDir':
          nlink.downloadDir(args['remote'] as String, args['destDir'] as String);
          ok = true;
        case 'uploadFile':
          nlink.uploadFile(args['destDir'] as String, args['src'] as String);
          ok = true;
        case 'exitExam':
          nlink.exitExam();
          ok = true;
        case 'backup':
          nlink.backup(args['dest'] as String);
          ok = true;
        case 'romDump':
          nlink.romDump(
            fd: args['fd'] as int,
            epIn: args['epIn'] as int,
            epOut: args['epOut'] as int,
            dest: args['dest'] as String,
          );
          ok = true;
        case 'screenshot':
          final shot = nlink.screenshot();
          ok = {
            'width': shot.width,
            'height': shot.height,
            'rgba': Uint8List.fromList(shot.rgba),
          };
        case 'viewFrame':
          final frame = nlink.viewFrame();
          ok = {
            'width': frame.width,
            'height': frame.height,
            'rgba': Uint8List.fromList(frame.rgba),
          };
        default:
          throw NlinkException('unknown op $op');
      }
      reply.send({'ok': ok});
    } catch (e) {
      reply.send({'error': e.toString()});
    }
  });
}
