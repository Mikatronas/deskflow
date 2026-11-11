// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "platform/WaylandClipboard.h"
#include "base/Log.h"
#include "platform/PortalClipboard.h"
#include "wlr-data-control-client.h"
#include <QByteArray>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace deskflow {
using Clock = std::chrono::steady_clock;
using Format = IClipboard::Format;
namespace {
constexpr size_t hardLimit = 10 * 1024 * 1024;
const std::pair<const char *, Format> formats[] = {
    {"text/plain;charset=utf-8", Format::Text},
    {"text/plain", Format::Text},
    {"text/html", Format::HTML},
    {"image/png", Format::Bitmap}
};
bool secret(const std::vector<std::string> &types)
{
  for (const auto &type : types)
    if (type == "x-kde-passwordManagerHint" || type == "application/x-keepassxc" ||
        type == "org.nspasteboard.ConcealedType" || type == "org.nspasteboard.TransientType" ||
        type == "application/x-deskflow-private")
      return true;
  return false;
}
} // namespace

struct WaylandClipboard::Impl
{
  struct Offer
  {
    zwlr_data_control_offer_v1 *object;
    std::vector<std::string> types;
  };
  struct Source
  {
    Impl *self;
    zwlr_data_control_source_v1 *object;
    std::map<std::string, std::string> data;
  };
  struct Transfer
  {
    int fd;
    Format format;
    std::string bytes;
    size_t offset = 0;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(2);
    bool writing = false;
  };
  std::mutex mutex;
  std::optional<Snapshot> incoming, outgoing;
  std::atomic<bool> available = false;
  std::atomic<size_t> limit = hardLimit;
  std::jthread worker;
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_seat *seat = nullptr;
  zwlr_data_control_manager_v1 *manager = nullptr;
  zwlr_data_control_device_v1 *device = nullptr;
  std::map<zwlr_data_control_offer_v1 *, std::unique_ptr<Offer>> offers;
  std::vector<std::unique_ptr<Source>> sources;
  std::vector<Transfer> transfers;
  Snapshot reading;
  bool readFailed = false, readPending = false, connectionFailed = false;
  const std::string marker =
      "application/x-deskflow-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();

