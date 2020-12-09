// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Settings/AudioPane.h"

#include <cassert>
#include <cmath>

#include <QCheckBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpacerItem>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QTimer>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Enums.h"
#include "AudioCommon/WASAPIStream.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"

#include "DolphinQt/Config/SettingsWindow.h"
#include "DolphinQt/Settings.h"

#ifdef _WIN32
#define WASAPI_DEFAULT_DEVICE_NAME "default"
#define WASAPI_INVALID_SAMPLE_RATE "0"
#endif

void ClickEventComboBox::showPopup()
{
  if (m_audio_pane)
  {
    m_audio_pane->OnCustomShowPopup(this);
  }
  QComboBox::showPopup();
}

AudioPane::AudioPane()
{
  m_running = false;
  m_ignore_save_settings = false;
#ifdef _WIN32
  m_wasapi_device_supports_default_sample_rate = false;
#endif

  CheckNeedForLatencyControl();
  CreateWidgets();
  LoadSettings();
  ConnectWidgets();

  connect(&Settings::Instance(), &Settings::VolumeChanged, this, &AudioPane::OnVolumeChanged);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          [=](Core::State state) { OnEmulationStateChanged(state != Core::State::Uninitialized); });

  OnEmulationStateChanged(Core::GetState() != Core::State::Uninitialized);

  m_timer = new QTimer(this);
}

void AudioPane::showEvent(QShowEvent* event)
{
  // Start updating the DPLII settings every second (we have no simple safe way to bind or listen
  // for events in the backend so we just refresh as the game is running to see if DPLII
  // was enabled successfully)
  connect(m_timer, &QTimer::timeout, this, &AudioPane::RefreshDolbyWidgets);
  RefreshDolbyWidgets();
  m_timer->start(1000);

  QWidget::showEvent(event);
}
void AudioPane::hideEvent(QHideEvent* event)
{
  disconnect(m_timer, &QTimer::timeout, this, &AudioPane::RefreshDolbyWidgets);
  m_timer->stop();

  QWidget::hideEvent(event);
}

