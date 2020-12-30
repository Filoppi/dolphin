// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/RenderWidget.h"

#include <array>

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include "imgui.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/State.h"

#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

#ifdef _WIN32
#include <WinUser.h>
#include <windef.h>
#endif

RenderWidget::RenderWidget(QWidget* parent) : QWidget(parent)
{
  setWindowTitle(QStringLiteral("Dolphin"));
  setWindowIcon(Resources::GetAppIcon());
  setWindowRole(QStringLiteral("renderer"));
  setAcceptDrops(true);

  QPalette p;
  p.setColor(QPalette::Window, Qt::black);
  setPalette(p);

  connect(Host::GetInstance(), &Host::RequestTitle, this, &RenderWidget::setWindowTitle);
  connect(Host::GetInstance(), &Host::RequestRenderSize, this, [this](int w, int h) {
    if (!Config::Get(Config::MAIN_RENDER_WINDOW_AUTOSIZE) || isFullScreen() || isMaximized())
      return;

    const auto dpr = window()->windowHandle()->screen()->devicePixelRatio();

    resize(w / dpr, h / dpr);
  });

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Running)
      SetImGuiKeyMap();
  });

  // We have to use Qt::DirectConnection here because we don't want those signals to get queued
  // (which results in them not getting called)
  connect(this, &RenderWidget::StateChanged, Host::GetInstance(), &Host::SetRenderFullscreen,
          Qt::DirectConnection);
  connect(this, &RenderWidget::HandleChanged, Host::GetInstance(), &Host::SetRenderHandle,
          Qt::DirectConnection);
  connect(this, &RenderWidget::SizeChanged, Host::GetInstance(), &Host::ResizeSurface,
          Qt::DirectConnection);
  connect(this, &RenderWidget::FocusChanged, Host::GetInstance(), &Host::SetRenderFocus,
          Qt::DirectConnection);

  m_mouse_timer = new QTimer(this);
  connect(m_mouse_timer, &QTimer::timeout, this, &RenderWidget::HandleCursorTimer);
  m_mouse_timer->setSingleShot(true);
  setMouseTracking(true);

  connect(&Settings::Instance(), &Settings::HideCursorChanged, this,
          &RenderWidget::OnHideCursorChanged);
  connect(&Settings::Instance(), &Settings::LockCursorChanged, this,
          &RenderWidget::OnLockCursorChanged);
  OnHideCursorChanged();
  OnLockCursorChanged();
  connect(&Settings::Instance(), &Settings::KeepWindowOnTopChanged, this,
          &RenderWidget::OnKeepOnTopChanged);
  OnKeepOnTopChanged(Settings::Instance().IsKeepWindowOnTopEnabled());
  m_mouse_timer->start(MOUSE_HIDE_DELAY);

  // We need a native window to render into.
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_PaintOnScreen);
}

QPaintEngine* RenderWidget::paintEngine() const
{
  return nullptr;
}

void RenderWidget::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1)
    event->acceptProposedAction();
}

void RenderWidget::dropEvent(QDropEvent* event)
{
  const auto& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  const auto& url = urls[0];
  QFileInfo file_info(url.toLocalFile());

  auto path = file_info.filePath();

  if (!file_info.exists() || !file_info.isReadable())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
    return;
  }

  if (!file_info.isFile())
  {
    return;
  }

  State::LoadAs(path.toStdString());
}

void RenderWidget::OnHideCursorChanged()
{
  UpdateCursor();
}
void RenderWidget::OnLockCursorChanged()
{
  SetCursorLocked(false);
  UpdateCursor();
}

// Calling this at any time will set the cursor (image) to the correst state
void RenderWidget::UpdateCursor()
{
  if (!Settings::Instance().GetLockCursor())
  {
    // Only hide if the cursor is automatically locking (it will hide on lock).
    // "Unhide" the cursor if we lost focus, otherwise it will disappear when hovering
    // on top of the game window in the background
    bool keep_on_top = (windowFlags() & Qt::WindowStaysOnTopHint) != 0;
    bool should_hide =
        Settings::Instance().GetHideCursor() &&
        (keep_on_top || SConfig::GetInstance().m_BackgroundInput || isActiveWindow());
    setCursor(should_hide ? Qt::BlankCursor : Qt::ArrowCursor);
  }
  else
  {
    setCursor((m_cursor_locked && Settings::Instance().GetHideCursor()) ? Qt::BlankCursor :
                                                                          Qt::ArrowCursor);
  }
}

