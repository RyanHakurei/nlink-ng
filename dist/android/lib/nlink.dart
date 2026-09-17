import 'dart:ffi';

import 'package:ffi/ffi.dart';

final class NLinkString extends Struct {
  external Pointer<Utf8> data;
  @Size()
  external int len;
}

final class NLinkImage extends Struct {
  external Pointer<Uint8> rgba;
  @Int32()
  external int width;
  @Int32()
  external int height;
  @Int32()
  external int stride;
}

typedef _OpenAndroidNative = Int32 Function(
  Int32 fd,
  Uint8 epIn,
  Uint8 epOut,
  Uint8 isCx2,
  Pointer<NLinkString> json,
  Pointer<NLinkString> err,
);
typedef _OpenAndroidDart = int Function(
  int fd,
  int epIn,
  int epOut,
  int isCx2,
  Pointer<NLinkString> json,
  Pointer<NLinkString> err,
);

typedef _StrFnNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<NLinkString> json,
  Pointer<NLinkString> err,
);
typedef _StrFnDart = int Function(
  int bus,
  int addr,
  Pointer<NLinkString> json,
  Pointer<NLinkString> err,
);

typedef _PathFnNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<Utf8> path,
  Pointer<NLinkString> json,
  Pointer<NLinkString> err,
);
typedef _PathFnDart = int Function(
  int bus,
  int addr,
  Pointer<Utf8> path,
  Pointer<NLinkString> json,
  Pointer<NLinkString> err,
);

typedef _MutPathNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<Utf8> path,
  Pointer<NLinkString> err,
);
typedef _MutPathDart = int Function(
  int bus,
  int addr,
  Pointer<Utf8> path,
  Pointer<NLinkString> err,
);

typedef _DlDirNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<Utf8> remote,
  Pointer<Utf8> dest,
  Pointer<Void> cb,
  Pointer<Void> user,
  Pointer<NLinkString> err,
);
typedef _DlDirDart = int Function(
  int bus,
  int addr,
  Pointer<Utf8> remote,
  Pointer<Utf8> dest,
  Pointer<Void> cb,
  Pointer<Void> user,
  Pointer<NLinkString> err,
);

typedef _ErrFnNative = Int32 Function(Uint8 bus, Uint8 addr, Pointer<NLinkString> err);
typedef _ErrFnDart = int Function(int bus, int addr, Pointer<NLinkString> err);

typedef _TwoPathNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<Utf8> a,
  Pointer<Utf8> b,
  Pointer<NLinkString> err,
);
typedef _TwoPathDart = int Function(
  int bus,
  int addr,
  Pointer<Utf8> a,
  Pointer<Utf8> b,
  Pointer<NLinkString> err,
);

typedef _DlNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<Utf8> remote,
  Uint64 size,
  Pointer<Utf8> dest,
  Pointer<Void> cb,
  Pointer<Void> user,
  Pointer<NLinkString> err,
);
typedef _DlDart = int Function(
  int bus,
  int addr,
  Pointer<Utf8> remote,
  int size,
  Pointer<Utf8> dest,
  Pointer<Void> cb,
  Pointer<Void> user,
  Pointer<NLinkString> err,
);

typedef _UlNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<Utf8> dest,
  Pointer<Utf8> src,
  Pointer<Void> cb,
  Pointer<Void> user,
  Pointer<NLinkString> err,
);
typedef _UlDart = int Function(
  int bus,
  int addr,
  Pointer<Utf8> dest,
  Pointer<Utf8> src,
  Pointer<Void> cb,
  Pointer<Void> user,
  Pointer<NLinkString> err,
);

typedef _ShotNative = Int32 Function(
  Uint8 bus,
  Uint8 addr,
  Pointer<NLinkImage> img,
  Pointer<NLinkString> err,
);
typedef _ShotDart = int Function(
  int bus,
  int addr,
  Pointer<NLinkImage> img,
  Pointer<NLinkString> err,
);

