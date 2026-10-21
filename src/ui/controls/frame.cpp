#include "ui/controls/frame.h"

#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/controls/image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

Frame::Frame() {
  setClipChildren(true);
  setAcceptedButtons(0);
  m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_image->setFit(ImageFit::Contain);
  setOnPress([this](const PointerData& pointer) { reportPointer(pointer.pressed ? "press" : "release", pointer); });
  setOnMotion([this](const PointerData& pointer) {
    if (m_activeButton == BTN_LEFT) {
      reportPointer("move", pointer);
    }
  });
  setOnCancel([this] {
    const auto button = std::exchange(m_activeButton, 0);
    if (button != 0 && m_pointerSink) {
      m_pointerSink("cancel", 0.0F, 0.0F, 0.0F, 0.0F, button);
    }
  });
  const auto key = [this](const KeyData& event) {
    if (m_keySink && m_stream.connected()) {
      m_keySink(event.pressed ? "press" : "release", event);
    }
  };
  setOnKeyDown(key);
  setOnKeyUp(key);
  setOnAxisHandler([this](const PointerData& pointer) {
    if (!m_pointerSink || m_activeButton != 0 || pointer.axis > 1) {
      return false;
    }
    const auto position = imagePosition(pointer);
    if (!position.has_value()) {
      return false;
    }
    const auto [x, y] = *position;
    const float delta = pointer.scrollSteps();
    if (x < 0.0F || x > 1.0F || y < 0.0F || y > 1.0F || !std::isfinite(delta)) {
      return false;
    }
    if (delta != 0.0F) {
      m_pointerSink("scroll", x, y, pointer.axis == 1 ? delta : 0.0F, pointer.axis == 0 ? delta : 0.0F, 0);
    }
    return true;
  });
}

Frame::~Frame() {
  m_timer.stop();
  dispatchCancel();
  if (m_renderer != nullptr && m_texture.valid()) {
    m_renderer->textureManager().unload(m_texture);
  }
}

void Frame::setPath(const std::string& path) {
  if (!m_stream.setPath(path)) {
    return;
  }
  dispatchCancel();
  m_pending = true;
  markLayoutDirty();
  if (path.empty()) {
    m_timer.stop();
  } else {
    m_timer.start(std::chrono::milliseconds(0), [this] { poll(); });
  }
}

void Frame::setPointerSink(PointerSink sink) {
  if (!sink) {
    dispatchCancel();
  }
  m_pointerSink = std::move(sink);
  setAcceptedButtons(
      m_pointerSink ? buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) : (m_keySink ? buttonMask(BTN_LEFT) : 0)
  );
}

void Frame::setKeySink(KeySink sink) {
  m_keySink = std::move(sink);
  setFocusable(static_cast<bool>(m_keySink));
  setRetainsFocusOnPointerRelease(static_cast<bool>(m_keySink));
  setAcceptedButtons(
      m_pointerSink ? buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) : (m_keySink ? buttonMask(BTN_LEFT) : 0)
  );
}

void Frame::setRadius(float radius) { m_image->setRadius(radius); }

void Frame::poll() {
  bool visible = true;
  for (const Node* node = this; node != nullptr; node = node->parent()) {
    if (!node->visible()) {
      visible = false;
      m_stream.disconnect();
      break;
    }
  }
  const auto oldWidth = m_stream.width();
  const auto oldHeight = m_stream.height();
  if (visible && m_stream.readFrame()) {
    m_pending = true;
    markLayoutDirty();
  }
  if (!m_stream.connected() || oldWidth != m_stream.width() || oldHeight != m_stream.height()) {
    dispatchCancel();
  }
  m_timer.start(std::chrono::milliseconds(m_stream.connected() ? 16 : 250), [this] { poll(); });
}

std::optional<std::pair<float, float>> Frame::imagePosition(const PointerData& pointer) const {
  if (!m_stream.connected() || m_stream.width() <= 0 || m_stream.height() <= 0 || width() <= 0.0F || height() <= 0.0F) {
    return std::nullopt;
  }
  const auto sourceWidth = static_cast<float>(m_stream.width());
  const auto sourceHeight = static_cast<float>(m_stream.height());
  const float scale = std::min(width() / sourceWidth, height() / sourceHeight);
  const float imageWidth = sourceWidth * scale;
  const float imageHeight = sourceHeight * scale;
  const float x = (pointer.localX - (width() - imageWidth) * 0.5F) / imageWidth;
  const float y = (pointer.localY - (height() - imageHeight) * 0.5F) / imageHeight;
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return std::nullopt;
  }
  return std::pair{x, y};
}

void Frame::reportPointer(std::string_view kind, const PointerData& pointer) {
  if (!m_pointerSink) {
    return;
  }
  const auto position = imagePosition(pointer);
  if (!position.has_value()) {
    return;
  }
  const auto [x, y] = *position;
  if (kind == "press") {
    if (x < 0.0F || x > 1.0F || y < 0.0F || y > 1.0F) {
      return;
    }
    m_activeButton = pointer.button;
  } else if (m_activeButton == 0 || (kind == "release" && pointer.button != m_activeButton)) {
    return;
  }
  const auto button = m_activeButton;
  if (kind == "release") {
    m_activeButton = 0;
  }
  m_pointerSink(kind, std::clamp(x, 0.0F, 1.0F), std::clamp(y, 0.0F, 1.0F), 0.0F, 0.0F, button);
}

void Frame::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  if (std::exchange(m_pending, false)) {
    auto& textures = renderer.textureManager();
    if (m_stream.pixels().empty()) {
      m_image->setExternalTexture(renderer, {});
      textures.unload(m_texture);
    } else {
      const bool sameSize =
          m_texture.valid() && m_texture.width == m_stream.width() && m_texture.height == m_stream.height();
      const bool uploaded = sameSize
          ? textures.updateSubImage(
                m_texture, m_stream.pixels().data(), 0, 0, m_stream.width(), m_stream.height(), TextureDataFormat::Rgba
            )
          : textures.replace(
                m_texture, m_stream.pixels().data(), m_stream.width(), m_stream.height(), TextureDataFormat::Rgba
            );
      if (uploaded) {
        m_image->setExternalTexture(renderer, m_texture);
        m_image->markPaintDirty();
      }
    }
  }
  m_image->setSize(width(), height());
  InputArea::doLayout(renderer);
}

void Frame::doInvalidateGpuResources(Renderer& renderer) {
  m_renderer = &renderer;
  m_texture = {};
  m_pending = true;
  markLayoutDirty();
}
