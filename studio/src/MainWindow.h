#pragma once

#include "PipelineTypes.h"

#include <QMainWindow>
#include <QSet>

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
class QTabWidget;
class QThread;
class QVBoxLayout;
class QWidget;

namespace oclero::qlementine {
class ThemeManager;
}

namespace psxstudio {

class PipelineWorker;
namespace ci { class CiPanel; struct BuilderInfo; }

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
  void updateSystemControls();
  void updateBatchMode();
  void updatePlatformControls();
  void updateExportMode();
  void startBuild();
  void cancelBuild();
  void revealOutput();
  void toggleTheme();
  void onCompleted(const QString& appPath);
  void onFailed(const QString& message, const QString& workspacePath);
  void onCiSourcePrepared(psxstudio::PipelineRequest request,
                          const QString& repositoryPath,
                          const QString& commit);
  void onCiBuildCompleted(const QString& jobId, const QString& outputPath);
  void onCiBuildFailed(const QString& jobId,
                       const QString& message,
                       const QString& reportPath);
  void onCiBuildCancelled(const QString& jobId);
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
  void planBuildBackends(QList<PipelineRequest>& requests);
  void finishIfIdle();
  void finishSuccessfulExport();

  struct BatchGameEntry {
    QString id;
    QString sourcePath;
    QStringList selectedBinPaths;
    QString title;
    QString iconPath;
    QString serial;
    QString volumeId;
  };

  QLabel* headerTitle_{ nullptr };
  QLabel* headerSubtitle_{ nullptr };
  QLabel* discLabel_{ nullptr };
  QLabel* biosLabel_{ nullptr };
  QLabel* padPolicyLabel_{ nullptr };
  QLineEdit* discEdit_{ nullptr };
  QComboBox* systemCombo_{ nullptr };
  QComboBox* platformCombo_{ nullptr };
  QComboBox* exportModeCombo_{ nullptr };
  QCheckBox* batchCheck_{ nullptr };
  QLineEdit* batchDirectoryEdit_{ nullptr };
  QListWidget* batchList_{ nullptr };
  QLabel* batchSummaryLabel_{ nullptr };
  QLineEdit* biosEdit_{ nullptr };
  QLineEdit* iconEdit_{ nullptr };
  QLineEdit* titleEdit_{ nullptr };
  QLineEdit* outputEdit_{ nullptr };
  QCheckBox* exportAsZip_{ nullptr };
  QCheckBox* useCi_{ nullptr };
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
  QCheckBox* nativeExecution_{ nullptr };
  QLabel* stageLabel_{ nullptr };
  QProgressBar* progressBar_{ nullptr };
  QPlainTextEdit* logView_{ nullptr };
  QPushButton* buildButton_{ nullptr };
  QPushButton* cancelButton_{ nullptr };
  QPushButton* revealButton_{ nullptr };
  QPushButton* themeButton_{ nullptr };
  QTabWidget* tabs_{ nullptr };
  ci::CiPanel* ciPanel_{ nullptr };
  QFrame* inputCard_{ nullptr };
  QFrame* toolsCard_{ nullptr };
  QFrame* brandingCard_{ nullptr };
  QFrame* statusCard_{ nullptr };
  QWidget* formsContainer_{ nullptr };
  QScrollArea* formsScroll_{ nullptr };
  QGridLayout* formsLayout_{ nullptr };
  oclero::qlementine::ThemeManager* themeManager_{ nullptr };
  SystemKind currentSystem_{ SystemKind::PlayStation };
  QString psxInputPath_;
  QString psxBiosPath_;
  QString psxBatchDirectory_;
  QString gbaInputPath_;
  QString gbaBiosPath_;
  QString gbaBatchDirectory_;
  QStringList selectedBins_;
  QList<BatchGameEntry> batchEntries_;
  QList<PipelineRequest> pendingRequests_;
  PipelineRequest activeRequest_;
  QStringList completedOutputs_;
  QSet<QString> activeCiJobs_;
  int activeRequestIndex_{ 0 };
  int totalRequestCount_{ 0 };
  QString outputAppPath_;
  bool formsAreColumns_{ false };
  bool workerActive_{ false };
  bool exportFailed_{ false };
  bool cancelling_{ false };
  QThread* workerThread_{ nullptr };
  PipelineWorker* worker_{ nullptr };
};

} // namespace psxstudio
