# Voice test clip

`libri198-v36.bit` is 13.9 s of read speech (696 frames of 6 bytes, no
header) coded with sotto v3.6, Ragtime's voice codec 1: libsotto built
from baitisj/sotto at 08d5047 (`make -C c`, the default variant), fed
sotto's `docs/samples/female_libri198_14s__original.flac` resampled to
8 kHz mono 16-bit.

The speech is from LibriSpeech (speaker 198), by Vassil Panayotov,
Guoguo Chen, Daniel Povey and Sanjeev Khudanpur, from LibriVox
recordings, licensed CC BY 4.0
(https://creativecommons.org/licenses/by/4.0/). The frames are a coded
derivative of it, under the same licence.

`tests/VoiceTest.cpp` uses it to check the pause gate, silence runs and
the pitch strip on real frames.
