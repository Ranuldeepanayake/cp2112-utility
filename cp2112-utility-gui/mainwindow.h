// Declares the Qt frontend for the shared CP2112 application service.
#pragma once

#include "Application.h"
#include <QMainWindow>
#include <QString>
#include <atomic>
#include <cstddef>
#include <thread>

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QSpinBox;
class QTableWidget;
class QTimer;
class QTabWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    struct ResultPoint { size_t deviceIndex = 0; QString timestamp; QString reading; double value = 0.0; double raw = 0.0; bool hasRaw = false; };

private slots:
    void refreshAll();
    void selectDeviceRow(int row, int column);
    void selectScannedDeviceRow(int row, int column);
    void refreshAdapterInformation();
    void showDriverInformation();
    void executeDeviceAction();
    void editSelectedDeviceSettings();
    void readSelectedDevice();
    void startStreaming();
    void stopStreaming();
    void startScan();
    void cancelScan();
    void readRegister();
    void writeRegister();
    void saveGeneralSettings();
    void reloadConfiguration();
    void cleanLogs();
    void clearAllLogs();
    void updateRefreshProgress();
    void filterScanResults();
    void applyTheme(const QString& theme);
    void refreshSelectedDeviceRegisters();
    void selectRegisterRow(int row, int column);
    void appendRegisterLog(const QString& message);

private:
    void buildUi();
    void loadConfiguration();
    void refreshAdapters();
    void refreshDevices();
    void refreshDeviceSettings();
    void updateSelectedDeviceLabel();
    void stopWorker();
    void showMessage(const QString& message);
    void showError(const QString& message);
    QString configPath() const;
    QString driverEventContext(size_t deviceIndex) const;
    QString deviceValueText(const DeviceValue& value) const;
    QString deviceResultText(const DeviceResult& result) const;
    QString adapterText(const AdapterInformation& adapter) const;
    void populateDeviceResult(size_t deviceIndex, const DeviceResult& result);

    ApplicationService app_;
    std::thread worker_;
    std::atomic<bool> scanCancel_{false};
    std::atomic<bool> streamCancel_{false};
    std::atomic<bool> readInProgress_{false};
    bool scanRunning_ = false;
    bool streamRunning_ = false;
    size_t selectedDevice_ = static_cast<size_t>(-1);
    int selectedScanAddress_ = -1;

    QLabel* versionLabel_ = nullptr;
    QLabel* adapterLabel_ = nullptr;
    QLabel* selectedDeviceLabel_ = nullptr;
    QTableWidget* deviceTable_ = nullptr;
    QTableWidget* resultTable_ = nullptr;
    QTableWidget* adapterTable_ = nullptr;
    QTableWidget* eventLog_ = nullptr;
    QTableWidget* registerTable_ = nullptr;
    QTableWidget* registerLog_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QPushButton* readButton_ = nullptr;
    QPushButton* streamButton_ = nullptr;
    QPushButton* stopStreamButton_ = nullptr;
    QPushButton* deviceSettingsButton_ = nullptr;
    QPushButton* driverInfoButton_ = nullptr;
    QPushButton* actionButton_ = nullptr;
    QSpinBox* scanAdapter_ = nullptr;
    QSpinBox* scanStart_ = nullptr;
    QSpinBox* scanEnd_ = nullptr;
    QSpinBox* scanClock_ = nullptr;
    QSpinBox* scanTimeout_ = nullptr;
    QSpinBox* scanRetries_ = nullptr;
    QCheckBox* showUnresponsive_ = nullptr;
    QLabel* scanCountLabel_ = nullptr;
    QProgressBar* scanProgress_ = nullptr;
    QPushButton* scanButton_ = nullptr;
    QPushButton* cancelScanButton_ = nullptr;
    QTableWidget* scanTable_ = nullptr;
    QLineEdit* registerDeviceHex_ = nullptr;
    QLineEdit* registerAddressHex_ = nullptr;
    QLineEdit* registerValueHex_ = nullptr;
    QSpinBox* registerReadLength_ = nullptr;
    QLabel* registerResult_ = nullptr;
    QLineEdit* registerOutput_ = nullptr;
    QPushButton* clearResultsButton_ = nullptr;
    QPushButton* chartButton_ = nullptr;

    QLineEdit* generalBridgeDll_ = nullptr;
    QLineEdit* generalLogDir_ = nullptr;
    QSpinBox* generalAdapter_ = nullptr;
    QSpinBox* generalTimeout_ = nullptr;
    QSpinBox* generalRetries_ = nullptr;
    QSpinBox* generalClock_ = nullptr;
    QSpinBox* generalStart_ = nullptr;
    QSpinBox* generalEnd_ = nullptr;
    QSpinBox* generalRefresh_ = nullptr;
    QSpinBox* generalRetention_ = nullptr;
    QCheckBox* generalLogStream_ = nullptr;
    QComboBox* generalTheme_ = nullptr;
    QCheckBox* followEvents_ = nullptr;
    QCheckBox* showDeviceRegisters_ = nullptr;
    QCheckBox* followRegisterLog_ = nullptr;
    std::vector<ScanReading> scanReadings_;
    std::vector<ResultPoint> resultHistory_;
    QCheckBox* followLatest_ = nullptr;
    QProgressBar* refreshProgress_ = nullptr;
    QLabel* refreshProgressLabel_ = nullptr;
    QLabel* scanProgressLabel_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    int refreshElapsedMs_ = 0;
};
