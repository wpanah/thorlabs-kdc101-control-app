#include "motor_control.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <initializer_list>
#include <memory>

#include "shared/display_text.h"

namespace ui {
namespace {

// Validate on commit so incomplete edits also report an error on focus loss.
// QLineEdit suppresses editingFinished for intermediate validator states.
std::optional<double> ReadMotionValue(QString text) {
  QDoubleValidator validator(0.000001, 1000000, 6);
  validator.setNotation(QDoubleValidator::StandardNotation);
  validator.setLocale(QLocale::c());
  int cursor = 0;
  if (validator.validate(text, cursor) != QValidator::Acceptable)
    return std::nullopt;
  return validator.locale().toDouble(text);
}

QString MotionValueText(std::optional<double> value) {
  return value ? QString::number(*value, 'g', 12) : QString{};
}

// A hold is one gesture: dragging out stops it, and dragging back in cannot
// restart it.
class HoldDriveButton : public QPushButton {
 public:
  HoldDriveButton(const QString& text, QWidget* parent)
      : QPushButton(text, parent) {
    setAutoRepeat(false);
    window()->installEventFilter(this);
  }

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint())) {
      BeginHold();
      event->accept();
    } else {
      event->ignore();
    }
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) EndHold();
    event->accept();
  }
  void mouseMoveEvent(QMouseEvent* event) override {
    if (!rect().contains(event->position().toPoint())) EndHold();
    event->accept();
  }
  void keyPressEvent(QKeyEvent* event) override {
    if (IsHoldKey(event->key())) {
      if (!event->isAutoRepeat()) BeginHold();
      event->accept();
    } else {
      QPushButton::keyPressEvent(event);
    }
  }
  void keyReleaseEvent(QKeyEvent* event) override {
    if (IsHoldKey(event->key())) {
      if (!event->isAutoRepeat()) EndHold();
      event->accept();
    } else {
      QPushButton::keyReleaseEvent(event);
    }
  }
  bool event(QEvent* event) override {
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide ||
        (event->type() == QEvent::EnabledChange && !isEnabled()))
      EndHold();
    return QPushButton::event(event);
  }
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::Hide || event->type() == QEvent::Close)
      EndHold();
    return QPushButton::eventFilter(watched, event);
  }

 private:
  static bool IsHoldKey(int key) {
    return key == Qt::Key_Space || key == Qt::Key_Return ||
           key == Qt::Key_Enter;
  }
  void BeginHold() {
    if (holding_ || !isEnabled()) return;
    holding_ = true;
    setDown(true);
    emit pressed();
  }
  void EndHold() {
    if (!holding_) return;
    holding_ = false;
    setDown(false);
    emit released();
  }
  bool holding_ = false;
};

QPushButton* CreateCommandButton(const QString& text, QWidget* parent,
                                 bool hold = false) {
  auto* button =
      hold ? static_cast<QPushButton*>(new HoldDriveButton(text, parent))
           : new QPushButton(text, parent);
  button->setMinimumWidth(88);
  button->setFixedHeight(28);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  button->setEnabled(false);
  button->setToolTip(QObject::tr("Motor is not connected."));

  return button;
}

