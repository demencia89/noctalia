#include "core/raw_frame_stream.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("frame-stream");
  constexpr std::uint32_t kMaxDimension = 4096;
  constexpr std::size_t kMaxPixels = 4 * 1024 * 1024;
  constexpr std::size_t kReadBudget = 8 * 1024 * 1024;
} // namespace

RawFrameStream::~RawFrameStream() { disconnect(); }

bool RawFrameStream::setPath(const std::string& path) {
  if (m_path == path) {
    return false;
  }
  disconnect();
  m_path = path;
  m_pixels.clear();
  m_width = 0;
  m_height = 0;
  m_retryAfter = {};
  return true;
}

void RawFrameStream::disconnect() {
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
  m_requested = false;
  m_headerRead = 0;
  m_pixelsRead = 0;
  m_incoming.clear();
  m_retryAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
}

bool RawFrameStream::connect() {
  if (m_path.empty() || std::chrono::steady_clock::now() < m_retryAfter) {
    return false;
  }
  m_retryAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  sockaddr_un address{};
  if (m_path.size() >= sizeof(address.sun_path) || m_path.front() != '/' || m_path.contains('\0')) {
    return false;
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
  m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (m_fd < 0) {
    return false;
  }
  if (::connect(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    disconnect();
    return false;
  }
  ucred peer{};
  socklen_t size = sizeof(peer);
  if (::getsockopt(m_fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid()) {
    disconnect();
    return false;
  }
  return true;
}

bool RawFrameStream::parseHeader() {
  const auto number = [this](std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = offset; i < offset + 4; ++i) {
      value = (value << 8U) | m_header[i];
    }
    return value;
  };
  const auto width = number(4);
  const auto height = number(8);
  if (std::memcmp(m_header.data(), "NRF1", 4) != 0
      || width == 0
      || height == 0
      || width > kMaxDimension
      || height > kMaxDimension
      || static_cast<std::size_t>(width) * height > kMaxPixels) {
    kLog.warn("Invalid raw frame header from '{}'", m_path);
    disconnect();
    return false;
  }
  m_incomingWidth = static_cast<int>(width);
  m_incomingHeight = static_cast<int>(height);
  m_incoming.resize(static_cast<std::size_t>(width) * height * 4);
  return true;
}

bool RawFrameStream::requestFrame() {
  const char request = 'R';
  const auto sent = ::send(m_fd, &request, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
  if (sent != 1) {
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      return false;
    }
    disconnect();
    return false;
  }
  m_requested = true;
  m_replyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  return true;
}

bool RawFrameStream::readFrame() {
  if (m_requested && std::chrono::steady_clock::now() >= m_replyDeadline) {
    disconnect();
    return false;
  }
  if (m_fd < 0 && !connect()) {
    return false;
  }
  if (!m_requested && !requestFrame()) {
    return false;
  }

  std::size_t budget = kReadBudget;
  while (budget > 0) {
    const bool header = m_headerRead < m_header.size();
    auto* destination = header ? m_header.data() + m_headerRead : m_incoming.data() + m_pixelsRead;
    const auto remaining = header ? m_header.size() - m_headerRead : m_incoming.size() - m_pixelsRead;
    const auto count = ::recv(m_fd, destination, std::min(remaining, budget), MSG_DONTWAIT);
    if (count <= 0) {
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return false;
      }
      disconnect();
      return false;
    }
    const auto received = static_cast<std::size_t>(count);
    budget -= received;
    if (header) {
      m_headerRead += received;
      if (m_headerRead == m_header.size() && !parseHeader()) {
        return false;
      }
    } else {
      m_pixelsRead += received;
      if (m_pixelsRead == m_incoming.size()) {
        m_pixels.swap(m_incoming);
        m_width = m_incomingWidth;
        m_height = m_incomingHeight;
        m_headerRead = 0;
        m_pixelsRead = 0;
        m_requested = false;
        // Keep one reply in flight between polls without queuing video.
        (void)requestFrame();
        return true;
      }
    }
  }
  return false;
}
