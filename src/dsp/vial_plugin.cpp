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

namespace juce {
const char* juce_compilationDate = __DATE__;
const char* juce_compilationTime = __TIME__;

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

/* =====================================================================
 * Headless Vial instance class
 * ===================================================================== */
class VialMoveInstance : public HeadlessSynth {
public:
  VialMoveInstance() : HeadlessSynth(), current_time_seconds_(0.0) {}
  ~VialMoveInstance() override {}

  using SynthBase::getConnection;
  using SynthBase::controls_;

  void prepare(double sample_rate, int buffer_size) {
    (void)buffer_size;
    engine_->setSampleRate(sample_rate);
    engine_->updateAllModulationSwitches();
    midi_manager_->setSampleRate(sample_rate);
  }

  void allSoundsOff() {
    engine_->allSoundsOff();
  }

  void setBpm(float bpm) {
    engine_->setBpm(bpm);
  }

  void process(juce::AudioSampleBuffer *buffer, juce::MidiBuffer &midi_messages,
               int num_samples) {
    processModulationChanges();
    if (num_samples > 0)
      processKeyboardEvents(midi_messages, num_samples);

    double sample_time = 1.0 / getSampleRate();
    for (int sample_offset = 0; sample_offset < num_samples;) {
      int chunk = std::min<int>(num_samples - sample_offset, vial::kMaxBufferSize);

      engine_->correctToTime(current_time_seconds_);
      processMidi(midi_messages, sample_offset, sample_offset + chunk);
      processAudio(buffer, 2, chunk, sample_offset);

      current_time_seconds_ += chunk * sample_time;
      sample_offset += chunk;
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

    vial::PredefinedWaveFrames::Shape shape = vial::PredefinedWaveFrames::kSaw;
    if (helm_wave == 0)
      shape = vial::PredefinedWaveFrames::kSin;
    else if (helm_wave == 1)
      shape = vial::PredefinedWaveFrames::kTriangle;
    else if (helm_wave == 2)
      shape = vial::PredefinedWaveFrames::kSaw;
    else if (helm_wave == 3)
      shape = vial::PredefinedWaveFrames::kSquare;

    keyframe->wave_frame()->copy(vial::PredefinedWaveFrames::getWaveFrame(shape));
    group->addComponent(source);
    creator->addGroup(group);
    creator->render();
  }

  bool loadNativePreset(const juce::File &file, std::string &error) {
    return loadFromFile(file, error);
  }

  void setControl(const std::string &name, float val) {
    auto it = controls_.find(name);
    if (it != controls_.end() && it->second != nullptr) {
      it->second->set(val);
    }
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

    // Turn off oscillator 3 by default when translating 2-osc Helm patches
    setControl("osc_3_on", 0.0f);

    // Sub oscillator (if present in engine controls)
    float sub_vol = settings.value("sub_volume", 0.0f);
    setControl("sub_on", sub_vol > 0.001f ? 1.0f : 0.0f);
    setControl("sub_level", std::sqrt(std::clamp(sub_vol, 0.0f, 1.0f)));
    setControl("sub_transpose", settings.value("sub_octave", 0.0f) * 12.0f);
    int sub_wave = settings.value("sub_waveform", 0);
    // 0: Sin, 1: Triangle, 2: Saw, 3: Square
    int sub_vial_wave = (sub_wave == 0) ? 0 : (sub_wave == 1) ? 2 : (sub_wave == 2) ? 5 : 3;
    setControl("sub_waveform", (float)sub_vial_wave);

    // Noise/sample
    float noise_vol = settings.value("noise_volume", 0.0f);
    setControl("sample_on", noise_vol > 0.001f ? 1.0f : 0.0f);
    setControl("sample_level", noise_vol);

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
    setControl("filter_1_cutoff", settings.value("cutoff", 60.0f));
    setControl("filter_1_resonance", std::clamp((float)settings.value("resonance", 0.0f), 0.0f, 1.0f));
    setControl("filter_1_drive", settings.value("filter_drive", 0.0f));
    setControl("filter_1_osc1_input", 1.0f);
    setControl("filter_1_osc2_input", 1.0f);
    setControl("filter_1_sample_input", 1.0f);

    // Filter style mapping
    int filter_style = settings.value("filter_style", 0);
    if (filter_style == 0) { // 12dB Low Pass
      setControl("filter_1_model", 0.0f); // 12dB
      setControl("filter_1_blend", 0.0f);
    } else if (filter_style == 1) { // 24dB Low Pass
      setControl("filter_1_model", 1.0f); // 24dB
      setControl("filter_1_blend", 0.0f);
    } else if (filter_style == 2) { // Band Pass
      setControl("filter_1_model", 0.0f);
      setControl("filter_1_blend", 1.0f);
    } else if (filter_style == 3) { // High Pass
      setControl("filter_1_model", 0.0f);
      setControl("filter_1_blend", 2.0f);
    }

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

    // Polyphony & Portamento
    setControl("polyphony", (float)std::clamp((int)settings.value("polyphony", 8.0f), 1, 32));
    float portamento = settings.value("portamento", -10.0f);
    setControl("portamento_time", portamento);

    // Modulation routings
    clearModulations();

    float fil_env_depth = settings.value("fil_env_depth", 0.0f);
    if (std::abs(fil_env_depth) > 0.001f) {
      connectModulation("env_2", "filter_1_cutoff");
      vial::ModulationConnection *conn = getConnection("env_2", "filter_1_cutoff");
      if (conn && conn->modulation_processor) {
        conn->modulation_processor->setBaseValue(fil_env_depth / 64.0f);
      }
    }

    if (settings.count("modulations") && settings["modulations"].is_array()) {
      for (const auto &m : settings["modulations"]) {
        std::string src = m.value("source", "");
        std::string dst = m.value("destination", "");
        float amount = m.value("amount", 0.0f);

        std::string vial_src = translateModSource(src);
        std::string vial_dst = translateModDest(dst);

        if (!vial_src.empty() && !vial_dst.empty()) {
          connectModulation(vial_src, vial_dst);
          vial::ModulationConnection *conn = getConnection(vial_src, vial_dst);
          if (conn && conn->modulation_processor) {
            conn->modulation_processor->setBaseValue(std::clamp(amount, -1.0f, 1.0f));
          }
        }
      }
    }

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
    if (helm_src == "velocity")     return "note_velocity";
    return "";
  }

  static std::string translateModDest(const std::string &helm_dst) {
    if (helm_dst == "cutoff")           return "filter_1_cutoff";
    if (helm_dst == "resonance")        return "filter_1_resonance";
    if (helm_dst == "volume")           return "volume";
    if (helm_dst == "osc_1_volume")     return "osc_1_level";
    if (helm_dst == "osc_2_volume")     return "osc_2_level";
    if (helm_dst == "sub_volume")       return "sub_level";
    if (helm_dst == "delay_dry_wet")    return "delay_dry_wet";
    if (helm_dst == "reverb_dry_wet")   return "reverb_dry_wet";
    if (helm_dst == "distortion_drive") return "distortion_drive";
    return "";
  }

  double current_time_seconds_;
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
  char preset_name[64];

  /* Preset list */
  juce::Array<juce::File> patches;

  /* Patch Categories */
  struct category_entry {
    char name[64];
    int first_idx;
  };
  std::vector<category_entry> categories;

  /* Thread-safe MIDI queue */
  juce::CriticalSection midi_lock;
  juce::MidiBuffer midi_queue;

  /* Auto BPM Sync */
  bool sync_bpm;
  double last_clock_time;
  int clock_count;
  double clock_time_sum;

  /* Pre-built JSON strings */
  char *chain_params_json;

  vial_instance_t() {
    memset(module_dir, 0, sizeof(module_dir));
    memset(error_msg, 0, sizeof(error_msg));
    synth = nullptr;
    current_preset = 0;
    preset_count = 0;
    octave_transpose = 0;
    output_gain = 0.8f;
    memset(preset_name, 0, sizeof(preset_name));
    sync_bpm = true;
    last_clock_time = 0.0;
    clock_count = 0;
    clock_time_sum = 0.0;
    chain_params_json = nullptr;
  }
} vial_instance_t;

/* =====================================================================
 * Preset loading
 * ===================================================================== */
static void load_preset_by_index(vial_instance_t *inst, int idx) {
  if (!inst->synth || idx < 0 || idx >= inst->preset_count)
    return;

  const juce::File &file = inst->patches[idx];
  std::string error;
  bool ok = false;

  juce::String ext = file.getFileExtension().toLowerCase();
  if (ext == ".helm") {
    ok = inst->synth->loadHelmPreset(file, error);
  } else {
    ok = inst->synth->loadNativePreset(file, error);
  }

  if (ok) {
    inst->current_preset = idx;
    juce::String name = file.getFileNameWithoutExtension();
    strncpy(inst->preset_name, name.toRawUTF8(), sizeof(inst->preset_name) - 1);
    inst->preset_name[sizeof(inst->preset_name) - 1] = '\0';

    char msg[128];
    snprintf(msg, sizeof(msg), "Loaded preset [%d]: %s", idx, inst->preset_name);
    plugin_log(msg);
  } else {
    char msg[256];
    snprintf(msg, sizeof(msg), "Failed to load preset [%d] %s: %s",
             idx, file.getFileName().toRawUTF8(), error.c_str());
    plugin_log(msg);
  }
}

/* =====================================================================
 * Scan presets recursively
 * ===================================================================== */
static void scan_directory_presets(const juce::File &dir, juce::Array<juce::File> &out_list) {
  juce::Array<juce::File> files = dir.findChildFiles(
      juce::File::findFiles, false, "*.vital;*.vial;*.helm");
  files.sort();
  for (int i = 0; i < files.size(); ++i)
    out_list.add(files[i]);

  juce::Array<juce::File> subdirs = dir.findChildFiles(
      juce::File::findDirectories, false);
  subdirs.sort();
  for (int i = 0; i < subdirs.size(); ++i)
    scan_directory_presets(subdirs[i], out_list);
}

/* =====================================================================
 * JSON builder for chain_params
 * ===================================================================== */
static void build_chain_params(vial_instance_t *inst) {
  std::string json = "[";
  json += "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,";
  json += "\"max_param\":\"preset_count\",\"default\":0},";
  json += "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3,\"default\":0}";
  json += "]";

  free(inst->chain_params_json);
  inst->chain_params_json = strdup(json.c_str());
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

  /* Scan and load presets */
  char patches_path[512];
  snprintf(patches_path, sizeof(patches_path), "%s/vial-data/patches", module_dir);
  juce::File patches_dir(patches_path);

  if (!patches_dir.exists()) {
    /* Fallback to helm-data/patches if vial-data does not exist yet */
    snprintf(patches_path, sizeof(patches_path), "%s/helm-data/patches", module_dir);
    patches_dir = juce::File(patches_path);
  }

  inst->patches.clear();
  if (patches_dir.exists()) {
    scan_directory_presets(patches_dir, inst->patches);
  }
  inst->preset_count = inst->patches.size();

  inst->categories.clear();
  std::string last_cat_name = "";
  for (int i = 0; i < inst->preset_count; i++) {
    juce::String category = inst->patches[i].getParentDirectory().getFileName();
    std::string cat_name = category.toStdString();
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
    juce::String name = inst->patches[i].getFileNameWithoutExtension();
    if (name == "Move Organ") {
      default_idx = i;
      break;
    }
  }

  if (inst->preset_count > 0) {
    load_preset_by_index(inst, default_idx);
  }

  build_chain_params(inst);
  return inst;
}

static void v2_destroy_instance(void *instance) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst)
    return;

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
              inst->synth->setBpm((float)bpm);
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