void AudioPane::CreateWidgets()
{
  auto* dsp_box = new QGroupBox(tr("DSP Emulation Engine"));
  auto* dsp_layout = new QVBoxLayout;

  QFontMetrics font_metrics(font());

  dsp_box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  dsp_box->setLayout(dsp_layout);
  m_dsp_hle = new QRadioButton(tr("DSP HLE (fast)"));
  m_dsp_lle = new QRadioButton(tr("DSP LLE Recompiler"));
  m_dsp_interpreter = new QRadioButton(tr("DSP LLE Interpreter (slow)"));

  dsp_layout->addStretch(1);
  dsp_layout->addWidget(m_dsp_hle);
  dsp_layout->addWidget(m_dsp_lle);
  dsp_layout->addWidget(m_dsp_interpreter);
  dsp_layout->addStretch(1);

  auto* volume_box = new QGroupBox(tr("Volume"));
  auto* volume_layout = new QVBoxLayout;
  m_volume_slider = new QSlider;
  m_volume_indicator = new QLabel();

  volume_box->setLayout(volume_layout);

  m_volume_slider->setMinimum(0);
  m_volume_slider->setMaximum(100);

  m_volume_slider->setToolTip(tr("Using this is preferred over the OS mixer volume"));

  m_volume_indicator->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
  m_volume_indicator->setFixedWidth(font_metrics.boundingRect(tr("%1 %").arg(100)).width());

  volume_layout->addWidget(m_volume_slider, 0, Qt::AlignHCenter);
  volume_layout->addWidget(m_volume_indicator, 0, Qt::AlignHCenter);

  auto* backend_box = new QGroupBox(tr("Backend Settings"));
  auto* backend_layout = new QFormLayout;

  backend_box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  backend_box->setLayout(backend_layout);
  m_backend_label = new QLabel(tr("Audio Backend:"));
  m_backend_combo = new QComboBox();
  m_dolby_pro_logic = new QCheckBox(tr("Dolby Pro Logic II (5.1)"));

  m_dolby_pro_logic->setToolTip(
      tr("Enables Dolby Pro Logic II emulation using 5.1 surround.\nCertain backends and DPS "
         "emulation engines only.\n"
         "You need to enable surround from the game settings in GC games or in the menu settings "
         "on Wii."
         "\nThe emulation will still output 2.0, but the encoder will extract information for 5.1."
         "\nSome backends will notify when failed to enable it,"
         "\nwhile some other will just downmix it to stereo if not supported."
         "\nIt will add a latency on top of the backend one."
         "\nIf unsure, leave off."));

  if (m_latency_control_supported)
  {
    m_latency_label = new QLabel(tr("Latency:"));
    m_latency_spin = new QSpinBox();
    m_latency_spin->setMinimum(0);
    m_latency_spin->setMaximum(std::min((int)AudioCommon::GetMaxSupportedLatency(), 200));
    m_latency_spin->setSuffix(tr(" ms"));
    m_latency_spin->setToolTip(
        tr("Target latency (in ms). Higher values may reduce audio"
           " crackling.\nCertain backends only. Values above 20ms are not suggested."));
  }

  // TODO: implement this in other Operative Systems,
  // as of now this is only supported on (all) Windows backends
  m_use_os_sample_rate = new QCheckBox(tr("Use OS Mixer sample rate"));

  m_use_os_sample_rate->setToolTip(
      tr("Directly mixes and outputs at the current OS mixer sample rate (as opposed to %1 "
         "Hz).\nIt avoids any additional resamplings, possibly improving quality and performance."
         "\nIt won't follow changes to your OS setting after starting the emulation."
         "\nIf unsure, leave off.")
          .arg(AudioCommon::GetDefaultSampleRate()));

  // Unfortunately this creates an empty space when added to the layout. We should find a better way
  m_use_os_sample_rate->setHidden(true);

  backend_layout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  backend_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  backend_layout->addRow(m_backend_label, m_backend_combo);
  if (m_latency_control_supported)
    backend_layout->addRow(m_latency_label, m_latency_spin);

#ifdef _WIN32
  m_wasapi_device_label = new QLabel(tr("Device:"));
  m_wasapi_device_sample_rate_label = new QLabel(tr("Device Sample Rate:"));
  m_wasapi_device_combo = new ClickEventComboBox;
  m_wasapi_device_combo->m_audio_pane = this;
  m_wasapi_device_sample_rate_combo = new QComboBox;

  m_wasapi_device_combo->setToolTip(tr("Some devices might not work with WASAPI Exclusive mode"));
  m_wasapi_device_sample_rate_combo->setToolTip(
      tr("Output (and mix) sample rate.\nAnything above 48 kHz will have very minimal "
         "improvements to quality at the cost of performance."));

  backend_layout->addRow(m_wasapi_device_label, m_wasapi_device_combo);
  backend_layout->addRow(m_wasapi_device_sample_rate_label, m_wasapi_device_sample_rate_combo);
#endif
  backend_layout->addRow(m_use_os_sample_rate);

  backend_layout->addRow(m_dolby_pro_logic);

  auto* mixer_box = new QGroupBox(tr("Mixer Settings"));
  auto* mixer_layout = new QGridLayout;

  mixer_layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  mixer_box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  mixer_box->setLayout(mixer_layout);
  m_stretching_enable = new QCheckBox(tr("Audio Stretching"));
  m_emu_speed_tolerance_slider = new QSlider(Qt::Horizontal);
  m_emu_speed_tolerance_indicator = new QLabel();
  QSize min_size = m_emu_speed_tolerance_indicator->minimumSize();
  min_size.setWidth(
      std::max(std::max(font_metrics.width(tr("Disabled")), font_metrics.width(tr("None"))),
               font_metrics.width(tr("%1 ms").arg(125))));
  m_emu_speed_tolerance_indicator->setMinimumSize(min_size);
  m_emu_speed_tolerance_label = new QLabel(tr("Emulation Speed Tolerance:"));
  //To review: maybe have a set of 4 or 5 options to keep it simpler (low, high, ...).
  //To add descrpition to m_emu_speed_tolerance_label explaining that it's the time the emulation need to be offsetted by to start using the current speed

  m_stretching_enable->setToolTip(tr(
      "Enables stretching of the audio (pitch correction) to match the emulation speed.\nIt might "
      "incur in a slight loss of quality.\nIt might have undesired side effects with DPLII."));

  m_emu_speed_tolerance_slider->setMinimum(-1);
  m_emu_speed_tolerance_slider->setMaximum(125);
  m_emu_speed_tolerance_slider->setToolTip(
      tr("Time(ms) we need to fall behind the emulation for sound to start using the actual "
         "emulation speed.\nIf set "
         "too high (>40), sound will play old samples backwards when we slow down or stutter."
         "\nif set too low (<10), sound might lose quality if you have frequent small stutters."
         "\nSet 0 to have it on all the times. Slide all the way left to disable."));

  m_dolby_quality_label = new QLabel(tr("DPLII Decoding Quality:"));

  int max_dolby_quality = int(AudioCommon::DPL2Quality::Extreme);

  m_dolby_quality_slider = new QSlider(Qt::Horizontal);
  m_dolby_quality_slider->setMinimum(0);
  m_dolby_quality_slider->setMaximum(max_dolby_quality);
  m_dolby_quality_slider->setPageStep(1);
  m_dolby_quality_slider->setTickPosition(QSlider::TicksBelow);
  m_dolby_quality_slider->setToolTip(
      tr("Quality of the DPLII decoder. Also increases audio latency.\nThe selected preset will be "
         "used to find the best compromise between quality and latency."));

  m_dolby_quality_slider->setTracking(true);

  m_dolby_quality_latency_label = new QLabel();

  min_size = m_dolby_quality_latency_label->minimumSize();
  int max_width = 0;
  for (int i = 0; i <= max_dolby_quality; ++i)
  {
    max_width = std::max(max_width, font_metrics.width(GetDPL2QualityAndLatencyLabel(
                                        AudioCommon::DPL2Quality(max_dolby_quality))));
  }
  min_size.setWidth(max_width);
  m_dolby_quality_latency_label->setMinimumSize(min_size);

  mixer_layout->addWidget(m_stretching_enable, 0, 0, 1, -1);
  mixer_layout->addWidget(m_emu_speed_tolerance_label, 1, 0);
  mixer_layout->addWidget(m_emu_speed_tolerance_slider, 1, 1);
  mixer_layout->addWidget(m_emu_speed_tolerance_indicator, 1, 2);
  mixer_layout->addWidget(m_dolby_quality_label, 2, 0);
  mixer_layout->addWidget(m_dolby_quality_slider, 2, 1);
  mixer_layout->addWidget(m_dolby_quality_latency_label, 2, 2);

  QGridLayout* m_main_layout = new QGridLayout;
  m_main_layout->setRowStretch(0, 0);
  m_main_layout->setAlignment(Qt::AlignTop);

  m_main_layout->addWidget(dsp_box, 0, 0, Qt::AlignTop);
  m_main_layout->addWidget(volume_box, 0, 1, -1, 1);
  m_main_layout->addWidget(backend_box, 1, 0, Qt::AlignTop);
  m_main_layout->addWidget(mixer_box, 2, 0, Qt::AlignTop);

  setLayout(m_main_layout);
}

