# Sonivox EAS for Linux/ALSA

This project is a Linux daemon which provides [ALSA](https://en.wikipedia.org/wiki/Advanced_Linux_Sound_Architecture) MIDI sequencer interface using *Sonivox EAS* with audio output using ALSA.

Sonivox EAS is a [software synthesizer](https://en.wikipedia.org/wiki/Software_synthesizer) licensed by [Google](https://en.wikipedia.org/wiki/Google) from the company Sonic Network Inc. under the terms of the [Apache License 2.0](https://spdx.org/licenses/Apache-2.0.html).

It's a Wave Table synthesizer using embedded samples, but it can optionally load samples from an external DLS file.

The source code (of the Linux daemon) is released with [MIT license](https://spdx.org/licenses/MIT.html).

<hr>

## Compiling

Use `git clone --recurse-submodules` to clone the repository including the submodule. Or use `git clone` followed by `git submodule update --init --recursive`.

Use [CMake](https://cmake.org/) to build the project.
