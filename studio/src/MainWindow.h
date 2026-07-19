#pragma once

#include "PipelineTypes.h"

#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QThread;
class QVBoxLayout;
class QWidget;

namespace oclero::qlementine {
class ThemeManager;
}

namespace psxstudio {

class PipelineWorker;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

signals:
  void runRequested(psxstudio::PipelineRequest request);

protected:
  void resizeEvent(QResizeEvent* event) override;

private slots:
  void chooseDisc();
  void chooseBatchDirectory();
  void chooseBios();
  void chooseIcon();
  void chooseOutputDirectory();
  void chooseCertificate();
  void chooseGhidraHome();
  void chooseBiosInitialSplash();
  void chooseBiosHandoffImage();
  void updateBiosPatchControls();
  void updateBatchMode();
  void updatePlatformControls();
  void startBuild();
  void cancelBuild();
  void revealOutput();
  void toggleTheme();
  void onCompleted(const QString& appPath);
  void onFailed(const QString& message, const QString& workspacePath);
  void setBusy(bool busy);
  void updateBuildButton();

private:
  QLineEdit* addPathRow(QWidget* parent,
                        QVBoxLayout* layout,
                        const QString& label,
                        const QString& placeholder,
                        const char* slot);
  QString detectGhidraHome() const;
  void loadSettings();
  void saveSettings() const;
  void applyTheme();
  void reflowForms(bool force = false);
  PipelineRequest requestFromUi(bool overwrite) const;
  QList<PipelineRequest> requestsFromUi(bool overwrite) const;
  QString outputPathForRequest(const PipelineRequest& request) const;
  void populateBatchDirectory(const QString& path, bool showDialogs);
  void rebuildBatchList();
  void chooseBatchIcon(const QString& id);
  void clearBatchIcon(const QString& id);
  void removeBatchEntry(const QString& id);
  void startNextRequest();

  struct BatchGameEntry {
    QString id;
    QString sourcePath;
    QStringList selectedBinPaths;
    QString title;
    QString iconPath;
    QString serial;
    QString volumeId;
  };

  QLineEdit* discEdit_{ nullptr };
  QComboBox* platformCombo_{ nullptr };
  QCheckBox* batchCheck_{ nullptr };
  QLineEdit* batchDirectoryEdit_{ nullptr };
  QListWidget* batchList_{ nullptr };
  QLabel* batchSummaryLabel_{ nullptr };
  QLineEdit* biosEdit_{ nullptr };
  QLineEdit* iconEdit_{ nullptr };
  QLineEdit* titleEdit_{ nullptr };
  QLineEdit* outputEdit_{ nullptr };
  QCheckBox* exportAsZip_{ nullptr };
  QLineEdit* certificateEdit_{ nullptr };
  QLineEdit* certificatePasswordEdit_{ nullptr };
  QCheckBox* signingEnabled_{ nullptr };
  QLabel* signingNote_{ nullptr };
  QLineEdit* ghidraEdit_{ nullptr };
  QCheckBox* biosPatchEnabled_{ nullptr };
  QLineEdit* biosInitialSplashEdit_{ nullptr };
  QLineEdit* biosHandoffImageEdit_{ nullptr };
  QCheckBox* biosMuteAudio_{ nullptr };
  QCheckBox* biosRemovePsGlyph_{ nullptr };
  QCheckBox* skipBiosBoot_{ nullptr };
  QCheckBox* macosGipGamepad_{ nullptr };
  QLabel* stageLabel_{ nullptr };
  QProgressBar* progressBar_{ nullptr };
  QPlainTextEdit* logView_{ nullptr };
  QPushButton* buildButton_{ nullptr };
  QPushButton* cancelButton_{ nullptr };
  QPushButton* revealButton_{ nullptr };
  QPushButton* themeButton_{ nullptr };
  QFrame* inputCard_{ nullptr };
  QFrame* toolsCard_{ nullptr };
  QFrame* brandingCard_{ nullptr };
  QFrame* statusCard_{ nullptr };
  QWidget* formsContainer_{ nullptr };
  QScrollArea* formsScroll_{ nullptr };
  QGridLayout* formsLayout_{ nullptr };
  oclero::qlementine::ThemeManager* themeManager_{ nullptr };
  QStringList selectedBins_;
  QList<BatchGameEntry> batchEntries_;
  QList<PipelineRequest> pendingRequests_;
  PipelineRequest activeRequest_;
  QStringList completedOutputs_;
  int activeRequestIndex_{ 0 };
  int totalRequestCount_{ 0 };
  QString outputAppPath_;
  bool formsAreColumns_{ false };
  QThread* workerThread_{ nullptr };
  PipelineWorker* worker_{ nullptr };
};

} // namespace psxstudio
