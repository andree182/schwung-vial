# Vial for Schwung (Ableton Move)

Polyphonic spectral-morphing wavetable synthesizer module for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move.

Based on [Vial](https://github.com/buchenberg/vial) (a JUCE 8 / CMake fork of [Vital](https://vital.audio/) by Matt Tytel).

## Features

* **3 Wavetable Oscillators + Sampler/Noise Generator** with spectral morphing, phase/FM/RM distortion, up to 16-voice unison per oscillator, and **81 selectable wavetables** (`Preset` + 6 Vial built-in shapes + 74 multi-frame Amino wavetables)
* **Dual Multi-Mode Filters** (Analog, Dirty, Ladder, Digital, Diode, Formant, Comb, Phaser)
* **3 ADSR Envelopes** (with Delay & Hold) and **LFOs** (syncable to MIDI clock, with 7 selectable LFO shapes: `Preset`, `Triangle`, `Sine`, `Saw Up`, `Saw Down`, `Square`, `Ramp`)
* **8-Slot Live Modulation Matrix (`Slot 1–8`)** pre-populated from each preset's active modulations and editable on Move or via the Remote Web UI
* **Built-in Effects Chain:** Chorus, Flanger, Phaser, Distortion, Delay, Reverb, Multiband Compressor
* **Remote Web UI (`web_ui.html`):** Full browser-based control panel via Schwung Remote API (`window.schwungRemote`) with searchable preset browser, live wavetable terrain visualizers (drag horizontally to scrub `Wave Frame`), oscillators, filters, envelopes, LFOs, 8-slot modulation matrix, effects rack, and Move CPU settings
* **Multi-Format Preset Support:**
  * Native Vital/Vial presets (`.vital`, `.vial`) — wavetables and samples are embedded directly inside each preset file
  * On-the-fly Helm preset translation (`.helm`) — translates Helm's 2 oscillators, sub oscillator (mapped to Oscillator 3), noise, envelopes, filter, FM cross-modulation, portamento, LFOs (with waveform shapes), effects, and modulation routings
  * On-the-fly Surge XT preset translation (`.fxp`) — parses the embedded `<patch>` XML and binary wavetable chunks inside `.fxp` files and translates Scene A's 3 oscillators, embedded wavetables, noise, dual filters, Amp/Filter envelopes, LFOs (with shapes), FX slots (including Rotary Speaker → Chorus), portamento/legato, and `<modrouting>` connections into Vial
* **Full Schwung Shadow UI Hierarchy:** Navigate categories (`Jump to Category`), oscillators (with wavetable & frame selection), sampler, filters, envelopes, LFOs (with shape selection), effects, macros, 8-slot modulations, and CPU/engine settings directly from Move's jog wheel and 8 knobs
* **Symbol-Isolated (`plugin.lds`):** Exports only `move_plugin_init_v2` so Vial (`JUCE 8`) can run side-by-side with Helm (`JUCE 5`) on parallel tracks without C++/JUCE symbol collisions

## Preset Directories & Category Naming

### Build-Time Source Directories (`./scripts/build.sh`)
During packaging, `scripts/build.sh` populates `dist/vial/vial-data/patches/` from `presets/`:
* **`presets/helm/`** — 275 factory `.helm` presets (including `Move Organ.helm`).
* **`presets/surge/`** — Surge XT `.fxp` presets (excluding `Templates`, `Tutorials`, and `Vocoder`).
* **`presets/<Bank>/`** — Any native `*.vital` / `*.vial` preset banks placed in `presets/` (skipping standalone `.wav`/`.vitaltable`/`.vitallfo` files since `.vital` presets embed wavetables and samples inline).

### Runtime Search Directories on Ableton Move
When the plugin initializes (`v2_create_instance`), it recursively scans for `*.vital`, `*.vial`, `*.helm`, and `*.fxp` files in:
1. **`<module_dir>/vial-data/patches`** *(primary — `/data/UserData/schwung/modules/sound_generators/vial/vial-data/patches` on Move)*
2. **`<module_dir>/helm-data/patches`** *(fallback if `vial-data/patches` does not exist)*

### Category Prefixes & Ordering in `Jump to Category`
Presets are automatically sorted into three cleanly separated groups:
1. **`Vial-<Category>`** for `.vital` / `.vial` files (e.g., `Vial-ESW - Origins`, `Vial-GM Feb-25-Bass`, `Vial-GM Feb-25-Keys`).
2. **`Helm-<Category>`** for `.helm` files (e.g., `Helm-Arp`, `Helm-Bass`, `Helm-Keys`, `Helm-Lead`, `Helm-Pad`).
3. **`Surge-<Category>`** for `.fxp` files (e.g., `Surge-Basses`, `Surge-Keys`, `Surge-Leads`, `Surge-Pads`, `Surge-Plucks`, `Surge-Polysynths`).

## Move CPU Optimizations (ARM Cortex-A72)

Vital's spectral wavetable engine is significantly heavier than Helm. To run smoothly within Move's 128-frame (`2902 µs`) real-time audio budget on the BCM2711 quad-core **ARM Cortex-A72**:

1. **Per-Oscillator 32-Slot IFFT Spectrum Cache ([JuceHeader.h](src/dsp/JuceLibraryCode/JuceHeader.h)):**
   * Vial's `SynthOscillator` runs a 2048-point inverse real FFT every `5 ms` (220 samples) per active voice per oscillator to bandlimit wavetable frames.
   * By caching the 32 most recent harmonic spectra per oscillator, held notes and same-octave notes hit the cache via an L1 `memcpy` (`< 0.5 µs`) instead of recomputing a 2048-point FFT, cutting oscillator CPU by **>30%**.
2. **1x Oversampling by Default (`oversampling = 0`):**
   * Vial defaults to `2x` oversampling internally; switching to `1x (Draft)` cuts voice and effect DSP load nearly in half. Configurable in **Settings → Oversampling** (`1x (Draft)`, `2x`, `4x`, `8x`).
3. **Configurable Polyphony, Unison & FX Caps (Settings Menu):**
   * **Max Polyphony (`max_polyphony`):** Defaults to `4` voices (`2` 4-wide NEON `AggregateVoice` pairs) with `voice_override = Steal` so releasing voices never spawn unbounded parallel voices.
   * **Max Unison (`max_unison`):** Defaults to `3` unison voices per oscillator (clamping 13–16 voice unison patches on load).
   * **Bypass FX (`fx_bypass`):** Optional toggle in Settings to disable all effects globally.
4. **Idle Auto-Sleep:**
   * Once active voices reach 0 and effect tails decay below `-100 dB` (`1e-5f`) for ~0.4 seconds, DSP processing pauses automatically (`< 1 µs/block`, saving ~227 µs/block on idle tracks) and wakes instantaneously on the next MIDI or parameter event.
5. **Cortex-A72 NEON Tuning:**
   * Compiled with `-O3 -mcpu=cortex-a72 -ffast-math -fno-finite-math-only -fno-math-errno -funroll-loops`.

## Build & Install

Requires Docker (recommended for Ubuntu 22.04 / `glibc 2.35` compatibility with Move) or a local `aarch64-linux-gnu-g++` toolchain:

```bash
./scripts/build.sh
./scripts/install.sh
```

## Controls

| Control | Function |
|---------|----------|
| **Jog wheel** | Browse presets (at root) or navigate parameter menus (`Jump to Category`, `Oscillator 1–3`, `Sampler / Noise`, `Filter 1–2`, `Amp/Env 2–3`, `LFO 1–2`, `Chorus`, `Flanger`, `Phaser`, `Distortion`, `Delay`, `Reverb`, `Compressor`, `Macros`, `Modulations`, `Settings`) |
| **Knobs 1–8** | Adjust the 8 mapped parameters for the active menu level (at root: `Filter 1 Cutoff`, `Resonance`, `Drive`, `Amp Attack`, `Decay`, `Sustain`, `Release`, `Volume`; in `Oscillator 1–3`: Knob 7 = `Wavetable`, Knob 8 = `Wave Frame`) |

## Remote Web UI

Open the module's Remote Web UI from Schwung Manager (`web_ui.html`) for full browser control over Wi-Fi or USB (zero external CDN dependencies):

* **Preset Browser:** Search and filter all discovered `Vial-*`, `Helm-*`, and `Surge-*` presets (`presets_index.json` generated automatically at runtime).
* **Live Wavetable Visualizers:** Displays accurate waveforms with multi-frame terrain previews for all 74 Amino wavetables (`assets/wave-previews/00.bin`–`73.bin`), Vial's 6 built-in shapes, and each loaded preset's native wavetables (`osc_waves.bin`). Drag horizontally across any oscillator waveform to scrub `Wave Frame` (`0–255`).
* **8-Slot Modulation Matrix:** Enable/disable slots, pick sources and destinations, and adjust bipolar amounts (`-1.00..+1.00`) in real time.

## License

GPL-3.0 — See [LICENSE](LICENSE).