void AudioPane::ConnectWidgets()
{
  connect(m_backend_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AudioPane::SaveSettings);
  connect(m_volume_slider, &QSlider::valueChanged, this, &AudioPane::SaveSettings);
  if (m_latency_control_supported)
  {
    connect(m_latency_spin, qOverload<int>(&QSpinBox::valueChanged), this,
            &AudioPane::SaveSettings);
  }
  connect(m_emu_speed_tolerance_slider, &QSlider::valueChanged, this, &AudioPane::SaveSettings);
  connect(m_use_os_sample_rate, &QCheckBox::toggled, this, &AudioPane::SaveSettings);
  connect(m_dolby_pro_logic, &QCheckBox::toggled, this, &AudioPane::SaveSettings);
  connect(m_dolby_quality_slider, &QSlider::valueChanged, this, &AudioPane::SaveSettings);
  connect(m_stretching_enable, &QCheckBox::toggled, this, &AudioPane::SaveSettings);
  connect(m_dsp_hle, &QRadioButton::toggled, this, &AudioPane::SaveSettings);
  connect(m_dsp_lle, &QRadioButton::toggled, this, &AudioPane::SaveSettings);
  connect(m_dsp_interpreter, &QRadioButton::toggled, this, &AudioPane::SaveSettings);

#ifdef _WIN32
  connect(m_wasapi_device_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AudioPane::SaveSettings);
  connect(m_wasapi_device_sample_rate_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AudioPane::SaveSettings);
#endif
}

