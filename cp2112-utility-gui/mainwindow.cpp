// Implements the Qt frontend while keeping all hardware work in ApplicationService.
#include "mainwindow.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <type_traits>
#include <set>

namespace {
constexpr int HexBase = 16;

QSpinBox* numberBox(int minimum, int maximum, int value, QWidget* parent) {
    auto* box = new QSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setValue(value);
    box->setMaximumWidth(130);
    return box;
}

QTableWidget* makeTable(const QStringList& headers, QWidget* parent) {
    auto* result = new QTableWidget(parent);
    result->setColumnCount(headers.size());
    result->setHorizontalHeaderLabels(headers);
    result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->setSelectionBehavior(QAbstractItemView::SelectRows);
    result->horizontalHeader()->setStretchLastSection(true);
    result->verticalHeader()->setVisible(false);
    return result;
}

void setTableRow(QTableWidget* target, int row, const QStringList& values) {
    target->insertRow(row);
    for (int column = 0; column < values.size(); ++column)
        target->setItem(row, column, new QTableWidgetItem(values[column]));
}

bool parseHexField(const QLineEdit* field, unsigned maximum, unsigned& value) {
    bool ok = false;
    const QString text = field->text().trimmed();
    QString normalized = text;
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) normalized = normalized.mid(2);
    const uint parsed = normalized.toUInt(&ok, 16);
    if (!ok || parsed > maximum) return false;
    value = parsed;
    return true;
}

double numericValue(const DeviceValue& value, bool& ok) {
    ok = false;
    return std::visit([&ok](const auto& item) -> double {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_arithmetic_v<T>) { ok = true; return static_cast<double>(item); }
        return 0.0;
    }, value);
}

class ChartWindow final : public QWidget {
public:
    explicit ChartWindow(const std::vector<MainWindow::ResultPoint>* points,
                         const size_t* selectedDevice,
                         QWidget* parent = nullptr)
        : QWidget(parent), points_(points), selectedDevice_(selectedDevice) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                       Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint |
                       Qt::WindowCloseButtonHint);
        setWindowTitle("Structured result chart");
        resize(760, 460);
        auto* root = new QVBoxLayout(this);
        auto* controls = new QHBoxLayout();
        reading_ = new QComboBox(this);
        reading2_ = new QComboBox(this);
        value_ = new QComboBox(this);
        interval_ = numberBox(1, 86400, 5, this);
        value_->addItems({"Processed value", "Raw value"});
        rebuildReadingOptions();
        controls->addWidget(new QLabel("Reading", this)); controls->addWidget(reading_);
        controls->addWidget(new QLabel("Second reading", this)); controls->addWidget(reading2_);
        controls->addWidget(new QLabel("Value", this)); controls->addWidget(value_);
        controls->addWidget(new QLabel("X interval (s)", this)); controls->addWidget(interval_);
        controls->addStretch(); root->addLayout(controls);
        canvas_ = new QWidget(this); canvas_->setMinimumHeight(300); root->addWidget(canvas_, 1);
        auto* close = new QPushButton("Close", this);
        auto* closeRow = new QHBoxLayout(); closeRow->addStretch(); closeRow->addWidget(close); root->addLayout(closeRow);
        connect(close, &QPushButton::clicked, this, &QWidget::close);
        connect(reading_, &QComboBox::currentTextChanged, this, [this](const QString& value) {
            Q_UNUSED(value);
            canvas_->update();
        });
        connect(reading2_, &QComboBox::currentTextChanged, this, [this](const QString& value) {
            Q_UNUSED(value);
            canvas_->update();
        });
        connect(value_, &QComboBox::currentTextChanged, this, [this](const QString&) {
            canvas_->update();
        });
        connect(interval_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            Q_UNUSED(value);
            canvas_->update();
        });
        refreshTimer_ = new QTimer(this);
        refreshTimer_->setInterval(250);
        connect(refreshTimer_, &QTimer::timeout, this, [this] {
            const size_t currentDevice = currentDeviceIndex();
            bool hasCurrentData = false;
            if (points_) {
                for (const auto& point : *points_) {
                    if (point.deviceIndex == currentDevice) {
                        hasCurrentData = true;
                        break;
                    }
                }
            }
            const bool showingNoData = reading_->currentText() == "(No data)";
            if (currentDevice != lastDeviceIndex_ || hasCurrentData == showingNoData) {
                lastDeviceIndex_ = currentDevice;
                rebuildReadingOptions();
            }
            canvas_->update();
        });
        refreshTimer_->start();
        canvas_->setAttribute(Qt::WA_OpaquePaintEvent, false);
        canvas_->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == canvas_ && event->type() == QEvent::Paint) {
            QPainter painter(canvas_); painter.setRenderHint(QPainter::Antialiasing);
            painter.fillRect(canvas_->rect(), palette().base());
            const QRect area = canvas_->rect().adjusted(85, 45, -95, -95);
            painter.setPen(palette().text().color()); painter.drawRect(area);
            const QString selected = reading_->currentText();
            const QString selected2 = reading2_->currentText() == "(None)" ? QString() : reading2_->currentText();
            const bool raw = value_->currentIndex() == 1;
            struct Sample { double seconds; double value; };
            QDateTime firstTime;
            if (!points_) { painter.drawText(area, Qt::AlignCenter, "No data source available"); return true; }
            const size_t deviceIndex = currentDeviceIndex();
            for (const auto& point : *points_) {
                if (point.deviceIndex != deviceIndex ||
                    (point.reading != selected && point.reading != selected2)) continue;
                const QDateTime timestamp = QDateTime::fromString(point.timestamp, Qt::ISODateWithMs);
                if (timestamp.isValid() && (!firstTime.isValid() || timestamp < firstTime)) firstTime = timestamp;
            }
            auto collect = [&](const QString& name, std::vector<Sample>& samples, double& minimum, double& maximum) {
                bool first = true;
                for (const auto& point : *points_)
                    if (point.deviceIndex == deviceIndex && point.reading == name && (!raw || point.hasRaw)) {
                    const double y = raw ? point.raw : point.value;
                    const QDateTime timestamp = QDateTime::fromString(point.timestamp, Qt::ISODateWithMs);
                    if (!std::isfinite(y) || !timestamp.isValid() || !firstTime.isValid()) continue;
                    if (first) minimum = maximum = y; else { minimum = std::min(minimum, y); maximum = std::max(maximum, y); }
                    samples.push_back({firstTime.msecsTo(timestamp) / 1000.0, y}); first = false;
                }
                std::sort(samples.begin(), samples.end(), [](const Sample& left, const Sample& right) { return left.seconds < right.seconds; });
            };
            std::vector<Sample> samples1, samples2; double minimum1 = 0, maximum1 = 0, minimum2 = 0, maximum2 = 0;
            collect(selected, samples1, minimum1, maximum1);
            if (!selected2.isEmpty()) collect(selected2, samples2, minimum2, maximum2);
            painter.drawText(10, 20, QString("Blue: %1 | Green: %2").arg(selected, selected2.isEmpty() ? "(none)" : selected2));
            if (samples1.empty() && samples2.empty()) { painter.drawText(area, Qt::AlignCenter, "No timestamped numeric data available"); return true; }
            auto padded = [](double& minimum, double& maximum) {
                const double range = std::max(1.0, maximum - minimum); minimum -= range * 0.05; maximum += range * 0.05;
            };
            if (!samples1.empty()) padded(minimum1, maximum1);
            if (!samples2.empty()) padded(minimum2, maximum2);
            const double interval = interval_->value();
            // Keep the interval control continuous. Rounding the range to whole
            // interval multiples caused the line to jump in size at thresholds.
            const double xMaximum = 9.0 * interval;
            const double latest = std::max(samples1.empty() ? 0.0 : samples1.back().seconds,
                                           samples2.empty() ? 0.0 : samples2.back().seconds);
            const double windowStart = std::max(0.0, latest - xMaximum);
            auto makePlot = [&](const std::vector<Sample>& samples, double minimum, double maximum) {
                std::vector<QPointF> plot;
                for (const Sample& sample : samples) {
                    if (sample.seconds < windowStart) continue;
                    const double x = std::clamp((sample.seconds - windowStart) / xMaximum, 0.0, 1.0);
                    const double normalized = std::clamp((sample.value - minimum) / (maximum - minimum), 0.0, 1.0);
                    plot.emplace_back(area.left() + x * area.width(), area.bottom() - normalized * area.height());
                }
                return plot;
            };
            const std::vector<QPointF> plot1 = makePlot(samples1, minimum1, maximum1);
            const std::vector<QPointF> plot2 = makePlot(samples2, minimum2, maximum2);
            painter.setPen(QPen(palette().mid().color(), 1, Qt::DashLine));
            for (int tick = 0; tick < 10; ++tick) {
                const double x = area.left() + static_cast<double>(tick) * area.width() / 9.0;
                painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            }
            painter.setPen(palette().text().color());
            for (int tick = 0; tick < 10; ++tick) {
                const double y = area.bottom() - static_cast<double>(tick) * area.height() / 9.0;
                painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            }
            painter.save();
            painter.setClipRect(area.adjusted(1, 1, -1, -1));
            const QColor cobaltBlue(0, 71, 171); const QColor hunterGreen(53, 94, 59);
            painter.setPen(QPen(cobaltBlue, 2));
            for (size_t i = 1; i < plot1.size(); ++i) painter.drawLine(plot1[i - 1], plot1[i]);
            painter.setPen(QPen(hunterGreen, 2));
            for (size_t i = 1; i < plot2.size(); ++i) painter.drawLine(plot2[i - 1], plot2[i]);
            painter.restore();
            painter.setPen(palette().text().color());
            for (int tick = 0; tick < 10; ++tick) {
                const double value = maximum1 - static_cast<double>(tick) * (maximum1 - minimum1) / 9.0;
                const int y = area.top() + static_cast<int>(static_cast<double>(tick) * area.height() / 9.0);
                painter.setPen(cobaltBlue); painter.drawText(5, y + 8, QString::number(value, 'g', 5));
                if (!samples2.empty()) {
                    const double rightValue = maximum2 - static_cast<double>(tick) * (maximum2 - minimum2) / 9.0;
                    painter.setPen(hunterGreen); painter.drawText(area.right() + 6, y + 8, QString::number(rightValue, 'g', 5));
                }
            }
            for (int tick = 0; tick < 10; ++tick) {
                const double seconds = static_cast<double>(tick) * xMaximum / 9.0;
                const QDateTime labelTime = firstTime.addMSecs(static_cast<qint64>((windowStart + seconds) * 1000.0));
                const QString label = labelTime.toString("HH:mm:ss");
                const int x = area.left() + static_cast<int>(static_cast<double>(tick) * area.width() / 9.0);
                painter.drawText(x - 28, area.bottom() + 22, label);
            }
            painter.drawText(area.center().x() - 18, area.bottom() + 55, "Time");
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }
private:
    size_t currentDeviceIndex() const {
        return selectedDevice_ ? *selectedDevice_ : static_cast<size_t>(-1);
    }

    void rebuildReadingOptions() {
        const QString previousPrimary = reading_->currentText();
        const QString previousSecondary = reading2_->currentText();
        std::set<QString> names;
        const size_t deviceIndex = currentDeviceIndex();
        if (points_) for (const auto& point : *points_)
            if (point.deviceIndex == deviceIndex) names.insert(point.reading);

        const QSignalBlocker blockPrimary(reading_);
        const QSignalBlocker blockSecondary(reading2_);
        reading_->clear();
        reading2_->clear();
        if (names.empty()) reading_->addItem("(No data)");
        for (const QString& name : names) reading_->addItem(name);
        reading2_->addItem("(None)");
        for (const QString& name : names) reading2_->addItem(name);

        const int primaryIndex = reading_->findText(previousPrimary);
        if (primaryIndex >= 0) reading_->setCurrentIndex(primaryIndex);
        else if (!names.empty()) reading_->setCurrentIndex(0);
        const int secondaryIndex = reading2_->findText(previousSecondary);
        reading2_->setCurrentIndex(secondaryIndex >= 0 ? secondaryIndex : 0);
    }

    const std::vector<MainWindow::ResultPoint>* points_ = nullptr;
    const size_t* selectedDevice_ = nullptr;
    size_t lastDeviceIndex_ = static_cast<size_t>(-1);
    QComboBox* reading_ = nullptr;
    QComboBox* reading2_ = nullptr;
    QComboBox* value_ = nullptr;
    QSpinBox* interval_ = nullptr;
    QWidget* canvas_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
};
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    loadConfiguration();
}

