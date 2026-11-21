/*
 * Vial DSP Plugin for Move Anything / Schwung
 *
 * GPL-3.0 License
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <vector>

/* Plugin API definitions */
extern "C" {
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
  uint32_t api_version;
  int sample_rate;
  int frames_per_block;
  uint8_t *mapped_memory;
  int audio_out_offset;
  int audio_in_offset;
  void (*log)(const char *msg);
  int (*midi_send_internal)(const uint8_t *msg, int len);
  int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
  uint32_t api_version;
  void *(*create_instance)(const char *module_dir, const char *json_defaults);
  void (*destroy_instance)(void *instance);
  void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
  void (*set_param)(void *instance, const char *key, const char *val);
  int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
  int (*get_error)(void *instance, char *buf, int buf_len);
  void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t *(*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"
}

/* Vial headers */
#include <JuceHeader.h>
#include "json/json.h"
#include "load_save.h"
#include "midi_manager.h"
#include "synth_base.h"
#include "synth_parameters.h"
#include "synth_engine/sound_engine.h"
#include "wavetable_creator.h"
#include "lookups/wave_frame.h"
#include "modules/modulation_connection_processor.h"

using json = nlohmann::json;

/* Minimal JUCE stubs for headless build without juce_graphics/FreeType */
namespace juce {
const char *juce_compilationDate = __DATE__;
const char *juce_compilationTime = __TIME__;

Colour::Colour(uint32 col) noexcept
    : argb(static_cast<uint8>((col >> 24) & 0xff),
           static_cast<uint8>((col >> 16) & 0xff),
           static_cast<uint8>((col >> 8) & 0xff),
           static_cast<uint8>(col & 0xff)) {}
}

/* Host API reference */
static const host_api_v1_t *g_host = nullptr;

extern "C" void plugin_log(const char *msg) {
  if (g_host && g_host->log) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "[vial] %s", msg);
    g_host->log(buf);
  }
}

/* Curated Vial parameters exposed to Schwung UI & chain_params */
static const char *kUiVialParams[] = {
    /* Oscillator 1 */
    "osc_1_on", "osc_1_level", "osc_1_transpose", "osc_1_tune", "osc_1_pan",
    "osc_1_unison_voices", "osc_1_unison_detune", "osc_1_wave_frame",
    "osc_1_distortion_type", "osc_1_distortion_amount",
    "osc_1_spectral_morph_type", "osc_1_spectral_morph_amount",
    "osc_1_destination",
    /* Oscillator 2 */
    "osc_2_on", "osc_2_level", "osc_2_transpose", "osc_2_tune", "osc_2_pan",
    "osc_2_unison_voices", "osc_2_unison_detune", "osc_2_wave_frame",
    "osc_2_distortion_type", "osc_2_distortion_amount",
    "osc_2_spectral_morph_type", "osc_2_spectral_morph_amount",
    "osc_2_destination",
    /* Oscillator 3 */
    "osc_3_on", "osc_3_level", "osc_3_transpose", "osc_3_tune", "osc_3_pan",
    "osc_3_unison_voices", "osc_3_unison_detune", "osc_3_wave_frame",
    "osc_3_distortion_type", "osc_3_distortion_amount",
    "osc_3_spectral_morph_type", "osc_3_spectral_morph_amount",
    "osc_3_destination",
    /* Sample / Noise */
    "sample_on", "sample_level", "sample_transpose", "sample_tune",
    "sample_pan", "sample_keytrack", "sample_loop", "sample_destination",
    /* Filter 1 */
    "filter_1_on", "filter_1_model", "filter_1_style", "filter_1_cutoff",
    "filter_1_resonance", "filter_1_drive", "filter_1_blend", "filter_1_mix",
    "filter_1_keytrack", "filter_1_osc1_input", "filter_1_osc2_input",
    "filter_1_osc3_input", "filter_1_sample_input",
    /* Filter 2 */
    "filter_2_on", "filter_2_model", "filter_2_style", "filter_2_cutoff",
    "filter_2_resonance", "filter_2_drive", "filter_2_blend", "filter_2_mix",
    "filter_2_keytrack", "filter_2_osc1_input", "filter_2_osc2_input",
    "filter_2_osc3_input", "filter_2_sample_input", "filter_2_filter_input",
    /* Envelopes 1-3 */
    "env_1_attack", "env_1_decay", "env_1_sustain", "env_1_release",
    "env_1_delay", "env_1_hold",
    "env_2_attack", "env_2_decay", "env_2_sustain", "env_2_release",
    "env_2_delay", "env_2_hold",
    "env_3_attack", "env_3_decay", "env_3_sustain", "env_3_release",
    "env_3_delay", "env_3_hold",
    /* LFOs 1-2 */
    "lfo_1_frequency", "lfo_1_sync", "lfo_1_tempo", "lfo_1_sync_type",
    "lfo_1_phase", "lfo_1_fade_time", "lfo_1_smooth_time",
    "lfo_2_frequency", "lfo_2_sync", "lfo_2_tempo", "lfo_2_sync_type",
    "lfo_2_phase", "lfo_2_fade_time", "lfo_2_smooth_time",
    /* Effects */
    "chorus_on", "chorus_dry_wet", "chorus_feedback", "chorus_frequency",
    "chorus_mod_depth", "chorus_voices",
    "flanger_on", "flanger_dry_wet", "flanger_feedback", "flanger_frequency",
    "flanger_mod_depth", "flanger_center",
    "phaser_on", "phaser_dry_wet", "phaser_feedback", "phaser_frequency",
    "phaser_mod_depth", "phaser_center",
    "distortion_on", "distortion_type", "distortion_drive", "distortion_mix",
    "delay_on", "delay_dry_wet", "delay_feedback", "delay_frequency",
    "delay_sync", "delay_tempo", "delay_style",
    "reverb_on", "reverb_dry_wet", "reverb_decay_time", "reverb_size",
    "reverb_high_shelf_gain", "reverb_low_shelf_gain",
    "compressor_on", "compressor_mix", "compressor_attack",
    "compressor_release", "compressor_low_gain", "compressor_band_gain",
    "compressor_high_gain",
    /* Macros */
    "macro_control_1", "macro_control_2", "macro_control_3", "macro_control_4",
    /* Settings */
    "polyphony", "portamento_time", "portamento_slope", "legato",
    "pitch_bend_range", "velocity_track", "voice_tune"
};

static std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  return out;
}

/* =====================================================================
 * Headless Vial instance class
 * ===================================================================== */
class VialMoveInstance : public HeadlessSynth {
public:
  VialMoveInstance()
      : HeadlessSynth(), current_time_seconds_(0.0), silent_blocks_(0) {}
  ~VialMoveInstance() override {}

  using SynthBase::getConnection;
  using SynthBase::controls_;

  void wakeFromSleep() { silent_blocks_ = 0; }
  bool isSleeping() const { return silent_blocks_ >= 150; }

  void prepare(double sample_rate, int buffer_size) {
    (void)buffer_size;
    juce::ScopedLock lock(getCriticalSection());
    engine_->setSampleRate(sample_rate);
    engine_->updateAllModulationSwitches();
    midi_manager_->setSampleRate(sample_rate);
  }

  void allSoundsOff() {
    juce::ScopedLock lock(getCriticalSection());
    engine_->allSoundsOff();
  }

  void setBpm(float bpm) {
    juce::ScopedLock lock(getCriticalSection());
    engine_->setBpm(bpm);
  }

  void applyOversampling(int level) {
    juce::ScopedLock lock(getCriticalSection());
    auto it = controls_.find("oversampling");
    if (it != controls_.end() && it->second != nullptr) {
      it->second->set((float)std::clamp(level, 0, 3));
    }
    checkOversampling();
  }

  bool hasControl(const std::string &name) const {
    auto it = controls_.find(name);
    return it != controls_.end() && it->second != nullptr;
  }

  void setControl(const std::string &name, float val) {
    juce::ScopedLock lock(getCriticalSection());
    silent_blocks_ = 0;
    auto it = controls_.find(name);
    if (it != controls_.end() && it->second != nullptr) {
      it->second->set(val);
    }
  }

  float getControl(const std::string &name, float fallback = 0.0f) {
    juce::ScopedLock lock(getCriticalSection());
    auto it = controls_.find(name);
    if (it != controls_.end() && it->second != nullptr) {
      return it->second->value();
    }
    return fallback;
  }

  void process(juce::AudioSampleBuffer *buffer, juce::MidiBuffer &midi_messages,
               int num_samples) {
    /* Non-blocking lock on the audio thread: if a preset is mid-load on the UI
     * thread, output silence for this 128-frame block instead of racing or xrunning. */
    juce::ScopedTryLock lock(getCriticalSection());
    if (!lock.isLocked())
      return;

    if (!midi_messages.isEmpty()) {
      silent_blocks_ = 0;
    } else if (silent_blocks_ >= 150 && engine_->getNumActiveVoices() == 0) {
      /* Engine and FX tails have decayed to silence; skip DSP until next MIDI/param event */
      return;
    }

    processModulationChanges();
    if (num_samples > 0)
      processKeyboardEvents(midi_messages, num_samples);

    double sample_rate = getSampleRate();
    if (sample_rate <= 0.0)
      sample_rate = (double)MOVE_SAMPLE_RATE;
    double sample_time = 1.0 / sample_rate;

    for (int sample_offset = 0; sample_offset < num_samples;) {
      int chunk = std::min<int>(num_samples - sample_offset, vial::kMaxBufferSize);

      engine_->correctToTime(current_time_seconds_);
      processMidi(midi_messages, sample_offset, sample_offset + chunk);
      processAudio(buffer, 2, chunk, sample_offset);

      current_time_seconds_ += chunk * sample_time;
      sample_offset += chunk;
    }

    if (midi_messages.isEmpty() && engine_->getNumActiveVoices() == 0) {
      float peak = buffer->getMagnitude(0, num_samples);
      if (peak < 1e-5f)
        silent_blocks_++;
      else
        silent_blocks_ = 0;
    } else {
      silent_blocks_ = 0;
    }
  }

  void setOscillatorShape(int osc_index, int helm_wave) {
    WavetableCreator *creator = getWavetableCreator(osc_index);
    if (!creator)
      return;

    creator->clear();
    WavetableGroup *group = new WavetableGroup();
    WaveSource *source = new WaveSource();
    source->insertNewKeyframe(0);
    WaveSourceKeyframe *keyframe = source->getKeyframe(0);

    /* Helm waveform indices:
     * 0=Sine, 1=Triangle, 2=Square, 3=Saw Down, 4=Saw Up,
     * 5..7=Step waves, 8..10=Pyramid waves */
    vial::PredefinedWaveFrames::Shape shape = vial::PredefinedWaveFrames::kSaw;
    if (helm_wave == 0)
      shape = vial::PredefinedWaveFrames::kSin;
    else if (helm_wave == 1 || (helm_wave >= 8 && helm_wave <= 10))
      shape = vial::PredefinedWaveFrames::kTriangle;
    else if (helm_wave == 2 || (helm_wave >= 5 && helm_wave <= 7))
      shape = vial::PredefinedWaveFrames::kSquare;
    else
      shape = vial::PredefinedWaveFrames::kSaw;

    keyframe->wave_frame()->copy(vial::PredefinedWaveFrames::getWaveFrame(shape));
    group->addComponent(source);
    creator->addGroup(group);
    creator->render();
  }