  Impl()
  {
    worker = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        run(stop);
        for (int i = 0; i < 50 && !stop.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }
  ~Impl()
  {
    worker.request_stop();
    worker.join();
  }

  void discardReads()
  {
    for (auto it = transfers.begin(); it != transfers.end();) {
      if (!it->writing) {
        close(it->fd);
        it = transfers.erase(it);
      } else
        ++it;
    }
    reading.clear();
    readPending = false;
    readFailed = false;
  }
  void receive(zwlr_data_control_offer_v1 *object)
  {
    discardReads();
    // Only the selected offer remains valid for further reads.
    for (auto it = offers.begin(); it != offers.end();) {
      if (it->first != object) {
        zwlr_data_control_offer_v1_destroy(it->first);
        it = offers.erase(it);
      } else
        ++it;
    }
    if (!object) {
      std::scoped_lock lock(mutex);
      incoming = Snapshot{};
      return;
    }
    const auto found = offers.find(object);
    if (found == offers.end())
      return;
    const auto &types = found->second->types;
    if (std::find(types.begin(), types.end(), marker) != types.end())
      return;
    if (secret(types)) {
      // Never expose a secret or the stale selection it replaced.
      std::scoped_lock lock(mutex);
      incoming = Snapshot{};
      return;
    }
    std::vector<Format> requested;
    for (const auto &[mime, format] : formats) {
      if (std::find(types.begin(), types.end(), mime) == types.end() ||
          std::find(requested.begin(), requested.end(), format) != requested.end())
        continue;
      int fds[2];
      if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        readFailed = true;
        break;
      }
      // The owner receives a blocking write end; our reader is nonblocking.
      fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) & ~O_NONBLOCK);
      zwlr_data_control_offer_v1_receive(object, mime, fds[1]);
      close(fds[1]);
      transfers.push_back({fds[0], format, {}});
      requested.push_back(format);
    }
    readPending = !requested.empty();
    if (!readPending && !readFailed) {
      std::scoped_lock lock(mutex);
      incoming = Snapshot{};
    }
  }
  static void sourceSend(void *data, zwlr_data_control_source_v1 *, const char *mime, int fd)
  {
    auto &source = *static_cast<Source *>(data);
    auto &self = *source.self;
    const auto found = source.data.find(mime);
    if (found == source.data.end() || self.transfers.size() >= 16) {
      close(fd);
      return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    self.transfers.push_back({fd, Format::Text, found->second, 0, Clock::now() + std::chrono::seconds(2), true});
  }
  static void sourceCancelled(void *data, zwlr_data_control_source_v1 *object)
  {
    auto *source = static_cast<Source *>(data);
    auto &sources = source->self->sources;
    zwlr_data_control_source_v1_destroy(object);
    std::erase_if(sources, [source](const auto &entry) { return entry.get() == source; });
  }
  void claim(Snapshot snapshot)
  {
    auto source = std::make_unique<Source>();
    source->self = this;
    source->object = zwlr_data_control_manager_v1_create_data_source(manager);
    static const zwlr_data_control_source_v1_listener listener{sourceSend, sourceCancelled};
    zwlr_data_control_source_v1_add_listener(source->object, &listener, source.get());
    size_t total = 0;
    for (const auto &[mime, format] : formats) {
      auto found = snapshot.find(format);
      if (found == snapshot.end())
        continue;
      const auto bytes = PortalClipboard::encodeFormat(format, QByteArray::fromStdString(found->second));
      if ((format == Format::Bitmap && bytes.isEmpty()) || bytes.size() > qsizetype(limit.load()))
        continue;
      total += bytes.size();
      if (total > hardLimit * 3)
        break;
      source->data.emplace(mime, bytes.toStdString());
      zwlr_data_control_source_v1_offer(source->object, mime);
    }
    // A marker-only source is an explicit clear, with echo prevention intact.
    source->data.emplace(marker, "");
    zwlr_data_control_source_v1_offer(source->object, marker.c_str());
    zwlr_data_control_device_v1_set_selection(device, source->object);
    sources.push_back(std::move(source));
  }
  void serviceTransfers()
  {
    for (auto it = transfers.begin(); it != transfers.end();) {
      bool done = false, failed = Clock::now() >= it->deadline;
      if (!failed) {
        if (it->writing) {
          auto n = write(it->fd, it->bytes.data() + it->offset, std::min<size_t>(65536, it->bytes.size() - it->offset));
          if (n >= 0)
            it->offset += n;
          else if (errno != EAGAIN && errno != EINTR)
            failed = true;
          done = it->offset == it->bytes.size();
        } else {
          char buf[65536];
          auto n = read(it->fd, buf, sizeof(buf));
          if (n > 0) {
            it->bytes.append(buf, n);
            size_t total = it->bytes.size();
            for (const auto &[format, bytes] : reading)
              total += bytes.size();
            if (total > limit.load())
              failed = true;
          } else if (n == 0) {
            auto bytes = PortalClipboard::decodeFormat(it->format, QByteArray::fromStdString(it->bytes));
            failed = bytes.size() > qsizetype(limit.load()) || (it->format == Format::Bitmap && bytes.isEmpty());
            if (!failed)
              reading[it->format] = bytes.toStdString();
            done = true;
          } else if (errno != EAGAIN && errno != EINTR)
            failed = true;
        }
      }
      if (failed && !it->writing)
        readFailed = true;
      if (failed || done) {
        close(it->fd);
        it = transfers.erase(it);
      } else
        ++it;
    }
    if (readPending && std::none_of(transfers.begin(), transfers.end(), [](const auto &t) { return !t.writing; })) {
      size_t total = 0;
      for (const auto &[format, bytes] : reading)
        total += bytes.size();
      if (!readFailed && total <= limit.load()) {
        std::scoped_lock lock(mutex);
        incoming = std::move(reading);
      }
      readPending = false;
    }
  }
  static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
  {
    auto &self = *static_cast<Impl *>(data);
    if (!strcmp(interface, wl_seat_interface.name) && !self.seat)
      self.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    if (!strcmp(interface, zwlr_data_control_manager_v1_interface.name) && !self.manager)
      self.manager = static_cast<zwlr_data_control_manager_v1 *>(
          wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, std::min(version, 1u))
      );
  }
  void run(std::stop_token stop)
  {
    // Pipe consumers can disappear; a broken clipboard must not kill input sharing.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    display = wl_display_connect(nullptr);
    if (!display)
      return;
    registry = wl_display_get_registry(display);
    static const wl_registry_listener registryListener{global, [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry, &registryListener, this);
    while (!stop.stop_requested() && !connectionFailed) {
      if (wl_display_dispatch_pending(display) < 0)
        break;
      if (!device && manager && seat) {
        device = zwlr_data_control_manager_v1_get_data_device(manager, seat);
        static const zwlr_data_control_device_v1_listener listener{
            [](void *data, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *object) {
              auto &self = *static_cast<Impl *>(data);
              auto offer = std::make_unique<Offer>();
              offer->object = object;
              static const zwlr_data_control_offer_v1_listener listener{[](void *data, zwlr_data_control_offer_v1 *,
                                                                           const char *mime) {
                auto &types = static_cast<Offer *>(data)->types;
                if (types.size() < 256 && strlen(mime) < 4096)
                  types.emplace_back(mime);
              }};
              zwlr_data_control_offer_v1_add_listener(object, &listener, offer.get());
              self.offers.emplace(object, std::move(offer));
            },
            [](void *data, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *offer) {
              static_cast<Impl *>(data)->receive(offer);
            },
            [](void *data, zwlr_data_control_device_v1 *) { static_cast<Impl *>(data)->connectionFailed = true; },
            [](void *, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *) {}
        };
        zwlr_data_control_device_v1_add_listener(device, &listener, this);
        available = true;
        LOG_INFO("integrated Wayland clipboard ready (data-control)");
      }
      if (device) {
        std::optional<Snapshot> next;
        {
          std::scoped_lock lock(mutex);
          next.swap(outgoing);
        }
        if (next) {
          discardReads();
          claim(std::move(*next));
        }
      }
      serviceTransfers();
      if (wl_display_flush(display) < 0 && errno != EAGAIN)
        break;
      // This connection belongs exclusively to this thread; dispatch only after poll.
      std::vector<pollfd> fds{{wl_display_get_fd(display), POLLIN, 0}};
      for (const auto &t : transfers)
        fds.push_back({t.fd, short(t.writing ? POLLOUT : POLLIN), 0});
      if (poll(fds.data(), fds.size(), 20) < 0 && errno != EINTR)
        break;
      if (fds[0].revents & (POLLERR | POLLHUP))
        break;
      if (fds[0].revents & POLLIN)
        if (wl_display_dispatch(display) < 0)
          break;
    }
    available = false;
    for (auto &t : transfers)
      close(t.fd);
    for (auto &[object, offer] : offers)
      zwlr_data_control_offer_v1_destroy(object);
    for (auto &source : sources)
      zwlr_data_control_source_v1_destroy(source->object);
    if (device)
      zwlr_data_control_device_v1_destroy(device);
    if (manager)
      zwlr_data_control_manager_v1_destroy(manager);
    if (seat)
      wl_seat_destroy(seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    display = nullptr;
    registry = nullptr;
    seat = nullptr;
    manager = nullptr;
    device = nullptr;
    offers.clear();
    sources.clear();
    transfers.clear();
    reading.clear();
    readPending = readFailed = connectionFailed = false;
  }
};
WaylandClipboard::WaylandClipboard() : m_impl(std::make_unique<Impl>())
{
}
WaylandClipboard::~WaylandClipboard() = default;
bool WaylandClipboard::ready() const
{
  return m_impl->available;
}
void WaylandClipboard::setLimit(size_t bytes)
{
  m_impl->limit = std::min(bytes, hardLimit);
}
std::optional<WaylandClipboard::Snapshot> WaylandClipboard::take()
{
  std::scoped_lock lock(m_impl->mutex);
  std::optional<Snapshot> result;
  result.swap(m_impl->incoming);
  return result;
}
void WaylandClipboard::publish(Snapshot data)
{
  std::scoped_lock lock(m_impl->mutex);
  m_impl->outgoing = std::move(data);
}
} // namespace deskflow