MainWindow::~MainWindow() {
    scanCancel_.store(true);
    streamCancel_.store(true);
    stopWorker();
}

void MainWindow::buildUi() {
    setWindowTitle("CP2112 Utility by ranuldeepanayake and AI");
    resize(1180, 760);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    auto* header = new QHBoxLayout();
    header->addWidget(new QLabel("CP2112 Utility by ranuldeepanayake and AI", central));
    header->addStretch();
    versionLabel_ = new QLabel("Build: loading...", central);
    header->addWidget(versionLabel_);
    adapterLabel_ = new QLabel("Adapters connected: unknown", central);
    root->addLayout(header);

    tabs_ = new QTabWidget(central);
    auto* tabs = tabs_;
    root->addWidget(tabs, 1);

    auto* overview = new QWidget(tabs);
    auto* overviewLayout = new QVBoxLayout(overview);
    auto* deviceActions = new QHBoxLayout();
    readButton_ = new QPushButton("Read selected device", overview);
    streamButton_ = new QPushButton("Start live polling", overview);
    stopStreamButton_ = new QPushButton("Stop polling", overview);
    driverInfoButton_ = new QPushButton("Driver info", overview);
    actionButton_ = new QPushButton("Device action", overview);
    deviceSettingsButton_ = new QPushButton("Device settings", overview);
    stopStreamButton_->setEnabled(false);
    deviceActions->addWidget(readButton_);
    deviceActions->addWidget(streamButton_);
    deviceActions->addWidget(stopStreamButton_);
    deviceActions->addWidget(driverInfoButton_);
    deviceActions->addWidget(actionButton_);
    deviceActions->addWidget(deviceSettingsButton_);
    overviewLayout->addLayout(deviceActions);
    auto* devicePane = new QWidget(overview);
    auto* devicePaneLayout = new QVBoxLayout(devicePane);
    deviceTable_ = makeTable({"Device", "Type", "Address", "Adapter", "Enabled", "Refresh"}, devicePane);
    deviceTable_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    devicePaneLayout->addWidget(new QLabel("Configured devices (click a row to select)", devicePane));
    devicePaneLayout->addWidget(deviceTable_, 1);

    auto* resultPane = new QWidget(overview);
    auto* resultPaneLayout = new QVBoxLayout(resultPane);
    resultTable_ = makeTable({"Timestamp", "Reading", "Value", "Unit", "Raw value"}, resultPane);
    resultTable_->horizontalHeader()->setStretchLastSection(false);
    for (int column = 0; column < 5; ++column)
        resultTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
    auto* resultHeader = new QHBoxLayout();
    resultHeader->addWidget(new QLabel("Structured results", resultPane));
    followLatest_ = new QCheckBox("Follow latest", resultPane);
    followLatest_->setChecked(true);
    resultHeader->addStretch();
    resultHeader->addWidget(followLatest_);
    clearResultsButton_ = new QPushButton("Clear results", resultPane);
    chartButton_ = new QPushButton("Open chart", resultPane);
    resultHeader->addWidget(clearResultsButton_);
    resultHeader->addWidget(chartButton_);
    resultPaneLayout->addLayout(resultHeader);
    resultPaneLayout->addWidget(resultTable_, 1);
    auto* deviceResultSplitter = new QSplitter(Qt::Vertical, overview);
    deviceResultSplitter->addWidget(devicePane);
    deviceResultSplitter->addWidget(resultPane);
    deviceResultSplitter->setStretchFactor(0, 1);
    deviceResultSplitter->setStretchFactor(1, 1);
    deviceResultSplitter->setSizes({260, 360});
    overviewLayout->addWidget(deviceResultSplitter, 1);
    tabs->addTab(overview, "Devices");

    auto* adapters = new QWidget(tabs);
    auto* adapterLayout = new QVBoxLayout(adapters);
    auto* refreshAdapterButton = new QPushButton("Query adapter information", adapters);
    refreshAdapterButton->setMaximumWidth(220);
    auto* gpioBox = new QGroupBox("GPIO indicators", adapters);
    auto* gpioLayout = new QHBoxLayout(gpioBox);
    gpioActivity_ = new QCheckBox("TX/RX activity LEDs", gpioBox);
    gpioErrorPattern_ = new QCheckBox("I2C error pattern", gpioBox);
    gpioActivity_->setChecked(app_.config().gpioActivityEnabled);
    gpioErrorPattern_->setChecked(app_.config().gpioErrorPatternEnabled);
    auto* testGpioButton = new QPushButton("Test LEDs", gpioBox);
    testGpioButton->setMaximumWidth(110);
    gpioLayout->addWidget(gpioActivity_); gpioLayout->addWidget(gpioErrorPattern_);
    gpioLayout->addWidget(testGpioButton); gpioLayout->addStretch();
    adapterTable_ = makeTable({"Property", "Value"}, adapters);
    adapterLayout->addWidget(refreshAdapterButton);
    adapterLayout->addWidget(gpioBox);
    adapterLayout->addWidget(adapterTable_, 1);
    tabs->addTab(adapters, "Adapter information");

    auto* scan = new QWidget(tabs);
    auto* scanLayout = new QVBoxLayout(scan);
    auto* scanForm = new QFormLayout();
    const AppConfig defaults{};
    scanAdapter_ = numberBox(0, 255, defaults.scanAdapterIndex, scan);
    scanStart_ = numberBox(0x08, 0x77, defaults.scanStartAddress, scan);
    scanEnd_ = numberBox(0x08, 0x77, defaults.scanEndAddress, scan);
    scanStart_->setDisplayIntegerBase(HexBase); scanStart_->setPrefix("0x");
    scanEnd_->setDisplayIntegerBase(HexBase); scanEnd_->setPrefix("0x");
    scanClock_ = numberBox(1, 400000, defaults.scanClockHz, scan);
    scanTimeout_ = numberBox(1, 1000, defaults.transferTimeoutMs, scan);
    scanRetries_ = numberBox(0, 1000, defaults.transferRetries, scan);
    showUnresponsive_ = new QCheckBox("Show addresses with no response", scan);
    showUnresponsive_->setChecked(false);
    scanCountLabel_ = new QLabel("Scanned: 0 | Responded: 0", scan);
    scanForm->addRow("Adapter index", scanAdapter_);
    scanForm->addRow("Scan start (hex)", scanStart_);
    scanForm->addRow("Scan end (hex)", scanEnd_);
    scanForm->addRow("Clock (Hz)", scanClock_);
    scanForm->addRow("Timeout (ms)", scanTimeout_);
    scanForm->addRow("Retries", scanRetries_);
    scanForm->addRow(showUnresponsive_);
    scanLayout->addLayout(scanForm);
    auto* scanActions = new QHBoxLayout();
    scanButton_ = new QPushButton("Scan addresses", scan);
    cancelScanButton_ = new QPushButton("Cancel scan", scan);
    cancelScanButton_->setEnabled(false);
    scanProgress_ = new QProgressBar(scan);
    scanProgress_->setRange(0, 100);
    scanProgress_->setMaximumHeight(10);
    scanProgress_->setTextVisible(false);
    scanActions->addWidget(scanButton_);
    scanActions->addWidget(cancelScanButton_);
    scanActions->addWidget(scanCountLabel_);
    scanLayout->addLayout(scanActions);
    scanTable_ = makeTable({"Address (hex)", "Address (decimal)", "Probe result", "Returned byte", "Retries used", "Timed out", "API status", "Transfer state", "Transfer detail"}, scan);
    scanTable_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scanTable_->horizontalHeaderItem(6)->setToolTip("Raw CP2112 API return code; 0 means the API call succeeded.");
    scanTable_->horizontalHeaderItem(7)->setToolTip("CP2112 transfer state: 1 busy, 2 complete, 3 error.");
    scanTable_->horizontalHeaderItem(8)->setToolTip("CP2112 detailed transfer status; the meaning depends on the transfer state.");
    scanLayout->addWidget(scanTable_, 1);
    tabs->addTab(scan, "Address Scan");

    auto* registers = new QWidget(tabs);
    auto* registerPageLayout = new QVBoxLayout(registers);
    auto* registerAccess = new QWidget(registers);
    auto* registerAccessLayout = new QVBoxLayout(registerAccess);
    auto* registerFormWidget = new QWidget(registerAccess);
    auto* registerLayout = new QFormLayout(registerFormWidget);
    registerDeviceHex_ = new QLineEdit("0x76", registers);
    registerAddressHex_ = new QLineEdit("0x00", registers);
    registerValueHex_ = new QLineEdit("0x00", registers);
    registerReadLength_ = numberBox(1, 512, 1, registers);
    registerDeviceHex_->setMaximumWidth(160); registerAddressHex_->setMaximumWidth(160); registerValueHex_->setMaximumWidth(160);
    auto* readRegisterButton = new QPushButton("Read register", registers);
    auto* writeRegisterButton = new QPushButton("Write register", registers);
    readRegisterButton->setMaximumWidth(160); writeRegisterButton->setMaximumWidth(160);
    registerResult_ = new QLabel("No register operation performed.", registers);
    registerOutput_ = new QLineEdit(registers);
    registerOutput_->setReadOnly(true);
    registerOutput_->setPlaceholderText("Read value or write status");
    auto* registerButtons = new QHBoxLayout();
    registerButtons->addWidget(readRegisterButton);
    registerButtons->addWidget(writeRegisterButton);
    registerLayout->addRow("Device address (hex)", registerDeviceHex_);
    registerLayout->addRow("Register address (hex)", registerAddressHex_);
    registerLayout->addRow("Expected read bytes", registerReadLength_);
    registerLayout->addRow("Write value (hex)", registerValueHex_);
    registerLayout->addRow(registerButtons);
    registerLayout->addRow("Status", registerResult_);
    registerLayout->addRow("Output", registerOutput_);
    registerAccessLayout->addWidget(registerFormWidget);
    showDeviceRegisters_ = new QCheckBox("Show registers for the selected device in the Devices page", registerAccess);
    registerTable_ = makeTable({"Address", "Name", "Access", "Description"}, registerAccess);
    registerTable_->setVisible(false);
    registerAccessLayout->addWidget(showDeviceRegisters_);
    registerAccessLayout->addWidget(registerTable_, 1);
    auto* registerLogHeader = new QHBoxLayout();
    registerLogHeader->addWidget(new QLabel("Register operation log", registers));
    followRegisterLog_ = new QCheckBox("Follow latest", registers); followRegisterLog_->setChecked(true);
    registerLogHeader->addStretch(); registerLogHeader->addWidget(followRegisterLog_);
    registerLog_ = makeTable({"Timestamp", "Status"}, registers);
    auto* registerLogPane = new QWidget(registers);
    auto* registerLogLayout = new QVBoxLayout(registerLogPane);
    registerLogLayout->addLayout(registerLogHeader); registerLogLayout->addWidget(registerLog_, 1);
    auto* registerSplitter = new QSplitter(Qt::Vertical, registers);
    registerSplitter->addWidget(registerAccess); registerSplitter->addWidget(registerLogPane);
    registerSplitter->setStretchFactor(0, 1); registerSplitter->setStretchFactor(1, 1);
    registerPageLayout->addWidget(registerSplitter, 1);
    tabs->addTab(registers, "Register Access");

    auto* settings = new QWidget(tabs);
    auto* settingsLayout = new QVBoxLayout(settings);
    auto* generalBox = new QGroupBox("General settings", settings);
    auto* generalForm = new QFormLayout(generalBox);
    generalBridgeDll_ = new QLineEdit(generalBox);
    generalLogDir_ = new QLineEdit(generalBox);
    generalBridgeDll_->setMaximumWidth(280); generalLogDir_->setMaximumWidth(280);
    generalAdapter_ = numberBox(0, 255, defaults.scanAdapterIndex, generalBox);
    generalTimeout_ = numberBox(1, 1000, defaults.transferTimeoutMs, generalBox);
    generalRetries_ = numberBox(0, 1000, defaults.transferRetries, generalBox);
    generalClock_ = numberBox(1, 400000, defaults.scanClockHz, generalBox);
    generalStart_ = numberBox(0x08, 0x77, defaults.scanStartAddress, generalBox);
    generalEnd_ = numberBox(0x08, 0x77, defaults.scanEndAddress, generalBox);
    generalStart_->setDisplayIntegerBase(HexBase); generalStart_->setPrefix("0x");
    generalEnd_->setDisplayIntegerBase(HexBase); generalEnd_->setPrefix("0x");
    generalRefresh_ = numberBox(1, 3600, defaults.defaultRefreshSeconds, generalBox);
    generalRetention_ = numberBox(0, 36500, defaults.logRetentionDays, generalBox);
    generalLogStream_ = new QCheckBox(generalBox);
    generalTheme_ = new QComboBox(generalBox);
    generalTheme_->addItem("Default grey", "default");
    generalTheme_->addItem("Light", "light");
    generalTheme_->addItem("Dark", "dark");
    generalTheme_->setMaximumWidth(150);
    generalLogStream_->setChecked(defaults.defaultLogStream);
    auto* bridgeRow = new QHBoxLayout();
    auto* bridgeBrowse = new QPushButton("Browse...", generalBox); bridgeBrowse->setMaximumWidth(100);
    bridgeRow->addWidget(generalBridgeDll_); bridgeRow->addWidget(bridgeBrowse);
    auto* logRow = new QHBoxLayout();
    auto* logBrowse = new QPushButton("Browse...", generalBox); logBrowse->setMaximumWidth(100);
    logRow->addWidget(generalLogDir_); logRow->addWidget(logBrowse);
    generalForm->addRow("Bridge DLL", bridgeRow);
    generalForm->addRow("Log directory", logRow);
    generalForm->addRow("Scan adapter index", generalAdapter_);
    generalForm->addRow("Transfer timeout (ms)", generalTimeout_);
    generalForm->addRow("Transfer retries", generalRetries_);
    generalForm->addRow("Scan clock (Hz)", generalClock_);
    generalForm->addRow("Scan start (hex)", generalStart_);
    generalForm->addRow("Scan end (hex)", generalEnd_);
    generalForm->addRow("Default refresh (s)", generalRefresh_);
    generalForm->addRow("Default log stream", generalLogStream_);
    generalForm->addRow("Theme", generalTheme_);
    generalForm->addRow("Log retention (days)", generalRetention_);
    auto* saveGeneral = new QPushButton("Save general settings", generalBox);
    saveGeneral->setMaximumWidth(220);
    generalForm->addRow(saveGeneral);
    auto* reloadConfigurationButton = new QPushButton("Reload configuration", generalBox);
    reloadConfigurationButton->setMaximumWidth(220);
    generalForm->addRow(reloadConfigurationButton);
    settingsLayout->addWidget(generalBox);
    settingsLayout->addWidget(new QLabel("Device-specific settings are available from the selected device row."));
    settingsLayout->addStretch();
    tabs->addTab(settings, "Settings");

    auto* logs = new QWidget(tabs);
    auto* logsLayout = new QVBoxLayout(logs);
    auto* cleanLogsButton = new QPushButton("Clean logs using configured retention", logs);
    cleanLogsButton->setMaximumWidth(260);
    auto* clearAllLogsButton = new QPushButton("Clear all logs", logs);
    clearAllLogsButton->setMaximumWidth(180);
    eventLog_ = makeTable({"Timestamp", "Event"}, logs);
    followEvents_ = new QCheckBox("Follow latest", logs); followEvents_->setChecked(true);
    auto* eventHeader = new QHBoxLayout(); eventHeader->addWidget(followEvents_); eventHeader->addStretch();
    logsLayout->addLayout(eventHeader);
    auto* logActions = new QHBoxLayout();
    logActions->addWidget(cleanLogsButton); logActions->addWidget(clearAllLogsButton); logActions->addStretch();
    logsLayout->addLayout(logActions);
    logsLayout->addWidget(eventLog_, 1);
    tabs->addTab(logs, "Events");

    setCentralWidget(central);
    connect(deviceTable_, &QTableWidget::cellClicked, this, &MainWindow::selectDeviceRow);
    connect(scanTable_, &QTableWidget::cellClicked, this, &MainWindow::selectScannedDeviceRow);
    connect(showDeviceRegisters_, &QCheckBox::toggled, registerTable_, &QTableWidget::setVisible);
    connect(showDeviceRegisters_, &QCheckBox::toggled, this, [this] { refreshSelectedDeviceRegisters(); });
    connect(registerTable_, &QTableWidget::cellClicked, this, &MainWindow::selectRegisterRow);
    connect(readButton_, &QPushButton::clicked, this, &MainWindow::readSelectedDevice);
    connect(streamButton_, &QPushButton::clicked, this, &MainWindow::startStreaming);
    connect(stopStreamButton_, &QPushButton::clicked, this, &MainWindow::stopStreaming);
    connect(driverInfoButton_, &QPushButton::clicked, this, &MainWindow::showDriverInformation);
    connect(actionButton_, &QPushButton::clicked, this, &MainWindow::executeDeviceAction);
    connect(deviceSettingsButton_, &QPushButton::clicked, this, &MainWindow::editSelectedDeviceSettings);
    connect(scanButton_, &QPushButton::clicked, this, &MainWindow::startScan);
    connect(cancelScanButton_, &QPushButton::clicked, this, &MainWindow::cancelScan);
    connect(readRegisterButton, &QPushButton::clicked, this, &MainWindow::readRegister);
    connect(writeRegisterButton, &QPushButton::clicked, this, &MainWindow::writeRegister);
    connect(saveGeneral, &QPushButton::clicked, this, &MainWindow::saveGeneralSettings);
    connect(refreshAdapterButton, &QPushButton::clicked, this, &MainWindow::refreshAdapterInformation);
    connect(testGpioButton, &QPushButton::clicked, this, &MainWindow::testGpioIndicators);
    connect(gpioActivity_, &QCheckBox::toggled, this, &MainWindow::toggleGpioActivity);
    connect(gpioErrorPattern_, &QCheckBox::toggled, this, &MainWindow::toggleGpioErrorPattern);
    connect(reloadConfigurationButton, &QPushButton::clicked, this, &MainWindow::reloadConfiguration);
    connect(cleanLogsButton, &QPushButton::clicked, this, &MainWindow::cleanLogs);
    connect(clearAllLogsButton, &QPushButton::clicked, this, &MainWindow::clearAllLogs);
    connect(bridgeBrowse, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getOpenFileName(this, "Select CP2112 bridge DLL", generalBridgeDll_->text(), "DLL files (*.dll);;All files (*.*)");
        if (!selected.isEmpty()) generalBridgeDll_->setText(selected);
    });
    connect(logBrowse, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getExistingDirectory(this, "Select log directory", generalLogDir_->text());
        if (!selected.isEmpty()) generalLogDir_->setText(selected);
    });
    connect(showUnresponsive_, &QCheckBox::toggled, this, &MainWindow::filterScanResults);
    connect(clearResultsButton_, &QPushButton::clicked, this, [this] {
        const size_t deviceToClear = selectedDevice_;
        resultHistory_.erase(std::remove_if(resultHistory_.begin(), resultHistory_.end(),
            [deviceToClear](const ResultPoint& point) { return point.deviceIndex == deviceToClear; }),
            resultHistory_.end());
        resultTable_->setRowCount(0);
        showMessage("Structured results and chart data cleared for the selected device.");
    });
    connect(chartButton_, &QPushButton::clicked, this, [this] {
        if (selectedDevice_ == static_cast<size_t>(-1)) {
            showMessage("Select a device before opening its chart.");
            return;
        }
        auto* chart = new ChartWindow(&resultHistory_, &selectedDevice_, this); chart->show();
    });

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(100);
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::updateRefreshProgress);
    auto* footer = new QHBoxLayout();
    refreshProgress_ = new QProgressBar(central);
    refreshProgress_->setRange(0, 100);
    refreshProgress_->setMaximumHeight(8);
    refreshProgress_->setMaximumWidth(450);
    refreshProgress_->setTextVisible(false);
    refreshProgressLabel_ = new QLabel("Refresh interval", central);
    scanProgressLabel_ = new QLabel("Scan progress", central);
    scanProgress_->setMaximumWidth(450);
    footer->addWidget(refreshProgressLabel_);
    footer->addWidget(refreshProgress_, 1);
    footer->addWidget(scanProgressLabel_);
    footer->addWidget(scanProgress_, 1);
    footer->addStretch(1);
    selectedDeviceLabel_ = new QLabel("Selected device: none", central);
    footer->addWidget(selectedDeviceLabel_, 0, Qt::AlignCenter);
    footer->addStretch(1);
    footer->addWidget(adapterLabel_, 0, Qt::AlignRight);
    scanProgressLabel_->setVisible(false);
    scanProgress_->setVisible(false);
    root->addLayout(footer);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        const bool devicesPage = index == 0;
        const bool scanPage = index == 2;
        refreshProgressLabel_->setVisible(devicesPage);
        refreshProgress_->setVisible(devicesPage);
        scanProgressLabel_->setVisible(scanPage);
        scanProgress_->setVisible(scanPage);
    });
}

