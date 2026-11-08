# asterisk-chan-quectel

An Asterisk channel driver for Quectel LTE modules, carrying voice over the
module's USB Audio Class interface and control/SMS over its AT serial port.

## Supported modules

| Module | Voice (UAC) | SMS | Notes |
|---|---|---|---|
| EC25-E | yes | yes | developed and tested against firmware `EC25EFAR06A11M4G` |

Only the EC25-E has been tested. Other Quectel modules that expose a USB Audio
Class interface and accept `AT+QPCMV=1,2` are likely to work, and reports are
welcome — there is an issue template for it. Modules that only offer PCM over
the serial port are **not** supported: that path is unimplemented, and on
EC25-E firmware `AT+QPCMV=1,0` returns `ERROR` anyway.

## Relationship to asterisk-chan-quectel (RoEdAl)

This is a ground-up rewrite of
[RoEdAl/asterisk-chan-quectel](https://github.com/RoEdAl/asterisk-chan-quectel),
written for two reasons.

**1. Newer OpenWrt and Asterisk.** The original targets Asterisk 16-20 and its
CI stops at OpenWrt 23.05; upstream has had no commits since mid-2024. Its
OpenWrt packaging predates the switch to `apk` in OpenWrt 24.10 and will not
build there at all — the generated package version is rejected outright, and
`EXTRA_DEPENDS` entries without version constraints became a hard error. This
driver targets OpenWrt 25.12 / Asterisk 23.

**2. Working audio on the EC25-E.** The original's UAC audio path does not
deliver usable two-way audio on this module. Measured under OpenWrt 25.12.5 and
Asterisk 23.1.0, it captured **60 of an expected 600 frames** per 12-second
call — identically at its `v2023.12.13` release tag and at `master`, so this was
never a regression. The result is speech about 50 dB below normal level,
arriving in fragments. This driver captures **600 of 600** on the same hardware.

The cause was architectural: the original drives ALSA capture from Asterisk's
channel-service callback, keyed off `snd_pcm_poll_descriptors()`, which does not
work as a wakeup source on this device. See
[Why the audio is built the way it is](#why-the-audio-is-built-the-way-it-is)
for what replaced it. Two modem-side settings also matter and are handled
automatically here — see [Modem settings that matter](#modem-settings-that-matter).

That project was nonetheless the reference for this one, and the debt is
substantial:

- `src/pdu.c`, `src/char_conv.c`, `src/gsm7_luts.h` and `src/error.[ch]` are
  **ported from it largely unchanged**. That SMS code is well-tested and
  entirely orthogonal to the audio problems above, so it was worth reusing
  rather than rewriting.
- The CLI verbs, the configuration file layout and the `Quectel/<device>/<number>`
  dial string deliberately follow its conventions, so existing dialplans and
  muscle memory carry over.

It is GPL-2.0, as is this project. Thanks to its authors.

## Scope

Supported, because that is what the hardware in question does:

- voice over USB Audio Class (`AT+QPCMV=1,2`), 8 kHz signed linear, mono
- inbound and outbound calls, DTMF, caller id
- SMS send/receive in PDU mode: UCS-2 and concatenated (multipart) messages
- a small CLI, shaped after `asterisk-chan-quectel`'s

Deliberately **not** supported: SimCOM modules, PCM-over-serial audio,
multiparty calls, conferencing, USSD, and anything analog. One call at a time
per device.

## Why the audio is built the way it is

This is the part worth reading before changing anything in `src/audio.c`.

- **A dedicated thread owns both PCM handles** and performs a *blocking*
  `snd_pcm_readi()`. That call returns exactly when a period is ready, which
  paces the whole loop at the device clock — no timer, no sleep, no drift.
- **Ring buffers sit between that thread and the PBX thread.** `channel_read()`
  pops a frame; it never touches ALSA. A slow or bursty PBX therefore cannot
  stall capture.
- **Readiness is signalled on an eventfd**, not on
  `snd_pcm_poll_descriptors()`. That descriptor does not work as a wakeup source
  on this device; relying on it is why the previous driver starved. Driving
  reads from Asterisk's channel-service callback instead delivers roughly a
  tenth of the frames.
- **Playback is primed** with a few periods of silence and given a start
  threshold of ~3 periods. Without that it starts on the first write and
  immediately underruns — 375 of 750 writes xran before this was added.
- **The stream runs only for the duration of a call.** Leaving it up when idle
  keeps the USB audio endpoint transferring in both directions for nothing,
  which measured at ~3% of a CPU core permanently, and makes the ring buffers
  churn through audio no one reads. The device is opened once at startup purely
  to validate `alsadev`, then closed again.
- `snd_pcm_t` is **not thread-safe**. Only the audio thread may touch a handle.
  Calling e.g. `snd_pcm_drop()` from the stopping thread to hurry the audio
  thread along corrupts ALSA state and crashes Asterisk on module unload.

## Modem settings that matter

Two are mandatory and both fail silently in confusing ways:

| Setting | Why |
|---|---|
| `AT+QPCMV=1,2` | Enables the USB audio path. When off, the UAC interface streams **pure digital silence** — captures succeed and frame counts look perfect, so it reads like a driver bug. The driver sets this at startup and fails loudly if it is rejected. |
| `AT+QRXGAIN=65535` | The modem default of 8192 is about **44 dB too quiet** (RMS ~17 / peak ~109). At 65535 the same speech measures RMS ~2400 / peak ~22500. This is the `rxgain` option, and it defaults to 65535 here. |

Note the driver issues `AT+QPCMV=0` on unload, restoring the modem to its
original state. Anything that opens the audio device afterwards must re-enable it.

## Building

Needs the Asterisk headers, ALSA and pthreads.

```sh
cmake -S . -B build -DAST_HEADER_DIR=/usr/include
cmake --build build -j
```

Cross-compiling with the OpenWrt SDK, using the SDK's own Asterisk headers so
the module ABI matches the installed binary (this matters — Asterisk modules are
coupled to buildopts such as `LOW_MEMORY`):

```sh
export STAGING_DIR=<sdk>/staging_dir
cmake -S . -B build-owrt \
      -DCMAKE_TOOLCHAIN_FILE=owrt.toolchain.cmake \
      -DAST_HEADER_DIR=<sdk>/staging_dir/target-x86_64_musl/usr/include
cmake --build build-owrt -j
```

## OpenWrt package

`openwrt/make-feed.sh` assembles a package feed containing this source tree:

```sh
./openwrt/make-feed.sh                    # writes openwrt/feed/ + feeds-quectel.conf
```

Then, from an extracted SDK matching your target (for OpenWrt 25.12.6 x86/64,
`openwrt-sdk-25.12.6-x86-64_gcc-14.3.0_musl.Linux-x86_64.tar.zst`):

```sh
cp <project>/openwrt/feeds-quectel.conf .
cat feeds.conf.default feeds-quectel.conf > feeds.conf
./scripts/feeds update -a
./scripts/feeds install asterisk-chan-quectel
echo 'CONFIG_PACKAGE_asterisk-chan-quectel=m' >> .config
make defconfig
make package/asterisk-chan-quectel/compile V=s
```

The package lands in `bin/packages/<arch>/quectel/`. Install it with:

```sh
apk add --allow-untrusted ./asterisk-chan-quectel-*.apk
service asterisk restart
```

It declares everything it needs, so apk pulls the rest in:

| dependency | why |
|---|---|
| `asterisk` | the PBX itself |
| `alsa-lib` | the audio endpoint |
| `kmod-usb-serial-option` | `/dev/ttyUSB*`, the AT port (pulls `kmod-usb-serial`, `kmod-usb-serial-wwan`) |
| `kmod-usb-audio` | the UAC sound card the voice path uses (pulls `kmod-sound-core`, `kmod-usb-core`) |

`usb-modeswitch` is deliberately **not** a dependency: the EC25-E enumerates
directly and needs no mode switching.

The SDK builds Asterisk first so the module compiles against that SDK's own
headers — this matters, because Asterisk modules are ABI-coupled to buildopts
such as `LOW_MEMORY`.

### Two traps this packaging exists to avoid

**apk silently skips a package whose version already matches what is
installed.** Nothing is reported; the old binary simply stays. So
`make-feed.sh` derives a version that changes whenever the source does:

| tree | version |
|---|---|
| clean | `0.<commit-count>.0-r1` |
| dirty | `0.<commit-count>.0-r<unix timestamp>` |

The timestamp release guarantees every build of an uncommitted tree is strictly
newer, so it always installs. Only digits and dots are used — apk rejects a
suffix like `~g1a2b3c4` because it starts with a letter.

**`module unload` + `module load` does not replace module code on musl**, where
`dlclose()` is a no-op: `dlopen` hands back the original mapping while both
commands report success. Always `service asterisk restart`. The package's
postinst says so on every install.

Verify what is actually *running* rather than what is on disk — if in doubt,
grep the log for a message only the new build emits.

### Installing by hand

For quick iteration you can skip the package and copy `chan_quectel.so` into
`/usr/lib/asterisk/modules/` directly. Write to a temporary name and `mv` it
into place: truncating a mapped `.so` (`cat > .../chan_quectel.so`) crashes
Asterisk. A full restart is still required.

## Versioning

Everything is derived from the most recent git tag, so a release needs nothing
but a tag — no file in the source has to be edited. `PKG_RELEASE` carries the
distance from that tag, which is what it is for: a packaging iteration on top of
the same upstream version.

| state | `quectel show version` | package |
|---|---|---|
| exactly on tag `v0.1.0` | `v0.1.0` | `0.1.0` |
| 2 commits later | `v0.1.0-2-gabc123` | `0.1.0-r2` |
| dirty tree | `v0.1.0-2-gabc123-dirty` | `0.1.0-r<timestamp>` |

A clean tag gets **no `-r` suffix at all** — OpenWrt omits it when `PKG_RELEASE`
is empty. apk orders these correctly:

```
0.1.0 < 0.1.0-r2 < 0.1.0-r<timestamp> < 0.2.0
```

The timestamp for a dirty tree is deliberate: apk silently refuses to reinstall
a package whose version already matches what is installed, so an uncommitted
rebuild would otherwise never deploy.

## Configuration

See `quectel.conf`. Minimal example:

```ini
[quectel0]
data=/dev/ttyUSB2
alsadev=hw:Android
context=incoming-mobile
rxgain=65535
```

## Dialplan

Outbound:

```
exten => _X.,1,Dial(Quectel/quectel0/${EXTEN})
```

`Quectel/g<group>/<number>` picks any idle device in that dial group.

Inbound calls enter `context` at extension `s` (configurable via `exten`).
Incoming SMS runs extension `sms` in the same context with `SMS_FROM`,
`SMS_TEXT`, `SMS_TS` and `SMS_DEVICE` set:

```ini
[incoming-mobile]
exten => s,1,Dial(PJSIP/handset,20)
 same => n,Hangup()

exten => sms,1,Verbose(SMS from ${SMS_FROM}: ${SMS_TEXT})
 same => n,Hangup()
```

## CLI

```
quectel show devices
quectel show device <device>      ; full state incl. audio counters
quectel show version
quectel cmd <device> <AT command>
quectel sms send <device> <number> <message...>
quectel restart <device>
quectel audio gain <device> rx|tx [<value>]
```

`quectel show device` reports capture/playback frame counts, xruns and
ring-buffer drops. On a healthy call, capture and playback frame counts track
each other and `RX underruns` stays at zero.

The audio stream only runs while a call is up, so these counters are static when
the device is idle. If they climb with no call in progress, something is wrong.

Note `Verbose()` without a level argument logs at level 0 and is always shown.
`Verbose(1,...)` and above are filtered out unless `verbose` is raised in
`asterisk.conf` or `core set verbose` is run inside an interactive console —
setting it via `asterisk -rx` only affects that one short-lived connection.

## Known limitations

- Outgoing messages are capped at `QUECTEL_SMS_MAX_PARTS` (12) parts, i.e. about
  800 UCS-2 or 1800 GSM7 characters. Incoming messages with more parts than that
  are delivered part-by-part rather than reassembled.
- Reassembly state is held in memory, so parts still outstanding when Asterisk
  restarts are lost. Incomplete sets are discarded after 10 minutes.
- One call at a time per device. No call waiting, hold or transfer at the modem
  level (Asterisk-side bridging/transfer works normally).
- `Number` in `quectel show device` is often blank — many SIMs simply do not carry
  MSISDN in `AT+CNUM`.

## SMS

The modem is kept in PDU mode (`AT+CMGF=0`). Text mode cannot represent anything
outside the GSM alphabet and cannot carry concatenated messages, so it is
unusable for e.g. Hebrew or anything past 160 characters.

`pdu.c`, `char_conv.c` and `gsm7_luts.h` are ported from
`asterisk-chan-quectel`. That code is well-tested and entirely orthogonal to the
audio problems which motivated this driver, so it was worth reusing rather than
rewriting. `test/pdu_harness.c` exercises it without Asterisk — build and run it
to check encoding after any change:

```sh
gcc -o pduharness test/pdu_harness.c src/pdu.c src/char_conv.c src/error.c \
    <stubs> -I<asterisk-include> -Isrc -DAST_MODULE_SELF_SYM=x -DAST_MODULE='"t"'
```

Incoming concatenated messages are reassembled before reaching the dialplan, so
`${SMS_TEXT}` is always a whole message. Delivery reports (`+CDSI`) are fetched
and logged.

## Licence

GPL-2.0, matching Asterisk and the project this is based on. See `LICENSE`.