void AudioPane::LoadSettings()
{
  auto& settings = Settings::Instance();

  // DSP
  if (SConfig::GetInstance().bDSPHLE)
  {
    m_dsp_hle->setChecked(true);
  }
  else
  {
    m_dsp_lle->setChecked(SConfig::GetInstance().m_DSPEnableJIT);
    m_dsp_interpreter->setChecked(!SConfig::GetInstance().m_DSPEnableJIT);
  }

  // Backend
  m_ignore_save_settings = true;
  const auto current_backend = SConfig::GetInstance().sBackend;
  bool selection_set = false;
  m_backend_combo->clear();
  for (const auto& backend : AudioCommon::GetSoundBackends())
  {
    m_backend_combo->addItem(tr(backend.c_str()), QVariant(QString::fromStdString(backend)));
    if (backend == current_backend)
    {
      m_backend_combo->setCurrentIndex(m_backend_combo->count() - 1);
      selection_set = true;
    }
  }
  if (!selection_set)
    m_backend_combo->setCurrentIndex(-1);
  m_ignore_save_settings = false;

  OnBackendChanged();

  // Volume
  OnVolumeChanged(settings.GetVolume());

  // DPL2
  m_dolby_pro_logic->setChecked(SConfig::GetInstance().bDPL2Decoder);
  m_ignore_save_settings = true;
  m_dolby_quality_slider->setValue(int(Config::Get(Config::MAIN_DPL2_QUALITY)));
  m_ignore_save_settings = false;
  m_dolby_quality_latency_label->setText(
      GetDPL2QualityAndLatencyLabel(Config::Get(Config::MAIN_DPL2_QUALITY)));
  if (AudioCommon::SupportsDPL2Decoder(current_backend) && !m_dsp_hle->isChecked())
  {
    EnableDolbyQualityWidgets(m_dolby_pro_logic->isEnabled() && m_dolby_pro_logic->isChecked());
  }

  // Latency
  if (m_latency_control_supported)
  {
    m_ignore_save_settings = true;
    m_latency_spin->setValue(SConfig::GetInstance().iAudioBackendLatency);
    m_ignore_save_settings = false;
  }

  m_ignore_save_settings = true;

  // Sample rate
  m_use_os_sample_rate->setChecked(SConfig::GetInstance().bUseOSMixerSampleRate);

  // Stretch
  m_stretching_enable->setChecked(SConfig::GetInstance().m_audio_stretch);
  m_emu_speed_tolerance_slider->setValue(SConfig::GetInstance().m_audio_emu_speed_tolerance);
  if (m_emu_speed_tolerance_slider->value() < 0)
    m_emu_speed_tolerance_indicator->setText(tr("Disabled"));
  else if (m_emu_speed_tolerance_slider->value() == 0)
    m_emu_speed_tolerance_indicator->setText(tr("None"));
  else
    m_emu_speed_tolerance_indicator->setText(
        tr("%1 ms").arg(m_emu_speed_tolerance_slider->value()));

#ifdef _WIN32
  LoadWASAPIDevice();
  LoadWASAPIDeviceSampleRate();
#endif
  m_ignore_save_settings = false;

  // Call this again to "clamp" values that might not have been accepted
  SaveSettings();
}

