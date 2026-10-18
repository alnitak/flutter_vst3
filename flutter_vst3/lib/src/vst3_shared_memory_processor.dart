import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';

/// Magic constant identifying valid VST3 shared memory ('VST3').
const int _vst3ShmMagic = 0x56535433;

/// Command from host to process an audio block.
const int _vst3CmdProcess = 1;

/// Command from host requesting the DSP process to terminate cleanly.
const int _vst3CmdTerminate = 2;

// Memory byte offsets matching the C++ Vst3SharedMemory struct definition
const int _offsetMagic = 0;
const int _offsetSampleRate = 8;
const int _offsetMaxBlockSize = 16;
const int _offsetCommand = 20;
const int _offsetNumSamples = 24;
const int _offsetHostParamWrite = 28;
const int _offsetHostParamRead = 32;
const int _offsetHostParams = 36;
const int _offsetInputL = 4096;
const int _offsetInputR = 20480;
const int _offsetOutputL = 36864;
const int _offsetOutputR = 53248;
const int _vst3ShmTotalSize = 69632;

// C and Dart function signatures for POSIX shared memory and semaphores
typedef _ShmOpenC = Int32 Function(Pointer<Utf8>, Int32, Uint16);
typedef _ShmOpenDart = int Function(Pointer<Utf8>, int, int);
typedef _MmapC = Pointer<Void> Function(Pointer<Void>, IntPtr, Int32, Int32, Int32, Int64);
typedef _MmapDart = Pointer<Void> Function(Pointer<Void>, int, int, int, int, int);
typedef _MunmapC = Int32 Function(Pointer<Void>, IntPtr);
typedef _MunmapDart = int Function(Pointer<Void>, int);
typedef _CloseC = Int32 Function(Int32);
typedef _CloseDart = int Function(int);
typedef _SemOpenC = Pointer<Void> Function(Pointer<Utf8>, Int32);
typedef _SemOpenDart = Pointer<Void> Function(Pointer<Utf8>, int);
typedef _SemWaitC = Int32 Function(Pointer<Void>);
typedef _SemWaitDart = int Function(Pointer<Void>);
typedef _SemPostC = Int32 Function(Pointer<Void>);
typedef _SemPostDart = int Function(Pointer<Void>);
typedef _SemCloseC = Int32 Function(Pointer<Void>);
typedef _SemCloseDart = int Function(Pointer<Void>);

/// POSIX FFI bindings for inter-process shared memory and semaphores.
///
/// Resolves symbols dynamically from the process image via
/// [DynamicLibrary.process] without external C library dependencies.
final class _PosixNative {
  final _ShmOpenDart shmOpen;
  final _MmapDart mmap;
  final _MunmapDart munmap;
  final _CloseDart close;
  final _SemOpenDart semOpen;
  final _SemWaitDart semWait;
  final _SemPostDart semPost;
  final _SemCloseDart semClose;

  _PosixNative(DynamicLibrary lib)
      : shmOpen = lib.lookupFunction<_ShmOpenC, _ShmOpenDart>('shm_open'),
        mmap = lib.lookupFunction<_MmapC, _MmapDart>('mmap'),
        munmap = lib.lookupFunction<_MunmapC, _MunmapDart>('munmap'),
        close = lib.lookupFunction<_CloseC, _CloseDart>('close'),
        semOpen = lib.lookupFunction<_SemOpenC, _SemOpenDart>('sem_open'),
        semWait = lib.lookupFunction<_SemWaitC, _SemWaitDart>('sem_wait'),
        semPost = lib.lookupFunction<_SemPostC, _SemPostDart>('sem_post'),
        semClose = lib.lookupFunction<_SemCloseC, _SemCloseDart>('sem_close');
}

/// Runs a VST3 audio processor using preallocated zero-copy shared memory.
///
/// This is the main entry point for compiled Dart VST3 DSP executables.
/// It parses the shared memory and semaphore names passed via [args] from
/// the native C++ VST3 wrapper, attaches to the shared memory segment,
/// and executes the real-time DSP event loop with the provided [processor].
///
/// The [processor] object must implement:
/// - `initialize(double sampleRate, int maxBlockSize)`
/// - `setParameter(int paramId, double value)`
/// - `processStereo(List<double> inL, List<double> inR, List<double> outL, List<double> outR)`
/// - `dispose()`
void runVst3SharedMemoryProcessor({
  required List<String> args,
  required dynamic processor,
}) {
  switch (args.length >= 3) {
    case true:
      _startProcessing(args[0], args[1], args[2], processor);
    case false:
      throw ArgumentError(
        'Usage: processor <shm_name> <sem_ready_name> <sem_done_name>',
      );
  }
}

