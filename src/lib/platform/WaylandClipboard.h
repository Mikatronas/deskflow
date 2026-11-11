// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "deskflow/IClipboard.h"
#include <atomic>
#include <map>
#include <memory>
#include <optional>

namespace deskflow {
// The worker owns its Wayland connection and all pipe I/O. Only complete snapshots
// cross to the input thread; a new selection cancels older reads.
class WaylandClipboard
{
public:
  using Snapshot = std::map<IClipboard::Format, std::string>;
  WaylandClipboard();
  ~WaylandClipboard();
  bool ready() const;
  void setLimit(size_t bytes);
  std::optional<Snapshot> take();
  void publish(Snapshot data);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace deskflow