void RenderWidget::OnKeepOnTopChanged(bool top)
{
  const bool was_visible = isVisible();

  setWindowFlags(top ? windowFlags() | Qt::WindowStaysOnTopHint :
                       windowFlags() & ~Qt::WindowStaysOnTopHint);

  m_dont_lock_cursor_on_show = true;
  if (was_visible)
    show();
  m_dont_lock_cursor_on_show = false;

  UpdateCursor();
}

void RenderWidget::HandleCursorTimer()
{
  if (!isActiveWindow())
    return;
  if (!Settings::Instance().GetLockCursor() || m_cursor_locked)
  {
    setCursor(Qt::BlankCursor);
  }
}

void RenderWidget::showFullScreen()
{
  QWidget::showFullScreen();

  QScreen* screen = window()->windowHandle()->screen();

  const auto dpr = screen->devicePixelRatio();

  emit SizeChanged(width() * dpr, height() * dpr);
}

// Lock the cursor within the window/widget internal borders.
// Ignores the rendered aspect ratio for now, from an emulation point of view, it would make sense
// to clamp the cursor to the rendered portion of the widget, but it would just feel weird, also,
// users can always change the shape of the widget.
void RenderWidget::SetCursorLocked(bool locked)
{
  // It seems like QT doesn't scale the window frame correctly with some DPIs
  // so it might happen that the locked cursor can be on the frame of the window, 
  // being able to resize it, but that is a minor problem.
  QRect absolute_rect = geometry();
  if (parentWidget())
  {
    absolute_rect.moveTopLeft(parentWidget()->mapToGlobal(absolute_rect.topLeft()));
  }
  auto scale = devicePixelRatioF();
  QPoint screen_offset = QPoint(0, 0);
  if (window()->windowHandle() && window()->windowHandle()->screen())
  {
    screen_offset = window()->windowHandle()->screen()->geometry().topLeft();
  }
  absolute_rect.moveTopLeft(((absolute_rect.topLeft() - screen_offset) * scale) + screen_offset);
  absolute_rect.setSize(absolute_rect.size() * scale);

  if (locked)
  {
#ifdef _WIN32
    RECT rect;
    rect.left = absolute_rect.left();
    rect.right = absolute_rect.right();
    rect.top = absolute_rect.top();
    rect.bottom = absolute_rect.bottom();

    if (ClipCursor(&rect))
#else
    // TODO: implement on other platforms. Probably XGrabPointer on Linux.
    // The setting is hidden in the UI if not implemented
    if (false)
#endif
    {
      m_cursor_locked = true;

      if (Settings::Instance().GetHideCursor())
      {
        setCursor(Qt::BlankCursor);
      }

      Host::GetInstance()->SetRenderFullFocus(true);
    }
  }
  else
  {
#ifdef _WIN32
    ClipCursor(nullptr);
#endif

    if (m_cursor_locked)
    {
      m_cursor_locked = false;

      if (!Settings::Instance().GetLockCursor())
      {
        return;
      }

      // Center the mouse in the window if it's still active
      // Leave it where it was otherwise, e.g. a prompt has opened or we alt tabbed.
      if (isActiveWindow())
      {
        cursor().setPos(absolute_rect.left() + absolute_rect.width() / 2,
                        absolute_rect.top() + absolute_rect.height() / 2);
      }

      // Show the cursor or the user won't know the mouse is now unlocked
      setCursor(Qt::ArrowCursor);

      Host::GetInstance()->SetRenderFullFocus(false);
    }
  }
}

void RenderWidget::SetCursorLockedOnNextActivation(bool locked)
{
  if (Settings::Instance().GetLockCursor())
  {
    m_lock_cursor_on_next_activation = locked;
    return;
  }
  m_lock_cursor_on_next_activation = false;
}

