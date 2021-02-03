// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <vector>

#include <QString>
#include <QFormLayout>
#include <QWidget>
#include <QFormLayout>

constexpr int WIDGET_MAX_WIDTH = 112;

class ControlGroupBox;
class InputConfig;
class MappingButton;
class MappingNumeric;
class MappingWindow;
class QPushButton;
class QGroupBox;

namespace ControllerEmu
{
class Control;
class ControlGroup;
class EmulatedController;
class NumericSettingBase;
}  // namespace ControllerEmu

class MappingWidget : public QWidget
{
  Q_OBJECT
public:
  explicit MappingWidget(MappingWindow* window);

  ControllerEmu::EmulatedController* GetController() const;

  MappingWindow* GetParent() const;

  virtual void LoadSettings() = 0;
  virtual void SaveSettings() = 0;
  virtual InputConfig* GetConfig() = 0;

signals:
  void Update();
  void ConfigChanged();

protected:
  int GetPort() const;

  void RefreshSettingsEnabled();

  QGroupBox* CreateGroupBox(ControllerEmu::ControlGroup* group);
  QGroupBox* CreateGroupBox(const QString& name, ControllerEmu::ControlGroup* group);
  QPushButton* CreateSettingAdvancedMappingButton(ControllerEmu::NumericSettingBase& setting);

private:
  MappingWindow* const m_parent;

protected:
  std::vector<std::tuple<const ControllerEmu::NumericSettingBase*, QFormLayout::TakeRowResult,
                         const ControllerEmu::ControlGroup*>>
      m_edit_condition_numeric_settings;
  bool m_block_update = false;
};
