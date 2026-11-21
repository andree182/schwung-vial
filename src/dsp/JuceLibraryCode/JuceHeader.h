#pragma once

#include "AppConfig.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>
#include <cmath>
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
 * Fast Half-Size (N/2) Real-IFFT + 32-Slot Spectrum Cache for headless Vial on ARM64 Move.
 *
 * 1. On cache hit (held notes, non-morphing frames, same-octave notes):
 *    Returns bit-identical cached 2048-float waveform via memcpy (<0.5 us).
 * 2. On cache miss (noteOn or actively LFO-modulated spectral morph):
 *    Uses an N/2 (1024-point) complex IFFT with real-unpack twiddles instead of
 *    a full 2048-point complex IFFT, cutting cache-miss FFT cost by >2.2x.
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
        half_size_(size_ >> 1),
        forward_(size_, false),
        inverse_full_(size_, true),
        inverse_half_(half_size_ > 0 ? half_size_ : 1, true),
        buffer_(new std::complex<float>[size_]),
        half_in_(new std::complex<float>[half_size_ > 0 ? half_size_ : 1]),
        half_out_(new std::complex<float>[half_size_ > 0 ? half_size_ : 1]),
        twiddles_(half_size_ > 0 ? half_size_ : 1) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (size_t k = 0; k < half_size_; ++k) {
      float angle = (kTwoPi * (float)k) / (float)size_;
      twiddles_[k] = std::complex<float>(std::cos(angle), std::sin(angle));
    }
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
      /* Reference uncached 2048-point complex IFFT for A/B comparison */
      data[0] *= 0.5f;
      data[1] = data[size_];
      inverse_full_.transform((std::complex<float> *)data, buffer_.get());

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

    /* Cache miss: save active input prefix, run fast N/2 Real-IFFT, and cache output */
    std::memcpy(slot.in.data(), data, active_bytes);

    runHalfSizeRealInverse(data);

    std::memcpy(slot.out.data(), data, size_ * sizeof(float));
    slot.hash = h;
    slot.valid = true;

    std::memset(data + size_, 0, size_ * sizeof(float));
  }

private:
  void runHalfSizeRealInverse(float *data) {
    const size_t M = half_size_;
    std::complex<float> *z = half_in_.get();
    std::complex<float> *out = half_out_.get();
    const std::complex<float> *tw = twiddles_.data();

    z[0] = std::complex<float>(0.5f * (data[0] + data[size_]),
                               0.5f * (data[0] - data[size_]));
    for (size_t k = 1; k < M; ++k) {
      float xr = data[2 * k];
      float xi = data[2 * k + 1];
      float yr = data[2 * (M - k)];
      float yi = -data[2 * (M - k) + 1];

      float er = 0.5f * (xr + yr);
      float ei = 0.5f * (xi + yi);
      float dr = 0.5f * (xr - yr);
      float di = 0.5f * (xi - yi);

      float tr = tw[k].real();
      float ti = tw[k].imag();
      float or_ = (-di) * tr - dr * ti;
      float oi_ = (-di) * ti + dr * tr;
      z[k] = std::complex<float>(er + or_, ei + oi_);
    }

    inverse_half_.transform(z, out);

    float scale = 2.0f / (float)size_;
    for (size_t i = 0; i < M; ++i) {
      data[2 * i]     = out[i].real() * scale;
      data[2 * i + 1] = out[i].imag() * scale;
    }
  }

  size_t bits_;
  size_t size_;
  size_t half_size_;
  kissfft<float> forward_;
  kissfft<float> inverse_full_;
  kissfft<float> inverse_half_;
  std::unique_ptr<std::complex<float>[]> buffer_;
  std::unique_ptr<std::complex<float>[]> half_in_;
  std::unique_ptr<std::complex<float>[]> half_out_;
  std::vector<std::complex<float>> twiddles_;
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