  if (strcmp(key, "preset") == 0) {
    int idx = atoi(val);
    if (idx >= 0 && idx < inst->preset_count && idx != inst->current_preset) {
      load_preset_by_index(inst, idx);
    }
    return;
  }

  if (strcmp(key, "octave_transpose") == 0) {
    inst->octave_transpose = atoi(val);
    if (inst->octave_transpose < -3)
      inst->octave_transpose = -3;
    if (inst->octave_transpose > 3)
      inst->octave_transpose = 3;
    return;
  }

  if (strcmp(key, "all_notes_off") == 0) {
    inst->synth->allSoundsOff();
    return;
  }

  if (strcmp(key, "volume") == 0) {
    inst->output_gain = (float)atof(val);
    return;
  }

  if (strcmp(key, "bpm") == 0) {
    float bpm = (float)atof(val);
    if (bpm >= 20.0f && bpm <= 300.0f) {
      inst->synth->setBpm(bpm);
    }
    return;
  }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
  vial_instance_t *inst = (vial_instance_t *)instance;
  if (!inst || !key || !buf || buf_len < 1)
    return 0;

  if (strcmp(key, "preset") == 0)
    return snprintf(buf, buf_len, "%d", inst->current_preset);

  if (strcmp(key, "preset_count") == 0)
    return snprintf(buf, buf_len, "%d", inst->preset_count);