QWidget* CreateAxisControl(MainWindow& view, ui::Axis axis_id,
                           const QString& axis, QWidget* parent) {
  auto* section = new QGroupBox(QObject::tr("%1 Axis").arg(axis), parent);
  section->setObjectName(QStringLiteral("axis%1").arg(axis));

  auto* layout = new QVBoxLayout(section);
  layout->setContentsMargins(10, 2, 10, 2);
  layout->setSpacing(2);

  auto* header = new QHBoxLayout();
  header->setSpacing(6);
  header->addStretch();
  auto* indicator = new QLabel(section);
  indicator->setFixedSize(8, 8);
  indicator->setStyleSheet("background: #ed1735; border-radius: 4px;");
  header->addWidget(indicator);
  auto* status = new QLabel(QObject::tr("Disconnected"), section);
  status->setAccessibleName(QObject::tr("%1 axis connection status").arg(axis));
  status->setStyleSheet("color: #52627c; font-size: 13px;");
  header->addWidget(status);
  layout->addLayout(header);

  auto* body = new QHBoxLayout();
  body->setSpacing(12);
  layout->addLayout(body, 1);
  auto* manual = new QVBoxLayout();
  manual->setSpacing(2);
  body->addLayout(manual, 4);
  auto* position_label = new QLabel(QObject::tr("Current position"), section);
  position_label->setObjectName(QStringLiteral("axisFieldLabel"));
  manual->addWidget(position_label);
  auto* readout_widget = new QWidget(section);
  readout_widget->setFixedHeight(28);
  readout_widget->setObjectName(QStringLiteral("axisReadout"));
  readout_widget->setStyleSheet(
      "QWidget#axisReadout { background: transparent; }");
  auto* readout = new QHBoxLayout(readout_widget);
  readout->setContentsMargins(0, 0, 0, 0);
  readout->setSpacing(6);
  auto* position = new QLabel(QStringLiteral("--"), section);
  position->setAccessibleName(
      QObject::tr("%1 axis current position").arg(axis));
  position->setStyleSheet("font-size: 22px; font-weight: 600;");
  position->setToolTip(
      QObject::tr("Position is unavailable until the motor is connected."));
  readout->addWidget(position);
  readout->addWidget(new QLabel(QObject::tr("mm"), section), 0,
                     Qt::AlignBottom);
  readout->addStretch();
  manual->addWidget(readout_widget);
  manual->addStretch();

  auto* jog_down = CreateCommandButton(QStringLiteral("<"), section);
  auto* jog_up = CreateCommandButton(QStringLiteral(">"), section);
  auto* drive_down = CreateCommandButton(QStringLiteral("<<"), section, true);
  auto* drive_up = CreateCommandButton(QStringLiteral(">>"), section, true);
  jog_up->setAccessibleName(QObject::tr("%1 axis jog up").arg(axis));
  jog_down->setAccessibleName(QObject::tr("%1 axis jog down").arg(axis));
  drive_up->setAccessibleName(
      QObject::tr("%1 axis continuous move up").arg(axis));
  drive_down->setAccessibleName(
      QObject::tr("%1 axis continuous move down").arg(axis));
  for (auto* button : {jog_down, jog_up, drive_down, drive_up}) {
    button->setMinimumWidth(36);
  }
  auto* motion = new QGridLayout();
  motion->setHorizontalSpacing(6);
  motion->setVerticalSpacing(2);
  auto* jog_label = new QLabel(QObject::tr("Jog"), section);
  auto* drive_label = new QLabel(QObject::tr("Drive"), section);
  jog_label->setObjectName(QStringLiteral("axisFieldLabel"));
  drive_label->setObjectName(QStringLiteral("axisFieldLabel"));
  motion->addWidget(jog_label, 0, 0, 1, 2);
  motion->addWidget(drive_label, 0, 3, 1, 2);
  motion->setColumnMinimumWidth(2, 8);
  motion->addWidget(jog_down, 1, 0);
  motion->addWidget(jog_up, 1, 1);
  motion->addWidget(drive_down, 1, 3);
  motion->addWidget(drive_up, 1, 4);
  manual->addLayout(motion);
  manual->addSpacing(2);
  auto* actions = new QHBoxLayout();
  actions->setSpacing(8);
  auto* home = CreateCommandButton(QObject::tr("Home"), section);
  home->setMinimumWidth(72);
  home->setAccessibleName(QObject::tr("Home %1 axis").arg(axis));
  auto* stop = CreateCommandButton(QObject::tr("\u25a0 Stop"), section);
  // Keep a keyboard-held Drive pressed until the mouse click reaches Stop.
  stop->setFocusPolicy(Qt::TabFocus);
  stop->setMinimumWidth(72);
  stop->setObjectName(QStringLiteral("axisStop"));
  stop->setAccessibleName(QObject::tr("Stop %1 axis").arg(axis));
  actions->addWidget(home);
  actions->addWidget(stop);
  manual->addLayout(actions);

  auto* divider = new QFrame(section);
  divider->setFixedWidth(1);
  divider->setStyleSheet("background: #e1e5eb;");
  body->addWidget(divider);
  auto* inputs = new QGridLayout();
  inputs->setHorizontalSpacing(8);
  inputs->setVerticalSpacing(2);
  inputs->setColumnStretch(0, 1);
  body->addLayout(inputs, 5);
  auto add_field_label = [section, inputs](const QString& text, int row) {
    auto* label = new QLabel(text, section);
    label->setObjectName(QStringLiteral("axisFieldLabel"));
    inputs->addWidget(label, row, 0, 1, 3);
    return label;
  };
  auto* move_label = add_field_label(QObject::tr("Move to"), 0);
  move_label->setAccessibleName(
      QObject::tr("%1 axis applied travel range").arg(axis));
  move_label->setToolTip(
      QObject::tr("The motor slows near a limit. Boundary targets leave "
                  "0.00025 mm of settling clearance."));
  QObject::connect(
      &view, &MainWindow::AxisSettingsUpdated, section,
      [=](ui::Axis changed, ui::MotorSettings settings) {
        if (changed != axis_id) return;
        const auto limits = settings.travel_limits;
        move_label->setText(
            limits ? QObject::tr("Move to (%1\u2013%2 mm)")
                         .arg(QString::number(limits->minimum_mm, 'g', 12),
                              QString::number(limits->maximum_mm, 'g', 12))
                   : QObject::tr("Move to (limits unverified)"));
      });

  auto* absolute_position = new QDoubleSpinBox(section);
  absolute_position->setDecimals(3);
  absolute_position->setRange(-1000000.0, 1000000.0);
  absolute_position->setButtonSymbols(QAbstractSpinBox::NoButtons);
  absolute_position->setMinimumWidth(84);
  absolute_position->setFixedHeight(28);
  absolute_position->setAlignment(Qt::AlignRight);
  absolute_position->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  absolute_position->setEnabled(false);
  absolute_position->setAccessibleName(
      QObject::tr("%1 axis absolute position").arg(axis));
  absolute_position->setToolTip(
      QObject::tr("Units and travel limits require a configured stage."));
  inputs->addWidget(absolute_position, 1, 0);
  inputs->addWidget(new QLabel(QObject::tr("mm"), section), 1, 1);

  auto* go_button = CreateCommandButton(QObject::tr("Move"), section);
  go_button->setProperty("primary", true);
  inputs->addWidget(go_button, 1, 2);

  add_field_label(QObject::tr("Step size"), 2);

  auto* step_size = new QComboBox(section);
  step_size->setEditable(true);
  step_size->addItems({QString::number(thorlabs::kDefaultJogStepMm),
                       QStringLiteral("0.1"), QStringLiteral("1.0")});
  step_size->setInsertPolicy(QComboBox::NoInsert);

  step_size->setCompleter(nullptr);
  step_size->setMinimumWidth(84);
  step_size->setFixedHeight(28);
  step_size->lineEdit()->setAlignment(Qt::AlignRight);
  step_size->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  step_size->setAccessibleName(
      QObject::tr("%1 axis jog step size in mm").arg(axis));
  step_size->setToolTip(
      QObject::tr("Choose a preset or type a positive step size in mm."));
  step_size->setStyleSheet(
      "QComboBox { background: white; border: 1px solid #cfd8e5; "
      "border-radius: 4px; "
      "padding: 1px 5px; min-height: 18px; }"
      "QComboBox:focus { border-color: #126bf0; }"
      "QComboBox QLineEdit { border: none; padding: 0; min-height: 0; }");
  inputs->addWidget(step_size, 3, 0);
  inputs->addWidget(new QLabel(QObject::tr("mm"), section), 3, 1);
  step_size->setEnabled(false);

  struct SubmittedSettings {
    std::optional<double> step;
    std::optional<double> speed;
    bool shared_speed_applied = false;
    bool speed_edited = false;
    bool pending_step = false;
    bool pending_speed = false;
  };
  const auto submitted = std::make_shared<SubmittedSettings>();
  step_size->setToolTip(
      QObject::tr("Selecting a preset applies it immediately. For a custom "
                  "step in mm, press Enter or leave the field."));
  const auto submit_step = [&view, axis_id, axis, step_size, submitted] {
    if (!step_size->isEnabled() || submitted->pending_step) return;
    const auto step = ReadMotionValue(step_size->currentText());
    if (!step) {
      step_size->setCurrentText(MotionValueText(submitted->step));
      view.ShowError(
          {axis_id, "Configure jog", "Enter a positive step size in mm."});
      return;
    }
    if (submitted->step == step) return;
    submitted->pending_step = true;
    ui::MotorSettings settings;
    settings.jog_step_mm = step;
    view.ShowMessage(QObject::tr("%1 axis: applying step size...").arg(axis));
    emit view.ConfigureAxisRequested(axis_id, settings);
  };
  QObject::connect(step_size, &QComboBox::activated, section,
                   [submit_step](int) { submit_step(); });
  QObject::connect(step_size->lineEdit(), &QLineEdit::editingFinished, section,
                   submit_step);
  add_field_label(QObject::tr("Speed"), 4);
  auto* speed = new QComboBox(section);
  speed->setEditable(true);
  speed->addItems({"0.01", "0.1", "1"});
  speed->setInsertPolicy(QComboBox::NoInsert);
  speed->setCurrentText(
      QString::number(thorlabs::kDefaultMoveSpeedMmPerSecond));
  speed->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  speed->setCompleter(nullptr);
  speed->setMinimumWidth(84);
  speed->setFixedHeight(28);
  speed->lineEdit()->setAlignment(Qt::AlignRight);
  // Apply styling after sizing, in the same order as the step-size field.
  speed->setStyleSheet(step_size->styleSheet());
  speed->setEnabled(false);
  speed->setAccessibleName(
      QObject::tr("%1 axis speed in mm per second").arg(axis));
  speed->setToolTip(
      QObject::tr("Choose a preset in mm/s, or type a speed and press Enter or "
                  "leave the field to apply it to moves, "
                  "jogging and homing."));
  inputs->addWidget(speed, 5, 0);
  inputs->addWidget(new QLabel(QObject::tr("mm/s"), section), 5, 1);
  const auto submit_speed = [&view, axis_id, axis, speed, submitted](
                                const QString& text, bool preset = false) {
    if (!speed->isEnabled() || submitted->pending_speed) return;
    submitted->speed_edited = false;
    const auto value = ReadMotionValue(text);
    if (!value) {
      speed->setCurrentText(MotionValueText(submitted->speed));
      view.ShowError(
          {axis_id, "Configure speed", "Enter a positive speed in mm/s."});
      return;
    }
    if (!preset && submitted->shared_speed_applied && submitted->speed == value)
      return;
    submitted->pending_speed = true;
    ui::MotorSettings settings;
    settings.move.speed_mm_per_second = value;
    settings.jog.speed_mm_per_second = value;
    settings.homing_speed_mm_per_second = value;
    view.ShowMessage(QObject::tr("%1 axis: applying speed...").arg(axis));
    emit view.ConfigureAxisRequested(axis_id, settings);
  };
  QObject::connect(speed, &QComboBox::activated, section,
                   [speed, submit_speed](int index) {
                     submit_speed(speed->itemText(index), true);
                   });
  QObject::connect(speed->lineEdit(), &QLineEdit::textEdited, section,
                   [submitted] { submitted->speed_edited = true; });
  QObject::connect(
      speed->lineEdit(), &QLineEdit::editingFinished, section,
      [speed, section, submitted, submit_speed] {
        // Opening the dropdown also ends editing. Wait until Qt
        // has processed that focus change before applying typed
        // text.
        QTimer::singleShot(0, section, [speed, submitted, submit_speed] {
          if (submitted->speed_edited && !speed->view()->isVisible())
            submit_speed(speed->currentText());
        });
      });
  go_button->setFixedWidth(94);
  auto jog = [&view, axis_id](ui::Direction direction) {
    emit view.JogAxisRequested(axis_id, direction);
  };
  QObject::connect(jog_up, &QPushButton::clicked, section,
                   [jog] { jog(ui::Direction::kForward); });
  QObject::connect(jog_down, &QPushButton::clicked, section,
                   [jog] { jog(ui::Direction::kBackward); });
  for (auto* button : {drive_up, drive_down}) {
    const auto direction =
        button == drive_up ? ui::Direction::kForward : ui::Direction::kBackward;
    auto driving = std::make_shared<bool>(false);
    QObject::connect(button, &QPushButton::pressed, section, [=, &view] {
      *driving = true;
      emit view.DriveAxisRequested(axis_id, direction);
    });
    // A drive can finish at the boundary while the button is still held.
    // Its eventual release must not stop a subsequent command.
    QObject::connect(&view, &MainWindow::AxisRequestsPending, section,
                     [=](ui::Axis changed, bool pending, bool) {
                       if (changed == axis_id && !pending) *driving = false;
                     });
    QObject::connect(button, &QPushButton::released, section, [=, &view] {
      if (*driving && stop->isEnabled())
        emit view.StopAxisRequested(axis_id, ui::StopMode::kProfiled);
      *driving = false;
    });
  }
  QObject::connect(home, &QPushButton::clicked, section,
                   [&view, axis_id] { emit view.HomeAxisRequested(axis_id); });
  QObject::connect(stop, &QPushButton::clicked, section, [&view, axis_id] {
    emit view.StopAxisRequested(axis_id, ui::StopMode::kProfiled);
  });
  QObject::connect(go_button, &QPushButton::clicked, section,
                   [&view, axis_id, absolute_position] {
                     emit view.MoveAxisRequested(axis_id,
                                                 absolute_position->value());
                   });
  go_button->setAccessibleName(QObject::tr("Move %1 axis").arg(axis));
  for (auto* button :
       {jog_up, jog_down, drive_up, drive_down, home, stop, go_button}) {
    button->setToolTip(QString());
  }
  home->setToolTip(
      QObject::tr("Home seeks the hardware reference and may travel outside "
                  "the software limits."));
  jog_down->setToolTip(
      QObject::tr("Jog one applied step in the negative direction."));
  jog_up->setToolTip(
      QObject::tr("Jog one applied step in the positive direction."));
  drive_down->setToolTip(QObject::tr(
      "Hold to move to the software minimum. Release to stop early."));
  drive_up->setToolTip(QObject::tr(
      "Hold to move to the software maximum. Release to stop early."));
  struct Availability {
    ui::AxisState state{};
    bool pending = false;
    bool stopping = false;
    bool recovering = false;
  };
  const auto availability = std::make_shared<Availability>();
  const auto refresh = [=] {
    const auto& state = availability->state;
    const bool connected = state.connection == ui::ConnectionState::kConnected;
    const bool ready = connected &&
                       state.operation == ui::OperationState::kIdle &&
                       !availability->pending && !availability->recovering;
    for (auto* button : {jog_up, jog_down, home, go_button}) {
      button->setEnabled(ready);
    }
    stop->setEnabled(connected && !availability->stopping &&
                     state.operation != ui::OperationState::kStopping);
    // Keep the held button enabled so it receives mouse/key release while the
    // drive request is pending; all other movement controls remain locked.
    for (auto* button : {drive_up, drive_down}) {
      button->setEnabled(
          ready || (button->isDown() && connected && !availability->stopping &&
                    state.operation != ui::OperationState::kStopping));
    }
    absolute_position->setEnabled(ready);
    step_size->setEnabled(ready);
    speed->setEnabled(ready);
    const bool applying = availability->pending &&
                          (submitted->pending_step || submitted->pending_speed);
    status->setText(ConnectionText(state.connection) + " / " +
                    (applying ? QObject::tr("Applying settings...")
                              : OperationText(state.operation)));
  };
  QObject::connect(
      &view, &MainWindow::AxisSettingsUpdated, section,
      [=](ui::Axis axis, ui::MotorSettings settings) {
        if (axis != axis_id) return;
        if (submitted->pending_step || settings.jog_step_mm != submitted->step)
          step_size->setCurrentText(MotionValueText(settings.jog_step_mm));
        if (submitted->pending_speed ||
            settings.move.speed_mm_per_second != submitted->speed)
          speed->setCurrentText(
              MotionValueText(settings.move.speed_mm_per_second));
        submitted->step = settings.jog_step_mm;
        submitted->speed = settings.move.speed_mm_per_second;
      });
  QObject::connect(&view, &MainWindow::AxisSettingsApplied, section,
                   [=](ui::Axis axis, ui::MotorSettings settings) {
                     if (axis != axis_id) return;
                     if (settings.jog_step_mm) {
                       submitted->pending_step = false;
                     }
                     if (settings.move.speed_mm_per_second ||
                         settings.jog.speed_mm_per_second ||
                         settings.homing_speed_mm_per_second) {
                       submitted->shared_speed_applied =
                           settings.move.speed_mm_per_second &&
                           settings.jog.speed_mm_per_second ==
                               settings.move.speed_mm_per_second &&
                           settings.homing_speed_mm_per_second ==
                               settings.move.speed_mm_per_second;
                       submitted->pending_speed = false;
                     }
                     refresh();
                   });
  QObject::connect(&view, &MainWindow::RecoveryControlsEnabled, section,
                   [=, &view](bool) {
                     availability->recovering = view.IsRecovering();
                     refresh();
                   });
  QObject::connect(
      &view, &MainWindow::OperationErrorReported, section,
      [=](ui::OperationError error) {
        // A failed submission must be retryable even if the
        // entered value is unchanged.
        if (error.axis != axis_id || availability->pending) return;
        if (submitted->pending_step)
          step_size->setCurrentText(MotionValueText(submitted->step));
        if (submitted->pending_speed) {
          speed->setCurrentText(MotionValueText(submitted->speed));
          submitted->shared_speed_applied = false;
        }
        submitted->pending_step = submitted->pending_speed = false;
      });
  QObject::connect(&view, &MainWindow::AxisRequestsPending, section,
                   [=](ui::Axis axis, bool pending, bool stopping) {
                     if (axis != axis_id) return;
                     availability->pending = pending;
                     availability->stopping = stopping;
                     refresh();
                   });
  QObject::connect(
      &view, &MainWindow::AxisStateUpdated, section, [=](ui::AxisState state) {
        if (state.axis != axis_id) return;
        if (state.connection == ui::ConnectionState::kConnected &&
            availability->state.connection != ui::ConnectionState::kConnected) {
          step_size->setCurrentText({});
          speed->setCurrentText({});
          *submitted = SubmittedSettings{};
        }
        availability->state = state;
        refresh();
        const bool connected =
            state.connection == ui::ConnectionState::kConnected;
        if (!connected) move_label->setText(QObject::tr("Move to"));
        position->setText(connected && state.position_mm
                              ? QString::number(*state.position_mm, 'f', 3)
                              : QStringLiteral("--"));
        position->setToolTip(QString());
        indicator->setStyleSheet(
            connected ? "background: #00a34a; border-radius: 4px;"
                      : "background: #ed1735; border-radius: 4px;");
      });
  return section;
}
}  // namespace

QWidget* CreateMotorControlSection(MainWindow& view, QWidget* parent) {
  auto* section = new QGroupBox(QObject::tr("Motor control"), parent);
  section->setObjectName(QStringLiteral("motorControls"));

  auto* layout = new QVBoxLayout(section);
  layout->setContentsMargins(6, 2, 6, 2);
  layout->setSpacing(4);
  int axis_index = 0;
  for (const auto& axis :
       {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")}) {
    layout->addWidget(CreateAxisControl(view, static_cast<ui::Axis>(axis_index),
                                        axis, section));
    ++axis_index;
  }

  section->setEnabled(false);
  QObject::connect(&view, &MainWindow::RecoveryControlsEnabled, section,
                   &QWidget::setEnabled);

  return section;
}

}  // namespace ui
