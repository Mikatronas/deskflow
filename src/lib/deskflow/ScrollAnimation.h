// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "common/Coordinate.h"
#include <algorithm>
#include <cmath>
#include <deque>

// Short wheel animation, not momentum. Each input finishes within 100 ms;
// repeated notches overlap instead of postponing an ever-growing tail.
class ScrollAnimation
{
  struct Impulse
  {
    double start, x, y, progress = 0;
  };
  std::deque<Impulse> m_impulses;
  double m_x = 0, m_y = 0;
  int m_directionX = 0, m_directionY = 0;

public:
  void add(ScrollDelta delta, double now)
  {
    auto reverse = [](int value, int &direction) {
      const int next = (value > 0) - (value < 0);
      const bool reversed = next && direction && next != direction;
      if (next)
        direction = next;
      return reversed;
    };
    if (reverse(delta.x, m_directionX)) {
      for (auto &i : m_impulses)
        i.x = 0;
      m_x = 0;
    }
    if (reverse(delta.y, m_directionY)) {
      for (auto &i : m_impulses)
        i.y = 0;
      m_y = 0;
    }
    // Coalesce sub-frame input and bound storage even with very high polling rates.
    if (!m_impulses.empty() &&
        ((now - m_impulses.back().start < 0.004 && m_impulses.back().progress == 0) || m_impulses.size() >= 64)) {
      auto &last = m_impulses.back();
      const double remaining = 1.0 - last.progress;
      last.x += delta.x / remaining;
      last.y += delta.y / remaining;
    } else {
      m_impulses.push_back({now, double(delta.x), double(delta.y)});
    }
  }
  ScrollDelta step(double now)
  {
    for (auto &i : m_impulses) {
      const auto t = std::clamp((now - i.start) / 0.100, 0.0, 1.0);
      // Gentle acceleration spreads the initial kick across several frames,
      // while retaining immediate response and zero speed at the end.
      const auto progress = t + t * t - t * t * t;
      m_x += i.x * (progress - i.progress);
      m_y += i.y * (progress - i.progress);
      i.progress = progress;
    }
    while (!m_impulses.empty() && m_impulses.front().progress >= 1.0)
      m_impulses.pop_front();
    const auto quantize = [this](double value) {
      return static_cast<int32_t>(m_impulses.empty() ? std::round(value) : std::trunc(value));
    };
    ScrollDelta result{quantize(m_x), quantize(m_y)};
    m_x -= result.x;
    m_y -= result.y;
    return result;
  }
  bool active() const
  {
    return !m_impulses.empty();
  }
  void reset()
  {
    *this = ScrollAnimation{};
  }
};