typedef _ProgressGetNative = Void Function(Pointer<Uint64> done, Pointer<Uint64> total);
typedef _ProgressGetDart = void Function(Pointer<Uint64> done, Pointer<Uint64> total);

class Nlink {
  Nlink._(DynamicLibrary lib)
      : _openAndroid = lib.lookupFunction<_OpenAndroidNative, _OpenAndroidDart>('nlink_open_android'),
        _close = lib.lookupFunction<_ErrFnNative, _ErrFnDart>('nlink_close'),
        _info = lib.lookupFunction<_StrFnNative, _StrFnDart>('nlink_info'),
        _listDir = lib.lookupFunction<_PathFnNative, _PathFnDart>('nlink_list_dir'),
        _mkdir = lib.lookupFunction<_MutPathNative, _MutPathDart>('nlink_mkdir'),
        _rm = lib.lookupFunction<_MutPathNative, _MutPathDart>('nlink_rm'),
        _rmdir = lib.lookupFunction<_MutPathNative, _MutPathDart>('nlink_rmdir'),
        _move = lib.lookupFunction<_TwoPathNative, _TwoPathDart>('nlink_move'),
        _download = lib.lookupFunction<_DlNative, _DlDart>('nlink_download_file'),
        _downloadDir = lib.lookupFunction<_DlDirNative, _DlDirDart>('nlink_download_dir'),
        _upload = lib.lookupFunction<_UlNative, _UlDart>('nlink_upload_file'),
        _screenshot = lib.lookupFunction<_ShotNative, _ShotDart>('nlink_screenshot'),
        _exitExam = lib.lookupFunction<_ErrFnNative, _ErrFnDart>('nlink_exit_exam_mode'),
        _progressGet = lib.lookupFunction<_ProgressGetNative, _ProgressGetDart>('nlink_progress_get'),
        _freeString = lib.lookupFunction<Void Function(NLinkString), void Function(NLinkString)>(
          'nlink_string_free',
        ),
        _freeImage = lib.lookupFunction<Void Function(NLinkImage), void Function(NLinkImage)>(
          'nlink_image_free',
        );

  factory Nlink.load() {
    return Nlink._(DynamicLibrary.open('libnlink.so'));
  }

  final _OpenAndroidDart _openAndroid;
  final _ErrFnDart _close;
  final _StrFnDart _info;
  final _PathFnDart _listDir;
  final _MutPathDart _mkdir;
  final _MutPathDart _rm;
  final _MutPathDart _rmdir;
  final _TwoPathDart _move;
  final _DlDart _download;
  final _DlDirDart _downloadDir;
  final _UlDart _upload;
  final _ShotDart _screenshot;
  final _ErrFnDart _exitExam;
  final _ProgressGetDart _progressGet;
  final void Function(NLinkString) _freeString;
  final void Function(NLinkImage) _freeImage;

  ({int done, int total}) progressGet() {
    final done = calloc<Uint64>();
    final total = calloc<Uint64>();
    try {
      _progressGet(done, total);
      return (done: done.value, total: total.value);
    } finally {
      calloc.free(done);
      calloc.free(total);
    }
  }

  String _take(Pointer<NLinkString> p) {
    final s = p.ref;
    if (s.data == nullptr) return '';
    final text = s.data.toDartString();
    _freeString(s);
    return text;
  }

  void _check(int rc, Pointer<NLinkString> err) {
    final msg = _take(err);
    if (rc != 0) {
      throw NlinkException(msg.isEmpty ? 'nlink error $rc' : msg);
    }
  }

  String openAndroid({
    required int fd,
    required int epIn,
    required int epOut,
    required bool isCx2,
  }) {
    final json = calloc<NLinkString>();
    final err = calloc<NLinkString>();
    try {
      final rc = _openAndroid(fd, epIn, epOut, isCx2 ? 1 : 0, json, err);
      _check(rc, err);
      return _take(json);
    } finally {
      calloc.free(json);
      calloc.free(err);
    }
  }