bool RenderWidget::event(QEvent* event)
{
  PassEventToImGui(event);

  switch (event->type())
  {
  case QEvent::KeyPress:
  {
    QKeyEvent* ke = static_cast<QKeyEvent*>(event);
    if (ke->key() == Qt::Key_Escape)
      emit EscapePressed();

    // The render window might flicker on some platforms because Qt tries to change focus to a new
    // element when there is none (?) Handling this event before it reaches QWidget fixes the issue.
    if (ke->key() == Qt::Key_Tab)
      return true;

    break;
  }
  // Needed in case a new window open and it moves the mouse
  case QEvent::WindowBlocked:
    SetCursorLocked(false);
    break;
  case QEvent::MouseMove:
    if (g_freelook_camera.IsActive())
      OnFreeLookMouseMove(static_cast<QMouseEvent*>(event));
    [[fallthrough]];

  case QEvent::MouseButtonPress:
    if (isActiveWindow())
    {
      // Lock the cursor with any mouse button click (behave the same as window focus change).
      // This event is occasionally missed because isActiveWindow is laggy
      if (Settings::Instance().GetLockCursor() && event->type() == QEvent::MouseButtonPress)
      {
        SetCursorLocked(true);
      }
      // Unhide on movement
      if (!Settings::Instance().GetHideCursor())
      {
        setCursor(Qt::ArrowCursor);
        m_mouse_timer->start(MOUSE_HIDE_DELAY);
      }
    }
    break;
  case QEvent::WinIdChange:
    emit HandleChanged(reinterpret_cast<void*>(winId()));
    break;
  case QEvent::Show:
    // Don't do if "stay on top" changed (or was true)
    if (Settings::Instance().GetLockCursor() && Settings::Instance().GetHideCursor() &&
        !m_dont_lock_cursor_on_show)
    {
      // Auto lock when this window is shown (it was hidden)
      if (isActiveWindow())
        SetCursorLocked(true);
      else
        SetCursorLockedOnNextActivation();
    }
    break;
  // Note that this event in Windows is not always aligned to the window that is highlighted,
  // it's the window that has keyboard and mouse focus
  case QEvent::WindowActivate:
    if (SConfig::GetInstance().m_PauseOnFocusLost && Core::GetState() == Core::State::Paused)
      Core::SetState(Core::State::Running);

    UpdateCursor();

    if (m_lock_cursor_on_next_activation)
    {
      if (Settings::Instance().GetLockCursor())
        SetCursorLocked(true);

      m_lock_cursor_on_next_activation = false;
    }

    emit FocusChanged(true);
    break;
  case QEvent::WindowDeactivate:
    SetCursorLocked(false);

    UpdateCursor();

    if (SConfig::GetInstance().m_PauseOnFocusLost && Core::GetState() == Core::State::Running)
    {
      // If we are declared as the CPU thread, it means that the real CPU thread is waiting
      // for us to finish showing a panic alert (with that panic alert likely being the cause
      // of this event), so trying to pause the real CPU thread would cause a deadlock
      if (!Core::IsCPUThread())
        Core::SetState(Core::State::Paused);
    }

    emit FocusChanged(false);
    break;
  case QEvent::Move:
    SetCursorLocked(m_cursor_locked);
    break;
  case QEvent::Resize:
  {
    SetCursorLocked(m_cursor_locked);

    const QResizeEvent* se = static_cast<QResizeEvent*>(event);
    QSize new_size = se->size();

    QScreen* screen = window()->windowHandle()->screen();

    const auto dpr = screen->devicePixelRatio();

    emit SizeChanged(new_size.width() * dpr, new_size.height() * dpr);
    break;
  }
  // Happens when we add/remove the widget from the main window instead of the dedicated one
  case QEvent::ParentChange:
    SetCursorLocked(false);
    break;
  case QEvent::WindowStateChange:
    // Lock the mouse again when fullscreen changes (we might have missed some events)
    SetCursorLocked(m_cursor_locked || (isFullScreen() && Settings::Instance().GetLockCursor()));
    emit StateChanged(isFullScreen());
    break;
  case QEvent::Close:
    emit Closed();
    break;
  }
  return QWidget::event(event);
}