  bool loadSurgeWavetableIntoOsc(int osc_index, const uint8_t *ptr, size_t avail,
                                 size_t &consumed, float morph_pos) {
    consumed = 0;
    WavetableCreator *creator = getWavetableCreator(osc_index);
    if (!creator || avail < 12)
      return false;

    uint32_t n_samples = 0;
    uint16_t n_tables = 0;
    uint16_t flags = 0;
    memcpy(&n_samples, ptr + 4, 4);
    memcpy(&n_tables, ptr + 8, 2);
    memcpy(&flags, ptr + 10, 2);

    if (n_samples < 4 || n_samples > 4096 || n_tables < 1 || n_tables > 512)
      return false;

    bool is_int16 = (flags & 4) != 0;
    size_t sample_bytes = is_int16 ? 2 : 4;
    size_t data_bytes = (size_t)n_samples * (size_t)n_tables * sample_bytes;
    if (12 + data_bytes > avail)
      return false;

    consumed = 12 + data_bytes;
    const uint8_t *pcm = ptr + 12;

    creator->clear();
    WavetableGroup *group = new WavetableGroup();
    WaveSource *source = new WaveSource();

    /* Sample up to 16 evenly spaced frames across the Surge wavetable */
    int num_kf = std::min<int>((int)n_tables, 16);
    for (int kf = 0; kf < num_kf; ++kf) {
      int src_frame = (num_kf == 1) ? 0 : (kf * ((int)n_tables - 1)) / (num_kf - 1);
      int pos = (num_kf == 1) ? 0 : (kf * (vial::kNumOscillatorWaveFrames - 1)) / (num_kf - 1);
      source->insertNewKeyframe(pos);
      WaveSourceKeyframe *keyframe = source->getKeyframe(kf);
      vial::WaveFrame *wf = keyframe->wave_frame();

      const uint8_t *frame_ptr = pcm + (size_t)src_frame * n_samples * sample_bytes;
      for (int i = 0; i < vial::WaveFrame::kWaveformSize; ++i) {
        float src_pos = ((float)i * (float)n_samples) / (float)vial::WaveFrame::kWaveformSize;
        int idx0 = (int)src_pos % (int)n_samples;
        int idx1 = (idx0 + 1) % (int)n_samples;
        float frac = src_pos - std::floor(src_pos);

        float s0 = 0.0f, s1 = 0.0f;
        if (is_int16) {
          int16_t v0, v1;
          memcpy(&v0, frame_ptr + idx0 * 2, 2);
          memcpy(&v1, frame_ptr + idx1 * 2, 2);
          s0 = v0 * (1.0f / 32768.0f);
          s1 = v1 * (1.0f / 32768.0f);
        } else {
          memcpy(&s0, frame_ptr + idx0 * 4, 4);
          memcpy(&s1, frame_ptr + idx1 * 4, 4);
        }
        wf->time_domain[i] = s0 + frac * (s1 - s0);
      }
      wf->toFrequencyDomain();
    }

    group->addComponent(source);
    creator->addGroup(group);
    creator->render();

    std::string vkey = "osc_" + std::to_string(osc_index + 1);
    setControl(vkey + "_wave_frame",
               std::clamp(morph_pos, 0.0f, 1.0f) * (float)(vial::kNumOscillatorWaveFrames - 1));
    return true;
  }

  bool connectModulationSafe(const std::string &src, const std::string &dst, float amount) {
    if (src.empty() || dst.empty())
      return false;
    if (!engine_->getModulationSource(src))
      return false;
    if (!engine_->getMonoModulationDestination(dst))
      return false;
    if (!engine_->getMonoModulationSwitch(dst))
      return false;

    connectModulation(src, dst);
    vial::ModulationConnection *conn = getConnection(src, dst);
    if (conn && conn->modulation_processor) {
      conn->modulation_processor->setBaseValue(std::clamp(amount, -1.0f, 1.0f));
      return true;
    }
    return false;
  }

  bool loadNativePreset(const juce::File &file, std::string &error) {
    juce::ScopedLock lock(getCriticalSection());
    bool ok = loadFromFile(file, error);
    if (ok) {
      processModulationChanges();
      engine_->updateAllModulationSwitches();
    }
    return ok;
  }

  bool loadHelmPreset(const juce::File &file, std::string &error) {
    if (!file.existsAsFile()) {
      error = "File does not exist: " + file.getFullPathName().toStdString();
      return false;
    }

    juce::String content = file.loadFileAsString();
    json helm_json;
    try {
      helm_json = json::parse(content.toStdString(), nullptr);
    } catch (const json::exception &e) {
      error = std::string("JSON parse error: ") + e.what();
      return false;
    }

    if (!helm_json.count("settings") || !helm_json["settings"].is_object()) {
      error = "Missing settings object in Helm preset";
      return false;
    }

    const json &settings = helm_json["settings"];

    juce::ScopedLock lock(getCriticalSection());
    loadInitPreset();

    // Oscillators
    int osc1_wave = settings.value("osc_1_waveform", 2);
    setOscillatorShape(0, osc1_wave);
    float osc1_vol = settings.value("osc_1_volume", 0.7f);
    setControl("osc_1_on", osc1_vol > 0.001f ? 1.0f : 0.0f);
    setControl("osc_1_level", std::sqrt(std::clamp(osc1_vol, 0.0f, 1.0f)));
    setControl("osc_1_transpose", settings.value("osc_1_transpose", 0.0f));
    setControl("osc_1_tune", settings.value("osc_1_tune", 0.0f));
    setControl("osc_1_unison_voices", (float)std::clamp((int)settings.value("osc_1_unison_voices", 1.0f), 1, 16));
    setControl("osc_1_unison_detune", std::clamp(settings.value("osc_1_unison_detune", 0.0f) * 0.1f, 0.0f, 10.0f));

    int osc2_wave = settings.value("osc_2_waveform", 2);
    setOscillatorShape(1, osc2_wave);
    float osc2_vol = settings.value("osc_2_volume", 0.0f);
    setControl("osc_2_on", osc2_vol > 0.001f ? 1.0f : 0.0f);
    setControl("osc_2_level", std::sqrt(std::clamp(osc2_vol, 0.0f, 1.0f)));
    setControl("osc_2_transpose", settings.value("osc_2_transpose", 0.0f));
    setControl("osc_2_tune", settings.value("osc_2_tune", 0.0f));
    setControl("osc_2_unison_voices", (float)std::clamp((int)settings.value("osc_2_unison_voices", 1.0f), 1, 16));
    setControl("osc_2_unison_detune", std::clamp(settings.value("osc_2_unison_detune", 0.0f) * 0.1f, 0.0f, 10.0f));

    // Cross modulation (one-way FM: Osc 2 -> Osc 1; mutual FM in Vial destabilizes pitch)
    float cross_mod = std::clamp((float)settings.value("cross_modulation", 0.0f), 0.0f, 0.5f);
    if (cross_mod > 0.001f) {
      setControl("osc_1_distortion_type", 7.0f);   // kFmOscillatorA (FM <- Osc 2)
      setControl("osc_1_distortion_amount", std::clamp(cross_mod * 0.6f, 0.0f, 0.5f));
    }

    // Sub oscillator -> mapped onto Vial's Oscillator 3 (Vial has no separate sub module)
    float sub_vol = settings.value("sub_volume", 0.0f);
    int sub_wave = settings.value("sub_waveform", 0);
    float sub_octave = settings.value("sub_octave", 0.0f);
    setOscillatorShape(2, sub_wave);
    setControl("osc_3_on", sub_vol > 0.001f ? 1.0f : 0.0f);
    setControl("osc_3_level", std::sqrt(std::clamp(sub_vol, 0.0f, 1.0f)));
    setControl("osc_3_transpose", -12.0f - (sub_octave > 0.5f ? 12.0f : 0.0f));
    setControl("osc_3_unison_voices", 1.0f);

    // Noise/sample
    float noise_vol = settings.value("noise_volume", 0.0f);
    setControl("sample_on", noise_vol > 0.001f ? 1.0f : 0.0f);
    setControl("sample_level", noise_vol);

    // LFOs: poly_lfo -> lfo_1, mono_lfo_1 -> lfo_2, mono_lfo_2 -> lfo_3
    auto mapHelmLfo = [&](const std::string &h_prefix, const std::string &v_prefix, bool is_mono) {
      float freq = settings.value(h_prefix + "_frequency", 1.0f);
      float sync = settings.value(h_prefix + "_sync", 1.0f);
      float tempo = settings.value(h_prefix + "_tempo", 7.0f);
      float retrig = settings.value(h_prefix + "_retrigger", is_mono ? 0.0f : 1.0f);
      setControl(v_prefix + "_frequency", std::clamp(freq, -7.0f, 9.0f));
      setControl(v_prefix + "_sync", std::clamp(sync, 0.0f, 3.0f));
      setControl(v_prefix + "_tempo", std::clamp(tempo, 0.0f, 12.0f));
      setControl(v_prefix + "_sync_type", (retrig > 0.5f) ? 0.0f : 1.0f); // 0=Trigger, 1=Sync
    };
    mapHelmLfo("poly_lfo", "lfo_1", false);
    mapHelmLfo("mono_lfo_1", "lfo_2", true);
    mapHelmLfo("mono_lfo_2", "lfo_3", true);

    // Envelopes (Helm uses quadratic time t = x^2, Vial uses quartic t = y^4 -> y = sqrt(x))
    auto helmEnvTimeToVial = [](float x) -> float {
      return std::clamp(std::sqrt(std::max(0.0f, x)), 0.0f, 2.37842f);
    };
    setControl("env_1_attack", helmEnvTimeToVial(settings.value("amp_attack", 0.01f)));
    setControl("env_1_decay", helmEnvTimeToVial(settings.value("amp_decay", 1.0f)));
    setControl("env_1_sustain", std::clamp((float)settings.value("amp_sustain", 1.0f), 0.0f, 1.0f));
    setControl("env_1_release", helmEnvTimeToVial(settings.value("amp_release", 0.3f)));

    setControl("env_2_attack", helmEnvTimeToVial(settings.value("fil_attack", 0.01f)));
    setControl("env_2_decay", helmEnvTimeToVial(settings.value("fil_decay", 1.0f)));
    setControl("env_2_sustain", std::clamp((float)settings.value("fil_sustain", 0.5f), 0.0f, 1.0f));
    setControl("env_2_release", helmEnvTimeToVial(settings.value("fil_release", 0.5f)));

    setControl("env_3_attack", helmEnvTimeToVial(settings.value("mod_attack", 0.01f)));
    setControl("env_3_decay", helmEnvTimeToVial(settings.value("mod_decay", 1.0f)));
    setControl("env_3_sustain", std::clamp((float)settings.value("mod_sustain", 0.5f), 0.0f, 1.0f));
    setControl("env_3_release", helmEnvTimeToVial(settings.value("mod_release", 0.5f)));

    // Filter
    float filter_on = settings.value("filter_on", 1.0f);
    setControl("filter_1_on", filter_on);
    setControl("filter_1_cutoff", std::clamp((float)settings.value("cutoff", 60.0f), 8.0f, 136.0f));
    setControl("filter_1_resonance", std::clamp((float)settings.value("resonance", 0.0f), 0.0f, 1.0f));
    setControl("filter_1_drive", std::clamp((float)settings.value("filter_drive", 0.0f), 0.0f, 20.0f));
    setControl("filter_1_keytrack", std::clamp((float)settings.value("keytrack", 0.0f), -1.0f, 1.0f));
    setControl("filter_1_osc1_input", 1.0f);
    setControl("filter_1_osc2_input", 1.0f);
    setControl("filter_1_osc3_input", 1.0f);
    setControl("filter_1_sample_input", 1.0f);

    // Filter style & blend mapping (Helm: style 0=12dB, 1=24dB, 2=Shelf; blend 0=LP, 1=BP, 2=HP)
    int filter_style = settings.value("filter_style", 0);
    float filter_blend = std::clamp((float)settings.value("filter_blend", 0.0f), 0.0f, 2.0f);
    setControl("filter_1_model", 0.0f); // Analog
    setControl("filter_1_style", (filter_style == 1) ? 1.0f : 0.0f); // 0=12dB, 1=24dB
    setControl("filter_1_blend", filter_blend);

    // Delay
    setControl("delay_on", settings.value("delay_on", 0.0f));
    setControl("delay_dry_wet", settings.value("delay_dry_wet", 0.3f));
    setControl("delay_feedback", settings.value("delay_feedback", 0.5f));
    setControl("delay_sync", settings.value("delay_sync", 1.0f));
    setControl("delay_tempo", settings.value("delay_tempo", 7.0f));

    // Reverb
    setControl("reverb_on", settings.value("reverb_on", 0.0f));
    setControl("reverb_dry_wet", settings.value("reverb_dry_wet", 0.3f));
    setControl("reverb_decay_time", settings.value("reverb_feedback", 0.5f));
    setControl("reverb_high_shelf_gain", -6.0f * settings.value("reverb_damping", 0.5f));

    // Distortion
    setControl("distortion_on", settings.value("distortion_on", 0.0f));
    setControl("distortion_drive", settings.value("distortion_drive", 0.0f));
    setControl("distortion_mix", settings.value("distortion_mix", 1.0f));

    // Master Volume: Vial uses kSquareRoot scale with -80 dB offset: val = (dB + 80)^2, max 7399.44 (+6 dB)
    float master_vol = settings.value("volume", 0.7f);
    float db = (master_vol > 0.0001f) ? (20.0f * log10f(master_vol)) : -80.0f;
    float clamped_db = std::clamp(db, -80.0f, 6.0f);
    float vial_vol = std::clamp((clamped_db + 80.0f) * (clamped_db + 80.0f), 0.0f, 7399.4404f);
    setControl("volume", vial_vol);

    // Polyphony, Portamento, Legato, Pitch Bend, Velocity Track
    setControl("polyphony", (float)std::clamp((int)settings.value("polyphony", 8.0f), 1, 32));
    int portamento_type = (int)settings.value("portamento_type", 0.0f);
    float portamento = settings.value("portamento", -10.0f);
    if (portamento_type == 0) { // Off
      setControl("portamento_time", -10.0f);
      setControl("portamento_force", 0.0f);
    } else if (portamento_type == 1) { // Auto (legato only)
      setControl("portamento_time", std::clamp(portamento, -10.0f, 4.0f));
      setControl("portamento_force", 0.0f);
    } else { // Always glide
      setControl("portamento_time", std::clamp(portamento, -10.0f, 4.0f));
      setControl("portamento_force", 1.0f);
    }
    setControl("legato", settings.value("legato", 0.0f));
    setControl("pitch_bend_range", std::clamp((float)settings.value("pitch_bend_range", 2.0f), 0.0f, 48.0f));
    setControl("velocity_track", std::clamp((float)settings.value("velocity_track", 0.0f), -1.0f, 1.0f));

    // Modulation routings (validated before connecting!)
    clearModulations();

    float fil_env_depth = settings.value("fil_env_depth", 0.0f);
    if (std::abs(fil_env_depth) > 0.001f) {
      connectModulationSafe("env_2", "filter_1_cutoff", fil_env_depth / 64.0f);
    }

    if (settings.count("modulations") && settings["modulations"].is_array()) {
      for (const auto &m : settings["modulations"]) {
        std::string src = m.value("source", "");
        std::string dst = m.value("destination", "");
        float amount = m.value("amount", 0.0f);

        std::string vial_src = translateModSource(src);
        std::string vial_dst = translateModDest(dst);

        if (!vial_src.empty() && !vial_dst.empty()) {
          float scaled_amount = amount;
          if (vial_src == "env_1" && (vial_dst == "osc_1_level" || vial_dst == "osc_2_level") && amount < -0.5f) {
            scaled_amount = amount * 0.45f;
          } else if (vial_dst == "osc_1_distortion_amount") {
            scaled_amount = amount * 0.5f;
          }
          connectModulationSafe(vial_src, vial_dst, scaled_amount);
        }
      }
    }

    processModulationChanges();
    engine_->updateAllModulationSwitches();

    setPresetName(file.getFileNameWithoutExtension());
    active_file_ = file;
    return true;
  }

