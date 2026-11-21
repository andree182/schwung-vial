/*
 * Vial DSP Plugin for Move Anything / Schwung
 *
 * GPL-3.0 License
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
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

  /* Patch Categories (e.g., Helm-Bass, Helm-Keys, Vial-Leads) */
  struct category_entry {
    char name[64];
    int first_idx;
  };
  std::vector<category_entry> categories;

  /* Thread-safe MIDI queue */
  juce::CriticalSection midi_lock;
  juce::MidiBuffer midi_queue;

  /* CPU optimization settings (Move defaults: 1x oversampling, 4 voices, 3 unison) */
  int oversampling;
  int max_polyphony;
  int max_unison;
  bool fx_bypass;
  float saved_fx_on[7];

  /* Pre-allocated audio render buffer (avoids per-block heap allocation) */
  juce::AudioSampleBuffer float_buffer;

  /* Auto BPM Sync */
  bool sync_bpm;
  double last_clock_time;
  int clock_count;
  double clock_time_sum;

  /* Pre-built JSON strings */
  char *ui_hierarchy_json;
  char *chain_params_json;

  vial_instance_t() : float_buffer(2, MOVE_FRAMES_PER_BLOCK) {
    memset(module_dir, 0, sizeof(module_dir));
    memset(error_msg, 0, sizeof(error_msg));
    synth = nullptr;
    current_preset = 0;
    preset_count = 0;
    octave_transpose = 0;
    output_gain = 0.8f;
    bpm = 120.0f;
    memset(preset_name, 0, sizeof(preset_name));
    oversampling = 0;   /* 1x (Draft) */
    max_polyphony = 4;  /* Cap polyphony for Cortex-A53 */
    max_unison = 3;     /* Cap unison voices per osc */
    fx_bypass = false;
    memset(saved_fx_on, 0, sizeof(saved_fx_on));
    sync_bpm = true;
    last_clock_time = 0.0;
    clock_count = 0;
    clock_time_sum = 0.0;
    ui_hierarchy_json = nullptr;
    chain_params_json = nullptr;
  }
} vial_instance_t;

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

static void apply_fx_bypass(vial_instance_t *inst) {
  if (!inst || !inst->synth)
    return;
  for (int i = 0; i < 7; ++i) {
    inst->synth->setControl(kFxOnKeys[i], inst->fx_bypass ? 0.0f : inst->saved_fx_on[i]);
  }
}

