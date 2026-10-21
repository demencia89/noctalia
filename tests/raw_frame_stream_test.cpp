#include "core/raw_frame_stream.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
  class Server {
  public:
    Server() {
      std::array<char, 40> directory{};
      std::strcpy(directory.data(), "/tmp/noctalia-frame-test.XXXXXX");
      TEST_CHECK(::mkdtemp(directory.data()) != nullptr);
      m_directory = directory.data();
      path = m_directory + "/frames.sock";
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      TEST_CHECK(m_fd >= 0);
      TEST_CHECK(::bind(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(::listen(m_fd, 1) == 0);
    }
    ~Server() {
      ::close(m_fd);
      ::unlink(path.c_str());
      ::rmdir(m_directory.c_str());
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int accept(RawFrameStream& stream) const {
      stream.setPath(path);
      TEST_CHECK(!stream.readFrame());
      const int peer = ::accept4(m_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      TEST_CHECK(peer >= 0);
      request(peer);
      return peer;
    }
    static void request(int peer) {
      char byte = 0;
      TEST_CHECK(::recv(peer, &byte, 1, MSG_DONTWAIT) == 1);
      TEST_CHECK(byte == 'R');
      TEST_CHECK(::recv(peer, &byte, 1, MSG_DONTWAIT) == -1);
    }
    std::string path;

  private:
    std::string m_directory;
    int m_fd = -1;
  };

  void send(int peer, const void* bytes, std::size_t size) {
    TEST_CHECK(::send(peer, bytes, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size));
  }
} // namespace

int main() {
  Server server;
  RawFrameStream stream;
  TEST_CHECK(!stream.readFrame());
  TEST_CHECK(!stream.connected());
  TEST_CHECK(!stream.setPath(""));
  const int peer = server.accept(stream);
  TEST_CHECK(stream.connected());
  const std::array<std::uint8_t, 12> header{'N', 'R', 'F', '1', 0, 0, 0, 2, 0, 0, 0, 1};
  const std::array<std::uint8_t, 8> pixels{255, 0, 0, 255, 0, 255, 0, 255};

  // Every byte boundary may split across socket reads. Incomplete frames must
  // never replace the last complete frame or allocate from a partial header.
  for (const auto byte : header) {
    send(peer, &byte, 1);
    TEST_CHECK(!stream.readFrame());
    TEST_CHECK(stream.pixels().empty());
  }
  send(peer, pixels.data(), 3);
  TEST_CHECK(!stream.readFrame());
  send(peer, pixels.data() + 3, 5);
  TEST_CHECK(stream.readFrame());
  TEST_CHECK(stream.width() == 2 && stream.height() == 1);
  TEST_CHECK(std::equal(pixels.begin(), pixels.end(), stream.pixels().begin(), stream.pixels().end()));
  TEST_CHECK(!stream.setPath(server.path));
  TEST_CHECK(stream.connected());
  TEST_CHECK(stream.pixels().size() == pixels.size());

  // The next request is already available before another timer tick.
  Server::request(peer);
  TEST_CHECK(!stream.readFrame());
  auto rotated = header;
  rotated[7] = 1;
  rotated[11] = 2;
  send(peer, rotated.data(), rotated.size());
  send(peer, pixels.data(), pixels.size());
  TEST_CHECK(stream.readFrame());
  TEST_CHECK(stream.width() == 1 && stream.height() == 2);

  Server::request(peer);
  TEST_CHECK(!stream.readFrame());
  send(peer, header.data(), 5);
  ::close(peer);
  TEST_CHECK(!stream.readFrame());
  TEST_CHECK(!stream.connected());
  TEST_CHECK(stream.width() == 1 && stream.height() == 2);
  TEST_CHECK(stream.pixels().size() == pixels.size());

  // New sources and reconnects start on a fresh header, without old pixels.
  stream.setPath("");
  TEST_CHECK(stream.pixels().empty());
  int next = server.accept(stream);
  auto invalid = header;
  invalid[0] = 'X';
  send(next, invalid.data(), invalid.size());
  TEST_CHECK(!stream.readFrame());
  TEST_CHECK(stream.pixels().empty());
  ::close(next);

  // A stalled partial reply cannot hold the reader connected forever.
  stream.setPath("");
  next = server.accept(stream);
  send(next, header.data(), 5);
  TEST_CHECK(!stream.readFrame());
  std::this_thread::sleep_for(std::chrono::milliseconds(5010));
  TEST_CHECK(!stream.readFrame());
  TEST_CHECK(!stream.connected());
  TEST_CHECK(stream.pixels().empty());
  ::close(next);

  // Reject zero, excessive dimensions and a frame above the 16 MiB limit.
  for (const auto dimensions : {std::array<std::uint32_t, 2>{0, 1}, {0xffffffffU, 0xffffffffU}, {4096, 4096}}) {
    stream.setPath("");
    next = server.accept(stream);
    invalid = header;
    for (std::size_t axis = 0; axis < 2; ++axis) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        invalid[4 + axis * 4 + byte] = static_cast<std::uint8_t>(dimensions[axis] >> (24 - byte * 8));
      }
    }
    send(next, invalid.data(), invalid.size());
    TEST_CHECK(!stream.readFrame());
    TEST_CHECK(stream.pixels().empty());
    ::close(next);
  }
  return EXIT_SUCCESS;
}
