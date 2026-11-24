#include "options.h"

#include <QDoubleValidator>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <cmath>
#include <memory>

#include "main_window/main_window.h"

namespace ui {
namespace {
void BindSettingRow(MainWindow& view, QGroupBox* group, Axis axis,
                    QPushButton* apply, QList<QLineEdit*> fields) {
  struct Availability {
    bool idle = false;
    bool pending = false;
  };
  auto availability = std::make_shared<Availability>();
  const auto refresh = [=] {
    const bool ready = availability->idle && !availability->pending;
    for (auto* field : fields) field->setEnabled(ready);
    apply->setEnabled(ready);
  };
  refresh();
  QObject::connect(
      &view, &MainWindow::AxisStateUpdated, group, [=](AxisState state) {
        if (state.axis != axis) return;
        const bool connected = state.connection == ConnectionState::kConnected;
        availability->idle =
            connected && state.operation == OperationState::kIdle;
        if (!connected)
          for (auto* field : fields) field->clear();
        refresh();
      });
  QObject::connect(&view, &MainWindow::AxisRequestsPending, group,
                   [=](Axis changed, bool pending, bool stopping) {
                     if (changed != axis) return;
                     availability->pending = pending || stopping;
                     refresh();
                   });
}

QGroupBox* CreateLimitsGroup(MainWindow& view, QWidget* parent) {
  auto* group = new QGroupBox(QObject::tr("Software travel limits"), parent);
  auto* grid = new QGridLayout(group);
  grid->addWidget(new QLabel(QObject::tr("Minimum (mm)"), group), 0, 1);
  grid->addWidget(new QLabel(QObject::tr("Maximum (mm)"), group), 0, 2);
  grid->setColumnStretch(1, 1);
  grid->setColumnStretch(2, 1);
  const QStringList axes{"X", "Y", "Z"};
  for (int i = 0; i < axes.size(); ++i) {
    const auto axis = static_cast<Axis>(i);
    auto make_field = [&](const QString& bound, int column) {
      auto* field = new QLineEdit(group);
      field->setAccessibleName(
          QObject::tr("%1 axis software %2").arg(axes.at(i), bound));
      auto* validator = new QDoubleValidator(0, 1000000, 3, field);
      validator->setNotation(QDoubleValidator::StandardNotation);
      validator->setLocale(QLocale::c());
      field->setValidator(validator);
      grid->addWidget(field, i + 1, column);
      return field;
    };
    auto* minimum = make_field("minimum", 1);
    auto* maximum = make_field("maximum", 2);
    auto* apply = new QPushButton(QObject::tr("Apply"), group);
    apply->setAccessibleName(
        QObject::tr("Apply %1 axis software limits").arg(axes.at(i)));
    grid->addWidget(new QLabel(QObject::tr("Axis %1").arg(axes.at(i)), group),
                    i + 1, 0);
    grid->addWidget(apply, i + 1, 3);
    BindSettingRow(view, group, axis, apply, {minimum, maximum});
    QObject::connect(
        &view, &MainWindow::AxisSettingsUpdated, group,
        [=](Axis changed, MotorSettings settings) {
          if (axis != changed) return;
          auto limits = settings.travel_limits;
          minimum->setText(limits ? QString::number(limits->minimum_mm, 'g', 12)
                                  : QString{});
          maximum->setText(limits ? QString::number(limits->maximum_mm, 'g', 12)
                                  : QString{});
        });
    QObject::connect(apply, &QPushButton::clicked, group, [=, &view] {
      if (!minimum->hasAcceptableInput() || !maximum->hasAcceptableInput()) {
        view.ShowError({axis, "Software travel limits",
                        "Enter both limits in millimetres."});
        return;
      }
      MotorSettings settings;
      settings.travel_limits =
          thorlabs::TravelLimits{QLocale::c().toDouble(minimum->text()),
                                 QLocale::c().toDouble(maximum->text())};
      emit view.ConfigureAxisRequested(axis, settings);
    });
  }
  group->setEnabled(false);
  QObject::connect(&view, &MainWindow::ManualControlsEnabled, group,
                   &QWidget::setEnabled);
  return group;
}
enum class Setting { kBacklash, kAcceleration, kHomingVelocity };
std::optional<double> ReadSetting(const MotorSettings& settings,
                                  Setting setting) {
  switch (setting) {
    case Setting::kBacklash:
      return settings.backlash_mm;
    case Setting::kAcceleration:
      return settings.move.acceleration_mm_per_second_squared;
    case Setting::kHomingVelocity:
      return settings.homing_speed_mm_per_second;
  }
  return std::nullopt;
}
MotorSettings UpdateSetting(Setting setting, double value) {
  MotorSettings settings;
  switch (setting) {
    case Setting::kBacklash:
      settings.backlash_mm = value;
      break;
    case Setting::kAcceleration:
      settings.move.acceleration_mm_per_second_squared = value;
      settings.jog.acceleration_mm_per_second_squared = value;
      break;
    case Setting::kHomingVelocity:
      settings.homing_speed_mm_per_second = value;
      break;
  }
  return settings;
}
QGroupBox* CreateSettingGroup(MainWindow& view, QWidget* parent,
                              Setting setting, const QString& title,
                              const QString& unit) {
  auto* group = new QGroupBox(title, parent);
  auto* grid = new QGridLayout(group);
  grid->setColumnStretch(1, 1);
  const QStringList axes{"X", "Y", "Z"};
  for (int i = 0; i < axes.size(); ++i) {
    const auto axis = static_cast<Axis>(i);
    const auto name = axes.at(i);
    auto* input = new QLineEdit(group);
    input->setAccessibleName(QString("%1 axis %2").arg(name, title));
    const bool allow_zero = setting == Setting::kBacklash;
    auto* validator =
        new QDoubleValidator(allow_zero ? 0.0 : 0.000001, 1000000.0, 6, input);
    validator->setNotation(QDoubleValidator::StandardNotation);
    validator->setLocale(QLocale::c());
    input->setValidator(validator);
    auto* apply = new QPushButton(QObject::tr("Apply"), group);
    apply->setAccessibleName(QString("Apply %1 axis %2").arg(name, title));
    grid->addWidget(new QLabel(QObject::tr("%1 axis").arg(name), group), i, 0);
    grid->addWidget(input, i, 1);
    grid->addWidget(new QLabel(unit, group), i, 2);
    grid->addWidget(apply, i, 3);
    BindSettingRow(view, group, axis, apply, {input});
    QObject::connect(
        &view, &MainWindow::AxisSettingsUpdated, group,
        [=](Axis changed, MotorSettings settings) {
          if (changed != axis) return;
          const auto value = ReadSetting(settings, setting);
          input->setText(value ? QString::number(*value, 'g', 12) : QString{});
        });
    QObject::connect(apply, &QPushButton::clicked, group, [=, &view] {
      if (!apply->isEnabled()) return;
      bool ok = false;
      const double value = QLocale::c().toDouble(input->text(), &ok);
      if (!ok || !input->hasAcceptableInput() || !std::isfinite(value) ||
          value < 0 || (!allow_zero && value == 0)) {
        view.ShowError({axis, "Configure " + title.toStdString(),
                        "Enter a valid value in " + unit.toStdString() + "."});
        return;
      }
      emit view.ConfigureAxisRequested(axis, UpdateSetting(setting, value));
    });
  }
  group->setEnabled(false);
  QObject::connect(&view, &MainWindow::ManualControlsEnabled, group,
                   &QWidget::setEnabled);
  return group;
}
}  // namespace
QWidget* CreateOptionsSection(MainWindow& view, QWidget* parent) {
  auto* page = new QWidget(parent);
  auto* layout = new QGridLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(16);
  layout->setColumnStretch(0, 1);
  layout->addWidget(CreateLimitsGroup(view, page), 0, 0);
  layout->addWidget(CreateSettingGroup(view, page, Setting::kBacklash,
                                       QObject::tr("Backlash"), "mm"),
                    1, 0);
  layout->addWidget(
      CreateSettingGroup(view, page, Setting::kAcceleration,
                         QObject::tr("Motor acceleration"), "mm/s²"),
      2, 0);
  layout->addWidget(CreateSettingGroup(view, page, Setting::kHomingVelocity,
                                       QObject::tr("Homing velocity"), "mm/s"),
                    3, 0);
  layout->setRowStretch(4, 1);
  return page;
}
}  // namespace ui
