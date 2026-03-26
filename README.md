# Linux Kernel Sound Wave Synthesizer

This project contains the source code of a sound wave synthesizer implemented as a Linux kernel module. The code targets kernel version 6.1.130 and is provided "as is". There are some known unresolved bugs and issues (see description below).

## Overview

The kernel module is a character driver for Linux. A user-space program sends a "generate sound wave" command (`CMDADDWAVE`) to the driver via ioctl. The command contains the characteristics of the sound wave such as frequency, amplitude, and phase (only frequency is currently implemented).

The driver uses the ALSA (Advanced Linux Sound Architecture) subsystem. The driver generates a sound wave and writes it to the audio device buffer. Since a satisfactory way to write sound directly to the physical device speaker has not been found, the driver acts as a virtual microphone whose stream can be redirected to a physical speaker via the `alsaloop` utility (see description below).

The `CMDADDWAVE` command can be sent multiple times by one or more user-space programs. The driver combines individual waves into a single audio signal. The `CMDREMOVEWAVE` command removes a wave of the given frequency.

## Wave Structure

The user-space program sends a single 32-bit word describing the sound wave together with the ioctl command. Three values characterizing the wave are packed into the word: frequency, amplitude, and phase. The following layout is used:

```
+--------------------+--------------------+----------------------+
| 0-6                | 7-15               | 16-31                |
+--------------------+--------------------+----------------------+
| amplitude, 7 bits  | phase, 9 bits      | frequency, 16 bits   |
| 128 values (0..100)| 512 values (0..360)| 64k values (0..48000)|
+--------------------+--------------------+----------------------+
```

A number of helper macros are provided for convenient packing/unpacking: `MAKEWAVE`, `GETWAVEAMP`, `SETWAVEAMP`, etc.

## How to Build

The Makefile has several targets.

`kbuild` builds the kernel module and places it in the build directory. The built module can be loaded into the kernel manually using the `insmod` system utility.

`reinsmod` is a composite command that builds the kernel module, unloads the old module (if one was previously loaded), and loads the new one. Useful during development or debugging.

`app_us` builds the user-space program for sending commands to the driver.

To build the kernel module and the user-space program, run the following commands:

```shell
$ sudo make kbuild
$ sudo make app_us
```

Two files should appear in the build directory: `ex_oscillator.ko` (kernel module) and `us_oscillator` (user-space program).

## How to Run

To load the kernel module, in most cases it is sufficient to run:

```shell
$ sudo make reinsmod
```

If the kernel module is loaded successfully, the `dmesg` utility should show the message: `kernel ALSA sound module loaded successfully`.

Once the kernel module is loaded, the user-space program can be started (superuser privileges required), for example:

```shell
$ sudo ./build/us_oscillator
```

Commands can be sent to the driver through `us_oscillator`. For example, the command `a 100 0 480` sends the driver a request to generate a 480 Hz sound wave with amplitude 100 and phase 0. The command `r 480` cancels a previously sent request to generate a 480 Hz wave.

## How to Configure

To configure sound wave output to a physical speaker, start the `alsaloop` utility:

```shell
$ alsaloop -C hw:1,0 -P hw:0,0 -c 2 -f S16_LE -r 48000
```

The `-C` (capture device) and `-P` (playback device) arguments configure stream redirection. The values depend on the configuration of the specific system. A list of devices available on a particular machine can be obtained with `aplay -l` and `arecord -l`.

## Unresolved Issues

1. Occasional `buffer underrun` error;
2. No floating-point support, making it difficult to calculate the step; waves with close frequencies sound the same;
3. Cannot unload the module without the `-f` flag (ALSA appears to hold it);
4. Capture device is redirected to physical playback via `alsaloop`. How to write to physical playback bypassing `alsaloop`?

## Other Issues

1. No trigonometric functions available, `__fixp_sin32` is used instead;
2. Few examples of ALSA subsystem usage in a driver;
3. The best syntax highlighting so far has been achieved in Eclipse.
