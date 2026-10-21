#pragma once

#include "core/raw_frame_stream.h"
#include "core/timer_manager.h"
#include "render/core/texture_handle.h"
#include "render/scene/input_area.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class Image;

// Displays companion-owned pixels using the normal image renderer. All
// socket reads happen outside Layout; GPU uploads happen only in Layout.
class Frame final : public InputArea {
public:
  using PointerSink =
      std::function<void(std::string_view kind, float x, float y, float dx, float dy, std::uint32_t button)>;
  using KeySink = std::function<void(std::string_view kind, const KeyData& key)>;
  Frame();
  ~Frame() override;
  void setPath(const std::string& path);
  void setRadius(float radius);
  void setPointerSink(PointerSink sink);
  void setKeySink(KeySink sink);

private:
  void poll();
  [[nodiscard]] std::optional<std::pair<float, float>> imagePosition(const PointerData& pointer) const;
  void reportPointer(std::string_view kind, const PointerData& pointer);
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;
  void doRebindRenderer(Renderer& renderer) override { m_renderer = &renderer; }

  RawFrameStream m_stream;
  Timer m_timer;
  TextureHandle m_texture;
  Renderer* m_renderer = nullptr;
  Image* m_image = nullptr;
  PointerSink m_pointerSink;
  KeySink m_keySink;
  bool m_pending = false;
  std::uint32_t m_activeButton = 0;
};
