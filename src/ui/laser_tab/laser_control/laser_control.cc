#include "laser_control.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <limits>
#include <memory>

#include "shared/display_text.h"

namespace ui {
namespace {

// Keep native checkbox keyboard/accessibility behavior, but draw a compact
// switch.
class OutputSwitch : public QCheckBox {
 public:
  explicit OutputSwitch(QWidget* parent) : QCheckBox(parent) {
    setFixedSize(48, 28);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
  }

 protected:
  bool hitButton(const QPoint& point) const override {
    return rect().contains(point);
  }
  // The device observation confirms changes, not the click itself.
  void nextCheckState() override {}
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor track = !isEnabled()  ? QColor("#e0e5ec")
                         : isChecked() ? QColor("#126bf0")
                                       : QColor("#8794a8");
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(QRectF(2, 3, 44, 22), 11, 11);
    painter.setBrush(isEnabled() ? QColor("#ffffff") : QColor("#f3f5f8"));
    painter.drawEllipse(QRectF(isChecked() ? 26 : 5, 6, 16, 16));
    if (hasFocus()) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor("#126bf0"), 1, Qt::DashLine));
      painter.drawRoundedRect(QRectF(0.5, 0.5, 47, 27), 13, 13);
    }
  }
};

}  // namespace

QWidget* CreateLaserControlSection(MainWindow& view, QWidget* parent) {
  auto* section = new QGroupBox(QObject::tr("Laser control"), parent);

  section->setObjectName(QStringLiteral("laser_control"));
  auto* outer_layout = new QVBoxLayout(section);
  auto* manual = new QWidget(section);
  manual->setObjectName(QStringLiteral("laserManual"));
  manual->setStyleSheet("QWidget#laserManual { background: transparent; }");
  auto* layout = new QVBoxLayout(manual);
  layout->setContentsMargins(0, 0, 0, 0);
  outer_layout->addWidget(manual);
  layout->setSpacing(10);

  auto* connection = new QHBoxLayout();
  connection->setSpacing(12);

  auto* indicator = new QLabel(section);
  indicator->setFixedSize(12, 12);
  indicator->setProperty("connected", false);
  indicator->setStyleSheet(
      "QLabel[connected=\"false\"] { background: #ed1735; border-radius: 6px; }"
      "QLabel[connected=\"true\"] { background: #00a34a; border-radius: 6px; "
      "}");

  auto* connection_status = new QLabel(QObject::tr("Disconnected"), section);
  connection_status->setAccessibleName(QObject::tr("Laser connection status"));
  connection->addWidget(indicator);
  connection->addWidget(connection_status);
  connection->addStretch();
  layout->addLayout(connection);

  auto* channel_fields = new QWidget(manual);
  auto* channel_form = new QFormLayout(channel_fields);
  channel_form->setContentsMargins(0, 0, 0, 0);
  auto* device = new QLineEdit(channel_fields);
  device->setPlaceholderText(QObject::tr("NI MAX name (e.g. Dev1)"));
  channel_form->addRow(QObject::tr("NI device"), device);
  auto* port = new QComboBox(channel_fields);
  port->setPlaceholderText(QObject::tr("Select port"));
  port->addItems(
      {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2")});
  port->setCurrentIndex(-1);
  channel_form->addRow(QObject::tr("Port"), port);
  auto* line = new QComboBox(channel_fields);
  line->setPlaceholderText(QObject::tr("Select line"));
  line->setEnabled(false);
  channel_form->addRow(QObject::tr("Line"), line);
  QObject::connect(port, &QComboBox::currentIndexChanged, line,
                   [line](int port_index) {
                     line->clear();
                     // The PCIe-6323 has 32 lines on port 0 and eight each on
                     // ports 1 and 2.
                     const int line_count = port_index == 0 ? 32 : 8;
                     for (int i = 0; i < line_count; ++i)
                       line->addItem(QString::number(i));
                     line->setCurrentIndex(-1);
                     line->setEnabled(true);
                   });
  layout->addWidget(channel_fields);

  auto* connect_button = new QPushButton(QObject::tr("Connect"), section);
  connect_button->setProperty("primary", true);
  connect_button->setMinimumWidth(120);
  connect_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect_button->setAccessibleName(QObject::tr("Connect laser"));
  connect_button->setProperty("connected", false);
  layout->addWidget(connect_button);

  auto* output_row = new QHBoxLayout();
  auto* output_label = new QLabel(QObject::tr("Laser output"), section);
  auto* status = new OutputSwitch(section);
  status->setObjectName(QStringLiteral("laserOutputSwitch"));
  status->setAccessibleName(QObject::tr("Laser output status"));
  status->setEnabled(false);
  status->setToolTip(QObject::tr("Connect the laser to control its output."));
  output_label->setBuddy(status);
  output_row->addWidget(output_label);
  output_row->addStretch();
  output_row->addWidget(status);
  layout->addLayout(output_row);

  QObject::connect(
      connect_button, &QPushButton::clicked, section,
      [&view, connect_button, device, port, line] {
        if (connect_button->property("connected").toBool()) {
          emit view.DisconnectLaserRequested();
        } else if (device->text().trimmed().isEmpty() ||
                   line->currentIndex() < 0) {
          view.ShowError(
              {{},
               "Connect laser",
               "Enter the NI device name and select the wired port and line."});
        } else {
          const QString channel =
              QStringLiteral("%1/port%2/line%3")
                  .arg(device->text().trimmed(), port->currentText(),
                       line->currentText());
          emit view.ConnectLaserRequested({channel.toStdString()});
        }
      });
  QObject::connect(status, &QCheckBox::clicked, section, [&view, status] {
    emit view.SetLaserOutputRequested(
        !view.IsRecovering() && status->property("outputKnown").toBool() &&
        !status->property("outputEnabled").toBool());
  });
  struct Availability {
    ui::LaserState state{};
    bool pending = false;
    bool recovering = false;
  };
  const auto availability = std::make_shared<Availability>();
  const auto refresh = [=] {
    const auto& state = availability->state;
    const bool connected = state.connection == ui::ConnectionState::kConnected;
    connect_button->setProperty("connected", connected);
    connect_button->setText(connected ? QObject::tr("Disconnect")
                                      : QObject::tr("Connect"));
    connect_button->setEnabled(!availability->pending &&
                               state.connection !=
                                   ui::ConnectionState::kConnecting);
    channel_fields->setEnabled(!availability->pending && !connected &&
                               state.connection !=
                                   ui::ConnectionState::kConnecting);
    status->setEnabled(connected && !availability->pending &&
                       (!availability->recovering || !state.output_enabled ||
                        *state.output_enabled));
    status->setProperty("outputEnabled", state.output_enabled.value_or(false));
    status->setProperty("outputKnown", state.output_enabled.has_value());
    if (!availability->pending)
      status->setChecked(connected && state.output_enabled.value_or(false));
    status->setToolTip(
        !connected ? QObject::tr("Connect the laser to control its output.")
        : !state.output_enabled
            ? QObject::tr(
                  "Output state unavailable. Click to request output off.")
        : *state.output_enabled
            ? QObject::tr("Output on. Click to turn off. Last confirmed "
                          "command, not measured emission.")
            : QObject::tr("Output off. Click to turn on. Last confirmed "
                          "command, not measured emission."));
    connection_status->setText(ConnectionText(state.connection));
    indicator->setStyleSheet(connected
                                 ? "background: #00a34a; border-radius: 6px;"
                                 : "background: #ed1735; border-radius: 6px;");
  };
  QObject::connect(&view, &MainWindow::LaserStateUpdated, section,
                   [=](ui::LaserState state) {
                     availability->state = state;
                     refresh();
                   });
  QObject::connect(&view, &MainWindow::LaserRequestPending, section,
                   [=](bool pending) {
                     availability->pending = pending;
                     refresh();
                   });
  manual->setEnabled(false);
  QObject::connect(&view, &MainWindow::RecoveryControlsEnabled, manual,
                   &QWidget::setEnabled);
  QObject::connect(&view, &MainWindow::RecoveryControlsEnabled, section,
                   [=, &view](bool) {
                     availability->recovering = view.IsRecovering();
                     refresh();
                   });

  auto* pixel_size_label =
      new QLabel(QObject::tr("Pixel size (µm/pixel)"), section);
  auto* pixel_size = new QLineEdit(
      QString::number(view.PixelSizeMicrometres(), 'g', 12), section);
  pixel_size->setAccessibleName(
      QObject::tr("Pixel size in micrometres per pixel"));
  pixel_size->setAlignment(Qt::AlignRight);
  pixel_size->setToolTip(QObject::tr(
      "Physical travel per image pixel. Applied to the next scan."));
  pixel_size_label->setBuddy(pixel_size);
  QObject::connect(pixel_size, &QLineEdit::textChanged, section,
                   [&view](const QString& text) {
                     bool ok = false;
                     const double value = text.toDouble(&ok);
                     view.SetPixelSizeMicrometres(
                         ok ? value : std::numeric_limits<double>::quiet_NaN());
                   });
  QObject::connect(&view, &MainWindow::ScanInputsEnabled, pixel_size,
                   &QWidget::setEnabled);
  outer_layout->addWidget(pixel_size_label);
  outer_layout->addWidget(pixel_size);
  outer_layout->addStretch();

  return section;
}

}  // namespace ui