  bool loadSurgePreset(const juce::File &file, std::string &error) {
    if (!file.existsAsFile()) {
      error = "File does not exist: " + file.getFullPathName().toStdString();
      return false;
    }

    juce::MemoryBlock mb;
    if (!file.loadFileAsData(mb) || mb.getSize() < 100) {
      error = "Failed to read Surge FXP file";
      return false;
    }

    const char *raw = static_cast<const char *>(mb.getData());
    size_t len = mb.getSize();
    const char *xml_start = nullptr;
    for (size_t i = 0; i + 5 < len; ++i) {
      if (raw[i] == '<' && raw[i + 1] == '?' && raw[i + 2] == 'x' &&
          raw[i + 3] == 'm' && raw[i + 4] == 'l') {
        xml_start = raw + i;
        break;
      }
    }
    if (!xml_start) {
      error = "No XML payload found in Surge FXP";
      return false;
    }

    const char *xml_end = nullptr;
    const char *end_tag = "</patch>";
    size_t start_off = (size_t)(xml_start - raw);
    for (size_t i = start_off; i + 8 <= len; ++i) {
      if (memcmp(raw + i, end_tag, 8) == 0) {
        xml_end = raw + i + 8;
        break;
      }
    }
    if (!xml_end) {
      error = "Incomplete XML payload in Surge FXP";
      return false;
    }

    juce::String xml_str = juce::String::fromUTF8(xml_start, (int)(xml_end - xml_start));
    std::unique_ptr<juce::XmlElement> root(juce::XmlDocument::parse(xml_str));
    if (!root) {
      error = "XML parse error in Surge FXP";
      return false;
    }

    juce::XmlElement *params_el = root->getChildByName("parameters");
    if (!params_el) {
      error = "Missing <parameters> in Surge FXP";
      return false;
    }

    struct SurgeMod {
      int source;
      std::string target;
      float depth;
    };
    std::map<std::string, float> sp;
    std::vector<SurgeMod> smods;

    for (auto *el : params_el->getChildIterator()) {
      std::string tag = el->getTagName().toStdString();
      if (el->hasAttribute("value")) {
        sp[tag] = (float)el->getDoubleAttribute("value", 0.0);
      }
      for (auto *mod : el->getChildWithTagNameIterator("modrouting")) {
        SurgeMod m;
        m.source = mod->getIntAttribute("source", 0);
        m.target = tag;
        m.depth = (float)mod->getDoubleAttribute("depth", 0.0);
        smods.push_back(m);
      }
    }

    auto getP = [&](const std::string &k, float def = 0.0f) -> float {
      auto it = sp.find(k);
      return (it != sp.end()) ? it->second : def;
    };

    std::string sc = (getP("scene_active", 0.0f) > 0.5f) ? "b_" : "a_";
    float scene_oct = getP(sc + "octave", 0.0f) * 12.0f;

    juce::ScopedLock lock(getCriticalSection());
    loadInitPreset();

    // 3 Oscillators (a_osc1..3 -> osc_1..3), consuming any embedded Surge wavetable chunks after </patch>
    const uint8_t *wt_cursor = reinterpret_cast<const uint8_t *>(xml_end);
    size_t wt_remaining = (size_t)((raw + len) - xml_end);
    float noise_level = 0.0f;
    for (int o = 1; o <= 3; ++o) {
      std::string okey = sc + "osc" + std::to_string(o);
      std::string vkey = "osc_" + std::to_string(o);
      bool muted = (getP(sc + "mute_o" + std::to_string(o), (o == 1) ? 0.0f : 1.0f) > 0.5f);
      float level = std::clamp(getP(sc + "level_o" + std::to_string(o), (o == 1) ? 1.0f : 0.0f), 0.0f, 1.0f);
      int otype = (int)std::round(getP(okey + "_type", 0.0f));

      bool loaded_wt = false;
      if ((otype == 2 || otype == 7) && wt_remaining >= 12) {
        size_t consumed = 0;
        float morph = std::clamp(getP(okey + "_param0", 0.0f), 0.0f, 1.0f);
        if (loadSurgeWavetableIntoOsc(o - 1, wt_cursor, wt_remaining, consumed, morph)) {
          wt_cursor += consumed;
          wt_remaining -= consumed;
          loaded_wt = true;
        }
      }

      if (!muted && level > 0.001f && otype == 3) {
        // S&H Noise oscillator -> route to sample noise
        noise_level = std::max(noise_level, level * 0.5f);
        setControl(vkey + "_on", 0.0f);
        continue;
      }

      if (!loaded_wt) {
        int helm_wave = 3; // Default Saw
        if (otype == 1 || otype == 5 || otype == 6) {
          helm_wave = 0; // Sine / FM2 / FM3
        } else if (otype == 0) {
          // Classic osc: param0 is shape (-1..1: tri -> saw -> square)
          float shape = getP(okey + "_param0", 0.0f);
          if (shape < -0.35f) helm_wave = 1;      // Triangle
          else if (shape > 0.35f) helm_wave = 2;  // Square
          else helm_wave = 3;                     // Saw
        } else if (otype == 9) {
          helm_wave = 1; // String -> Triangle
        }
        setOscillatorShape(o - 1, helm_wave);
      }

      setControl(vkey + "_on", (!muted && level > 0.001f) ? 1.0f : 0.0f);
      setControl(vkey + "_level", std::sqrt(level));

      float oct = getP(okey + "_octave", 0.0f) * 12.0f + scene_oct;
      float pitch = getP(okey + "_pitch", 0.0f);
      int semi = (int)std::round(oct + pitch);
      float fine = (oct + pitch) - (float)semi;
      setControl(vkey + "_transpose", std::clamp((float)semi, -48.0f, 48.0f));
      setControl(vkey + "_tune", std::clamp(fine, -1.0f, 1.0f));

      int uni_voices = std::clamp((int)std::round(getP(okey + "_param6", 1.0f)), 1, 16);
      float uni_detune = std::clamp(getP(okey + "_param5", 0.1f) * 8.0f, 0.0f, 10.0f);
      setControl(vkey + "_unison_voices", (float)uni_voices);
      setControl(vkey + "_unison_detune", uni_detune);

      if ((otype == 5 || otype == 6) && o == 1) {
        float fm_amt = std::clamp(std::abs(getP(okey + "_param0", 0.0f)) * 0.4f, 0.0f, 0.45f);
        if (fm_amt > 0.01f) {
          setControl("osc_1_distortion_type", 7.0f);
          setControl("osc_1_distortion_amount", fm_amt);
        }
      }
    }

    // Scene Noise -> Vial Sample
    bool noise_muted = (getP(sc + "mute_noise", 1.0f) > 0.5f);
    if (!noise_muted) {
      noise_level = std::max(noise_level, std::clamp(getP(sc + "level_noise", 0.0f), 0.0f, 1.0f));
    }
    setControl("sample_on", noise_level > 0.001f ? 1.0f : 0.0f);
    setControl("sample_level", noise_level);

    // Envelopes: Surge env1 = Amp EG -> Vial env_1; Surge env2 = Filter EG -> Vial env_2
    auto surgeEnvToVial = [](float log2_sec) -> float {
      if (log2_sec <= -7.99f) return 0.0f;
      float sec = std::pow(2.0f, std::clamp(log2_sec, -8.0f, 5.0f));
      return std::clamp(std::pow(sec, 0.25f), 0.0f, 2.37842f);
    };
    setControl("env_1_attack", surgeEnvToVial(getP(sc + "env1_attack", -8.0f)));
    setControl("env_1_decay", surgeEnvToVial(getP(sc + "env1_decay", 0.0f)));
    setControl("env_1_sustain", std::clamp(getP(sc + "env1_sustain", 1.0f), 0.0f, 1.0f));
    setControl("env_1_release", surgeEnvToVial(getP(sc + "env1_release", -2.0f)));

    setControl("env_2_attack", surgeEnvToVial(getP(sc + "env2_attack", -8.0f)));
    setControl("env_2_decay", surgeEnvToVial(getP(sc + "env2_decay", 0.0f)));
    setControl("env_2_sustain", std::clamp(getP(sc + "env2_sustain", 0.5f), 0.0f, 1.0f));
    setControl("env_2_release", surgeEnvToVial(getP(sc + "env2_release", -2.0f)));

    // Dual Filters (a_filter1 / a_filter2 -> filter_1 / filter_2)
    auto mapSurgeFilter = [&](int fidx) {
      std::string fk = sc + "filter" + std::to_string(fidx);
      std::string vk = "filter_" + std::to_string(fidx);
      int ftype = (int)std::round(getP(fk + "_type", 0.0f));
      if (ftype <= 0) {
        setControl(vk + "_on", 0.0f);
        return;
      }
      setControl(vk + "_on", 1.0f);
      float cutoff = std::clamp(69.0f + getP(fk + "_cutoff", 0.0f), 8.0f, 136.0f);
      float res = std::clamp(getP(fk + "_resonance", 0.0f), 0.0f, 0.92f);
      float kt = std::clamp(getP(fk + "_keytrack", 0.0f), -1.0f, 1.0f);
      setControl(vk + "_cutoff", cutoff);
      setControl(vk + "_resonance", res);
      setControl(vk + "_keytrack", kt);
      setControl(vk + "_osc1_input", 1.0f);
      setControl(vk + "_osc2_input", 1.0f);
      setControl(vk + "_osc3_input", 1.0f);
      setControl(vk + "_sample_input", 1.0f);
      setControl(vk + "_model", 0.0f); // Analog
      // Surge filter types: 1/2=LP12/24, 3/4=HP12/24, 5/6=BP12/24
      if (ftype == 3 || ftype == 4 || ftype == 21) {
        setControl(vk + "_blend", 2.0f); // HP
      } else if (ftype == 5 || ftype == 6 || ftype == 22) {
        setControl(vk + "_blend", 1.0f); // BP
      } else {
        setControl(vk + "_blend", 0.0f); // LP
      }
      setControl(vk + "_style", (ftype == 2 || ftype == 4 || ftype == 6) ? 1.0f : 0.0f);
    };
    mapSurgeFilter(1);
    mapSurgeFilter(2);

    // LFOs 1-3 (a_lfo1..3 -> lfo_1..3)
    for (int l = 1; l <= 3; ++l) {
      std::string lk = sc + "lfo" + std::to_string(l);
      std::string vk = "lfo_" + std::to_string(l);
      setControl(vk + "_frequency", std::clamp(getP(lk + "_rate", 0.0f), -7.0f, 9.0f));
      setControl(vk + "_sync", 0.0f); // Seconds/Hz
      int trig = (int)std::round(getP(lk + "_trigmode", 1.0f));
      setControl(vk + "_sync_type", (trig == 0) ? 1.0f : 0.0f);
    }

    // FX Slots (fx1..fx8 -> Vial FX chain)
    if (getP("fx_bypass", 0.0f) < 0.5f) {
      for (int slot = 1; slot <= 8; ++slot) {
        std::string fxk = "fx" + std::to_string(slot);
        int fx_type = (int)std::round(getP(fxk + "_type", 0.0f));
        if (fx_type == 1) { // Delay
          setControl("delay_on", 1.0f);
          setControl("delay_dry_wet", std::clamp(getP(fxk + "_p10", 0.25f), 0.0f, 0.8f));
          setControl("delay_feedback", std::clamp(getP(fxk + "_p2", 0.35f), 0.0f, 0.85f));
        } else if (fx_type == 2 || fx_type == 11) { // Reverb 1 / 2
          setControl("reverb_on", 1.0f);
          setControl("reverb_dry_wet", std::clamp(getP(fxk + "_p10", 0.25f), 0.0f, 0.8f));
        } else if (fx_type == 3) { // Phaser
          setControl("phaser_on", 1.0f);
          setControl("phaser_dry_wet", std::clamp(getP(fxk + "_p10", 0.4f), 0.0f, 0.8f));
        } else if (fx_type == 4) { // Rotary Speaker (Leslie vibrato/Doppler) -> Vial Chorus
          setControl("chorus_on", 1.0f);
          setControl("chorus_sync", 0.0f);
          setControl("chorus_frequency", std::clamp(getP(fxk + "_p0", 2.0f), -6.0f, 3.0f));
          setControl("chorus_mod_depth", 0.65f);
          setControl("chorus_dry_wet", 0.7f);
          setControl("chorus_voices", 1.0f);
        } else if (fx_type == 5) { // Distortion
          setControl("distortion_on", 1.0f);
          setControl("distortion_drive", std::clamp(getP(fxk + "_p3", 6.0f), 0.0f, 20.0f));
          setControl("distortion_mix", std::clamp(getP(fxk + "_p10", 0.5f), 0.0f, 1.0f));
        } else if (fx_type == 9) { // Chorus
          setControl("chorus_on", 1.0f);
          setControl("chorus_dry_wet", std::clamp(getP(fxk + "_p10", 0.35f), 0.0f, 0.8f));
        } else if (fx_type == 12) { // Flanger
          setControl("flanger_on", 1.0f);
          setControl("flanger_dry_wet", std::clamp(getP(fxk + "_p10", 0.35f), 0.0f, 0.8f));
        }
      }
    }

    // Master Volume, Polyphony, Portamento, Legato
    float scene_vol = std::clamp(getP(sc + "volume", 0.85f), 0.05f, 1.5f);
    float global_db = std::clamp(getP("volume", 0.0f), -48.0f, 6.0f);
    float total_db = std::clamp(global_db + 20.0f * log10f(scene_vol), -80.0f, 6.0f);
    float vial_vol = std::clamp((total_db + 80.0f) * (total_db + 80.0f), 0.0f, 7399.4404f);
    setControl("volume", vial_vol);

    int polymode = (int)std::round(getP(sc + "polymode", 0.0f));
    int polylimit = std::clamp((int)std::round(getP("polylimit", 8.0f)), 1, 32);
    if (polymode == 1 || polymode == 2 || polymode == 4) {
      setControl("polyphony", 1.0f);
      setControl("legato", (polymode == 2 || polymode == 4) ? 1.0f : 0.0f);
    } else {
      setControl("polyphony", (float)polylimit);
      setControl("legato", 0.0f);
    }

    float porta = getP(sc + "portamento", -8.0f);
    if (porta <= -7.95f) {
      setControl("portamento_time", -10.0f);
      setControl("portamento_force", 0.0f);
    } else {
      setControl("portamento_time", std::clamp(porta, -10.0f, 4.0f));
      setControl("portamento_force", (polymode == 1) ? 1.0f : 0.0f);
    }

    // Modulations: Filter Env Depth + XML <modrouting> entries
    clearModulations();
    float f1_envmod = getP(sc + "filter1_envmod", 0.0f);
    if (std::abs(f1_envmod) > 0.1f) {
      connectModulationSafe("env_2", "filter_1_cutoff", std::clamp(f1_envmod / 128.0f, -1.0f, 1.0f));
    }
    float f2_envmod = getP(sc + "filter2_envmod", 0.0f);
    if (std::abs(f2_envmod) > 0.1f) {
      connectModulationSafe("env_2", "filter_2_cutoff", std::clamp(f2_envmod / 128.0f, -1.0f, 1.0f));
    }

    for (const auto &m : smods) {
      std::string vsrc = translateSurgeModSource(m.source);
      std::string vdst = translateSurgeModDest(sc, m.target);
      if (!vsrc.empty() && !vdst.empty() && std::abs(m.depth) > 0.001f) {
        float amt = m.depth;
        if (vdst == "filter_1_cutoff" || vdst == "filter_2_cutoff")
          amt = std::clamp(m.depth / 128.0f, -1.0f, 1.0f);
        else
          amt = std::clamp(m.depth, -1.0f, 1.0f);
        connectModulationSafe(vsrc, vdst, amt);
      }
    }

    processModulationChanges();
    engine_->updateAllModulationSwitches();

    setPresetName(file.getFileNameWithoutExtension());
    active_file_ = file;
    return true;
  }

private:
  static std::string translateModSource(const std::string &helm_src) {
    if (helm_src == "fil_envelope") return "env_2";
    if (helm_src == "amp_envelope") return "env_1";
    if (helm_src == "mod_envelope") return "env_3";
    if (helm_src == "poly_lfo")     return "lfo_1";
    if (helm_src == "mono_lfo_1")   return "lfo_2";
    if (helm_src == "mono_lfo_2")   return "lfo_3";
    if (helm_src == "mod_wheel")    return "mod_wheel";
    if (helm_src == "pitch_wheel")  return "pitch_wheel";
    if (helm_src == "aftertouch")   return "aftertouch";
    if (helm_src == "velocity")     return "velocity";
    return "";
  }