  if (strcmp(key, "preset_name") == 0)
    return snprintf(buf, buf_len, "%s", inst->preset_name);

  if (strcmp(key, "octave_transpose") == 0)
    return snprintf(buf, buf_len, "%d", inst->octave_transpose);

  if (strcmp(key, "bank_name") == 0) {
    if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count) {
      juce::String category = inst->patches[inst->current_preset].getParentDirectory().getFileName();
      return snprintf(buf, buf_len, "%s", category.toRawUTF8());
    }
    return snprintf(buf, buf_len, "Factory Presets");
  }

  if (strcmp(key, "chain_params") == 0) {
    if (inst->chain_params_json)
      return snprintf(buf, buf_len, "%s", inst->chain_params_json);
    return snprintf(buf, buf_len, "[]");
  }

  if (strcmp(key, "all_notes_off") == 0)
    return snprintf(buf, buf_len, "0");

  return 0;
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

  /* Render float audio from Vial engine */
  juce::AudioSampleBuffer float_buffer(2, frames);
  float_buffer.clear();
  inst->synth->process(&float_buffer, local_midi, frames);

  /* Convert and interleave output with soft clamping */
  const float *left_chan = float_buffer.getReadPointer(0);
  const float *right_chan = float_buffer.getReadPointer(1);

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

  return &g_plugin_api_v2;
}
