#pragma once

#include "PipelineTypes.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSharedPointer>
#include <QUrl>
#include <QWidget>

class QJsonObject;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTableWidget;
class QTimer;

namespace psxstudio::ci {

class SteganosClient;

struct BuilderInfo {
  QString id;
  QString name;
  QUrl endpoint;
  QString platform;
  QString architecture;
  QString ownerEmail;
  bool online{ false };
  int capacity{ 1 };
  int activeJobs{ 0 };

  bool isValid() const {
    return !id.isEmpty() && endpoint.isValid() && online;
  }
};

class CiPanel final : public QWidget {
  Q_OBJECT

public:
  explicit CiPanel(QWidget* parent = nullptr);
  ~CiPanel() override;

  bool canScheduleBuilds() const;
  bool authenticationInProgress() const;
  QString authenticatedIdentity() const;
  QList<BuilderInfo> builders() const;
  BuilderInfo chooseBuilder(TargetPlatform platform,
                            const QString& preferredArchitecture = {}) const;
  int activeJobCount() const;

  QString queueBuild(const PipelineRequest& request,
                     const BuilderInfo& builder,
                     const QString& repositoryPath,
                     const QString& commit,
                     const QString& outputPath);
  void cancelAll();

signals:
  void authenticationStateChanged();
  void buildersChanged();
  void logLine(const QString& line);
  void buildProgress(const QString& jobId, const QString& text);
  void buildCompleted(const QString& jobId, const QString& outputPath);
  void buildFailed(const QString& jobId, const QString& message, const QString& reportPath);
  void buildCancelled(const QString& jobId);

private:
  struct Job;

  void setupUi();
  void updateAuthenticationUi();
  void fetchAccess();
  void refreshBuilders();
  void updateBuilderTable();
  void updateJobTable(const QSharedPointer<Job>& job);
  void saveRouterUrl();
  void setStatus(const QString& text, bool error = false);

  void startPairing(const QSharedPointer<Job>& job);
  void waitForPair(const QSharedPointer<Job>& job);
  void prepareBundle(const QSharedPointer<Job>& job);
  void uploadBundle(const QSharedPointer<Job>& job);
  void submitBuild(const QSharedPointer<Job>& job);
  void pollJobs();
  void pollJob(const QSharedPointer<Job>& job);
  void downloadArtifact(const QSharedPointer<Job>& job);
  void downloadReport(const QSharedPointer<Job>& job,
                      const QString& failureMessage = {});
  void finishSuccess(const QSharedPointer<Job>& job);
  void finishFailure(const QSharedPointer<Job>& job,
                     const QString& message,
                     const QString& reportPath = {});
  void finishCancelled(const QSharedPointer<Job>& job);
  void cleanupJobFiles(const QSharedPointer<Job>& job);
  QNetworkReply* directRequest(const QSharedPointer<Job>& job,
                               const QString& method,
                               const QString& path,
                               const QJsonObject& body = {});

  SteganosClient* client_{ nullptr };
  QNetworkAccessManager* directNetwork_{ nullptr };
  QTimer* builderTimer_{ nullptr };
  QTimer* jobTimer_{ nullptr };
  QPointer<QNetworkReply> accessReply_;

  QLabel* identityLabel_{ nullptr };
  QLabel* accessLabel_{ nullptr };
  QLabel* statusLabel_{ nullptr };
  QLabel* builderSummaryLabel_{ nullptr };
  QPushButton* sessionButton_{ nullptr };
  QPushButton* refreshButton_{ nullptr };
  QLineEdit* routerEdit_{ nullptr };
  QTableWidget* builderTable_{ nullptr };
  QTableWidget* jobTable_{ nullptr };
  QPlainTextEdit* reportView_{ nullptr };

  bool accessLoaded_{ false };
  bool operatorAccess_{ false };
  bool administratorAccess_{ false };
  QString accessError_;
  QList<BuilderInfo> builders_;
  QHash<QString, QSharedPointer<Job>> jobs_;
};

} // namespace psxstudio::ci