  void close() {
    final err = calloc<NLinkString>();
    try {
      _check(_close(0, 0, err), err);
    } finally {
      calloc.free(err);
    }
  }

  String info() {
    final json = calloc<NLinkString>();
    final err = calloc<NLinkString>();
    try {
      _check(_info(0, 0, json, err), err);
      return _take(json);
    } finally {
      calloc.free(json);
      calloc.free(err);
    }
  }

  String listDir(String path) {
    final json = calloc<NLinkString>();
    final err = calloc<NLinkString>();
    final cpath = path.toNativeUtf8();
    try {
      _check(_listDir(0, 0, cpath, json, err), err);
      return _take(json);
    } finally {
      calloc.free(json);
      calloc.free(err);
      malloc.free(cpath);
    }
  }

  void mkdir(String path) => _mutPath(_mkdir, path);
  void rm(String path) => _mutPath(_rm, path);
  void rmdir(String path) => _mutPath(_rmdir, path);

  void _mutPath(_MutPathDart fn, String path) {
    final err = calloc<NLinkString>();
    final cpath = path.toNativeUtf8();
    try {
      _check(fn(0, 0, cpath, err), err);
    } finally {
      calloc.free(err);
      malloc.free(cpath);
    }
  }

  void move(String src, String dest) {
    final err = calloc<NLinkString>();
    final csrc = src.toNativeUtf8();
    final cdst = dest.toNativeUtf8();
    try {
      _check(_move(0, 0, csrc, cdst, err), err);
    } finally {
      calloc.free(err);
      malloc.free(csrc);
      malloc.free(cdst);
    }
  }

  void downloadFile(String remote, int size, String destDir) {
    final err = calloc<NLinkString>();
    final cremote = remote.toNativeUtf8();
    final cdest = destDir.toNativeUtf8();
    try {
      _check(
        _download(0, 0, cremote, size, cdest, nullptr, nullptr, err),
        err,
      );
    } finally {
      calloc.free(err);
      malloc.free(cremote);
      malloc.free(cdest);
    }
  }

  void downloadDir(String remote, String destDir) {
    final err = calloc<NLinkString>();
    final cremote = remote.toNativeUtf8();
    final cdest = destDir.toNativeUtf8();
    try {
      _check(
        _downloadDir(0, 0, cremote, cdest, nullptr, nullptr, err),
        err,
      );
    } finally {
      calloc.free(err);
      malloc.free(cremote);
      malloc.free(cdest);
    }
  }

  void uploadFile(String destDir, String src) {
    final err = calloc<NLinkString>();
    final cdest = destDir.toNativeUtf8();
    final csrc = src.toNativeUtf8();
    try {
      _check(_upload(0, 0, cdest, csrc, nullptr, nullptr, err), err);
    } finally {
      calloc.free(err);
      malloc.free(cdest);
      malloc.free(csrc);
    }
  }

  void exitExam() {
    final err = calloc<NLinkString>();
    try {
      _check(_exitExam(0, 0, err), err);
    } finally {
      calloc.free(err);
    }
  }

  ({int width, int height, List<int> rgba}) screenshot() {
    final img = calloc<NLinkImage>();
    final err = calloc<NLinkString>();
    try {
      _check(_screenshot(0, 0, img, err), err);
      final w = img.ref.width;
      final h = img.ref.height;
      final stride = img.ref.stride;
      final bytes = img.ref.rgba.asTypedList(stride * h).toList();
      _freeImage(img.ref);
      return (width: w, height: h, rgba: bytes);
    } finally {
      calloc.free(img);
      calloc.free(err);
    }
  }
}

class NlinkException implements Exception {
  NlinkException(this.message);
  final String message;
  @override
  String toString() => message;
}