void AudioPane::SaveSettings()
{
  // Avoids multiple calls to this when we are modifying the widgets
  //  in a way that would trigger multiple SaveSettings() callbacks
  if (m_ignore_save_settings)
  {
    return;
  }

  auto& settings = Settings::Instance();

  bool volume_changed = false;
  bool backend_setting_changed = false;
  bool surround_enabled_changed = false;

  // DSP
  if (SConfig::GetInstance().bDSPHLE != m_dsp_hle->isChecked() ||
      SConfig::GetInstance().m_DSPEnableJIT != m_dsp_lle->isChecked())
  {
    OnDSPChanged();
  }
  SConfig::GetInstance().bDSPHLE = m_dsp_hle->isChecked();
  Config::SetBaseOrCurrent(Config::MAIN_DSP_HLE, m_dsp_hle->isChecked());
  SConfig::GetInstance().m_DSPEnableJIT = m_dsp_lle->isChecked();
  Config::SetBaseOrCurrent(Config::MAIN_DSP_JIT, m_dsp_lle->isChecked());

  // Backend
  const auto selected_backend =
      m_backend_combo->itemData(m_backend_combo->currentIndex()).toString().toStdString();
  auto& backend = SConfig::GetInstance().sBackend;

  if (selected_backend != backend)
  {
    backend = selected_backend;
    OnBackendChanged();
  }

  // Volume
  if (m_volume_slider->value() != settings.GetVolume())
  {
    settings.SetVolume(m_volume_slider->value());
    OnVolumeChanged(settings.GetVolume());

    volume_changed = true;
  }

  // DPL2
  if (SConfig::GetInstance().bDPL2Decoder != m_dolby_pro_logic->isChecked())
  {
    SConfig::GetInstance().bDPL2Decoder = m_dolby_pro_logic->isChecked();
    if (!m_dolby_pro_logic->isChecked())
    {
      m_dolby_pro_logic->setText(tr("Dolby Pro Logic II (5.1)"));
    }
    backend_setting_changed = true;
    surround_enabled_changed = true;
  }
  if (Config::Get(Config::MAIN_DPL2_QUALITY) !=
      static_cast<AudioCommon::DPL2Quality>(m_dolby_quality_slider->value()))
  {
    Config::SetBase(Config::MAIN_DPL2_QUALITY,
                    static_cast<AudioCommon::DPL2Quality>(m_dolby_quality_slider->value()));
    m_dolby_quality_latency_label->setText(
        GetDPL2QualityAndLatencyLabel(Config::Get(Config::MAIN_DPL2_QUALITY)));
    backend_setting_changed = true;  // Not a mistake
  }
  // If we have disabled surround while the game is running, disable all its settings immediately,
  // don't wait for the timer
  if (AudioCommon::SupportsDPL2Decoder(backend) && !m_dsp_hle->isChecked() &&
      (!m_running || (surround_enabled_changed && !m_dolby_pro_logic->isChecked())))
  {
    EnableDolbyQualityWidgets(m_dolby_pro_logic->isEnabled() && m_dolby_pro_logic->isChecked());
  }

  // Latency
  if (m_latency_control_supported &&
      SConfig::GetInstance().iAudioBackendLatency != m_latency_spin->value())
  {
    SConfig::GetInstance().iAudioBackendLatency = m_latency_spin->value();
    backend_setting_changed = true;
  }

  // Sample rate
  if (m_use_os_sample_rate->isChecked() != SConfig::GetInstance().bUseOSMixerSampleRate)
  {
    SConfig::GetInstance().bUseOSMixerSampleRate = m_use_os_sample_rate->isChecked();
    backend_setting_changed = true;
  }

  // Stretch
  SConfig::GetInstance().m_audio_stretch = m_stretching_enable->isChecked();
  SConfig::GetInstance().m_audio_emu_speed_tolerance = m_emu_speed_tolerance_slider->value();
  if (m_emu_speed_tolerance_slider->value() < 0)
    m_emu_speed_tolerance_indicator->setText(tr("Disabled"));
  else if (m_emu_speed_tolerance_slider->value() == 0)
    m_emu_speed_tolerance_indicator->setText(tr("None"));
  else
    m_emu_speed_tolerance_indicator->setText(
        tr("%1 ms").arg(m_emu_speed_tolerance_slider->value()));

#ifdef _WIN32
  // If left at default, Dolphin will automatically
  // pick a device and sample rate
  std::string device = WASAPI_DEFAULT_DEVICE_NAME;

  if (m_wasapi_device_combo->currentIndex() > 0)
    device = m_wasapi_device_combo->currentText().toStdString();

  bool device_changed = SConfig::GetInstance().sWASAPIDevice != device;

  // Force OnWASAPIDeviceChanged() to be called if we have the default device to update some labels
  if (device_changed || device == WASAPI_DEFAULT_DEVICE_NAME)
  {
    assert(device != ""); //To turn into a log and delete the include WARN_LOG(AUDIO, "WASAPI: Device names called \"%s\" are not supported", DEFAULT_DEVICE_NAME);
    SConfig::GetInstance().sWASAPIDevice = device;

    bool is_wasapi = backend == BACKEND_WASAPI;
    if (is_wasapi)
    {
      OnWASAPIDeviceChanged();
      if (device_changed)
      {
        LoadWASAPIDeviceSampleRate();
        backend_setting_changed = true;
      }
    }
  }

  std::string device_sample_rate = GetWASAPIDeviceSampleRate();
  if (SConfig::GetInstance().sWASAPIDeviceSampleRate != device_sample_rate)
  {
    SConfig::GetInstance().sWASAPIDeviceSampleRate = device_sample_rate;
    backend_setting_changed = true;
  }
#endif

  AudioCommon::UpdateSoundStreamSettings(volume_changed, backend_setting_changed,
                                         surround_enabled_changed);
}