  static std::string translateModDest(const std::string &helm_dst) {
    if (helm_dst == "cutoff")           return "filter_1_cutoff";
    if (helm_dst == "resonance")        return "filter_1_resonance";
    if (helm_dst == "volume")           return "volume";
    if (helm_dst == "osc_1_volume")     return "osc_1_level";
    if (helm_dst == "osc_2_volume")     return "osc_2_level";
    if (helm_dst == "sub_volume")       return "osc_3_level";
    if (helm_dst == "cross_modulation") return "osc_1_distortion_amount";
    if (helm_dst == "delay_dry_wet")    return "delay_dry_wet";
    if (helm_dst == "reverb_dry_wet")   return "reverb_dry_wet";
    if (helm_dst == "distortion_drive") return "distortion_drive";
    return "";
  }

  static std::string translateSurgeModSource(int ms) {
    switch (ms) {
      case 1: return "velocity";
      case 2: return "note";
      case 3:
      case 4: return "aftertouch";
      case 5: return "pitch_wheel";
      case 6: return "mod_wheel";
      case 7: return "macro_control_1";
      case 8: return "macro_control_2";
      case 9: return "macro_control_3";
      case 10: return "macro_control_4";
      case 15: return "env_1";
      case 16: return "env_2";
      case 17: return "lfo_1";
      case 18: return "lfo_2";
      case 19: return "lfo_3";
      default: return "";
    }
  }

  static std::string translateSurgeModDest(const std::string &sc, const std::string &target) {
    if (target == sc + "filter1_cutoff")    return "filter_1_cutoff";
    if (target == sc + "filter1_resonance") return "filter_1_resonance";
    if (target == sc + "filter2_cutoff")    return "filter_2_cutoff";
    if (target == sc + "filter2_resonance") return "filter_2_resonance";
    if (target == sc + "volume")            return "volume";
    if (target == sc + "level_o1")          return "osc_1_level";
    if (target == sc + "level_o2")          return "osc_2_level";
    if (target == sc + "level_o3")          return "osc_3_level";
    if (target == sc + "osc1_pitch")        return "osc_1_tune";
    if (target == sc + "osc2_pitch")        return "osc_2_tune";
    if (target == sc + "osc3_pitch")        return "osc_3_tune";
    return "";
  }

  double current_time_seconds_;
  int silent_blocks_;
};

/* =====================================================================
 * Instance structure
 * ===================================================================== */
typedef struct vial_instance_t {
  char module_dir[256];
  char error_msg[256];

  VialMoveInstance *synth;
  VialMoveInstance *synth_aux; /* Secondary voice engine running on Core 2 */

  int current_preset;
  int preset_count;
  int octave_transpose;
  float output_gain;
  float bpm;
  char preset_name[64];

  /* Preset list (sorted by category then name) */
  struct patch_entry {
    juce::File file;
    std::string category;
    std::string name;
  };
  std::vector<patch_entry> patches;

  /* Patch Categories (e.g., Vial-ESW - Origins, Vial-GM Feb-25-Bass, Helm-Bass) */
  struct category_entry {
    char name[64];
    int first_idx;
  };
  std::vector<category_entry> categories;

  /* Thread-safe MIDI queues (engine 0 on Core 3, engine 1 on Core 2) */
  juce::CriticalSection midi_lock;
  juce::MidiBuffer midi_queue;
  juce::MidiBuffer midi_queue_aux;
  int8_t note_owner[128];
  int held_count[2];

  /* CPU optimization settings (Move defaults: 1x oversampling, 4 voices, 3 unison, multi-core on) */
  int oversampling;
  int max_polyphony;
  int max_unison;
  bool fx_bypass;
  bool multi_core;
  float saved_fx_on[7];

  /* Pre-allocated audio render buffers (avoids per-block heap allocation) */
  juce::AudioSampleBuffer float_buffer;
  juce::AudioSampleBuffer float_buffer_aux;
  juce::MidiBuffer local_midi_aux;

  /* Persistent Core 2 worker thread for synth_aux (SCHED_OTHER, explicit sched) */
  pthread_t worker_thread;
  bool worker_started;
  pthread_mutex_t worker_mutex;
  pthread_cond_t worker_cond;
  pthread_cond_t worker_done_cond;
  bool worker_has_work;
  bool worker_exit;
  int worker_frames;

  /* Auto BPM Sync */
  bool sync_bpm;
  double last_clock_time;
  int clock_count;
  double clock_time_sum;

  /* Pre-built JSON strings */
  char *ui_hierarchy_json;
  char *chain_params_json;

  vial_instance_t()
      : float_buffer(2, MOVE_FRAMES_PER_BLOCK),
        float_buffer_aux(2, MOVE_FRAMES_PER_BLOCK) {
    memset(module_dir, 0, sizeof(module_dir));
    memset(error_msg, 0, sizeof(error_msg));
    synth = nullptr;
    synth_aux = nullptr;
    current_preset = 0;
    preset_count = 0;
    octave_transpose = 0;
    output_gain = 0.8f;
    bpm = 120.0f;
    memset(preset_name, 0, sizeof(preset_name));
    memset(note_owner, -1, sizeof(note_owner));
    held_count[0] = 0;
    held_count[1] = 0;
    oversampling = 0;   /* 1x (Draft) */
    max_polyphony = 4;  /* Cap polyphony (split across 2 cores when multi_core=true) */
    max_unison = 3;     /* Cap unison voices per osc */
    fx_bypass = false;
    multi_core = false; /* Default off: 1 core is faster up to 4 voices due to futex wake overhead; turn On for 6-16 voices */
    memset(saved_fx_on, 0, sizeof(saved_fx_on));
    worker_started = false;
    pthread_mutex_init(&worker_mutex, nullptr);
    pthread_cond_init(&worker_cond, nullptr);
    pthread_cond_init(&worker_done_cond, nullptr);
    worker_has_work = false;
    worker_exit = false;
    worker_frames = MOVE_FRAMES_PER_BLOCK;
    sync_bpm = true;
    last_clock_time = 0.0;
    clock_count = 0;
    clock_time_sum = 0.0;
    ui_hierarchy_json = nullptr;
    chain_params_json = nullptr;
  }

  ~vial_instance_t() {
    pthread_cond_destroy(&worker_done_cond);
    pthread_cond_destroy(&worker_cond);
    pthread_mutex_destroy(&worker_mutex);
  }
} vial_instance_t;