static void apply_cpu_limits(vial_instance_t *inst) {
  if (!inst || !inst->synth)
    return;

  inst->synth->applyOversampling(inst->oversampling);

  /* Use Voice Override = Steal (1) so active voices never exceed polyphony cap */
  inst->synth->setControl("voice_override", 1.0f);

  float cur_poly = inst->synth->getControl("polyphony", (float)inst->max_polyphony);
  if (cur_poly > (float)inst->max_polyphony) {
    inst->synth->setControl("polyphony", (float)inst->max_polyphony);
  }

  const char *unison_keys[3] = {
      "osc_1_unison_voices", "osc_2_unison_voices", "osc_3_unison_voices"};
  for (int i = 0; i < 3; ++i) {
    float cur_u = inst->synth->getControl(unison_keys[i], 1.0f);
    if (cur_u > (float)inst->max_unison) {
      inst->synth->setControl(unison_keys[i], (float)inst->max_unison);
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
  } else {
    ok = inst->synth->loadNativePreset(file, error);
  }

  if (ok) {
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
      juce::File::findFiles, false, "*.vital;*.vial;*.helm");
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
            "\"knobs\":[\"oversampling\",\"fft_cache\",\"max_polyphony\",\"max_unison\",\"fx_bypass\",\"volume\",\"bpm\",\"sync_bpm\"],"
            "\"params\":[\"oversampling\",\"fft_cache\",\"max_polyphony\",\"max_unison\",\"fx_bypass\",\"bpm\",\"sync_bpm\",\"volume\",\"polyphony\",\"portamento_time\",\"portamento_slope\",\"legato\",\"pitch_bend_range\",\"velocity_track\",\"voice_tune\"]"
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
    plugin_log("VialMoveInstance created OK");
  } catch (const std::exception &e) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Exception creating Vial instance: %s", e.what());
    plugin_log(msg);
    delete inst;
    return nullptr;
  } catch (...) {
    plugin_log("Unknown exception creating Vial instance");
    delete inst;
    return nullptr;
  }

  /* Configure engine for Move specs (44.1kHz, 128 frames) */
  inst->synth->prepare((double)MOVE_SAMPLE_RATE, MOVE_FRAMES_PER_BLOCK);
  inst->synth->setBpm(inst->bpm);

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

  /* Sort patches: Vial-* categories FIRST, then Helm-* categories, then preset name */
  std::sort(inst->patches.begin(), inst->patches.end(),
            [](const vial_instance_t::patch_entry &a,
               const vial_instance_t::patch_entry &b) {
              bool a_helm = (a.category.rfind("Helm-", 0) == 0);
              bool b_helm = (b.category.rfind("Helm-", 0) == 0);
              if (a_helm != b_helm)
                return !a_helm; /* Vial first, Helm second */
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

  free(inst->ui_hierarchy_json);
  free(inst->chain_params_json);
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

  /* Octave transpose for Note On / Note Off */
  if (status == 0x90 || status == 0x80) {
    int note = data1 + inst->octave_transpose * 12;
    if (note < 0)
      note = 0;
    if (note > 127)
      note = 127;

    uint8_t mod_msg[3] = {msg[0], (uint8_t)note, data2};
    juce::ScopedLock l(inst->midi_lock);
    inst->midi_queue.addEvent(juce::MidiMessage(mod_msg, len), 0);
  } else {
    juce::ScopedLock l(inst->midi_lock);
    inst->midi_queue.addEvent(juce::MidiMessage(msg, len), 0);
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
        }
        if (st.count("volume") && st["volume"].is_number()) {
          float v = std::clamp(st["volume"].get<float>(), 0.0f, 1.0f);
          inst->synth->setControl("volume", v * v * 7399.4404f);
        }
        for (auto it = st.begin(); it != st.end(); ++it) {
          const std::string &k = it.key();
          if (k == "preset" || k == "octave_transpose" || k == "sync_bpm" ||
              k == "bpm" || k == "volume")
            continue;
          if (it.value().is_number() && inst->synth->hasControl(k)) {
            inst->synth->setControl(k, it.value().get<float>());
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
    }
    inst->synth->allSoundsOff();
    return;
  }

  if (strcmp(key, "volume") == 0) {
    float v = std::clamp((float)atof(val), 0.0f, 1.0f);
    inst->synth->setControl("volume", v * v * 7399.4404f);
    return;
  }

  if (strcmp(key, "bpm") == 0) {
    float bpm = (float)atof(val);
    if (bpm >= 20.0f && bpm <= 300.0f) {
      inst->bpm = bpm;
      inst->synth->setBpm(bpm);
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
    inst->synth->setControl(key, v);
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

  /* Thread-safe retrieval of MIDI events */
  juce::MidiBuffer local_midi;
  {
    juce::ScopedLock l(inst->midi_lock);
    local_midi.addEvents(inst->midi_queue, 0, frames, 0);
    inst->midi_queue.clear();
  }

  /* Render float audio from Vial engine using pre-allocated buffer */
  if (inst->float_buffer.getNumSamples() < frames) {
    inst->float_buffer.setSize(2, frames, false, false, true);
  }
  inst->float_buffer.clear();
  inst->synth->process(&inst->float_buffer, local_midi, frames);

  /* Convert and interleave output with clamping */
  const float *left_chan = inst->float_buffer.getReadPointer(0);
  const float *right_chan = inst->float_buffer.getReadPointer(1);

  for (int i = 0; i < frames; i++) {
    float left = left_chan[i] * inst->output_gain;
    float right = right_chan[i] * inst->output_gain;

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