void AudioPane::OnDSPChanged()
{
  const auto backend = SConfig::GetInstance().sBackend;

  m_dolby_pro_logic->setEnabled(AudioCommon::SupportsDPL2Decoder(backend) &&
                                !m_dsp_hle->isChecked());
  EnableDolbyQualityWidgets(m_dolby_pro_logic->isEnabled() && m_dolby_pro_logic->isChecked());
}

void AudioPane::OnBackendChanged()
{
  const auto backend = SConfig::GetInstance().sBackend;

  m_use_os_sample_rate->setEnabled(backend != BACKEND_NULLSOUND);

  m_dolby_pro_logic->setEnabled(AudioCommon::SupportsDPL2Decoder(backend) &&
                                !m_dsp_hle->isChecked());
  EnableDolbyQualityWidgets(m_dolby_pro_logic->isEnabled() && m_dolby_pro_logic->isChecked());

  if (m_latency_control_supported)
  {
    m_latency_label->setEnabled(AudioCommon::SupportsLatencyControl(backend));
    m_latency_spin->setEnabled(AudioCommon::SupportsLatencyControl(backend));
  }

#ifdef _WIN32
  bool is_wasapi = backend == BACKEND_WASAPI;
  m_wasapi_device_label->setHidden(!is_wasapi);
  m_wasapi_device_sample_rate_label->setHidden(!is_wasapi);
  m_wasapi_device_combo->setHidden(!is_wasapi);
  m_wasapi_device_sample_rate_combo->setHidden(!is_wasapi);

  m_use_os_sample_rate->setHidden(is_wasapi);

  if (is_wasapi)
  {
    m_ignore_save_settings = true;

    m_wasapi_device_combo->clear();
    m_wasapi_device_combo->addItem(tr("Default Device"));

    for (const auto device : WASAPIStream::GetAvailableDevices())
      m_wasapi_device_combo->addItem(QString::fromStdString(device));

    OnWASAPIDeviceChanged();

    m_ignore_save_settings = false;
  }
#endif

  m_volume_slider->setEnabled(AudioCommon::SupportsVolumeChanges(backend));
  m_volume_indicator->setEnabled(AudioCommon::SupportsVolumeChanges(backend));
}

#ifdef _WIN32
void AudioPane::OnWASAPIDeviceChanged()
{
  m_ignore_save_settings = true;

  m_wasapi_device_sample_rate_combo->clear();
  m_wasapi_device_supports_default_sample_rate = false;
  // Don't allow users to select a sample rate for the default device,
  // even though that would be possible, the default device can change
  // at any time so it wouldn't make sense
  bool can_select_device_sample_rate =
      SConfig::GetInstance().sWASAPIDevice != WASAPI_DEFAULT_DEVICE_NAME;
  if (can_select_device_sample_rate)
  {
    m_wasapi_device_sample_rate_combo->setEnabled(true);
    m_wasapi_device_sample_rate_label->setEnabled(true);

    for (const auto device_sample_rate : WASAPIStream::GetSelectedDeviceSampleRates())
    {
      if (device_sample_rate == AudioCommon::GetDefaultSampleRate())
      {
        m_wasapi_device_supports_default_sample_rate = true;
      }
      m_wasapi_device_sample_rate_combo->addItem(
          QString::number(device_sample_rate).append(tr(" Hz")));
    }

    // For clarity, add the default sample rate as a special, first, setting
    if (m_wasapi_device_supports_default_sample_rate)
    {
      m_wasapi_device_sample_rate_combo->insertItem(
          0, tr("Default Dolphin Sample Rate (%1 Hz)").arg(AudioCommon::GetDefaultSampleRate()));
    }
  }
  else
  {
    m_wasapi_device_sample_rate_combo->setEnabled(false);
    m_wasapi_device_sample_rate_label->setEnabled(false);
    m_wasapi_device_sample_rate_combo->addItem(
        tr("Select a Device (%1 Hz)").arg(AudioCommon::GetOSMixerSampleRate()));
  }

  m_ignore_save_settings = false;
}