QString MainWindow::configPath() const {
    const QStringList candidates = {
        QDir::current().filePath("config/devices.ini"),
        QCoreApplication::applicationDirPath() + "/config/devices.ini",
        QDir(QCoreApplication::applicationDirPath()).filePath("../config/devices.ini"),
        QDir(QCoreApplication::applicationDirPath()).filePath("../../config/devices.ini"),
        QDir(QCoreApplication::applicationDirPath()).filePath("../../../config/devices.ini"),
        QDir(QCoreApplication::applicationDirPath()).filePath("../../../../config/devices.ini")
    };
    for (const QString& candidate : candidates)
        if (QFileInfo::exists(candidate)) return QDir::cleanPath(candidate);
    return QDir::current().filePath("config/devices.ini");
}

void MainWindow::loadConfiguration() {
    std::string error;
    if (!app_.initialize(configPath().toStdString(), error, "gui")) {
        showError(QString::fromStdString(error));
        return;
    }
    const AppConfig config = app_.config();
    versionLabel_->setText(QString("Build v%1_%2_%3%4")
                           .arg(QString::fromStdString(config.version))
                           .arg(QString::fromStdString(config.buildTimestamp))
                           .arg(QString::fromStdString(config.buildNumber))
                           .arg(config.betaBuild ? " beta" : ""));
    generalBridgeDll_->setText(QString::fromStdString(config.bridgeDll));
    generalLogDir_->setText(QString::fromStdString(config.logDir));
    generalAdapter_->setValue(config.scanAdapterIndex);
    generalTimeout_->setValue(config.transferTimeoutMs);
    generalRetries_->setValue(config.transferRetries);
    generalClock_->setValue(config.scanClockHz);
    generalStart_->setValue(config.scanStartAddress);
    generalEnd_->setValue(config.scanEndAddress);
    generalRefresh_->setValue(config.defaultRefreshSeconds);
    generalRetention_->setValue(config.logRetentionDays);
    generalLogStream_->setChecked(config.defaultLogStream);
    { const QSignalBlocker block(gpioActivity_); gpioActivity_->setChecked(config.gpioActivityEnabled); }
    { const QSignalBlocker block(gpioErrorPattern_); gpioErrorPattern_->setChecked(config.gpioErrorPatternEnabled); }
    const QString configuredTheme = QString::fromStdString(config.theme.empty() ? (config.darkMode ? "dark" : "default") : config.theme);
    generalTheme_->setCurrentIndex(std::max(0, generalTheme_->findData(configuredTheme)));
    applyTheme(configuredTheme);
    scanAdapter_->setValue(config.scanAdapterIndex);
    scanStart_->setValue(config.scanStartAddress);
    scanEnd_->setValue(config.scanEndAddress);
    scanClock_->setValue(config.scanClockHz);
    scanTimeout_->setValue(config.transferTimeoutMs);
    scanRetries_->setValue(config.transferRetries);
    refreshAll();
}

