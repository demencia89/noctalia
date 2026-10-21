#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Local companion protocol: "NRF1", big-endian uint32 width/height, then
// tightly packed opaque RGBA8 pixels. The receiver requests each frame with
// one 'R' byte, so producers keep their latest frame instead of queuing video.
class RawFrameStream {
public:
  ~RawFrameStream();
  RawFrameStream() = default;
  RawFrameStream(const RawFrameStream&) = delete;
  RawFrameStream& operator=(const RawFrameStream&) = delete;

  bool setPath(const std::string& path);
  void disconnect();
  [[nodiscard]] bool connected() const { return m_fd >= 0; }
  // Nonblocking; at most one frame and 8 MiB are read per call. A complete
  // frame remains valid until the next read, including across disconnects.
  bool readFrame();
  [[nodiscard]] const std::vector<std::uint8_t>& pixels() const { return m_pixels; }
  [[nodiscard]] int width() const { return m_width; }
  [[nodiscard]] int height() const { return m_height; }

private:
  bool connect();
  bool requestFrame();
  bool parseHeader();

  std::string m_path;
  int m_fd = -1;
  std::chrono::steady_clock::time_point m_retryAfter;
  std::chrono::steady_clock::time_point m_replyDeadline;
  std::array<std::uint8_t, 12> m_header{};
  std::size_t m_headerRead = 0;
  std::size_t m_pixelsRead = 0;
  std::vector<std::uint8_t> m_incoming;
  std::vector<std::uint8_t> m_pixels;
  int m_incomingWidth = 0;
  int m_incomingHeight = 0;
  int m_width = 0;
  int m_height = 0;
  bool m_requested = false;
};