void RenderWidget::OnFreeLookMouseMove(QMouseEvent* event)
{
  const auto mouse_move = event->pos() - m_last_mouse;
  m_last_mouse = event->pos();

  if (event->buttons() & Qt::RightButton)
  {
    // Camera Pitch and Yaw:
    g_freelook_camera.Rotate(Common::Vec3{mouse_move.y() / 200.f, mouse_move.x() / 200.f, 0.f});
  }
  else if (event->buttons() & Qt::MiddleButton)
  {
    // Camera Roll:
    g_freelook_camera.Rotate({0.f, 0.f, mouse_move.x() / 200.f});
  }
}

void RenderWidget::PassEventToImGui(const QEvent* event)
{
  if (!Core::IsRunningAndStarted())
    return;

  switch (event->type())
  {
  case QEvent::KeyPress:
  case QEvent::KeyRelease:
  {
    // As the imgui KeysDown array is only 512 elements wide, and some Qt keys which
    // we need to track (e.g. alt) are above this value, we mask the lower 9 bits.
    // Even masked, the key codes are still unique, so conflicts aren't an issue.
    // The actual text input goes through AddInputCharactersUTF8().
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    const bool is_down = event->type() == QEvent::KeyPress;
    const u32 key = static_cast<u32>(key_event->key() & 0x1FF);
    auto lock = g_renderer->GetImGuiLock();
    if (key < std::size(ImGui::GetIO().KeysDown))
      ImGui::GetIO().KeysDown[key] = is_down;

    if (is_down)
    {
      auto utf8 = key_event->text().toUtf8();
      ImGui::GetIO().AddInputCharactersUTF8(utf8.constData());
    }
  }
  break;

  case QEvent::MouseMove:
  {
    auto lock = g_renderer->GetImGuiLock();

    // Qt multiplies all coordinates by the scaling factor in highdpi mode, giving us "scaled" mouse
    // coordinates (as if the screen was standard dpi). We need to update the mouse position in
    // native coordinates, as the UI (and game) is rendered at native resolution.
    const float scale = devicePixelRatio();
    ImGui::GetIO().MousePos.x = static_cast<const QMouseEvent*>(event)->x() * scale;
    ImGui::GetIO().MousePos.y = static_cast<const QMouseEvent*>(event)->y() * scale;
  }
  break;

  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonRelease:
  {
    auto lock = g_renderer->GetImGuiLock();
    const u32 button_mask = static_cast<u32>(static_cast<const QMouseEvent*>(event)->buttons());
    for (size_t i = 0; i < std::size(ImGui::GetIO().MouseDown); i++)
      ImGui::GetIO().MouseDown[i] = (button_mask & (1u << i)) != 0;
  }
  break;

  default:
    break;
  }
}

void RenderWidget::SetImGuiKeyMap()
{
  static constexpr std::array<std::array<int, 2>, 21> key_map{{
      {ImGuiKey_Tab, Qt::Key_Tab},
      {ImGuiKey_LeftArrow, Qt::Key_Left},
      {ImGuiKey_RightArrow, Qt::Key_Right},
      {ImGuiKey_UpArrow, Qt::Key_Up},
      {ImGuiKey_DownArrow, Qt::Key_Down},
      {ImGuiKey_PageUp, Qt::Key_PageUp},
      {ImGuiKey_PageDown, Qt::Key_PageDown},
      {ImGuiKey_Home, Qt::Key_Home},
      {ImGuiKey_End, Qt::Key_End},
      {ImGuiKey_Insert, Qt::Key_Insert},
      {ImGuiKey_Delete, Qt::Key_Delete},
      {ImGuiKey_Backspace, Qt::Key_Backspace},
      {ImGuiKey_Space, Qt::Key_Space},
      {ImGuiKey_Enter, Qt::Key_Return},
      {ImGuiKey_Escape, Qt::Key_Escape},
      {ImGuiKey_A, Qt::Key_A},
      {ImGuiKey_C, Qt::Key_C},
      {ImGuiKey_V, Qt::Key_V},
      {ImGuiKey_X, Qt::Key_X},
      {ImGuiKey_Y, Qt::Key_Y},
      {ImGuiKey_Z, Qt::Key_Z},
  }};
  auto lock = g_renderer->GetImGuiLock();

  for (auto [imgui_key, qt_key] : key_map)
    ImGui::GetIO().KeyMap[imgui_key] = (qt_key & 0x1FF);
}