void MainWindow::refreshAll() { refreshAdapterInformation(); refreshDevices(); }

void MainWindow::refreshAdapters() {
    std::vector<AdapterInformation> adapters;
    std::string error;
    if (!app_.adapters(adapters, error)) {
        adapterLabel_->setText("Adapter error");
        showError(QString::fromStdString(error));
        return;
    }
    adapterLabel_->setText(QString("Adapters connected: %1").arg(adapters.size()));
    if (!adapters.empty()) showMessage(QString("Adapter information queried for %1 adapter(s).").arg(adapters.size()));
}

void MainWindow::refreshDevices() {
    const AppConfig config = app_.config();
    deviceTable_->setRowCount(0);
    for (size_t i = 0; i < config.devices.size(); ++i) {
        const DeviceConfig& device = config.devices[i];
        setTableRow(deviceTable_, static_cast<int>(i), {
            QString::fromStdString(device.name), QString::fromStdString(device.type),
            QString("0x%1").arg(device.address, 2, HexBase, QLatin1Char('0')),
            QString::number(device.adapterIndex), device.enabled ? "Yes" : "No",
            QString::number(device.refreshSeconds) + " s"});
    }
    if (!config.devices.empty()) {
        if (selectedDevice_ < config.devices.size())
            deviceTable_->selectRow(static_cast<int>(selectedDevice_));
        refreshDeviceSettings();
    }
    updateSelectedDeviceLabel();
}

