import 'package:flutter_vst3/flutter_vst3.dart';
import 'src/echo_parameters.dart';
import 'src/echo_processor.dart';

/// Adapts the Echo DSP engine and parameter state to match the interface
/// expected by [runVst3SharedMemoryProcessor].
final class _EchoProcessorWrapper {
  final EchoProcessor _processor = EchoProcessor();
  final EchoParameters _parameters = EchoParameters();

  void initialize(double sampleRate, int maxBlockSize) =>
      _processor.initialize(sampleRate, maxBlockSize);

  void setParameter(int paramId, double value) =>
      _parameters.setParameter(paramId, value);

  void processStereo(
    List<double> inputL,
    List<double> inputR,
    List<double> outputL,
    List<double> outputR,
  ) =>
      _processor.processStereo(inputL, inputR, outputL, outputR, _parameters);

  void reset() => _processor.reset();

  void dispose() => _processor.dispose();
}


/// Entry point for Echo processor executable.
/// Uses preallocated zero-copy shared memory and POSIX semaphores.
void main(List<String> args) {
  runVst3SharedMemoryProcessor(
    args: args,
    processor: _EchoProcessorWrapper(),
  );
}