static void *aux_worker_main(void *arg) {
  vial_instance_t *inst = (vial_instance_t *)arg;
#ifdef __linux__
  pthread_setname_np(pthread_self(), "vial-dsp-1");
  /* Pin to Core 2 (Move SPI Audio Main is on Core 3) */
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(2, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

  pthread_mutex_lock(&inst->worker_mutex);
  while (!inst->worker_exit) {
    while (!inst->worker_has_work && !inst->worker_exit) {
      pthread_cond_wait(&inst->worker_cond, &inst->worker_mutex);
    }
    if (inst->worker_exit)
      break;

    int frames = inst->worker_frames;
    pthread_mutex_unlock(&inst->worker_mutex);

    if (inst->synth_aux) {
      if (inst->float_buffer_aux.getNumSamples() < frames) {
        inst->float_buffer_aux.setSize(2, frames, false, false, true);
      }
      inst->float_buffer_aux.clear();
      inst->synth_aux->process(&inst->float_buffer_aux, inst->local_midi_aux, frames);
    }

    pthread_mutex_lock(&inst->worker_mutex);
    inst->worker_has_work = false;
    pthread_cond_signal(&inst->worker_done_cond);
  }
  pthread_mutex_unlock(&inst->worker_mutex);
  return nullptr;
}

static void start_aux_worker(vial_instance_t *inst) {
  if (inst->worker_started)
    return;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  /* Explicitly use SCHED_OTHER so we never inherit SPI's SCHED_FIFO 90 */
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
  if (pthread_create(&inst->worker_thread, &attr, aux_worker_main, inst) == 0) {
    inst->worker_started = true;
    plugin_log("Started Core 2 worker thread (vial-dsp-1, SCHED_OTHER)");
  } else {
    plugin_log("Warning: failed to create Core 2 worker thread; falling back to single-core");
    inst->multi_core = false;
  }
  pthread_attr_destroy(&attr);
}

static void stop_aux_worker(vial_instance_t *inst) {
  if (!inst->worker_started)
    return;
  pthread_mutex_lock(&inst->worker_mutex);
  inst->worker_exit = true;
  pthread_cond_signal(&inst->worker_cond);
  pthread_mutex_unlock(&inst->worker_mutex);
  pthread_join(inst->worker_thread, nullptr);
  inst->worker_started = false;
}

/* =====================================================================
 * CPU-saver limits applied after preset load or setting change
 * ===================================================================== */
static const char *kFxOnKeys[7] = {
    "reverb_on", "delay_on", "chorus_on", "flanger_on",
    "phaser_on", "distortion_on", "compressor_on"
};

static void snapshot_fx_state(vial_instance_t *inst) {
  if (!inst || !inst->synth)
    return;
  for (int i = 0; i < 7; ++i) {
    inst->saved_fx_on[i] = inst->synth->getControl(kFxOnKeys[i], 0.0f);
  }
}

static void set_control_both(vial_instance_t *inst, const std::string &key, float val) {
  if (inst->synth)
    inst->synth->setControl(key, val);
  if (inst->synth_aux)
    inst->synth_aux->setControl(key, val);
}

static void apply_fx_bypass(vial_instance_t *inst) {
  if (!inst || !inst->synth)
    return;
  for (int i = 0; i < 7; ++i) {
    set_control_both(inst, kFxOnKeys[i], inst->fx_bypass ? 0.0f : inst->saved_fx_on[i]);
  }
}

static void apply_cpu_limits(vial_instance_t *inst) {
  if (!inst || !inst->synth)
    return;

  inst->synth->applyOversampling(inst->oversampling);
  if (inst->synth_aux)
    inst->synth_aux->applyOversampling(inst->oversampling);

  /* Disable per-unison-pair spectral IFFTs (massive CPU win on spectral-morph pads) */
  set_control_both(inst, "osc_1_spectral_unison", 0.0f);
  set_control_both(inst, "osc_2_spectral_unison", 0.0f);
  set_control_both(inst, "osc_3_spectral_unison", 0.0f);

  float cur_poly = inst->synth->getControl("polyphony", (float)inst->max_polyphony);
  int target_poly = std::min((int)std::round(cur_poly), inst->max_polyphony);
  if (target_poly < 1) target_poly = 1;
  if (inst->multi_core && inst->synth_aux && target_poly > 2) {
    int p0 = (target_poly + 1) / 2;
    int p1 = target_poly - p0;
    inst->synth->setControl("polyphony", (float)p0);
    inst->synth_aux->setControl("polyphony", (float)p1);
  } else {
    inst->synth->setControl("polyphony", (float)target_poly);
    if (inst->synth_aux)
      inst->synth_aux->setControl("polyphony", (float)target_poly);
  }

  const char *unison_keys[3] = {
      "osc_1_unison_voices", "osc_2_unison_voices", "osc_3_unison_voices"};
  for (int i = 0; i < 3; ++i) {
    float cur_u = inst->synth->getControl(unison_keys[i], 1.0f);
    if (cur_u > (float)inst->max_unison) {
      set_control_both(inst, unison_keys[i], (float)inst->max_unison);
    }
  }

  if (inst->fx_bypass) {
    apply_fx_bypass(inst);
  }
}

/* =====================================================================
 * Category formatting helper (Helm-Bass, Helm-Keys, Vial-XXX)
 * ===================================================================== */
static std::string format_category_name(const juce::File &file) {
  juce::String ext = file.getFileExtension().toLowerCase();
  if (ext == ".helm") {
    std::string dir_name = file.getParentDirectory().getFileName().toStdString();
    if (dir_name.empty() || dir_name == "patches")
      dir_name = "Factory";
    return "Helm-" + dir_name;
  }

  if (ext == ".fxp") {
    std::string dir_name = file.getParentDirectory().getFileName().toStdString();
    if (dir_name.empty() || dir_name == "patches" || dir_name == "surge")
      dir_name = "Factory";
    return "Surge-" + dir_name;
  }

  /* Native .vital / .vial presets:
   * - <Bank>/Presets/<Preset>.vital            -> Vial-<Bank>
   * - <Bank>/Presets/<Pack>/<Cat>/<Preset>.vital -> Vial-<Pack>-<Cat>
   * - <Cat>/<Preset>.vital                     -> Vial-<Cat>
   */
  juce::File parent = file.getParentDirectory();
  std::string dir_name = parent.getFileName().toStdString();
  if (dir_name == "Presets" || dir_name == "presets") {
    std::string bank_name = parent.getParentDirectory().getFileName().toStdString();
    if (!bank_name.empty() && bank_name != "patches")
      return "Vial-" + bank_name;
    return "Vial-Factory";
  }

  juce::File grand = parent.getParentDirectory();
  std::string grand_name = grand.getFileName().toStdString();
  if (!grand_name.empty() && grand_name != "patches" &&
      grand_name != "Presets" && grand_name != "presets" &&
      grand_name != "Factory Presets") {
    return "Vial-" + grand_name + "-" + dir_name;
  }

  if (dir_name.empty() || dir_name == "patches")
    dir_name = "Factory";
  return "Vial-" + dir_name;
}

/* =====================================================================
 * Preset loading
 * ===================================================================== */
static void load_preset_by_index(vial_instance_t *inst, int idx) {
  if (!inst || !inst->synth || idx < 0 || idx >= inst->preset_count) {
    char msg[128];
    snprintf(msg, sizeof(msg), "load_preset_by_index ignored: idx=%d count=%d",
             idx, inst ? inst->preset_count : -1);
    plugin_log(msg);
    return;
  }

  const auto &entry = inst->patches[idx];
  const juce::File &file = entry.file;

  char start_msg[256];
  snprintf(start_msg, sizeof(start_msg), "Loading preset [%d] (%s): %s",
           idx, entry.category.c_str(), entry.name.c_str());
  plugin_log(start_msg);

  std::string error;
  bool ok = false;

  juce::String ext = file.getFileExtension().toLowerCase();
  if (ext == ".helm") {
    ok = inst->synth->loadHelmPreset(file, error);
    if (ok && inst->synth_aux) {
      std::string err2;
      inst->synth_aux->loadHelmPreset(file, err2);
    }
  } else if (ext == ".fxp") {
    ok = inst->synth->loadSurgePreset(file, error);
    if (ok && inst->synth_aux) {
      std::string err2;
      inst->synth_aux->loadSurgePreset(file, err2);
    }
  } else {
    ok = inst->synth->loadNativePreset(file, error);
    if (ok && inst->synth_aux) {
      std::string err2;
      inst->synth_aux->loadNativePreset(file, err2);
    }
  }

  if (ok) {
    {
      juce::ScopedLock l(inst->midi_lock);
      memset(inst->note_owner, -1, sizeof(inst->note_owner));
      inst->held_count[0] = 0;
      inst->held_count[1] = 0;
    }
    snapshot_fx_state(inst);
    apply_cpu_limits(inst);
    inst->current_preset = idx;
    strncpy(inst->preset_name, entry.name.c_str(), sizeof(inst->preset_name) - 1);
    inst->preset_name[sizeof(inst->preset_name) - 1] = '\0';

    char msg[256];
    snprintf(msg, sizeof(msg), "Loaded preset [%d]: %s (%s)",
             idx, inst->preset_name, entry.category.c_str());
    plugin_log(msg);
  } else {
    char msg[512];
    snprintf(msg, sizeof(msg), "Failed to load preset [%d] %s: %s",
             idx, file.getFileName().toRawUTF8(), error.c_str());
    plugin_log(msg);
  }
}

/* =====================================================================
 * Scan presets recursively
 * ===================================================================== */
static void scan_directory_presets(const juce::File &dir,
                                   std::vector<vial_instance_t::patch_entry> &out_list) {
  juce::Array<juce::File> files = dir.findChildFiles(
      juce::File::findFiles, false, "*.vital;*.vial;*.helm;*.fxp");
  files.sort();
  for (int i = 0; i < files.size(); ++i) {
    vial_instance_t::patch_entry e;
    e.file = files[i];
    e.category = format_category_name(files[i]);
    e.name = files[i].getFileNameWithoutExtension().toStdString();
    out_list.push_back(e);
  }

  juce::Array<juce::File> subdirs = dir.findChildFiles(
      juce::File::findDirectories, false);
  subdirs.sort();
  for (int i = 0; i < subdirs.size(); ++i)
    scan_directory_presets(subdirs[i], out_list);
}

/* =====================================================================
 * JSON builders for ui_hierarchy and chain_params
 * ===================================================================== */
static void build_ui_hierarchy(vial_instance_t *inst) {
  std::string j =
      "{"
        "\"modes\":null,"
        "\"levels\":{"
          "\"root\":{"
            "\"list_param\":\"preset\","
            "\"count_param\":\"preset_count\","
            "\"name_param\":\"preset_name\","
            "\"children\":\"main\","
            "\"knobs\":[\"filter_1_cutoff\",\"filter_1_resonance\",\"filter_1_drive\",\"env_1_attack\","
              "\"env_1_decay\",\"env_1_sustain\",\"env_1_release\",\"volume\"],"
            "\"params\":["
              "{\"level\":\"category_jump\",\"label\":\"Jump to Category\"}"
            "]"
          "},"
          "\"main\":{"
            "\"children\":null,"
            "\"knobs\":[\"filter_1_cutoff\",\"filter_1_resonance\",\"filter_1_drive\",\"env_1_attack\","
              "\"env_1_decay\",\"env_1_sustain\",\"env_1_release\",\"volume\"],"
            "\"params\":["
              "{\"level\":\"category_jump\",\"label\":\"Jump to Category\"},"
              "{\"level\":\"osc1\",\"label\":\"Oscillator 1\"},"
              "{\"level\":\"osc2\",\"label\":\"Oscillator 2\"},"
              "{\"level\":\"osc3\",\"label\":\"Oscillator 3\"},"
              "{\"level\":\"sample\",\"label\":\"Sampler / Noise\"},"
              "{\"level\":\"filter1\",\"label\":\"Filter 1\"},"
              "{\"level\":\"filter2\",\"label\":\"Filter 2\"},"
              "{\"level\":\"env1\",\"label\":\"Amp Envelope\"},"
              "{\"level\":\"env2\",\"label\":\"Envelope 2\"},"
              "{\"level\":\"env3\",\"label\":\"Envelope 3\"},"
              "{\"level\":\"lfo1\",\"label\":\"LFO 1\"},"
              "{\"level\":\"lfo2\",\"label\":\"LFO 2\"},"
              "{\"level\":\"chorus\",\"label\":\"Chorus\"},"
              "{\"level\":\"flanger\",\"label\":\"Flanger\"},"
              "{\"level\":\"phaser\",\"label\":\"Phaser\"},"
              "{\"level\":\"distortion\",\"label\":\"Distortion\"},"
              "{\"level\":\"delay\",\"label\":\"Delay\"},"
              "{\"level\":\"reverb\",\"label\":\"Reverb\"},"
              "{\"level\":\"compressor\",\"label\":\"Compressor\"},"
              "{\"level\":\"macros\",\"label\":\"Macros\"},"
              "{\"level\":\"settings\",\"label\":\"Settings\"}"
            "]"
          "},"
          "\"category_jump\":{"
            "\"label\":\"Jump to Category\","
            "\"items_param\":\"category_list\","
            "\"select_param\":\"jump_to_category\","
            "\"navigate_to\":\"root\","
            "\"children\":null,"
            "\"knobs\":[],"
            "\"params\":[]"
          "},"
          "\"osc1\":{"
            "\"children\":null,"
            "\"knobs\":[\"osc_1_on\",\"osc_1_level\",\"osc_1_transpose\",\"osc_1_tune\","
              "\"osc_1_unison_voices\",\"osc_1_unison_detune\",\"osc_1_wave_frame\",\"osc_1_pan\"],"
            "\"params\":[\"osc_1_on\",\"osc_1_level\",\"osc_1_transpose\",\"osc_1_tune\",\"osc_1_pan\","
              "\"osc_1_unison_voices\",\"osc_1_unison_detune\",\"osc_1_wave_frame\","
              "\"osc_1_distortion_type\",\"osc_1_distortion_amount\","
              "\"osc_1_spectral_morph_type\",\"osc_1_spectral_morph_amount\",\"osc_1_destination\"]"
          "},"
          "\"osc2\":{"
            "\"children\":null,"
            "\"knobs\":[\"osc_2_on\",\"osc_2_level\",\"osc_2_transpose\",\"osc_2_tune\","
              "\"osc_2_unison_voices\",\"osc_2_unison_detune\",\"osc_2_wave_frame\",\"osc_2_pan\"],"
            "\"params\":[\"osc_2_on\",\"osc_2_level\",\"osc_2_transpose\",\"osc_2_tune\",\"osc_2_pan\","
              "\"osc_2_unison_voices\",\"osc_2_unison_detune\",\"osc_2_wave_frame\","
              "\"osc_2_distortion_type\",\"osc_2_distortion_amount\","
              "\"osc_2_spectral_morph_type\",\"osc_2_spectral_morph_amount\",\"osc_2_destination\"]"
          "},"
          "\"osc3\":{"
            "\"children\":null,"
            "\"knobs\":[\"osc_3_on\",\"osc_3_level\",\"osc_3_transpose\",\"osc_3_tune\","
              "\"osc_3_unison_voices\",\"osc_3_unison_detune\",\"osc_3_wave_frame\",\"osc_3_pan\"],"
            "\"params\":[\"osc_3_on\",\"osc_3_level\",\"osc_3_transpose\",\"osc_3_tune\",\"osc_3_pan\","
              "\"osc_3_unison_voices\",\"osc_3_unison_detune\",\"osc_3_wave_frame\","
              "\"osc_3_distortion_type\",\"osc_3_distortion_amount\","
              "\"osc_3_spectral_morph_type\",\"osc_3_spectral_morph_amount\",\"osc_3_destination\"]"
          "},"
          "\"sample\":{"
            "\"children\":null,"
            "\"knobs\":[\"sample_on\",\"sample_level\",\"sample_transpose\",\"sample_tune\","
              "\"sample_pan\",\"sample_keytrack\",\"sample_loop\",\"sample_destination\"],"
            "\"params\":[\"sample_on\",\"sample_level\",\"sample_transpose\",\"sample_tune\","
              "\"sample_pan\",\"sample_keytrack\",\"sample_loop\",\"sample_destination\"]"
          "},"
          "\"filter1\":{"
            "\"children\":null,"
            "\"knobs\":[\"filter_1_on\",\"filter_1_model\",\"filter_1_style\",\"filter_1_cutoff\","
              "\"filter_1_resonance\",\"filter_1_drive\",\"filter_1_blend\",\"filter_1_mix\"],"
            "\"params\":[\"filter_1_on\",\"filter_1_model\",\"filter_1_style\",\"filter_1_cutoff\","
              "\"filter_1_resonance\",\"filter_1_drive\",\"filter_1_blend\",\"filter_1_mix\","
              "\"filter_1_keytrack\",\"filter_1_osc1_input\",\"filter_1_osc2_input\","
              "\"filter_1_osc3_input\",\"filter_1_sample_input\"]"
          "},"
          "\"filter2\":{"
            "\"children\":null,"
            "\"knobs\":[\"filter_2_on\",\"filter_2_model\",\"filter_2_style\",\"filter_2_cutoff\","
              "\"filter_2_resonance\",\"filter_2_drive\",\"filter_2_blend\",\"filter_2_mix\"],"
            "\"params\":[\"filter_2_on\",\"filter_2_model\",\"filter_2_style\",\"filter_2_cutoff\","
              "\"filter_2_resonance\",\"filter_2_drive\",\"filter_2_blend\",\"filter_2_mix\","
              "\"filter_2_keytrack\",\"filter_2_osc1_input\",\"filter_2_osc2_input\","
              "\"filter_2_osc3_input\",\"filter_2_sample_input\",\"filter_2_filter_input\"]"
          "},"
          "\"env1\":{"
            "\"children\":null,"
            "\"knobs\":[\"env_1_attack\",\"env_1_decay\",\"env_1_sustain\",\"env_1_release\",\"env_1_delay\",\"env_1_hold\"],"
            "\"params\":[\"env_1_attack\",\"env_1_decay\",\"env_1_sustain\",\"env_1_release\",\"env_1_delay\",\"env_1_hold\"]"
          "},"
          "\"env2\":{"
            "\"children\":null,"
            "\"knobs\":[\"env_2_attack\",\"env_2_decay\",\"env_2_sustain\",\"env_2_release\",\"env_2_delay\",\"env_2_hold\"],"
            "\"params\":[\"env_2_attack\",\"env_2_decay\",\"env_2_sustain\",\"env_2_release\",\"env_2_delay\",\"env_2_hold\"]"
          "},"
          "\"env3\":{"
            "\"children\":null,"
            "\"knobs\":[\"env_3_attack\",\"env_3_decay\",\"env_3_sustain\",\"env_3_release\",\"env_3_delay\",\"env_3_hold\"],"
            "\"params\":[\"env_3_attack\",\"env_3_decay\",\"env_3_sustain\",\"env_3_release\",\"env_3_delay\",\"env_3_hold\"]"
          "},"
          "\"lfo1\":{"
            "\"children\":null,"
            "\"knobs\":[\"lfo_1_frequency\",\"lfo_1_sync\",\"lfo_1_tempo\",\"lfo_1_sync_type\",\"lfo_1_phase\",\"lfo_1_fade_time\",\"lfo_1_smooth_time\"],"
            "\"params\":[\"lfo_1_frequency\",\"lfo_1_sync\",\"lfo_1_tempo\",\"lfo_1_sync_type\",\"lfo_1_phase\",\"lfo_1_fade_time\",\"lfo_1_smooth_time\"]"
          "},"
          "\"lfo2\":{"
            "\"children\":null,"
            "\"knobs\":[\"lfo_2_frequency\",\"lfo_2_sync\",\"lfo_2_tempo\",\"lfo_2_sync_type\",\"lfo_2_phase\",\"lfo_2_fade_time\",\"lfo_2_smooth_time\"],"
            "\"params\":[\"lfo_2_frequency\",\"lfo_2_sync\",\"lfo_2_tempo\",\"lfo_2_sync_type\",\"lfo_2_phase\",\"lfo_2_fade_time\",\"lfo_2_smooth_time\"]"
          "},"
          "\"chorus\":{"
            "\"children\":null,"
            "\"knobs\":[\"chorus_on\",\"chorus_dry_wet\",\"chorus_feedback\",\"chorus_frequency\",\"chorus_mod_depth\",\"chorus_voices\"],"
            "\"params\":[\"chorus_on\",\"chorus_dry_wet\",\"chorus_feedback\",\"chorus_frequency\",\"chorus_mod_depth\",\"chorus_voices\"]"
          "},"
          "\"flanger\":{"
            "\"children\":null,"
            "\"knobs\":[\"flanger_on\",\"flanger_dry_wet\",\"flanger_feedback\",\"flanger_frequency\",\"flanger_mod_depth\",\"flanger_center\"],"
            "\"params\":[\"flanger_on\",\"flanger_dry_wet\",\"flanger_feedback\",\"flanger_frequency\",\"flanger_mod_depth\",\"flanger_center\"]"
          "},"
          "\"phaser\":{"
            "\"children\":null,"
            "\"knobs\":[\"phaser_on\",\"phaser_dry_wet\",\"phaser_feedback\",\"phaser_frequency\",\"phaser_mod_depth\",\"phaser_center\"],"
            "\"params\":[\"phaser_on\",\"phaser_dry_wet\",\"phaser_feedback\",\"phaser_frequency\",\"phaser_mod_depth\",\"phaser_center\"]"
          "},"
          "\"distortion\":{"
            "\"children\":null,"
            "\"knobs\":[\"distortion_on\",\"distortion_type\",\"distortion_drive\",\"distortion_mix\"],"
            "\"params\":[\"distortion_on\",\"distortion_type\",\"distortion_drive\",\"distortion_mix\"]"
          "},"
          "\"delay\":{"
            "\"children\":null,"
            "\"knobs\":[\"delay_on\",\"delay_dry_wet\",\"delay_feedback\",\"delay_frequency\",\"delay_sync\",\"delay_tempo\",\"delay_style\"],"
            "\"params\":[\"delay_on\",\"delay_dry_wet\",\"delay_feedback\",\"delay_frequency\",\"delay_sync\",\"delay_tempo\",\"delay_style\"]"
          "},"
          "\"reverb\":{"
            "\"children\":null,"
            "\"knobs\":[\"reverb_on\",\"reverb_dry_wet\",\"reverb_decay_time\",\"reverb_size\",\"reverb_high_shelf_gain\",\"reverb_low_shelf_gain\"],"
            "\"params\":[\"reverb_on\",\"reverb_dry_wet\",\"reverb_decay_time\",\"reverb_size\",\"reverb_high_shelf_gain\",\"reverb_low_shelf_gain\"]"
          "},"
          "\"compressor\":{"
            "\"children\":null,"
            "\"knobs\":[\"compressor_on\",\"compressor_mix\",\"compressor_attack\",\"compressor_release\",\"compressor_low_gain\",\"compressor_band_gain\",\"compressor_high_gain\"],"
            "\"params\":[\"compressor_on\",\"compressor_mix\",\"compressor_attack\",\"compressor_release\",\"compressor_low_gain\",\"compressor_band_gain\",\"compressor_high_gain\"]"
          "},"
          "\"macros\":{"
            "\"children\":null,"
            "\"knobs\":[\"macro_control_1\",\"macro_control_2\",\"macro_control_3\",\"macro_control_4\"],"
            "\"params\":[\"macro_control_1\",\"macro_control_2\",\"macro_control_3\",\"macro_control_4\"]"
          "},"
          "\"settings\":{"
            "\"children\":null,"
            "\"knobs\":[\"oversampling\",\"multi_core\",\"fft_cache\",\"max_polyphony\",\"max_unison\",\"fx_bypass\",\"volume\",\"bpm\"],"
            "\"params\":[\"oversampling\",\"multi_core\",\"fft_cache\",\"max_polyphony\",\"max_unison\",\"fx_bypass\",\"bpm\",\"sync_bpm\",\"volume\",\"polyphony\",\"portamento_time\",\"portamento_slope\",\"legato\",\"pitch_bend_range\",\"velocity_track\",\"voice_tune\"]"
          "}"
        "}"
      "}";

  free(inst->ui_hierarchy_json);
  inst->ui_hierarchy_json = strdup(j.c_str());

  char msg[128];
  snprintf(msg, sizeof(msg), "Built ui_hierarchy (%d bytes)", (int)j.size());
  plugin_log(msg);
}

static void build_chain_params(vial_instance_t *inst) {
  std::string j = "[";
  j += "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999}";
  j += ",{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3}";
  j += ",{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":20,\"max\":300}";
  j += ",{\"key\":\"sync_bpm\",\"name\":\"Auto BPM\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]}";
  j += ",{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.02}";
  j += ",{\"key\":\"oversampling\",\"name\":\"Oversampling\",\"type\":\"enum\",\"options\":[\"1x (Draft)\",\"2x\",\"4x\",\"8x\"]}";
  j += ",{\"key\":\"multi_core\",\"name\":\"Multi-Core\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]}";
  j += ",{\"key\":\"fft_cache\",\"name\":\"FFT Cache\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]}";
  j += ",{\"key\":\"max_polyphony\",\"name\":\"Max Polyphony\",\"type\":\"int\",\"min\":1,\"max\":16}";
  j += ",{\"key\":\"max_unison\",\"name\":\"Max Unison\",\"type\":\"int\",\"min\":1,\"max\":16}";
  j += ",{\"key\":\"fx_bypass\",\"name\":\"Bypass FX\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]}";

  size_t num_ui_params = sizeof(kUiVialParams) / sizeof(kUiVialParams[0]);
  for (size_t i = 0; i < num_ui_params; ++i) {
    std::string key = kUiVialParams[i];
    if (!vial::Parameters::isParameter(key)) {
      char warn[128];
      snprintf(warn, sizeof(warn), "Warning: UI param '%s' not in Vial Parameters", key.c_str());
      plugin_log(warn);
      continue;
    }

    const vial::ValueDetails &details = vial::Parameters::getDetails(key);
    std::string disp = details.display_name.empty() ? key : details.display_name;

    if (details.value_scale == vial::ValueDetails::kIndexed && details.string_lookup != nullptr) {
      int count = (int)std::round(details.max - details.min) + 1;
      j += ",{\"key\":\"" + json_escape(key) + "\",\"name\":\"" + json_escape(disp) + "\",\"type\":\"enum\",\"options\":[";
      for (int opt = 0; opt < count; ++opt) {
        if (opt > 0) j += ",";
        j += "\"" + json_escape(details.string_lookup[opt]) + "\"";
      }
      j += "]}";
    } else if (details.value_scale == vial::ValueDetails::kIndexed) {
      char buf[256];
      snprintf(buf, sizeof(buf),
               ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"int\",\"min\":%d,\"max\":%d}",
               json_escape(key).c_str(), json_escape(disp).c_str(),
               (int)std::round(details.min), (int)std::round(details.max));
      j += buf;
    } else {
      float range = details.max - details.min;
      float step = (range > 0.0f) ? (range / 100.0f) : 0.01f;
      char buf[256];
      snprintf(buf, sizeof(buf),
               ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"float\",\"min\":%.4f,\"max\":%.4f,\"step\":%.4f}",
               json_escape(key).c_str(), json_escape(disp).c_str(),
               details.min, details.max, step);
      j += buf;
    }
  }

  j += "]";
  free(inst->chain_params_json);
  inst->chain_params_json = strdup(j.c_str());

  char msg[128];
  snprintf(msg, sizeof(msg), "Built chain_params (%d bytes)", (int)j.size());
  plugin_log(msg);
}

/* =====================================================================
 * Instance lifecycle
 * ===================================================================== */
static void *v2_create_instance(const char *module_dir, const char *json_defaults) {
  (void)json_defaults;
  plugin_log("Creating Vial instance...");

  vial_instance_t *inst = new vial_instance_t();
  strncpy(inst->module_dir, module_dir ? module_dir : "",
          sizeof(inst->module_dir) - 1);
  inst->module_dir[sizeof(inst->module_dir) - 1] = '\0';
  inst->error_msg[0] = '\0';
  inst->octave_transpose = 0;

  /* Redirect JUCE HOME and config paths to writable directory on Move */
  char vial_home_path[512];
  snprintf(vial_home_path, sizeof(vial_home_path),
           "/data/UserData/schwung/vial-config");
  setenv("HOME", vial_home_path, 1);
  setenv("XDG_DATA_HOME", vial_home_path, 1);

  try {
    inst->synth = new VialMoveInstance();
    inst->synth_aux = new VialMoveInstance();
    plugin_log("VialMoveInstance (dual-core) created OK");
  } catch (const std::exception &e) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Exception creating Vial instance: %s", e.what());
    plugin_log(msg);
    delete inst->synth_aux;
    delete inst->synth;
    delete inst;
    return nullptr;
  } catch (...) {
    plugin_log("Unknown exception creating Vial instance");
    delete inst->synth_aux;
    delete inst->synth;
    delete inst;
    return nullptr;
  }

  /* Configure engines for Move specs (44.1kHz, 128 frames) */
  inst->synth->prepare((double)MOVE_SAMPLE_RATE, MOVE_FRAMES_PER_BLOCK);
  inst->synth->setBpm(inst->bpm);
  if (inst->synth_aux) {
    inst->synth_aux->prepare((double)MOVE_SAMPLE_RATE, MOVE_FRAMES_PER_BLOCK);
    inst->synth_aux->setBpm(inst->bpm);
  }
  start_aux_worker(inst);

  /* Scan and load presets */
  char patches_path[512];
  snprintf(patches_path, sizeof(patches_path), "%s/vial-data/patches", module_dir);
  juce::File patches_dir(patches_path);

  if (!patches_dir.exists()) {
    snprintf(patches_path, sizeof(patches_path), "%s/helm-data/patches", module_dir);
    patches_dir = juce::File(patches_path);
  }

  inst->patches.clear();
  if (patches_dir.exists()) {
    scan_directory_presets(patches_dir, inst->patches);
  }

  /* Sort patches: Vial-* first, then Helm-*, then Surge-*, then category & name */
  std::sort(inst->patches.begin(), inst->patches.end(),
            [](const vial_instance_t::patch_entry &a,
               const vial_instance_t::patch_entry &b) {
              auto groupRank = [](const std::string &cat) {
                if (cat.rfind("Vial-", 0) == 0) return 0;
                if (cat.rfind("Helm-", 0) == 0) return 1;
                return 2; /* Surge-* */
              };
              int ra = groupRank(a.category);
              int rb = groupRank(b.category);
              if (ra != rb)
                return ra < rb;
              if (a.category != b.category)
                return a.category < b.category;
              return a.name < b.name;
            });

  inst->preset_count = (int)inst->patches.size();

  inst->categories.clear();
  std::string last_cat_name = "";
  for (int i = 0; i < inst->preset_count; i++) {
    const std::string &cat_name = inst->patches[i].category;
    if (cat_name != last_cat_name) {
      vial_instance_t::category_entry entry;
      strncpy(entry.name, cat_name.c_str(), sizeof(entry.name) - 1);
      entry.name[sizeof(entry.name) - 1] = '\0';
      entry.first_idx = i;
      inst->categories.push_back(entry);
      last_cat_name = cat_name;
    }
  }

  char msg[768];
  snprintf(msg, sizeof(msg), "Scanned %d presets and %d categories in %s",
           inst->preset_count, (int)inst->categories.size(), patches_path);
  plugin_log(msg);

  int default_idx = 0;
  for (int i = 0; i < inst->preset_count; i++) {
    if (inst->patches[i].name == "Move Organ") {
      default_idx = i;
      break;
    }
  }

  if (inst->preset_count > 0) {
    load_preset_by_index(inst, default_idx);
  }

  build_ui_hierarchy(inst);
  build_chain_params(inst);
  return inst;
}