/// Opens the shared memory and semaphore handles, then starts the event loop.
void _startProcessing(
  String shmName,
  String semReadyName,
  String semDoneName,
  dynamic processor,
) {
  final posix = _PosixNative(DynamicLibrary.process());
  final shmNameNative = shmName.toNativeUtf8();
  final semRNameNative = semReadyName.toNativeUtf8();
  final semDNameNative = semDoneName.toNativeUtf8();

  final fd = posix.shmOpen(shmNameNative, 2, 0); // O_RDWR = 2
  final ptr = fd >= 0
      ? posix.mmap(nullptr, _vst3ShmTotalSize, 3, 1, fd, 0)
      : throw StateError('Failed to open shared memory: $shmName');
  final semReady = posix.semOpen(semRNameNative, 0);
  final semDone = posix.semOpen(semDNameNative, 0);

  _cleanupNativeStrings(shmNameNative, semRNameNative, semDNameNative);
  _validateMagic(ptr);
  _runEventLoop(posix, ptr, fd, semReady, semDone, processor);
}

/// Frees temporary native UTF-8 strings allocated for POSIX open calls.
void _cleanupNativeStrings(Pointer<Utf8> a, Pointer<Utf8> b, Pointer<Utf8> c) {
  calloc.free(a);
  calloc.free(b);
  calloc.free(c);
}

/// Verifies that the shared memory block contains the expected VST3 magic word.
void _validateMagic(Pointer<Void> ptr) {
  final magic = (ptr.cast<Uint8>() + _offsetMagic).cast<Uint32>().value;
  switch (magic == _vst3ShmMagic) {
    case true:
      return;
    case false:
      throw StateError('Invalid VST3 shared memory magic: $magic');
  }
}

/// Performs initialization handshake with host, enters DSP loop, and cleans up.
void _runEventLoop(
  _PosixNative posix,
  Pointer<Void> ptr,
  int fd,
  Pointer<Void> semReady,
  Pointer<Void> semDone,
  dynamic processor,
) {
  final bytePtr = ptr.cast<Uint8>();
  final sampleRate = (bytePtr + _offsetSampleRate).cast<Double>().value;
  final maxBlockSize = (bytePtr + _offsetMaxBlockSize).cast<Uint32>().value;
  processor.initialize(sampleRate, maxBlockSize);
  posix.semPost(semDone); // Signal C++ host that initialization is complete

  _processingLoop(posix, bytePtr, semReady, semDone, processor);
  _dispose(posix, ptr, fd, semReady, semDone, processor);
}

/// Core audio processing loop waiting on semReady and signaling semDone.
void _processingLoop(
  _PosixNative posix,
  Pointer<Uint8> bytePtr,
  Pointer<Void> semReady,
  Pointer<Void> semDone,
  dynamic processor,
) {
  var running = true;
  while (running) {
    posix.semWait(semReady);
    final cmd = (bytePtr + _offsetCommand).cast<Uint32>().value;
    switch (cmd) {
      case _vst3CmdProcess:
        _handleProcessBlock(bytePtr, processor);
        posix.semPost(semDone);
      case _vst3CmdTerminate:
        running = false;
      default:
        posix.semPost(semDone);
    }
  }
}

/// Drains parameters from the ring buffer and processes stereo audio in-place.
void _handleProcessBlock(Pointer<Uint8> bytePtr, dynamic processor) {
  _drainParameters(bytePtr, processor);
  final numSamples = (bytePtr + _offsetNumSamples).cast<Int32>().value;
  final inL = (bytePtr + _offsetInputL).cast<Float>().asTypedList(numSamples);
  final inR = (bytePtr + _offsetInputR).cast<Float>().asTypedList(numSamples);
  final outL = (bytePtr + _offsetOutputL).cast<Float>().asTypedList(numSamples);
  final outR = (bytePtr + _offsetOutputR).cast<Float>().asTypedList(numSamples);
  processor.processStereo(inL, inR, outL, outR);
}

/// Drains pending parameter automation events from the host lock-free ring buffer.
void _drainParameters(Pointer<Uint8> bytePtr, dynamic processor) {
  final writeIdx = (bytePtr + _offsetHostParamWrite).cast<Uint32>().value;
  var readIdx = (bytePtr + _offsetHostParamRead).cast<Uint32>().value;

  while (readIdx != writeIdx) {
    final eventOffset = _offsetHostParams + ((readIdx % 64) * 16);
    final paramId = (bytePtr + eventOffset).cast<Int32>().value;
    final value = (bytePtr + eventOffset + 8).cast<Double>().value;
    processor.setParameter(paramId, value);
    readIdx++;
  }
  (bytePtr + _offsetHostParamRead).cast<Uint32>().value = readIdx;
}

/// Cleans up shared memory mappings, closes semaphores, and exits process.
void _dispose(
  _PosixNative posix,
  Pointer<Void> ptr,
  int fd,
  Pointer<Void> semReady,
  Pointer<Void> semDone,
  dynamic processor,
) {
  processor.dispose();
  posix.semClose(semReady);
  posix.semClose(semDone);
  posix.munmap(ptr, _vst3ShmTotalSize);
  posix.close(fd);
  exit(0);
}