void MainWindow::updateSelectedDeviceLabel() {
    if (!selectedDeviceLabel_) return;
    const AppConfig config = app_.config();
    if (selectedDevice_ == static_cast<size_t>(-1) || selectedDevice_ >= config.devices.size()) {
        selectedDeviceLabel_->setText("Selected device: none");
        return;
    }
    const DeviceConfig& device = config.devices[selectedDevice_];
    selectedDeviceLabel_->setText(QString("Selected: %1 (0x%2)")
        .arg(QString::fromStdString(device.name))
        .arg(device.address, 2, HexBase, QLatin1Char('0')));
}

void MainWindow::selectDeviceRow(int row, int) {
    if (row < 0 || static_cast<size_t>(row) >= app_.config().devices.size()) return;
    if (selectedDevice_ == static_cast<size_t>(row)) {
        selectedDevice_ = static_cast<size_t>(-1);
        selectedScanAddress_ = -1;
        deviceTable_->clearSelection();
        refreshDeviceSettings();
        updateSelectedDeviceLabel();
        showMessage("Device selection cleared.");
        return;
    }
    selectedDevice_ = static_cast<size_t>(row);
    selectedScanAddress_ = -1;
    scanTable_->clearSelection();
    refreshDeviceSettings();
    updateSelectedDeviceLabel();
}

void MainWindow::selectScannedDeviceRow(int row, int) {
    if (row < 0 || !scanTable_->item(row, 0)) return;
    QString text = scanTable_->item(row, 0)->text().trimmed();
    if (text.startsWith("0x", Qt::CaseInsensitive)) text = text.mid(2);
    bool ok = false;
    const unsigned address = text.toUInt(&ok, HexBase);
    if (!ok || address > 0x77) return;
    const QString addressText = QString("0x%1").arg(address, 2, HexBase, QLatin1Char('0'));
    if (selectedScanAddress_ == static_cast<int>(address)) {
        selectedScanAddress_ = -1;
        registerDeviceHex_->clear();
        scanTable_->clearSelection();
        showMessage("Scanned address selection cleared.");
        return;
    }
    selectedDevice_ = static_cast<size_t>(-1);
    deviceTable_->clearSelection();
    registerTable_->setRowCount(0);
    updateSelectedDeviceLabel();
    selectedScanAddress_ = static_cast<int>(address);
    registerDeviceHex_->setText(addressText);
    showMessage(QString("Register Access device address set to %1.").arg(addressText));
}

void MainWindow::refreshAdapterInformation() {
    std::vector<AdapterInformation> adapters;
    std::string error;
    if (!app_.adapters(adapters, error)) {
        showError(QString::fromStdString(error));
        return;
    }
    adapterLabel_->setText(QString("Adapters connected: %1").arg(adapters.size()));
    adapterTable_->setRowCount(0);
    for (const AdapterInformation& adapter : adapters) {
        const QStringList lines = adapterText(adapter).split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const int separator = line.indexOf(':');
            setTableRow(adapterTable_, adapterTable_->rowCount(),
                        {separator < 0 ? line : line.left(separator).trimmed(),
                         separator < 0 ? QString() : line.mid(separator + 1).trimmed()});
        }
    }
    showMessage(QString("Adapter information refreshed: %1 adapter(s).").arg(adapters.size()));
}

// Runs the shared CP2112 GPIO indicator test without putting GPIO policy in Qt.
void MainWindow::testGpioIndicators() {
    if (streamRunning_ || scanRunning_) {
        showMessage("Stop live polling or the active scan before testing LEDs.");
        return;
    }
    std::string error;
    if (!app_.testGpioIndicators(error)) showError(QString::fromStdString(error));
    else showMessage("GPIO indicator test completed: GPIO0 and GPIO1 were tested and restored.");
}

// Persists and applies the TX/RX hardware-toggle indicator policy.
void MainWindow::toggleGpioActivity(bool enabled) {
    if (streamRunning_ || scanRunning_) {
        showMessage("Stop live polling or the active scan before changing GPIO activity indicators.");
        gpioActivity_->blockSignals(true); gpioActivity_->setChecked(!enabled); gpioActivity_->blockSignals(false);
        return;
    }
    std::string error;
    if (!app_.setGpioActivityIndicators(enabled, error)) {
        showError(QString::fromStdString(error));
        gpioActivity_->blockSignals(true); gpioActivity_->setChecked(!enabled); gpioActivity_->blockSignals(false);
    } else showMessage(QString("TX/RX activity indicators %1.").arg(enabled ? "enabled" : "disabled"));
}

// Persists and applies the software error-pattern indicator policy.
void MainWindow::toggleGpioErrorPattern(bool enabled) {
    if (streamRunning_ || scanRunning_) {
        showMessage("Stop live polling or the active scan before changing the I2C error pattern.");
        gpioErrorPattern_->blockSignals(true); gpioErrorPattern_->setChecked(!enabled); gpioErrorPattern_->blockSignals(false);
        return;
    }
    std::string error;
    if (!app_.setGpioErrorPattern(enabled, error)) {
        showError(QString::fromStdString(error));
        gpioErrorPattern_->blockSignals(true); gpioErrorPattern_->setChecked(!enabled); gpioErrorPattern_->blockSignals(false);
    } else showMessage(QString("I2C error pattern %1.").arg(enabled ? "enabled" : "disabled"));
}