void AudioPane::LoadWASAPIDevice()
{
  if (SConfig::GetInstance().sWASAPIDevice == WASAPI_DEFAULT_DEVICE_NAME)
  {
    m_wasapi_device_combo->setCurrentIndex(0);
  }
  else
  {
    QString device = QString::fromStdString(SConfig::GetInstance().sWASAPIDevice);
    m_wasapi_device_combo->setCurrentText(device);
    // Saved device not found, reset it (don't reset the saved sample rate)
    if (m_wasapi_device_combo->currentText() != device)
    {
      m_wasapi_device_combo->setCurrentIndex(0);
      SConfig::GetInstance().sWASAPIDevice = WASAPI_DEFAULT_DEVICE_NAME;
    }
  }
}

void AudioPane::LoadWASAPIDeviceSampleRate()
{
  bool can_select_device_sample_rate =
      SConfig::GetInstance().sWASAPIDevice != WASAPI_DEFAULT_DEVICE_NAME;
  if (SConfig::GetInstance().sWASAPIDeviceSampleRate == WASAPI_INVALID_SAMPLE_RATE ||
      !can_select_device_sample_rate)
  {
    // Even if we had specified a custom device thay didn't support Dolphin's default sample rate,
    // we pre-select the highest possible sample rate from the list and leave users the choice
    m_wasapi_device_sample_rate_combo->setCurrentIndex(0);
    SConfig::GetInstance().sWASAPIDeviceSampleRate = GetWASAPIDeviceSampleRate();
  }
  else
  {
    QString device_sample_rate =
        tr("%1 Hz").arg(QString::fromStdString(SConfig::GetInstance().sWASAPIDeviceSampleRate));
    m_wasapi_device_sample_rate_combo->setCurrentText(device_sample_rate);
    // Saved sample rate not found, reset it
    if (m_wasapi_device_sample_rate_combo->currentText() != device_sample_rate)
    {
      m_wasapi_device_sample_rate_combo->setCurrentIndex(0);
      SConfig::GetInstance().sWASAPIDeviceSampleRate = GetWASAPIDeviceSampleRate();
    }
  }
}

std::string AudioPane::GetWASAPIDeviceSampleRate() const
{
  bool can_select_device_sample_rate =
      SConfig::GetInstance().sWASAPIDevice != WASAPI_DEFAULT_DEVICE_NAME;
  if ((!m_wasapi_device_supports_default_sample_rate ||
       m_wasapi_device_sample_rate_combo->currentIndex() > 0) &&
      can_select_device_sample_rate)
  {
    QString device_sample_rate = m_wasapi_device_sample_rate_combo->currentText();
    device_sample_rate.chop(tr(" Hz").length());  // Remove " Hz" from the end
    return device_sample_rate.toStdString();
  }
  return WASAPI_INVALID_SAMPLE_RATE;
}
#endif

void AudioPane::OnEmulationStateChanged(bool running)
{
  m_running = running;

  const auto backend = SConfig::GetInstance().sBackend;

  bool supports_current_emulation_state =
      !running || AudioCommon::BackendSupportsRuntimeSettingsChanges();

  m_dsp_hle->setEnabled(!running);
  m_dsp_lle->setEnabled(!running);
  m_dsp_interpreter->setEnabled(!running);
  m_backend_label->setEnabled(!running);
  m_backend_combo->setEnabled(!running);

  m_use_os_sample_rate->setEnabled(supports_current_emulation_state &&
                                   backend != BACKEND_NULLSOUND);

  if (AudioCommon::SupportsDPL2Decoder(backend) && !m_dsp_hle->isChecked())
  {
    bool enable_dolby_pro_logic = supports_current_emulation_state;
    m_dolby_pro_logic->setEnabled(enable_dolby_pro_logic);
    EnableDolbyQualityWidgets(m_dolby_pro_logic->isEnabled() && m_dolby_pro_logic->isChecked());
    if (!m_running)
      m_dolby_pro_logic->setText(tr("Dolby Pro Logic II (5.1)"));
  }
  if (m_latency_control_supported)
  {
    bool enable_latency =
        supports_current_emulation_state && AudioCommon::SupportsLatencyControl(backend);
    m_latency_label->setEnabled(enable_latency);
    m_latency_spin->setEnabled(enable_latency);
  }

#ifdef _WIN32
  m_wasapi_device_label->setEnabled(supports_current_emulation_state);
  m_wasapi_device_combo->setEnabled(supports_current_emulation_state);
  bool can_select_device_sample_rate =
      SConfig::GetInstance().sWASAPIDevice != WASAPI_DEFAULT_DEVICE_NAME &&
      supports_current_emulation_state;
  m_wasapi_device_sample_rate_label->setEnabled(can_select_device_sample_rate);
  m_wasapi_device_sample_rate_combo->setEnabled(can_select_device_sample_rate);
  bool is_wasapi = backend == BACKEND_WASAPI;
  if (is_wasapi)
  {
    OnWASAPIDeviceChanged();
    m_ignore_save_settings = true;
    LoadWASAPIDeviceSampleRate();
    m_ignore_save_settings = false;
  }
#endif
}

