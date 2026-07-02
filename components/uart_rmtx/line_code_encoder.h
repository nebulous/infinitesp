#pragma once

#include <cstdint>
#include <cstddef>
#include <driver/rmt_types.h>  // rmt_symbol_word_t

namespace esphome::uart_rmtx {

/// Line-code encoder for the Waveshare 6CH RS485 auto-direction workaround.
///
/// Expands UART bytes (start + N-1 data LSB-first + stop) into an RMT symbol
/// stream using a late-transition sub-bit line code, so the board's discrete
/// auto-direction one-shot keeps seeing edges and never drops DE mid-byte.
///
/// For a logical bit of value V (0 or 1) at N sub-bits/bit:
///   V=1 → (N-1) highs then 1 low      ("110..." for N=3)
///   V=0 → (N-1) lows  then 1 high     ("001..." for N=3)
/// The transition sits at fraction (N-1)/N of the bit, past the UART receiver's
/// last 3x oversample point (62.5%), so every standard receiver reads it clean.
/// Worst inter-edge gap is one logical bit-time (N sub-bits) at any bit-value
/// boundary, which keeps the one-shot primed with 2.4x margin over its ~64us
/// dropout. See private/hardware-research/rs485-rmt-linecode-hack.md.
///
/// Header-only so the encoder can be unit-tested on-host (M0) without ESPHome.
class LineCodeEncoder {
 public:
  /// Sub-bits per logical UART bit. N=3 is the minimum safe value (clears the
  /// 0.625 oversample floor at the lowest clock). N>=4 trades symbols for more
  /// receiver-tolerance headroom.
  void set_sub_bits(uint8_t n) { sub_bits_ = n; }
  void set_baud_rate(uint32_t baud) { baud_rate_ = baud; }
  /// RMT channel resolution in Hz (the tick frequency; duration fields are in
  /// these ticks). On ESP32-S3 the default clock source is 80MHz.
  void set_resolution_hz(uint32_t hz) { resolution_hz_ = hz; }

  /// Worst-case symbols written by encode() for `len` bytes.
  /// Each byte = 10 logical bits = up to 20 runs = 10 symbols, plus up to 2
  /// for the trailing idle-high completion pair.
  static constexpr size_t symbols_for_bytes(size_t len) { return len * 10 + 4; }

  /// Initialize per-sub-bit tick math. Must be called after the setters, before
  /// encode(). Computes the integer ticks per sub-bit plus a Bresenham
  /// remainder so edges stay accurate over arbitrarily long frames (no drift).
  void init() {
    const uint32_t sub_rate = baud_rate_ * sub_bits_;  // sub-bits per second
    sub_tick_q_ = resolution_hz_ / sub_rate;
    sub_tick_r_ = resolution_hz_ % sub_rate;
    sub_rate_ = sub_rate;
    acc_ = 0;
  }

  uint8_t get_sub_bits() const { return sub_bits_; }
  uint32_t get_baud_rate() const { return baud_rate_; }
  uint32_t get_resolution_hz() const { return resolution_hz_; }

  /// Encode `len` bytes into RMT symbols written to `out` (capacity `cap`).
  /// Returns the number of rmt_symbol_word_t written. The stream always ends
  /// with the line high (UART idle), so eot_level=1 parks it cleanly.
  size_t encode(const uint8_t *data, size_t len, rmt_symbol_word_t *out, size_t cap) {
    // Run-pairing state machine: a symbol holds two constant-level runs.
    bool have_half = false;
    uint16_t half_lvl = 0;
    uint16_t half_dur = 0;
    size_t nsym = 0;

    auto emit_run = [&](uint16_t level, uint16_t dur) {
      if (dur == 0)
        return;
      if (!have_half) {
        have_half = true;
        half_lvl = level;
        half_dur = dur;
      } else if (half_lvl == level) {
        // Merge into the pending half (same level → one long run).
        half_dur += dur;
      } else {
        out[nsym++] = make_symbol(half_lvl, half_dur, level, dur);
        have_half = false;
      }
    };

    // Walk every logical bit of every byte, expanding to sub-bits, coalescing
    // consecutive same-level sub-bits into runs handed to emit_run().
    uint8_t prev_level = 0xFF;  // forces a fresh first run
    uint16_t run_lvl = 0;
    uint16_t run_dur = 0;

    auto flush_run = [&]() {
      if (run_dur > 0) {
        emit_run(run_lvl, run_dur);
        run_dur = 0;
      }
    };

    for (size_t b = 0; b < len; b++) {
      // UART frame: start (0), 8 data LSB-first, stop (1).
      uint8_t bits[10];
      bits[0] = 0;                       // start
      for (int i = 0; i < 8; i++)
        bits[1 + i] = (data[b] >> i) & 1;
      bits[9] = 1;                       // stop

      for (int bi = 0; bi < 10; bi++) {
        uint8_t v = bits[bi];
        // (N-1) sub-bits at level v, then 1 sub-bit at level !v. At N=1 there
        // is no transition — each bit is just its own level = plain NRZ.
        for (uint8_t s = 0; s < sub_bits_; s++) {
          bool last = (s + 1 == sub_bits_);
          uint8_t level = (last && sub_bits_ > 1) ? (uint8_t)(v ^ 1) : v;
          uint16_t ticks = next_sub_ticks();
          if (level != prev_level) {
            flush_run();
            prev_level = level;
            run_lvl = level;
          }
          run_dur += ticks;
        }
      }
    }
    flush_run();

    // Complete any dangling half-symbol by pairing it with an idle-high run.
    // This also guarantees the final symbol's level1 == 1 (line parked high).
    if (have_half) {
      out[nsym++] = make_symbol(half_lvl, half_dur, 1, idle_sub_ticks());
    }
    (void) cap;
    return nsym;
  }

 protected:
  // Integer tick count for the next sub-bit, distributing the rounding
  // remainder evenly across the frame (Bresenham) so long frames don't drift.
  uint16_t next_sub_ticks() {
    acc_ += sub_tick_r_;
    uint16_t t = sub_tick_q_;
    if (acc_ >= sub_rate_) {
      acc_ -= sub_rate_;
      t++;
    }
    return t;
  }
  // A short idle-high run used to complete a dangling half. Duration is
  // immaterial (eot_level holds the line high afterwards); use 1 sub-bit.
  uint16_t idle_sub_ticks() const { return sub_tick_q_ > 0 ? sub_tick_q_ : 1; }

  static rmt_symbol_word_t make_symbol(uint16_t l0, uint16_t d0, uint16_t l1, uint16_t d1) {
    rmt_symbol_word_t s{};
    s.duration0 = d0;
    s.level0 = l0 & 1;
    s.duration1 = d1;
    s.level1 = l1 & 1;
    return s;
  }

  uint8_t sub_bits_{3};
  uint32_t baud_rate_{38400};
  uint32_t resolution_hz_{80000000};
  // tick math (set by init())
  uint16_t sub_tick_q_{0};   // floor ticks per sub-bit
  uint32_t sub_tick_r_{0};   // remainder to distribute
  uint32_t sub_rate_{0};     // baud_rate_ * sub_bits_ (Bresenham denominator)
  uint32_t acc_{0};          // running remainder accumulator
};

}  // namespace esphome::uart_rmtx