void MainWindow::showDriverInformation() {
    if (selectedDevice_ == static_cast<size_t>(-1)) return;
    DriverInformation information;
    std::string error;
    if (!app_.driverInformation(selectedDevice_, information, error)) {
        showError(QString::fromStdString(error));
        return;
    }
    // Keep this dialog modeless so driver metadata remains available while
    // the user interacts with the main window, result tables, or chart windows.
    auto* dialog = new QDialog(this, Qt::Window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setWindowTitle("Driver information"); dialog->resize(950, 650);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(QString::fromStdString(information.title), dialog));
    layout->addWidget(new QLabel(QString::fromStdString(information.capabilities), dialog));
    auto* table = makeTable({"Category", "Address / Action", "Access", "Purpose / Parameters"}, dialog);
    for (const DriverRegister& reg : information.registers) {
        setTableRow(table, table->rowCount(), {"Register",
            QString("0x%1 - %2").arg(reg.address, 2, HexBase, QLatin1Char('0')).arg(QString::fromStdString(reg.name)),
            reg.readable && reg.writable ? "R/W" : reg.writable ? "W" : "R",
            QString::fromStdString(reg.purpose)});
    }
    for (const DriverAction& action : information.actions) {
        setTableRow(table, table->rowCount(), {"Action", QString::fromStdString(action.label), "Command",
            QString::fromStdString(action.parameters.empty() ? "No parameters" :
                ("Parameters: " + action.parameters.front()))});
    }
    layout->addWidget(table, 1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::executeDeviceAction() {
    if (selectedDevice_ == static_cast<size_t>(-1)) return;
    DriverInformation information;
    std::string error;
    if (!app_.driverInformation(selectedDevice_, information, error)) {
        showError(QString::fromStdString(error));
        return;
    }
    if (information.actions.empty()) {
        showMessage("The selected driver has no write/action operations.");
        return;
    }
    QStringList labels;
    for (const DriverAction& action : information.actions) labels << QString::fromStdString(action.label);
    bool accepted = false;
    const QString label = QInputDialog::getItem(this, "Device action", "Action:", labels, 0, false, &accepted);
    if (!accepted) return;
    const auto actionIt = std::find_if(information.actions.begin(), information.actions.end(),
        [&label](const DriverAction& action) { return QString::fromStdString(action.label) == label; });
    if (actionIt == information.actions.end()) return;
    std::vector<std::string> parameters;
    for (const std::string& parameter : actionIt->parameters) {
        const QString value = QInputDialog::getText(this, "Device action", QString::fromStdString(parameter), QLineEdit::Normal, {}, &accepted);
        if (!accepted) return;
        parameters.push_back(value.toStdString());
    }
    std::string result;
    if (!app_.executeDeviceAction(selectedDevice_, actionIt->id, parameters, result, error)) {
        showError(QString("%1: action %2 failed: %3")
            .arg(driverEventContext(selectedDevice_), QString::fromStdString(actionIt->id),
                 QString::fromStdString(error.empty() ? result : error)));
        return;
    }
    showMessage(QString("%1: %2").arg(driverEventContext(selectedDevice_),
                                      QString::fromStdString(result)));
}

void MainWindow::refreshDeviceSettings() {
    const AppConfig config = app_.config();
    if (selectedDevice_ >= config.devices.size()) {
        registerDeviceHex_->clear();
        registerTable_->setRowCount(0);
        return;
    }
    const DeviceConfig& device = config.devices[selectedDevice_];
    registerDeviceHex_->setText(QString("0x%1").arg(device.address, 2, HexBase, QLatin1Char('0')));
    refreshSelectedDeviceRegisters();
}

void MainWindow::refreshSelectedDeviceRegisters() {
    if (!registerTable_ || !showDeviceRegisters_ || !showDeviceRegisters_->isChecked()) return;
    registerTable_->setRowCount(0);
    if (selectedDevice_ == static_cast<size_t>(-1)) return;
    DriverInformation information; std::string error;
    if (!app_.driverInformation(selectedDevice_, information, error)) return;
    for (const DriverRegister& reg : information.registers) {
        setTableRow(registerTable_, registerTable_->rowCount(), {
            QString("0x%1").arg(reg.address, 2, HexBase, QLatin1Char('0')),
            QString::fromStdString(reg.name),
            reg.readable && reg.writable ? "R/W" : reg.writable ? "W" : "R",
            QString::fromStdString(reg.purpose)});
    }
}

void MainWindow::selectRegisterRow(int row, int) {
    if (row < 0 || !registerTable_ || !registerTable_->item(row, 0)) return;
    QString text = registerTable_->item(row, 0)->text().trimmed();
    if (text.startsWith("0x", Qt::CaseInsensitive)) text = text.mid(2);
    bool ok = false; const uint address = text.toUInt(&ok, 16);
    if (ok && address <= 0xFF)
        registerAddressHex_->setText(QString("0x%1").arg(address, 2, HexBase, QLatin1Char('0')));
}

void MainWindow::appendRegisterLog(const QString& message) {
    if (!registerLog_) return;
    setTableRow(registerLog_, registerLog_->rowCount(),
                {QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message});
    if (followRegisterLog_) QTimer::singleShot(0, this, [this] {
        if (followRegisterLog_->isChecked()) registerLog_->scrollToBottom();
    });
}

void MainWindow::readSelectedDevice() {
    if (selectedDevice_ == static_cast<size_t>(-1) || readInProgress_.exchange(true)) return;
    stopWorker();
    readButton_->setEnabled(false);
    const size_t index = selectedDevice_;
    worker_ = std::thread([this, index] {
        DeviceResult result;
        std::string error;
        const bool ok = app_.readDeviceResult(index, result, error);
        QMetaObject::invokeMethod(this, [this, index, result, error, ok] {
            readInProgress_.store(false);
            readButton_->setEnabled(true);
            if (!ok) showError(QString("%1: %2")
                .arg(driverEventContext(index), QString::fromStdString(error.empty() ? result.error : error)));
            populateDeviceResult(index, result);
        }, Qt::QueuedConnection);
    });
}

QString MainWindow::deviceValueText(const DeviceValue& value) const {
    return QString::fromStdString(std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) return {};
        else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return item;
        else { std::ostringstream stream; stream << item; return stream.str(); }
    }, value));
}

QString MainWindow::driverEventContext(size_t deviceIndex) const {
    const AppConfig config = app_.config();
    if (deviceIndex >= config.devices.size()) return "Device unavailable";
    const DeviceConfig& device = config.devices[deviceIndex];
    DriverInformation information;
    std::string error;
    if (!app_.driverInformation(deviceIndex, information, error)) {
        return QString("Device \"%1\" [%2]")
            .arg(QString::fromStdString(device.name), QString::fromStdString(device.type));
    }
    return QString("Device \"%1\" [%2] — Driver: %3")
        .arg(QString::fromStdString(device.name),
             QString::fromStdString(device.type),
             QString::fromStdString(information.title));
}

QString MainWindow::deviceResultText(const DeviceResult& result) const {
    if (!result.succeeded) return QString("ERROR: %1").arg(QString::fromStdString(result.error));
    QStringList values;
    for (const DeviceReading& reading : result.readings)
        values << QString("%1=%2 %3").arg(QString::fromStdString(reading.key), deviceValueText(reading.value), QString::fromStdString(reading.unit));
    return values.join(", ");
}

void MainWindow::populateDeviceResult(size_t deviceIndex, const DeviceResult& result) {
    const QString when = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    for (const DeviceReading& reading : result.readings) {
        const QString raw = std::holds_alternative<std::monostate>(reading.raw) ? QString() : deviceValueText(reading.raw);
        setTableRow(resultTable_, resultTable_->rowCount(), {when, QString::fromStdString(reading.key), deviceValueText(reading.value), QString::fromStdString(reading.unit), raw});
        bool valueOk = false, rawOk = false;
        const double value = numericValue(reading.value, valueOk);
        const double rawValue = numericValue(reading.raw, rawOk);
        if (valueOk) resultHistory_.push_back({deviceIndex, when, QString::fromStdString(reading.key), value, rawValue, rawOk});
    }
    if (followLatest_) QTimer::singleShot(0, this, [this] {
        if (followLatest_->isChecked()) resultTable_->scrollToBottom();
    });
    showMessage(QString("%1: %2")
        .arg(driverEventContext(deviceIndex),
             result.succeeded ? "reading completed" : "reading failed"));
}

QString MainWindow::adapterText(const AdapterInformation& adapter) const {
    QStringList lines;
    lines << QString("Adapter %1").arg(adapter.index)
          << QString("Product: %1").arg(QString::fromStdString(adapter.product))
          << QString("Manufacturer: %1").arg(QString::fromStdString(adapter.manufacturer))
          << QString("Serial: %1").arg(QString::fromStdString(adapter.serial));
    if (adapter.attributesAvailable)
        lines << QString("USB VID/PID: 0x%1/0x%2").arg(adapter.vid, 4, HexBase, QLatin1Char('0')).arg(adapter.pid, 4, HexBase, QLatin1Char('0'));
    if (adapter.partNumberAvailable)
        lines << QString("Part number/version: %1/%2").arg(adapter.partNumber).arg(adapter.partVersion);
    if (adapter.usbConfigAvailable)
        lines << QString("Stored USB VID/PID: 0x%1/0x%2, release %3, power %4 mA, mode %5")
                    .arg(adapter.configuredVid, 4, HexBase, QLatin1Char('0')).arg(adapter.configuredPid, 4, HexBase, QLatin1Char('0'))
                    .arg(adapter.configuredRelease).arg(adapter.configuredPower).arg(adapter.configuredPowerMode);
    if (adapter.lockAvailable)
        lines << QString("Lock bits: 0x%1").arg(adapter.lockBits, 2, HexBase, QLatin1Char('0'));
    if (adapter.smbusConfigAvailable)
        lines << QString("SMBus: %1 Hz, target 0x%2, retries %3, read timeout %4 ms, write timeout %5 ms, auto-read %6, SCL-low timeout %7")
                    .arg(adapter.smbusBitRate).arg(adapter.smbusAddress, 2, HexBase, QLatin1Char('0'))
                    .arg(adapter.smbusTransferRetries).arg(adapter.smbusReadTimeout).arg(adapter.smbusWriteTimeout)
                    .arg(adapter.smbusAutoReadRespond ? "on" : "off").arg(adapter.smbusSclLowTimeout ? "on" : "off");
    if (adapter.smbusLibraryVersionAvailable)
        lines << QString("SMBus library: %1.%2%3").arg(adapter.smbusLibraryMajor).arg(adapter.smbusLibraryMinor).arg(adapter.smbusLibraryRelease ? " release" : " debug");
    if (adapter.hidLibraryVersionAvailable)
        lines << QString("HID library: %1.%2%3").arg(adapter.hidLibraryMajor).arg(adapter.hidLibraryMinor).arg(adapter.hidLibraryRelease ? " release" : " debug");
    return lines.join("\n");
}