void AudioPane::OnVolumeChanged(int volume)
{
  m_ignore_save_settings = true;
  m_volume_slider->setValue(volume);
  m_ignore_save_settings = false;
  m_volume_indicator->setText(tr("%1 %").arg(volume));
}

void AudioPane::OnCustomShowPopup(QWidget* widget)
{
#ifdef _WIN32
  // Refresh the WASAPI devices every time we try to select them
  if (widget == m_wasapi_device_combo)
  {
    m_ignore_save_settings = true;

    m_wasapi_device_combo->clear();
    m_wasapi_device_combo->addItem(tr("Default Device"));

    for (const auto device : WASAPIStream::GetAvailableDevices())
      m_wasapi_device_combo->addItem(QString::fromStdString(device));

    std::string device = SConfig::GetInstance().sWASAPIDevice;

    LoadWASAPIDevice();

    m_ignore_save_settings = false;

    if (device != SConfig::GetInstance().sWASAPIDevice)
    {
      // Restore it so that saving will trigger OnWASAPIDeviceChanged() again
      SConfig::GetInstance().sWASAPIDevice = device;

      SaveSettings();
    }
  }
#endif
}

void AudioPane::CheckNeedForLatencyControl()
{
  std::vector<std::string> backends = AudioCommon::GetSoundBackends();
  // Don't show latency related widgets if none of our backends support latency
  m_latency_control_supported =
      std::any_of(backends.cbegin(), backends.cend(), AudioCommon::SupportsLatencyControl);
}

QString AudioPane::GetDPL2QualityAndLatencyLabel(AudioCommon::DPL2Quality value) const
{
  switch (value)
  {
  case AudioCommon::DPL2Quality::Low:
    return tr("Low (Block Size: ~%1 ms)").arg(10);
  case AudioCommon::DPL2Quality::High:
    return tr("High (Block Size: ~%1 ms)").arg(50);
  case AudioCommon::DPL2Quality::Extreme:
    return tr("Extreme (Block Size: ~%1 ms)").arg(80);
  case AudioCommon::DPL2Quality::Normal:
  default:
    return tr("Normal (Block Size: ~%1 ms)").arg(30);
  }
}

void AudioPane::RefreshDolbyWidgets()
{
  const auto backend = SConfig::GetInstance().sBackend;
  if (AudioCommon::SupportsDPL2Decoder(backend) && !m_dsp_hle->isChecked() && m_running)
  {
    bool surround_enabled = AudioCommon::IsSurroundEnabled();
    if (!surround_enabled && SConfig::GetInstance().bDPL2Decoder)
    {
      // This message will likely only appear if the audio backend has failed to
      // initialize because of 5.1, if it fails before that, it won't disable surround
      m_dolby_pro_logic->setText(tr("Dolby Pro Logic II (5.1) (FAILED)"));
    }
    else
    {
      m_dolby_pro_logic->setText(tr("Dolby Pro Logic II (5.1)"));
    }
    EnableDolbyQualityWidgets(surround_enabled &&
                              AudioCommon::BackendSupportsRuntimeSettingsChanges());
  }
}

void AudioPane::EnableDolbyQualityWidgets(bool enabled) const
{
  m_dolby_quality_label->setEnabled(enabled);
  m_dolby_quality_slider->setEnabled(enabled);
  m_dolby_quality_latency_label->setEnabled(enabled);
}