static void v2_destroy_instance(void *instance) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst)
    return;

  stop_aux_worker(inst);
  free(inst->ui_hierarchy_json);
  free(inst->chain_params_json);
  delete inst->synth_aux;
  delete inst->synth;
  delete inst;
  plugin_log("Vial instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst || !inst->synth || len < 1)
    return;
  (void)source;

  uint8_t status = msg[0] & 0xF0;
  uint8_t data1 = (len > 1) ? msg[1] : 0;
  uint8_t data2 = (len > 2) ? msg[2] : 0;

  /* Auto BPM Sync calculation */
  if (msg[0] == 0xF8) {
    double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (inst->last_clock_time > 0.0) {
      double delta = now - inst->last_clock_time;
      if (delta > 0.005 && delta < 0.2) {
        inst->clock_time_sum += delta;
        inst->clock_count++;
        if (inst->clock_count >= 24) {
          double avg_delta = inst->clock_time_sum / inst->clock_count;
          double bpm = 60.0 / (avg_delta * 24.0);
          if (bpm >= 20.0 && bpm <= 300.0) {
            if (inst->sync_bpm && inst->synth) {
              inst->bpm = (float)bpm;
              inst->synth->setBpm(inst->bpm);
              if (inst->synth_aux)
                inst->synth_aux->setBpm(inst->bpm);
            }
          }
          inst->clock_time_sum = 0.0;
          inst->clock_count = 0;
        }
      } else {
        inst->clock_time_sum = 0.0;
        inst->clock_count = 0;
      }
    }
    inst->last_clock_time = now;
  }

  /* Octave transpose + Dual-Core Note Allocator for Note On / Note Off */
  if (status == 0x90 || status == 0x80) {
    int note = data1 + inst->octave_transpose * 12;
    if (note < 0)
      note = 0;
    if (note > 127)
      note = 127;

    uint8_t mod_msg[3] = {msg[0], (uint8_t)note, data2};
    bool is_note_on = (status == 0x90 && data2 > 0);
    juce::ScopedLock l(inst->midi_lock);

    if (inst->multi_core && inst->synth_aux && inst->worker_started && inst->max_polyphony > 2) {
      if (is_note_on) {
        /* Pack 2 notes onto Engine 0 first (1 full 4-wide NEON AggregateVoice),
         * then balance voices across Engine 0 (Core 3) and Engine 1 (Core 2). */
        int target = (inst->held_count[0] < 2 || inst->held_count[0] <= inst->held_count[1]) ? 0 : 1;
        int prev_owner = inst->note_owner[note];
        if (prev_owner == 0 && inst->held_count[0] > 0) inst->held_count[0]--;
        if (prev_owner == 1 && inst->held_count[1] > 0) inst->held_count[1]--;
        inst->note_owner[note] = (int8_t)target;
        inst->held_count[target]++;
        if (target == 0)
          inst->midi_queue.addEvent(juce::MidiMessage(mod_msg, len), 0);
        else
          inst->midi_queue_aux.addEvent(juce::MidiMessage(mod_msg, len), 0);
      } else {
        int owner = inst->note_owner[note];
        if (owner == 1) {
          inst->midi_queue_aux.addEvent(juce::MidiMessage(mod_msg, len), 0);
          if (inst->held_count[1] > 0) inst->held_count[1]--;
        } else if (owner == 0) {
          inst->midi_queue.addEvent(juce::MidiMessage(mod_msg, len), 0);
          if (inst->held_count[0] > 0) inst->held_count[0]--;
        } else {
          inst->midi_queue.addEvent(juce::MidiMessage(mod_msg, len), 0);
          inst->midi_queue_aux.addEvent(juce::MidiMessage(mod_msg, len), 0);
        }
        inst->note_owner[note] = -1;
      }
    } else {
      inst->midi_queue.addEvent(juce::MidiMessage(mod_msg, len), 0);
    }
  } else {
    juce::ScopedLock l(inst->midi_lock);
    inst->midi_queue.addEvent(juce::MidiMessage(msg, len), 0);
    if (inst->synth_aux)
      inst->midi_queue_aux.addEvent(juce::MidiMessage(msg, len), 0);
  }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst || !inst->synth || !key || !val)
    return;

  if (strcmp(key, "state") == 0) {
    try {
      json st = json::parse(val, nullptr);
      if (st.is_object()) {
        if (st.count("preset") && st["preset"].is_number()) {
          int idx = st["preset"].get<int>();
          if (idx >= 0 && idx < inst->preset_count && idx != inst->current_preset) {
            load_preset_by_index(inst, idx);
          }
        }
        if (st.count("octave_transpose") && st["octave_transpose"].is_number()) {
          inst->octave_transpose = std::clamp(st["octave_transpose"].get<int>(), -3, 3);
        }
        if (st.count("sync_bpm") && st["sync_bpm"].is_number()) {
          inst->sync_bpm = (st["sync_bpm"].get<int>() != 0);
        }
        if (st.count("bpm") && st["bpm"].is_number()) {
          float b = std::clamp(st["bpm"].get<float>(), 20.0f, 300.0f);
          inst->bpm = b;
          inst->synth->setBpm(b);
          if (inst->synth_aux) inst->synth_aux->setBpm(b);
        }
        if (st.count("volume") && st["volume"].is_number()) {
          float v = std::clamp(st["volume"].get<float>(), 0.0f, 1.0f);
          set_control_both(inst, "volume", v * v * 7399.4404f);
        }
        for (auto it = st.begin(); it != st.end(); ++it) {
          const std::string &k = it.key();
          if (k == "preset" || k == "octave_transpose" || k == "sync_bpm" ||
              k == "bpm" || k == "volume")
            continue;
          if (it.value().is_number() && inst->synth->hasControl(k)) {
            set_control_both(inst, k, it.value().get<float>());
          }
        }
      }
    } catch (...) {
      plugin_log("Failed to parse state JSON in set_param");
    }
    return;
  }

  if (strcmp(key, "preset") == 0) {
    int idx = atoi(val);
    char msg[128];
    snprintf(msg, sizeof(msg), "set_param preset=%s (idx=%d, cur=%d)", val, idx, inst->current_preset);
    plugin_log(msg);
    if (idx >= 0 && idx < inst->preset_count && idx != inst->current_preset) {
      load_preset_by_index(inst, idx);
    }
    return;
  }

  if (strcmp(key, "jump_to_category") == 0) {
    int idx = atoi(val);
    char msg[128];
    snprintf(msg, sizeof(msg), "set_param jump_to_category=%d", idx);
    plugin_log(msg);
    if (idx >= 0 && idx < (int)inst->categories.size()) {
      load_preset_by_index(inst, inst->categories[idx].first_idx);
    }
    return;
  }

  if (strcmp(key, "octave_transpose") == 0) {
    inst->octave_transpose = std::clamp(atoi(val), -3, 3);
    return;
  }

  if (strcmp(key, "all_notes_off") == 0) {
    plugin_log("set_param all_notes_off");
    {
      juce::ScopedLock l(inst->midi_lock);
      inst->midi_queue.clear();
      inst->midi_queue_aux.clear();
      memset(inst->note_owner, -1, sizeof(inst->note_owner));
      inst->held_count[0] = 0;
      inst->held_count[1] = 0;
    }
    inst->synth->allSoundsOff();
    if (inst->synth_aux)
      inst->synth_aux->allSoundsOff();
    return;
  }

  if (strcmp(key, "volume") == 0) {
    float v = std::clamp((float)atof(val), 0.0f, 1.0f);
    set_control_both(inst, "volume", v * v * 7399.4404f);
    return;
  }

  if (strcmp(key, "bpm") == 0) {
    float bpm = (float)atof(val);
    if (bpm >= 20.0f && bpm <= 300.0f) {
      inst->bpm = bpm;
      inst->synth->setBpm(bpm);
      if (inst->synth_aux)
        inst->synth_aux->setBpm(bpm);
    }
    return;
  }

  if (strcmp(key, "sync_bpm") == 0) {
    inst->sync_bpm = (atoi(val) != 0);
    return;
  }

  if (strcmp(key, "oversampling") == 0) {
    inst->oversampling = std::clamp(atoi(val), 0, 3);
    inst->synth->applyOversampling(inst->oversampling);
    if (inst->synth_aux)
      inst->synth_aux->applyOversampling(inst->oversampling);
    return;
  }

  if (strcmp(key, "multi_core") == 0) {
    bool new_mc = (atoi(val) != 0);
    if (new_mc != inst->multi_core) {
      inst->multi_core = new_mc && inst->worker_started;
      if (!inst->multi_core && inst->synth_aux) {
        inst->synth_aux->allSoundsOff();
      }
      apply_cpu_limits(inst);
    }
    return;
  }

  if (strcmp(key, "fft_cache") == 0) {
    bool enabled = (atoi(val) != 0);
    juce::dsp::fftCacheEnabled().store(enabled, std::memory_order_relaxed);
    plugin_log(enabled ? "FFT Cache enabled" : "FFT Cache disabled");
    return;
  }

  if (strcmp(key, "max_polyphony") == 0) {
    inst->max_polyphony = std::clamp(atoi(val), 1, 16);
    apply_cpu_limits(inst);
    return;
  }

  if (strcmp(key, "max_unison") == 0) {
    inst->max_unison = std::clamp(atoi(val), 1, 16);
    apply_cpu_limits(inst);
    return;
  }

  if (strcmp(key, "fx_bypass") == 0) {
    bool new_bypass = (atoi(val) != 0);
    if (new_bypass != inst->fx_bypass) {
      if (new_bypass) {
        snapshot_fx_state(inst);
      }
      inst->fx_bypass = new_bypass;
      apply_fx_bypass(inst);
    }
    return;
  }

  /* Generic Vial parameter setter */
  if (inst->synth->hasControl(key)) {
    float v = (float)atof(val);
    set_control_both(inst, key, v);
    return;
  }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst || !key || !buf || buf_len < 1)
    return -1;

  if (strcmp(key, "state") == 0) {
    int offset = 0;
    float vial_vol = inst->synth->getControl("volume", 5473.0404f);
    float ui_vol = std::sqrt(std::clamp(vial_vol / 7399.4404f, 0.0f, 1.0f));
    offset += snprintf(buf + offset, buf_len - offset,
                       "{\"preset\":%d,\"octave_transpose\":%d,\"sync_bpm\":%d,\"bpm\":%.2f,\"volume\":%.4f",
                       inst->current_preset, inst->octave_transpose,
                       inst->sync_bpm ? 1 : 0, inst->bpm, ui_vol);

    size_t num_ui_params = sizeof(kUiVialParams) / sizeof(kUiVialParams[0]);
    for (size_t i = 0; i < num_ui_params && offset < buf_len - 96; ++i) {
      const char *pname = kUiVialParams[i];
      if (inst->synth->hasControl(pname)) {
        float val = inst->synth->getControl(pname);
        offset += snprintf(buf + offset, buf_len - offset, ",\"%s\":%.4f", pname, val);
      }
    }
    offset += snprintf(buf + offset, buf_len - offset, "}");
    return offset;
  }

  if (strcmp(key, "name") == 0)
    return snprintf(buf, buf_len, "Vial");

  if (strcmp(key, "preset") == 0)
    return snprintf(buf, buf_len, "%d", inst->current_preset);

  if (strcmp(key, "preset_count") == 0)
    return snprintf(buf, buf_len, "%d", inst->preset_count);

  if (strcmp(key, "preset_name") == 0)
    return snprintf(buf, buf_len, "%s", inst->preset_name);

  if (strcmp(key, "octave_transpose") == 0)
    return snprintf(buf, buf_len, "%d", inst->octave_transpose);

  if (strcmp(key, "bpm") == 0)
    return snprintf(buf, buf_len, "%d", (int)std::round(inst->bpm));

  if (strcmp(key, "sync_bpm") == 0)
    return snprintf(buf, buf_len, "%d", inst->sync_bpm ? 1 : 0);

  if (strcmp(key, "oversampling") == 0)
    return snprintf(buf, buf_len, "%d", inst->oversampling);

  if (strcmp(key, "multi_core") == 0)
    return snprintf(buf, buf_len, "%d", inst->multi_core ? 1 : 0);

  if (strcmp(key, "fft_cache") == 0)
    return snprintf(buf, buf_len, "%d",
                    juce::dsp::fftCacheEnabled().load(std::memory_order_relaxed) ? 1 : 0);

  if (strcmp(key, "jump_to_category") == 0) {
    int cur_cat = 0;
    for (int i = 0; i < (int)inst->categories.size(); ++i) {
      if (inst->categories[i].first_idx <= inst->current_preset)
        cur_cat = i;
      else
        break;
    }
    return snprintf(buf, buf_len, "%d", cur_cat);
  }

  if (strcmp(key, "max_polyphony") == 0)
    return snprintf(buf, buf_len, "%d", inst->max_polyphony);

  if (strcmp(key, "max_unison") == 0)
    return snprintf(buf, buf_len, "%d", inst->max_unison);

  if (strcmp(key, "fx_bypass") == 0)
    return snprintf(buf, buf_len, "%d", inst->fx_bypass ? 1 : 0);

  if (strcmp(key, "volume") == 0) {
    float vial_vol = inst->synth->getControl("volume", 5473.0404f);
    float ui_vol = std::sqrt(std::clamp(vial_vol / 7399.4404f, 0.0f, 1.0f));
    return snprintf(buf, buf_len, "%.4f", ui_vol);
  }

  if (strcmp(key, "bank_name") == 0) {
    if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count) {
      return snprintf(buf, buf_len, "%s", inst->patches[inst->current_preset].category.c_str());
    }
    return snprintf(buf, buf_len, "Helm-Factory");
  }

  if (strcmp(key, "category_list") == 0) {
    std::string j = "[";
    for (size_t i = 0; i < inst->categories.size(); i++) {
      if (i > 0) j += ",";
      j += "{\"index\":" + std::to_string(i) + ",\"label\":\"" +
           json_escape(inst->categories[i].name) + "\"}";
    }
    j += "]";
    int len = (int)j.size();
    if (len < buf_len) {
      strcpy(buf, j.c_str());
      return len;
    }
    return -1;
  }

  if (strcmp(key, "ui_hierarchy") == 0 && inst->ui_hierarchy_json) {
    int len = (int)strlen(inst->ui_hierarchy_json);
    if (len < buf_len) {
      strcpy(buf, inst->ui_hierarchy_json);
      return len;
    }
    return -1;
  }

  if (strcmp(key, "chain_params") == 0 && inst->chain_params_json) {
    int len = (int)strlen(inst->chain_params_json);
    if (len < buf_len) {
      strcpy(buf, inst->chain_params_json);
      return len;
    }
    return -1;
  }

  if (strcmp(key, "all_notes_off") == 0)
    return snprintf(buf, buf_len, "0");

  /* Generic Vial parameter getter */
  if (inst->synth->hasControl(key)) {
    float val = inst->synth->getControl(key);
    if (vial::Parameters::isParameter(key)) {
      const vial::ValueDetails &details = vial::Parameters::getDetails(key);
      if (details.value_scale == vial::ValueDetails::kIndexed) {
        return snprintf(buf, buf_len, "%d", (int)std::round(val));
      }
    }
    return snprintf(buf, buf_len, "%.4f", val);
  }

  return -1;
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst || !inst->synth) {
    memset(out_interleaved_lr, 0, frames * 4);
    return;
  }

  /* Thread-safe retrieval of MIDI events for both engines */
  juce::MidiBuffer local_midi;
  inst->local_midi_aux.clear();
  {
    juce::ScopedLock l(inst->midi_lock);
    local_midi.addEvents(inst->midi_queue, 0, frames, 0);
    inst->midi_queue.clear();
    if (inst->synth_aux) {
      inst->local_midi_aux.addEvents(inst->midi_queue_aux, 0, frames, 0);
      inst->midi_queue_aux.clear();
    }
  }

  /* Kick off secondary voice engine on Core 2 ONLY if it has MIDI or is awake */
  bool run_aux = inst->multi_core && inst->synth_aux && inst->worker_started &&
                 (!inst->local_midi_aux.isEmpty() || !inst->synth_aux->isSleeping());
  if (run_aux) {
    pthread_mutex_lock(&inst->worker_mutex);
    inst->worker_frames = frames;
    inst->worker_has_work = true;
    pthread_cond_signal(&inst->worker_cond);
    pthread_mutex_unlock(&inst->worker_mutex);
  }

  /* Render primary voice engine on Core 3 (SPI thread) simultaneously */
  if (inst->float_buffer.getNumSamples() < frames) {
    inst->float_buffer.setSize(2, frames, false, false, true);
  }
  inst->float_buffer.clear();
  inst->synth->process(&inst->float_buffer, local_midi, frames);

  /* Wait for Core 2 worker to finish if it was dispatched */
  if (run_aux) {
    pthread_mutex_lock(&inst->worker_mutex);
    while (inst->worker_has_work) {
      pthread_cond_wait(&inst->worker_done_cond, &inst->worker_mutex);
    }
    pthread_mutex_unlock(&inst->worker_mutex);
  }

  /* Sum stereo float outputs and interleave with clamping */
  const float *left_chan = inst->float_buffer.getReadPointer(0);
  const float *right_chan = inst->float_buffer.getReadPointer(1);
  const float *left_aux = run_aux ? inst->float_buffer_aux.getReadPointer(0) : nullptr;
  const float *right_aux = run_aux ? inst->float_buffer_aux.getReadPointer(1) : nullptr;

  for (int i = 0; i < frames; i++) {
    float left = (left_chan[i] + (left_aux ? left_aux[i] : 0.0f)) * inst->output_gain;
    float right = (right_chan[i] + (right_aux ? right_aux[i] : 0.0f)) * inst->output_gain;

    int32_t l = (int32_t)(left * 32767.0f);
    int32_t r = (int32_t)(right * 32767.0f);
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;

    out_interleaved_lr[i * 2] = (int16_t)l;
    out_interleaved_lr[i * 2 + 1] = (int16_t)r;
  }
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst || inst->error_msg[0] == '\0')
    return 0;
  return snprintf(buf, buf_len, "%s", inst->error_msg);
}

/* =====================================================================
 * Plugin API v2 export
 * ===================================================================== */
static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
  g_host = host;

  memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
  g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
  g_plugin_api_v2.create_instance = v2_create_instance;
  g_plugin_api_v2.destroy_instance = v2_destroy_instance;
  g_plugin_api_v2.on_midi = v2_on_midi;
  g_plugin_api_v2.set_param = v2_set_param;
  g_plugin_api_v2.get_param = v2_get_param;
  g_plugin_api_v2.get_error = v2_get_error;
  g_plugin_api_v2.render_block = v2_render_block;

  plugin_log("Vial plugin_api_v2 initialized");
  return &g_plugin_api_v2;
}