void MainWindow::startStreaming() {
    if (selectedDevice_ == static_cast<size_t>(-1)) return;
    if (scanRunning_) {
        showMessage("Cannot start live polling while a scan is active. Cancel or complete the scan first.");
        return;
    }
    if (streamRunning_) return;
    if (worker_.joinable()) stopWorker();
    streamCancel_.store(false);
    streamRunning_ = true;
    refreshElapsedMs_ = 0;
    refreshProgress_->setValue(0);
    refreshTimer_->start();
    streamButton_->setEnabled(false);
    stopStreamButton_->setEnabled(true);
    readButton_->setEnabled(false);
    scanButton_->setEnabled(false);
    const size_t index = selectedDevice_;
    worker_ = std::thread([this, index] {
        std::string error;
        app_.streamDeviceStructured(index, streamCancel_, [this, index](const std::string& when, const DeviceResult& result) {
            QMetaObject::invokeMethod(this, [this, index, when, result] {
                populateDeviceResult(index, result);
                showMessage(QString("%1: live poll received at %2")
                    .arg(driverEventContext(index), QString::fromStdString(when)));
            }, Qt::QueuedConnection);
        }, error);
        QMetaObject::invokeMethod(this, [this, index, error] {
            streamRunning_ = false;
            streamButton_->setEnabled(true);
            stopStreamButton_->setEnabled(false);
            scanButton_->setEnabled(true);
            readButton_->setEnabled(true);
            if (!error.empty()) showError(QString("%1: %2")
                .arg(driverEventContext(index), QString::fromStdString(error)));
        }, Qt::QueuedConnection);
    });
}

void MainWindow::stopStreaming() {
    if (scanRunning_) {
        showMessage("A scan is active. Cancel or complete the scan before stopping live polling.");
        return;
    }
    streamCancel_.store(true);
    refreshTimer_->stop();
    refreshProgress_->setValue(0);
    if (worker_.joinable()) stopWorker();
    streamRunning_ = false;
    streamButton_->setEnabled(true);
    stopStreamButton_->setEnabled(false);
    readButton_->setEnabled(true);
    scanButton_->setEnabled(true);
}

void MainWindow::startScan() {
    if (streamRunning_) {
        showMessage("Cannot start a scan while live polling is active. Stop polling first.");
        return;
    }
    if (scanRunning_) return;
    if (worker_.joinable()) stopWorker();
    scanCancel_.store(false);
    selectedScanAddress_ = -1;
    scanRunning_ = true;
    scanButton_->setEnabled(false);
    cancelScanButton_->setEnabled(true);
    streamButton_->setEnabled(false);
    readButton_->setEnabled(false);
    scanTable_->setRowCount(0);
    scanProgress_->setValue(0);
    scanCountLabel_->setText("Scanned: 0 | Responded: 0");
    ScanOptions options;
    options.adapterIndex = scanAdapter_->value(); options.startAddress = scanStart_->value();
    options.endAddress = scanEnd_->value(); options.clockHz = scanClock_->value();
    options.transferTimeoutMs = scanTimeout_->value(); options.transferRetries = scanRetries_->value();
    const bool includeUnresponsive = showUnresponsive_->isChecked();
    worker_ = std::thread([this, options, includeUnresponsive] {
        std::vector<ScanReading> readings;
        std::string error;
        unsigned scanned = 0;
        unsigned responded = 0;
        const auto updateCounts = [this, options, &scanned, &responded](const ScanReading& reading) {
            ++scanned;
            if (reading.readSucceeded) ++responded;
            const int percent = options.endAddress == options.startAddress ? 100 : static_cast<int>((reading.address - options.startAddress + 1) * 100u / (options.endAddress - options.startAddress + 1));
            QMetaObject::invokeMethod(this, [this, percent, scanned, responded] {
                scanProgress_->setValue(percent);
                scanCountLabel_->setText(QString("Scanned: %1 | Responded: %2").arg(scanned).arg(responded));
            }, Qt::QueuedConnection);
            return !scanCancel_.load();
        };
        const bool ok = app_.scan(options, readings, error, {}, &scanCancel_, updateCounts);
        QMetaObject::invokeMethod(this, [this, readings, error, ok] {
            scanRunning_ = false;
            scanButton_->setEnabled(true); cancelScanButton_->setEnabled(false); scanProgress_->setValue(100);
            streamButton_->setEnabled(true);
            readButton_->setEnabled(true);
            scanReadings_ = readings;
            filterScanResults();
            if (!ok && !error.empty()) showError(QString::fromStdString(error));
            else showMessage(QString("Scan complete: %1 address(es) checked.").arg(readings.size()));
        }, Qt::QueuedConnection);
    });
}

void MainWindow::cancelScan() {
    if (!scanRunning_) return;
    scanCancel_.store(true); showMessage("Scan cancellation requested; waiting for the current transfer to finish.");
}

void MainWindow::filterScanResults() {
    scanTable_->setRowCount(0);
    unsigned responded = 0;
    for (const ScanReading& reading : scanReadings_) {
        if (reading.readSucceeded) ++responded;
        if (!showUnresponsive_->isChecked() && !reading.readSucceeded) continue;
        const TransferDiagnostics& transfer = reading.transfer;
        const QString apiStatus = transfer.cancelled ? "Cancelled" :
            QString("%1%2").arg(transfer.apiStatus).arg(transfer.apiStatus == CP2112_SUCCESS ? " (success)" : " (error)");
        const QString transferState = transfer.transferState == TRANSFER_BUSY ? "Busy" :
            transfer.transferState == TRANSFER_COMPLETE ? "Complete" :
            transfer.transferState == TRANSFER_ERROR ? "Error" :
            transfer.transferState == TRANSFER_IDLE ? "Idle" : QString::number(transfer.transferState);
        setTableRow(scanTable_, scanTable_->rowCount(), {
            QString("0x%1").arg(reading.address, 2, HexBase, QLatin1Char('0')),
            QString::number(reading.address),
            reading.readSucceeded ? "Device responded" : "No response",
            reading.readSucceeded ? QString("0x%1").arg(reading.value, 2, HexBase, QLatin1Char('0')) : QString(),
            QString::number(transfer.retries), transfer.timedOut ? "Yes" : "No", apiStatus,
            transferState, transfer.detailedStatus < 0 ? QString("-") : QString::number(transfer.detailedStatus)});
    }
    scanCountLabel_->setText(QString("Scanned: %1 | Responded: %2").arg(scanReadings_.size()).arg(responded));
}

void MainWindow::readRegister() {
    unsigned device = 0, address = 0;
    if (!parseHexField(registerDeviceHex_, 0x77, device) || !parseHexField(registerAddressHex_, 0xFF, address)) {
        registerResult_->setStyleSheet("color: #b3261e;"); registerResult_->setText("Warning: invalid input"); registerOutput_->clear();
        appendRegisterLog("WARNING: invalid read address input");
        showError("Enter valid hexadecimal device and register addresses.");
        return;
    }
    std::vector<BYTE> values; std::string error;
    if (app_.readRegister(device, address, static_cast<unsigned>(registerReadLength_->value()), values, error)) {
        QStringList formatted;
        for (const BYTE value : values) formatted << QString("0x%1").arg(value, 2, HexBase, QLatin1Char('0'));
        const QString output = formatted.join(' ');
        registerResult_->setStyleSheet("color: #16803c;");
        registerResult_->setText(QString("Success: read %1 byte(s)").arg(values.size()));
        registerOutput_->setText(output);
        appendRegisterLog(QString("SUCCESS: read 0x%1, %2 byte(s): %3").arg(address, 2, HexBase, QLatin1Char('0')).arg(values.size()).arg(output));
    }
    else { registerResult_->setStyleSheet("color: #b3261e;"); registerResult_->setText("Warning: operation failed"); registerOutput_->setText(QString::fromStdString(error)); appendRegisterLog("WARNING: " + QString::fromStdString(error)); showError(QString::fromStdString(error)); }
}

