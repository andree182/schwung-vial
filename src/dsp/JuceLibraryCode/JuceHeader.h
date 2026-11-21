#pragma once

#include "AppConfig.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "kissfft/kissfft.h"

namespace juce {
namespace dsp {

inline std::atomic<bool> &fftCacheEnabled() {
  static std::atomic<bool> enabled{true};
  return enabled;
}

/*
 * Fast cached Real-FFT implementation for headless Vial on ARM64 Move.
 *
 * Vial's SynthOscillator re-runs a 2048-point inverse real FFT every 5 ms
 * (220 samples) per active voice per oscillator to bandlimit wavetable frames.
 * For held notes or non-morphing frames, the input spectrum is identical across
 * consecutive 5 ms windows and across voices in the same octave.
 * Caching the 32 most recent spectra per oscillator eliminates >98% of
 * audio-thread IFFTs during sustained notes.
 */
class FFT {
public:
  static constexpr int kCacheSlots = 32;

  struct CacheEntry {
    uint64_t hash = 0;
    bool valid = false;
    std::vector<float> in;
    std::vector<float> out;
  };

  explicit FFT(int bits)
      : bits_(bits),
        size_(1 << bits),
        forward_(size_, false),
        inverse_(size_, true),
        buffer_(new std::complex<float>[size_]) {
    for (int i = 0; i < kCacheSlots; ++i) {
      cache_[i].in.resize(size_ + 2, 0.0f);
      cache_[i].out.resize(size_, 0.0f);
    }
  }

  void performRealOnlyForwardTransform(float *data, bool /*onlyCalculateNonNegativeFrequencies*/ = true) {
    for (int i = (int)size_ - 1; i >= 0; --i) {
      data[2 * i] = data[i];
      data[2 * i + 1] = 0.0f;
    }

    forward_.transform((std::complex<float> *)data, buffer_.get());

    int num_floats = (int)size_ * 2;
    std::memcpy(data, buffer_.get(), num_floats * sizeof(float));
    data[size_] = data[1];
    data[size_ + 1] = 0.0f;
    data[1] = 0.0f;
  }

  void performRealOnlyInverseTransform(float *data) {
    if (!fftCacheEnabled().load(std::memory_order_relaxed)) {
      data[0] *= 0.5f;
      data[1] = data[size_];
      inverse_.transform((std::complex<float> *)data, buffer_.get());

      float multiplier = 2.0f / (float)size_;
      const std::complex<float> *buf = buffer_.get();
      for (size_t i = 0; i < size_; ++i)
        data[i] = buf[i].real() * multiplier;

      std::memset(data + size_, 0, size_ * sizeof(float));
      return;
    }

    /* Find last non-zero uint64 word so we only hash/compare active harmonics */
    const uint64_t *words = reinterpret_cast<const uint64_t *>(data);
    int num_words = (int)(size_ + 2) / 2;
    int active_words = num_words;
    while (active_words > 1 && words[active_words - 1] == 0) {
      --active_words;
    }

    uint64_t h = 14695981039346656037ULL ^ (uint64_t)active_words;
    for (int i = 0; i < active_words; ++i) {
      h ^= words[i];
      h *= 1099511628211ULL;
    }

    CacheEntry &slot = cache_[h & (kCacheSlots - 1)];
    size_t active_bytes = (size_t)active_words * sizeof(uint64_t);
    if (slot.valid && slot.hash == h &&
        std::memcmp(slot.in.data(), data, active_bytes) == 0) {
      std::memcpy(data, slot.out.data(), size_ * sizeof(float));
      std::memset(data + size_, 0, size_ * sizeof(float));
      return;
    }

    /* Cache miss: save active input prefix, run IFFT, and cache output */
    std::memcpy(slot.in.data(), data, active_bytes);

    data[0] *= 0.5f;
    data[1] = data[size_];
    inverse_.transform((std::complex<float> *)data, buffer_.get());

    float multiplier = 2.0f / (float)size_;
    const std::complex<float> *buf = buffer_.get();
    for (size_t i = 0; i < size_; ++i)
      data[i] = buf[i].real() * multiplier;

    std::memcpy(slot.out.data(), data, size_ * sizeof(float));
    slot.hash = h;
    slot.valid = true;

    std::memset(data + size_, 0, size_ * sizeof(float));
  }

private:
  size_t bits_;
  size_t size_;
  kissfft<float> forward_;
  kissfft<float> inverse_;
  std::unique_ptr<std::complex<float>[]> buffer_;
  CacheEntry cache_[kCacheSlots];
};

} // namespace dsp
} // namespace juce

using namespace juce;

namespace ProjectInfo {
    const char* const  projectName    = "Vial";
    const char* const  companyName    = "buchenberg";
    const char* const  versionString  = "1.5.5";
    const int          versionNumber  = 0x10505;
}