void MainWindow::writeRegister() {
    unsigned device = 0, address = 0, value = 0;
    if (!parseHexField(registerDeviceHex_, 0x77, device) || !parseHexField(registerAddressHex_, 0xFF, address) || !parseHexField(registerValueHex_, 0xFF, value)) {
        registerResult_->setStyleSheet("color: #b3261e;"); registerResult_->setText("Warning: invalid input"); registerOutput_->clear();
        appendRegisterLog("WARNING: invalid write address/value input");
        showError("Enter valid hexadecimal device, register, and write values.");
        return;
    }
    std::string error;
    if (app_.writeRegister(device, address, static_cast<BYTE>(value), error)) { registerResult_->setStyleSheet("color: #16803c;"); registerResult_->setText("Success: write completed"); registerOutput_->setText(QString("Wrote 0x%1").arg(value, 2, HexBase, QLatin1Char('0'))); appendRegisterLog(QString("SUCCESS: write 0x%1 = 0x%2").arg(address, 2, HexBase, QLatin1Char('0')).arg(value, 2, HexBase, QLatin1Char('0'))); }
    else { registerResult_->setStyleSheet("color: #b3261e;"); registerResult_->setText("Warning: operation failed"); registerOutput_->setText(QString::fromStdString(error)); appendRegisterLog("WARNING: " + QString::fromStdString(error)); showError(QString::fromStdString(error)); }
}

void MainWindow::saveGeneralSettings() {
    struct Update { const char* key; uint64_t value; };
    const Update numeric[] = {{"transfer_timeout_ms", static_cast<uint64_t>(generalTimeout_->value())}, {"transfer_retries", static_cast<uint64_t>(generalRetries_->value())}, {"scan_clock_hz", static_cast<uint64_t>(generalClock_->value())}, {"scan_start_address", static_cast<uint64_t>(generalStart_->value())}, {"scan_end_address", static_cast<uint64_t>(generalEnd_->value())}, {"default_refresh_seconds", static_cast<uint64_t>(generalRefresh_->value())}, {"log_retention_days", static_cast<uint64_t>(generalRetention_->value())}};
    std::string error;
    if (!app_.updateGeneralSetting("bridge_dll", generalBridgeDll_->text().toStdString(), error) ||
        !app_.updateGeneralSetting("log_dir", generalLogDir_->text().toStdString(), error) ||
        !app_.updateGeneralSetting("scan_adapter_index", static_cast<uint64_t>(generalAdapter_->value()), error)) {
        showError(QString::fromStdString(error));
        return;
    }
    for (const Update& update : numeric) if (!app_.updateGeneralSetting(update.key, update.value, error)) { showError(QString::fromStdString(error)); return; }
    if (!app_.updateGeneralSetting("default_log_stream", generalLogStream_->isChecked(), error)) { showError(QString::fromStdString(error)); return; }
    if (!app_.updateGeneralSetting("theme", generalTheme_->currentData().toString().toStdString(), error)) { showError(QString::fromStdString(error)); return; }
    applyTheme(generalTheme_->currentData().toString());
    showMessage("General settings saved.");
}

void MainWindow::editSelectedDeviceSettings() {
    if (selectedDevice_ == static_cast<size_t>(-1)) return;
    const AppConfig config = app_.config();
    if (selectedDevice_ >= config.devices.size()) return;
    const DeviceConfig& device = config.devices[selectedDevice_];
    QDialog dialog(this);
    dialog.setWindowTitle(QString("Device settings: %1").arg(QString::fromStdString(device.name)));
    auto* form = new QFormLayout(&dialog);
    auto* address = new QLineEdit(QString("0x%1").arg(device.address, 2, HexBase, QLatin1Char('0')), &dialog);
    address->setMaximumWidth(160);
    auto* clock = numberBox(1, 400000, device.clock, &dialog);
    auto* refresh = numberBox(1, 3600, device.refreshSeconds, &dialog);
    auto* logging = new QCheckBox(&dialog); logging->setChecked(device.logStream);
    auto* enabled = new QCheckBox(&dialog); enabled->setChecked(device.enabled);
    form->addRow("Address (hex)", address);
    form->addRow("Clock (Hz)", clock);
    form->addRow("Refresh (s)", refresh);
    form->addRow("Log stream", logging);
    form->addRow("Enabled", enabled);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    struct Update { const char* key; SettingValue value; };
    unsigned parsedAddress = 0;
    if (!parseHexField(address, 0x77, parsedAddress) || parsedAddress < 0x08) {
        showError("Enter a valid hexadecimal 7-bit device address from 0x08 to 0x77.");
        return;
    }
    const Update updates[] = {{"address", static_cast<uint64_t>(parsedAddress)}, {"clock_hz", static_cast<uint64_t>(clock->value())}, {"refresh_seconds", static_cast<uint64_t>(refresh->value())}, {"log_stream", logging->isChecked()}, {"enabled", enabled->isChecked()}};
    std::string error;
    for (const Update& update : updates) if (!app_.updateDeviceSetting(selectedDevice_, update.key, update.value, error)) { showError(QString::fromStdString(error)); return; }
    refreshDevices(); showMessage("Device settings saved.");
}

void MainWindow::reloadConfiguration() {
    std::string error;
    if (!app_.reload(error)) {
        showError(QString::fromStdString(error));
        return;
    }
    const AppConfig config = app_.config();
    generalBridgeDll_->setText(QString::fromStdString(config.bridgeDll));
    generalLogDir_->setText(QString::fromStdString(config.logDir));
    generalAdapter_->setValue(config.scanAdapterIndex);
    generalTimeout_->setValue(config.transferTimeoutMs);
    generalRetries_->setValue(config.transferRetries);
    generalClock_->setValue(config.scanClockHz);
    generalStart_->setValue(config.scanStartAddress);
    generalEnd_->setValue(config.scanEndAddress);
    generalRefresh_->setValue(config.defaultRefreshSeconds);
    generalRetention_->setValue(config.logRetentionDays);
    generalLogStream_->setChecked(config.defaultLogStream);
    const QString configuredTheme = QString::fromStdString(config.theme.empty() ? (config.darkMode ? "dark" : "default") : config.theme);
    generalTheme_->setCurrentIndex(std::max(0, generalTheme_->findData(configuredTheme)));
    applyTheme(configuredTheme);
    refreshAll();
    showMessage("Configuration reloaded.");
}

void MainWindow::cleanLogs() {
    LogCleanupResult result; std::string error;
    if (!app_.cleanLogs(app_.config().logRetentionDays, result, error)) showError(QString::fromStdString(error));
    else showMessage(QString("Log cleanup complete: %1 deleted, %2 failed").arg(result.deleted).arg(result.failed));
}

void MainWindow::clearAllLogs() {
    if (QMessageBox::question(this, "Clear all logs",
            "Delete all timestamped audit and device log files? The current audit log will remain active.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    LogCleanupResult result; std::string error;
    if (!app_.clearAllLogs(result, error)) { showError(QString::fromStdString(error)); return; }
    eventLog_->setRowCount(0);
    showMessage(QString("All logs cleared: %1 deleted, %2 failed").arg(result.deleted).arg(result.failed));
}

void MainWindow::stopWorker() { if (worker_.joinable()) worker_.join(); }

void MainWindow::showMessage(const QString& message) {
    if (eventLog_) setTableRow(eventLog_, eventLog_->rowCount(), {QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message});
    if (eventLog_ && followEvents_) QTimer::singleShot(0, this, [this] {
        if (followEvents_->isChecked()) eventLog_->scrollToBottom();
    });
    statusBar()->showMessage(message, 8000);
}

void MainWindow::applyTheme(const QString& theme) {
    qApp->setStyle("Fusion");
    if (theme == "default") { qApp->setPalette(qApp->style()->standardPalette()); return; }
    QPalette palette;
    if (theme == "light") {
        palette.setColor(QPalette::Window, QColor(245, 245, 245)); palette.setColor(QPalette::WindowText, QColor(25, 25, 25));
        palette.setColor(QPalette::Base, Qt::white); palette.setColor(QPalette::AlternateBase, QColor(235, 235, 235));
        palette.setColor(QPalette::Text, QColor(25, 25, 25)); palette.setColor(QPalette::Button, QColor(235, 235, 235));
        palette.setColor(QPalette::ButtonText, QColor(25, 25, 25)); palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::HighlightedText, Qt::white); qApp->setPalette(palette); return;
    }
    palette.setColor(QPalette::Window, QColor(36, 36, 36)); palette.setColor(QPalette::WindowText, Qt::white);
    palette.setColor(QPalette::Base, QColor(28, 28, 28)); palette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    palette.setColor(QPalette::Text, Qt::white); palette.setColor(QPalette::Button, QColor(55, 55, 55));
    palette.setColor(QPalette::ButtonText, Qt::white); palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    palette.setColor(QPalette::HighlightedText, Qt::white); qApp->setPalette(palette);
}

void MainWindow::showError(const QString& message) {
    showMessage("ERROR: " + message);
    QMessageBox::warning(this, "CP2112 Utility", message);
}

void MainWindow::updateRefreshProgress() {
    if (!streamButton_ || streamButton_->isEnabled()) return;
    const AppConfig config = app_.config();
    if (selectedDevice_ >= config.devices.size()) return;
    const int intervalMs = std::max(100, static_cast<int>(config.devices[selectedDevice_].refreshSeconds * 1000));
    refreshElapsedMs_ = std::min(intervalMs, refreshElapsedMs_ + 100);
    refreshProgress_->setValue(refreshElapsedMs_ * 100 / intervalMs);
    if (refreshElapsedMs_ >= intervalMs) refreshElapsedMs_ = 0;
}
