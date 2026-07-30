#include "CiPanel.h"

#include "PipelineSupport.h"
#include "SteganosClient.h"

#include "quazip.h"
#include "quazipfile.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#ifndef PSX_CI_ROUTER_DEFAULT_URL
#define PSX_CI_ROUTER_DEFAULT_URL "https://cybertwip.com:4343"
#endif

namespace psxstudio::ci {

namespace {

QString platformKey(TargetPlatform platform) {
  return targetPlatformKey(platform);
}

QString normalizedArchitecture(QString value) {
  value = value.trimmed().toLower();
  if (value == QStringLiteral("amd64") || value == QStringLiteral("x64") ||
      value == QStringLiteral("x86-64")) {
    return QStringLiteral("x86_64");
  }
  if (value == QStringLiteral("aarch64")) {
    return QStringLiteral("arm64");
  }
  return value;
}

QString joinEndpoint(const QUrl& base, const QString& path) {
  auto result = base.adjusted(QUrl::StripTrailingSlash);
  result.setPath(result.path() + (path.startsWith('/') ? path
                                                       : QStringLiteral("/") + path));
  result.setQuery(QString{});
  result.setFragment({});
  return result.toString(QUrl::FullyEncoded);
}

QString apiError(QNetworkReply* reply,
                 const QByteArray& body,
                 const QString& fallback) {
  const auto object = QJsonDocument::fromJson(body).object();
  const QString message = object.value(QStringLiteral("message")).toString();
  if (!message.isEmpty()) return message;
  const QString error = object.value(QStringLiteral("error")).toString();
  if (!error.isEmpty()) return error;
  return reply->errorString().isEmpty() ? fallback : reply->errorString();
}

bool runGit(const QStringList& arguments, QString& error, int timeoutMs = 30000) {
  const QString git = findExecutable(QStringLiteral("git"));
  if (git.isEmpty()) {
    error = QStringLiteral("Git is required for CI source replication.");
    return false;
  }
  QProcess process;
  process.start(git, arguments);
  if (!process.waitForStarted(5000) || !process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(3000);
    error = QStringLiteral("Git did not complete: %1").arg(process.errorString());
    return false;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (error.isEmpty()) error = QStringLiteral("Git command failed.");
    return false;
  }
  return true;
}

QString safeFilePart(QString value) {
  for (auto& character : value) {
    if (!character.isLetterOrNumber() && character != '-' &&
        character != '_' && character != '.') {
      character = '-';
    }
  }
  while (value.contains(QStringLiteral("--"))) {
    value.replace(QStringLiteral("--"), QStringLiteral("-"));
  }
  value = value.trimmed();
  return value.isEmpty() ? QStringLiteral("builder") : value;
}

bool verifyDownloadedArtifact(const QString& archivePath,
                              const PipelineRequest& request,
                              QString& error) {
  QuaZip archive(archivePath);
  if (!archive.open(QuaZip::mdUnzip)) {
    error = QStringLiteral("The CI artifact is not a readable ZIP archive.");
    return false;
  }
  const QStringList entries = archive.getFileNameList();
  const int manifestCount = entries.count(QStringLiteral("game.manifest.json"));
  if (manifestCount != 1 || !archive.setCurrentFile(QStringLiteral("game.manifest.json"))) {
    archive.close();
    error = QStringLiteral("The CI artifact must contain exactly one root game.manifest.json.");
    return false;
  }
  QuaZipFile manifestFile(&archive);
  if (!manifestFile.open(QIODevice::ReadOnly)) {
    archive.close();
    error = QStringLiteral("The CI artifact game.manifest.json could not be read.");
    return false;
  }
  const auto observed = QJsonDocument::fromJson(manifestFile.readAll()).object();
  manifestFile.close();
  const auto expected = gameManifestForRequest(request);
  if (observed.value(QStringLiteral("executable")) != expected.value(QStringLiteral("executable")) ||
      observed.value(QStringLiteral("name")) != expected.value(QStringLiteral("name")) ||
      observed.value(QStringLiteral("platform")) != expected.value(QStringLiteral("platform"))) {
    archive.close();
    error = QStringLiteral("The CI artifact game.manifest.json does not match the requested export.");
    return false;
  }
  const QString executable = observed.value(QStringLiteral("executable")).toString();
  bool executableFound = entries.contains(executable);
  if (request.targetPlatform == TargetPlatform::MacOS) {
    const QString prefix = executable + QLatin1Char('/');
    executableFound = std::any_of(entries.cbegin(), entries.cend(),
                                  [&](const QString& entry) {
                                    return entry.startsWith(prefix);
                                  });
  }
  archive.close();
  if (!executableFound) {
    error = QStringLiteral("The executable declared by the CI game manifest is missing from the artifact.");
    return false;
  }
  return true;
}

} // namespace

struct CiPanel::Job {
  QString id;
  PipelineRequest request;
  BuilderInfo builder;
  QString repositoryPath;
  QString commit;
  QString outputPath;
  QString pairId;
  QString pairToken;
  QString repositoryKey;
  QString bundlePath;
  QString bundleId;
  QString remoteJobId;
  QString remoteArtifactName;
  QString reportPath;
  QString status{ QStringLiteral("Queued") };
  QString detail;
  int row{ -1 };
  int pairAttempts{ 0 };
  bool requestInFlight{ false };
  bool terminal{ false };
  bool cancelRequested{ false };
  QPointer<QProcess> bundleProcess;
};

CiPanel::CiPanel(QWidget* parent)
: QWidget(parent),
  directNetwork_(new QNetworkAccessManager(this)) {
  setupUi();

  QSettings settings;
  const QString router = settings.value(
    QStringLiteral("ci/router_url"),
    QString::fromLatin1(PSX_CI_ROUTER_DEFAULT_URL)).toString();
  routerEdit_->setText(router);
  client_ = new SteganosClient(QUrl(router), this);

  connect(client_, &SteganosClient::authenticationChanged, this,
          [this](bool authenticated) {
            if (!authenticated) {
              if (accessReply_) {
                accessReply_->abort();
                accessReply_->deleteLater();
                accessReply_.clear();
              }
              accessLoaded_ = false;
              operatorAccess_ = false;
              administratorAccess_ = false;
              accessError_.clear();
              builders_.clear();
              updateBuilderTable();
            }
            updateAuthenticationUi();
            emit authenticationStateChanged();
            if (authenticated) fetchAccess();
          });
  connect(client_, &SteganosClient::authenticationInProgressChanged, this,
          [this](bool) {
            updateAuthenticationUi();
            emit authenticationStateChanged();
          });
  connect(client_, &SteganosClient::authenticationSucceeded, this, [this]() {
    setStatus(QStringLiteral("Microsoft sign-in and Steganos 2FA completed."));
    fetchAccess();
  });
  connect(client_, &SteganosClient::authenticationFailed, this,
          [this](const QString& message) {
            setStatus(message, true);
            updateAuthenticationUi();
            emit authenticationStateChanged();
          });
  connect(sessionButton_, &QPushButton::clicked, this, [this]() {
    if (client_->authenticationInProgress()) {
      client_->cancelSignIn();
    } else if (client_->isAuthenticated()) {
      client_->signOut();
    } else {
      accessLoaded_ = false;
      accessError_.clear();
      client_->signIn(QStringLiteral("player"));
    }
  });
  connect(refreshButton_, &QPushButton::clicked, this, &CiPanel::refreshBuilders);
  connect(routerEdit_, &QLineEdit::editingFinished, this, &CiPanel::saveRouterUrl);

  builderTimer_ = new QTimer(this);
  builderTimer_->setInterval(10000);
  connect(builderTimer_, &QTimer::timeout, this, &CiPanel::refreshBuilders);
  builderTimer_->start();

  jobTimer_ = new QTimer(this);
  jobTimer_->setInterval(2000);
  connect(jobTimer_, &QTimer::timeout, this, &CiPanel::pollJobs);
  jobTimer_->start();

  updateAuthenticationUi();
}

CiPanel::~CiPanel() {
  if (accessReply_) {
    accessReply_->abort();
  }
}

void CiPanel::setupUi() {
  setObjectName(QStringLiteral("ciPanel"));
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(18, 16, 18, 18);
  root->setSpacing(12);

  auto* authCard = new QFrame(this);
  authCard->setFrameShape(QFrame::StyledPanel);
  authCard->setObjectName(QStringLiteral("ciAuthCard"));
  auto* authLayout = new QVBoxLayout(authCard);
  authLayout->setContentsMargins(16, 14, 16, 14);
  auto* authHeader = new QHBoxLayout();
  auto* title = new QLabel(QStringLiteral("Build CI"), authCard);
  auto titleFont = title->font();
  titleFont.setBold(true);
  titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
  title->setFont(titleFont);
  identityLabel_ = new QLabel(QStringLiteral("Not signed in"), authCard);
  identityLabel_->setObjectName(QStringLiteral("secondaryText"));
  accessLabel_ = new QLabel(authCard);
  accessLabel_->setObjectName(QStringLiteral("secondaryText"));
  sessionButton_ = new QPushButton(QStringLiteral("Sign in with Microsoft"), authCard);
  sessionButton_->setObjectName(QStringLiteral("ciSessionButton"));
  authHeader->addWidget(title);
  authHeader->addSpacing(10);
  authHeader->addWidget(identityLabel_);
  authHeader->addWidget(accessLabel_);
  authHeader->addStretch(1);
  authHeader->addWidget(sessionButton_);
  authLayout->addLayout(authHeader);

  auto* description = new QLabel(
    QStringLiteral("Build exports can share their generated Git repository with authenticated "
                   "macOS, Windows, and Linux workers. The local builder keeps one job while "
                   "available CI workers execute the remaining plan in parallel."), authCard);
  description->setWordWrap(true);
  description->setObjectName(QStringLiteral("secondaryText"));
  authLayout->addWidget(description);

  auto* routerRow = new QHBoxLayout();
  auto* routerLabel = new QLabel(QStringLiteral("Router"), authCard);
  routerLabel->setMinimumWidth(80);
  routerEdit_ = new QLineEdit(authCard);
  routerEdit_->setObjectName(QStringLiteral("ciRouterUrl"));
  routerEdit_->setPlaceholderText(QString::fromLatin1(PSX_CI_ROUTER_DEFAULT_URL));
  refreshButton_ = new QPushButton(QStringLiteral("Refresh builders"), authCard);
  routerRow->addWidget(routerLabel);
  routerRow->addWidget(routerEdit_, 1);
  routerRow->addWidget(refreshButton_);
  authLayout->addLayout(routerRow);
  statusLabel_ = new QLabel(authCard);
  statusLabel_->setWordWrap(true);
  authLayout->addWidget(statusLabel_);
  root->addWidget(authCard);

  auto* buildersCard = new QFrame(this);
  buildersCard->setFrameShape(QFrame::StyledPanel);
  buildersCard->setObjectName(QStringLiteral("ciBuildersCard"));
  auto* buildersLayout = new QVBoxLayout(buildersCard);
  buildersLayout->setContentsMargins(16, 14, 16, 14);
  builderSummaryLabel_ = new QLabel(QStringLiteral("No CI builders loaded."), buildersCard);
  buildersLayout->addWidget(builderSummaryLabel_);
  builderTable_ = new QTableWidget(0, 6, buildersCard);
  builderTable_->setObjectName(QStringLiteral("ciBuilderTable"));
  builderTable_->setHorizontalHeaderLabels({
    QStringLiteral("Builder"), QStringLiteral("Platform"),
    QStringLiteral("Architecture"), QStringLiteral("Status"),
    QStringLiteral("Load"), QStringLiteral("Owner") });
  builderTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  builderTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
  builderTable_->verticalHeader()->setVisible(false);
  builderTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  builderTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  builderTable_->setMinimumHeight(150);
  buildersLayout->addWidget(builderTable_);
  root->addWidget(buildersCard);

  auto* jobsCard = new QFrame(this);
  jobsCard->setFrameShape(QFrame::StyledPanel);
  jobsCard->setObjectName(QStringLiteral("ciJobsCard"));
  auto* jobsLayout = new QVBoxLayout(jobsCard);
  jobsLayout->setContentsMargins(16, 14, 16, 14);
  jobsLayout->addWidget(new QLabel(QStringLiteral("CI build activity"), jobsCard));
  jobTable_ = new QTableWidget(0, 5, jobsCard);
  jobTable_->setObjectName(QStringLiteral("ciJobTable"));
  jobTable_->setHorizontalHeaderLabels({
    QStringLiteral("Game"), QStringLiteral("Target"), QStringLiteral("Builder"),
    QStringLiteral("Status"), QStringLiteral("Detail") });
  jobTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  jobTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
  jobTable_->verticalHeader()->setVisible(false);
  jobTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  jobTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  jobTable_->setMinimumHeight(120);
  reportView_ = new QPlainTextEdit(jobsCard);
  reportView_->setReadOnly(true);
  reportView_->setPlaceholderText(QStringLiteral("Remote builder reports appear here when a job fails."));
  reportView_->setMaximumBlockCount(10000);
  reportView_->setMinimumHeight(90);
  jobsLayout->addWidget(jobTable_);
  jobsLayout->addWidget(reportView_);
  root->addWidget(jobsCard, 1);
}

bool CiPanel::canScheduleBuilds() const {
  return client_ && client_->isAuthenticated() && accessLoaded_ &&
         (operatorAccess_ || administratorAccess_);
}

bool CiPanel::authenticationInProgress() const {
  return client_ && client_->authenticationInProgress();
}

QString CiPanel::authenticatedIdentity() const {
  if (!client_ || !client_->isAuthenticated()) return {};
  return client_->displayName().isEmpty() ? client_->userEmail()
                                          : client_->displayName();
}

QList<BuilderInfo> CiPanel::builders() const {
  return builders_;
}

BuilderInfo CiPanel::chooseBuilder(TargetPlatform platform,
                                   const QString& preferredArchitecture) const {
  BuilderInfo selected;
  double bestScore = 1.0e9;
  const QString wantedPlatform = platformKey(platform);
  const QString wantedArchitecture = normalizedArchitecture(preferredArchitecture);
  for (const auto& builder : builders_) {
    if (!builder.isValid() || builder.platform != wantedPlatform) continue;
    const double load = static_cast<double>(builder.activeJobs) /
                        static_cast<double>(std::max(1, builder.capacity));
    const double architecturePenalty = !wantedArchitecture.isEmpty() &&
        normalizedArchitecture(builder.architecture) != wantedArchitecture
      ? 10.0 : 0.0;
    const double score = architecturePenalty + load;
    if (!selected.isValid() || score < bestScore) {
      selected = builder;
      bestScore = score;
    }
  }
  return selected;
}

int CiPanel::activeJobCount() const {
  int active = 0;
  for (const auto& job : jobs_) {
    if (job && !job->terminal) ++active;
  }
  return active;
}

void CiPanel::updateAuthenticationUi() {
  const bool pending = client_ && client_->authenticationInProgress();
  const bool authenticated = client_ && client_->isAuthenticated();
  if (pending) {
    sessionButton_->setText(QStringLiteral("Cancel sign-in"));
    identityLabel_->setText(QStringLiteral("Waiting for Microsoft sign-in and 2FA…"));
    accessLabel_->clear();
  } else if (!authenticated) {
    sessionButton_->setText(QStringLiteral("Sign in with Microsoft"));
    identityLabel_->setText(QStringLiteral("Not signed in"));
    accessLabel_->clear();
  } else {
    sessionButton_->setText(QStringLiteral("Sign out"));
    identityLabel_->setText(QStringLiteral("%1 (%2)")
      .arg(authenticatedIdentity(), client_->userEmail()));
    if (!accessError_.isEmpty()) {
      accessLabel_->setText(QStringLiteral("Access unavailable"));
    } else if (!accessLoaded_) {
      accessLabel_->setText(QStringLiteral("Checking access…"));
    } else if (administratorAccess_) {
      accessLabel_->setText(QStringLiteral("Administrator"));
    } else if (operatorAccess_) {
      accessLabel_->setText(QStringLiteral("Operator"));
    } else {
      accessLabel_->setText(QStringLiteral("No build access"));
    }
  }
  refreshButton_->setEnabled(canScheduleBuilds());
}

void CiPanel::fetchAccess() {
  if (!client_ || !client_->isAuthenticated() || accessReply_) return;
  accessLoaded_ = false;
  accessError_.clear();
  updateAuthenticationUi();
  auto* reply = client_->get(QStringLiteral("/api/v1/access"), 15000);
  accessReply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (accessReply_ != reply) {
      reply->deleteLater();
      return;
    }
    accessReply_.clear();
    const QByteArray body = reply->readAll();
    const int statusCode = reply->attribute(
      QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || statusCode < 200 ||
        statusCode >= 300) {
      accessError_ = apiError(reply, body,
                              QStringLiteral("Could not determine CI access."));
      accessLoaded_ = false;
      operatorAccess_ = false;
      administratorAccess_ = false;
      reply->deleteLater();
      setStatus(accessError_, true);
      updateAuthenticationUi();
      emit authenticationStateChanged();
      if (statusCode == 401) client_->refreshAccessToken();
      return;
    }
    const auto document = QJsonDocument::fromJson(body);
    const auto access = document.object().value(QStringLiteral("access")).toObject();
    reply->deleteLater();
    if (!document.isObject() || access.isEmpty()) {
      accessError_ = QStringLiteral("The CI router returned an invalid access response.");
      setStatus(accessError_, true);
      updateAuthenticationUi();
      emit authenticationStateChanged();
      return;
    }
    operatorAccess_ = access.value(QStringLiteral("operator")).toBool();
    administratorAccess_ = access.value(QStringLiteral("administrator")).toBool();
    accessLoaded_ = true;
    accessError_.clear();
    setStatus(operatorAccess_ || administratorAccess_
      ? QStringLiteral("CI build access is ready.")
      : QStringLiteral("This account is authenticated but is not on the CI operator allowlist."),
      !(operatorAccess_ || administratorAccess_));
    updateAuthenticationUi();
    emit authenticationStateChanged();
    refreshBuilders();
  });
}

void CiPanel::saveRouterUrl() {
  const QUrl value(routerEdit_->text().trimmed());
  if (!value.isValid() ||
      (value.scheme() != QStringLiteral("http") &&
       value.scheme() != QStringLiteral("https")) || value.host().isEmpty()) {
    setStatus(QStringLiteral("Enter a valid HTTP or HTTPS CI router URL."), true);
    return;
  }
  const QUrl normalized = value.adjusted(QUrl::StripTrailingSlash);
  routerEdit_->setText(normalized.toString());
  QSettings().setValue(QStringLiteral("ci/router_url"), normalized.toString());
  if (client_->baseUrl() == normalized) return;
  accessLoaded_ = false;
  operatorAccess_ = false;
  administratorAccess_ = false;
  accessError_.clear();
  builders_.clear();
  updateBuilderTable();
  client_->setBaseUrl(normalized);
  updateAuthenticationUi();
  emit authenticationStateChanged();
}

void CiPanel::refreshBuilders() {
  if (!canScheduleBuilds()) return;
  auto* reply = client_->get(QStringLiteral("/api/v1/builders"));
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
      setStatus(apiError(reply, body, QStringLiteral("Could not refresh CI builders.")), true);
      reply->deleteLater();
      return;
    }
    QList<BuilderInfo> refreshed;
    const auto rows = QJsonDocument::fromJson(body).object()
                        .value(QStringLiteral("builders")).toArray();
    for (const auto& value : rows) {
      const auto object = value.toObject();
      BuilderInfo builder;
      builder.id = object.value(QStringLiteral("id")).toString();
      builder.name = object.value(QStringLiteral("name")).toString();
      builder.endpoint = QUrl(object.value(QStringLiteral("endpoint")).toString());
      builder.platform = object.value(QStringLiteral("platform")).toString().toLower();
      builder.architecture = normalizedArchitecture(
        object.value(QStringLiteral("architecture")).toString());
      builder.ownerEmail = object.value(QStringLiteral("owner_email")).toString();
      builder.online = object.value(QStringLiteral("online")).toBool();
      builder.capacity = std::max(1, object.value(QStringLiteral("slots")).toInt(1));
      builder.activeJobs = std::max(0, object.value(QStringLiteral("active_jobs")).toInt());
      if (!builder.id.isEmpty() && builder.endpoint.isValid()) refreshed.append(builder);
    }
    builders_ = refreshed;
    reply->deleteLater();
    updateBuilderTable();
    emit buildersChanged();
  });
}

void CiPanel::updateBuilderTable() {
  builderTable_->setRowCount(builders_.size());
  QHash<QString, int> onlineCounts;
  for (int row = 0; row < builders_.size(); ++row) {
    const auto& builder = builders_.at(row);
    if (builder.online) ++onlineCounts[builder.platform];
    const QStringList values{
      builder.name,
      targetPlatformDisplayName(targetPlatformFromKey(builder.platform)),
      builder.architecture,
      builder.online ? QStringLiteral("Available") : QStringLiteral("Offline"),
      QStringLiteral("%1 / %2").arg(builder.activeJobs).arg(builder.capacity),
      builder.ownerEmail,
    };
    for (int column = 0; column < values.size(); ++column) {
      builderTable_->setItem(row, column, new QTableWidgetItem(values.at(column)));
    }
  }
  builderSummaryLabel_->setText(
    QStringLiteral("Available builders — macOS: %1   Windows: %2   Linux: %3")
      .arg(onlineCounts.value(QStringLiteral("macos")))
      .arg(onlineCounts.value(QStringLiteral("windows")))
      .arg(onlineCounts.value(QStringLiteral("linux"))));
}

QString CiPanel::queueBuild(const PipelineRequest& request,
                            const BuilderInfo& builder,
                            const QString& repositoryPath,
                            const QString& commit,
                            const QString& outputPath) {
  if (!canScheduleBuilds() || !builder.isValid() ||
      !QFileInfo(repositoryPath).isDir() || commit.isEmpty()) {
    return {};
  }
  auto job = QSharedPointer<Job>::create();
  job->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  job->request = request;
  job->builder = builder;
  job->repositoryPath = QFileInfo(repositoryPath).absoluteFilePath();
  job->commit = commit;
  job->outputPath = outputPath;
  job->row = jobTable_->rowCount();
  jobTable_->insertRow(job->row);
  for (int column = 0; column < jobTable_->columnCount(); ++column) {
    jobTable_->setItem(job->row, column, new QTableWidgetItem());
  }
  jobs_.insert(job->id, job);
  updateJobTable(job);
  emit logLine(QStringLiteral("CI plan: %1 (%2) → %3 [%4/%5]")
    .arg(request.windowTitle,
         targetPlatformDisplayName(request.targetPlatform),
         builder.name, builder.platform, builder.architecture));
  startPairing(job);
  return job->id;
}

void CiPanel::updateJobTable(const QSharedPointer<Job>& job) {
  if (!job || job->row < 0 || job->row >= jobTable_->rowCount()) return;
  const QStringList values{
    job->request.windowTitle,
    QStringLiteral("%1/%2")
      .arg(targetPlatformDisplayName(job->request.targetPlatform),
           job->builder.architecture),
    job->builder.name,
    job->status,
    job->detail,
  };
  for (int column = 0; column < values.size(); ++column) {
    jobTable_->item(job->row, column)->setText(values.at(column));
  }
  emit buildProgress(job->id,
                     QStringLiteral("%1 — %2").arg(job->status, job->detail));
}

void CiPanel::startPairing(const QSharedPointer<Job>& job) {
  if (!job || job->terminal) return;
  job->status = QStringLiteral("Pairing");
  job->detail = job->builder.name;
  updateJobTable(job);
  auto* reply = client_->postJson(
    QStringLiteral("/api/v1/builders/%1/pair").arg(job->builder.id));
  connect(reply, &QNetworkReply::finished, this, [this, job, reply]() {
    const QByteArray body = reply->readAll();
    if (job->terminal) {
      reply->deleteLater();
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      finishFailure(job, apiError(reply, body,
        QStringLiteral("Could not pair the selected CI builder.")));
      reply->deleteLater();
      return;
    }
    const auto object = QJsonDocument::fromJson(body).object();
    job->pairId = object.value(QStringLiteral("pair_id")).toString();
    job->pairToken = object.value(QStringLiteral("pair_token")).toString();
    const QUrl mappedEndpoint(object.value(QStringLiteral("builder_endpoint")).toString());
    if (mappedEndpoint.isValid()) job->builder.endpoint = mappedEndpoint;
    const bool accepted = object.value(QStringLiteral("accepted")).toBool();
    reply->deleteLater();
    if (job->pairId.isEmpty() || job->pairToken.isEmpty() ||
        !job->builder.endpoint.isValid()) {
      finishFailure(job, QStringLiteral("The CI router returned an incomplete builder pairing."));
      return;
    }
    if (accepted) prepareBundle(job);
    else waitForPair(job);
  });
}

void CiPanel::waitForPair(const QSharedPointer<Job>& job) {
  if (!job || job->terminal) return;
  if (++job->pairAttempts > 30) {
    finishFailure(job, QStringLiteral("The CI builder did not accept the pairing signal."));
    return;
  }
  QTimer::singleShot(1000, this, [this, job]() {
    if (job->terminal) return;
    auto* reply = client_->get(
      QStringLiteral("/api/v1/pairs/%1").arg(job->pairId));
    connect(reply, &QNetworkReply::finished, this, [this, job, reply]() {
      const QByteArray body = reply->readAll();
      if (job->terminal) {
        reply->deleteLater();
        return;
      }
      if (reply->error() != QNetworkReply::NoError) {
        job->detail = apiError(reply, body, QStringLiteral("Waiting for builder"));
        updateJobTable(job);
        reply->deleteLater();
        waitForPair(job);
        return;
      }
      const bool accepted = QJsonDocument::fromJson(body).object()
                              .value(QStringLiteral("accepted")).toBool();
      reply->deleteLater();
      if (accepted) prepareBundle(job);
      else waitForPair(job);
    });
  });
}

void CiPanel::prepareBundle(const QSharedPointer<Job>& job) {
  if (!job || job->terminal) return;
  const QString canonicalDisc = QFileInfo(job->request.cuePath).canonicalFilePath();
  const QString repositoryIdentity = QStringLiteral("%1\n%2\n%3")
    .arg(canonicalDisc.isEmpty() ? QFileInfo(job->request.cuePath).absoluteFilePath()
                                 : canonicalDisc,
         targetPlatformKey(job->request.targetPlatform),
         job->request.windowTitle.trimmed());
  job->repositoryKey = QString::fromLatin1(QCryptographicHash::hash(
    repositoryIdentity.toUtf8(), QCryptographicHash::Sha256).toHex());
  const QString bundleRoot = QDir(
    QStandardPaths::writableLocation(QStandardPaths::TempLocation))
      .filePath(QStringLiteral("psxrecomp-ci-bundles"));
  if (!QDir().mkpath(bundleRoot)) {
    finishFailure(job, QStringLiteral("Could not create the CI Git-bundle directory."));
    return;
  }
  job->bundlePath = QDir(bundleRoot).filePath(job->id + QStringLiteral(".bundle"));
  QFile::remove(job->bundlePath);
  const QString temporaryRef = QStringLiteral("refs/psxrecomp-ci/") + job->id;
  QString error;
  if (!runGit({ QStringLiteral("-C"), job->repositoryPath,
                QStringLiteral("update-ref"), temporaryRef, job->commit }, error)) {
    finishFailure(job, error);
    return;
  }

  const QString git = findExecutable(QStringLiteral("git"));
  auto* process = new QProcess(this);
  job->bundleProcess = process;
  job->status = QStringLiteral("Replicating source");
  job->detail = QStringLiteral("Creating Git bundle");
  updateJobTable(job);
  connect(process, &QProcess::readyReadStandardError, this, [this, job, process]() {
    const QString text = QString::fromUtf8(process->readAllStandardError()).trimmed();
    if (!text.isEmpty()) {
      job->detail = text;
      updateJobTable(job);
    }
  });
  connect(process, &QProcess::errorOccurred, this,
          [this, job, process](QProcess::ProcessError) {
            if (!job->terminal && process->state() == QProcess::NotRunning) {
              finishFailure(job, QStringLiteral("Git bundle process failed to start: %1")
                                   .arg(process->errorString()));
            }
          });
  connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, job, process, temporaryRef](int exitCode, QProcess::ExitStatus status) {
            QString ignored;
            runGit({ QStringLiteral("-C"), job->repositoryPath,
                     QStringLiteral("update-ref"), QStringLiteral("-d"), temporaryRef },
                   ignored);
            const QString stderrText = QString::fromUtf8(process->readAllStandardError()).trimmed();
            process->deleteLater();
            job->bundleProcess.clear();
            if (job->terminal) return;
            if (status != QProcess::NormalExit || exitCode != 0 ||
                !QFileInfo(job->bundlePath).isFile()) {
              finishFailure(job, stderrText.isEmpty()
                ? QStringLiteral("Git did not create the CI source bundle.")
                : stderrText);
              return;
            }
            uploadBundle(job);
          });
  process->start(git, { QStringLiteral("-C"), job->repositoryPath,
                        QStringLiteral("bundle"), QStringLiteral("create"),
                        job->bundlePath, temporaryRef });
}

void CiPanel::uploadBundle(const QSharedPointer<Job>& job) {
  if (!job || job->terminal) return;
  auto* file = new QFile(job->bundlePath);
  if (!file->open(QIODevice::ReadOnly)) {
    finishFailure(job, file->errorString());
    file->deleteLater();
    return;
  }
  job->status = QStringLiteral("Uploading source");
  job->detail.clear();
  updateJobTable(job);
  QNetworkRequest request{
    QUrl(joinEndpoint(job->builder.endpoint, QStringLiteral("/api/v1/bundles"))) };
  request.setRawHeader("Authorization", "Pair " + job->pairToken.toUtf8());
  request.setRawHeader("X-NeoGeo-Pair-ID", job->pairId.toUtf8());
  request.setRawHeader("X-NeoGeo-Repository-Key", job->repositoryKey.toUtf8());
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/x-git-bundle"));
  request.setTransferTimeout(10 * 60 * 1000);
  auto* reply = directNetwork_->post(request, file);
  file->setParent(reply);
  connect(reply, &QNetworkReply::uploadProgress, this,
          [this, job](qint64 sent, qint64 total) {
            if (total > 0) {
              job->detail = QStringLiteral("%1 / %2 MiB")
                .arg(sent / (1024 * 1024)).arg(total / (1024 * 1024));
              updateJobTable(job);
            }
          });
  connect(reply, &QNetworkReply::finished, this, [this, job, reply]() {
    const QByteArray body = reply->readAll();
    if (job->terminal) {
      reply->deleteLater();
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      finishFailure(job, apiError(reply, body,
        QStringLiteral("The CI builder rejected the source bundle.")));
      reply->deleteLater();
      return;
    }
    job->bundleId = QJsonDocument::fromJson(body).object()
                      .value(QStringLiteral("bundle_id")).toString();
    reply->deleteLater();
    QFile::remove(job->bundlePath);
    if (job->bundleId.isEmpty()) {
      finishFailure(job, QStringLiteral("The CI builder returned no source-bundle identifier."));
      return;
    }
    submitBuild(job);
  });
}

void CiPanel::submitBuild(const QSharedPointer<Job>& job) {
  if (!job || job->terminal) return;
  job->status = QStringLiteral("Submitting build");
  job->detail.clear();
  updateJobTable(job);
  const QJsonObject request{
    { QStringLiteral("repository"), job->repositoryPath },
    { QStringLiteral("commit"), job->commit },
    // The CMake target the generated project builds. Vita and Horizon share
    // one, because their packages differ only in which front-end they add.
    { QStringLiteral("target"),
      job->request.system == SystemKind::GameBoyAdvance ? QStringLiteral("gba-runtime")
        : systemRunsGuestNatively(job->request.system) ? QStringLiteral("guest-runtime")
        : QStringLiteral("psx-runtime") },
    { QStringLiteral("platform"), job->builder.platform },
    { QStringLiteral("architecture"), job->builder.architecture },
    { QStringLiteral("configuration"), QStringLiteral("Release") },
    { QStringLiteral("bundle_id"), job->bundleId },
    { QStringLiteral("repository_key"), job->repositoryKey },
  };
  auto* reply = directRequest(job, QStringLiteral("POST"),
                              QStringLiteral("/api/v1/jobs"), request);
  connect(reply, &QNetworkReply::finished, this, [this, job, reply]() {
    const QByteArray body = reply->readAll();
    if (job->terminal) {
      reply->deleteLater();
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      finishFailure(job, apiError(reply, body,
        QStringLiteral("The CI builder rejected the build plan.")));
      reply->deleteLater();
      return;
    }
    const auto object = QJsonDocument::fromJson(body).object()
                          .value(QStringLiteral("job")).toObject();
    job->remoteJobId = object.value(QStringLiteral("id")).toString();
    job->status = object.value(QStringLiteral("duplicate")).toBool()
      ? QStringLiteral("Joined existing build") : QStringLiteral("Queued remotely");
    job->detail = object.value(QStringLiteral("message")).toString();
    reply->deleteLater();
    if (job->remoteJobId.isEmpty()) {
      finishFailure(job, QStringLiteral("The CI builder returned no job identifier."));
      return;
    }
    updateJobTable(job);
  });
}

void CiPanel::pollJobs() {
  for (const auto& job : std::as_const(jobs_)) {
    if (job && !job->terminal && !job->remoteJobId.isEmpty() &&
        !job->requestInFlight) {
      pollJob(job);
    }
  }
}

void CiPanel::pollJob(const QSharedPointer<Job>& job) {
  job->requestInFlight = true;
  auto* reply = directRequest(job, QStringLiteral("GET"),
    QStringLiteral("/api/v1/jobs/%1").arg(job->remoteJobId));
  connect(reply, &QNetworkReply::finished, this, [this, job, reply]() {
    job->requestInFlight = false;
    const QByteArray body = reply->readAll();
    if (job->terminal) {
      reply->deleteLater();
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      job->detail = apiError(reply, body, QStringLiteral("Could not poll CI builder."));
      updateJobTable(job);
      reply->deleteLater();
      return;
    }
    const auto object = QJsonDocument::fromJson(body).object()
                          .value(QStringLiteral("job")).toObject();
    const QString status = object.value(QStringLiteral("status")).toString();
    job->detail = object.value(QStringLiteral("message")).toString();
    job->remoteArtifactName = object.value(QStringLiteral("artifact_name")).toString();
    reply->deleteLater();
    if (status == QStringLiteral("succeeded")) {
      downloadArtifact(job);
    } else if (status == QStringLiteral("failed") ||
               status == QStringLiteral("canceled")) {
      if (status == QStringLiteral("canceled") || job->cancelRequested) {
        finishCancelled(job);
      } else {
        downloadReport(job, job->detail.isEmpty()
          ? QStringLiteral("The CI build failed.") : job->detail);
      }
    } else {
      job->status = status == QStringLiteral("running")
        ? QStringLiteral("Building") : QStringLiteral("Queued remotely");
      updateJobTable(job);
    }
  });
}

void CiPanel::downloadArtifact(const QSharedPointer<Job>& job) {
  if (!job || job->terminal || job->requestInFlight) return;
  job->requestInFlight = true;
  job->status = QStringLiteral("Downloading artifact");
  job->detail.clear();
  updateJobTable(job);
  auto* reply = directRequest(job, QStringLiteral("GET"),
    QStringLiteral("/api/v1/jobs/%1/artifact").arg(job->remoteJobId));
  auto output = QSharedPointer<QSaveFile>::create(job->outputPath);
  if (!QDir().mkpath(QFileInfo(job->outputPath).absolutePath()) ||
      !output->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    job->requestInFlight = false;
    reply->abort();
    reply->deleteLater();
    finishFailure(job, output->errorString());
    return;
  }
  connect(reply, &QNetworkReply::readyRead, this,
          [reply, output]() { output->write(reply->readAll()); });
  connect(reply, &QNetworkReply::downloadProgress, this,
          [this, job](qint64 received, qint64 total) {
            if (total > 0) {
              job->detail = QStringLiteral("%1 / %2 MiB")
                .arg(received / (1024 * 1024)).arg(total / (1024 * 1024));
              updateJobTable(job);
            }
          });
  connect(reply, &QNetworkReply::finished, this, [this, job, reply, output]() {
    job->requestInFlight = false;
    output->write(reply->readAll());
    if (job->terminal) {
      output->cancelWriting();
      reply->deleteLater();
      return;
    }
    if (reply->error() != QNetworkReply::NoError || !output->commit()) {
      const QString message = reply->error() != QNetworkReply::NoError
        ? reply->errorString() : output->errorString();
      reply->deleteLater();
      finishFailure(job, message);
      return;
    }
    reply->deleteLater();
    QString verificationError;
    if (!verifyDownloadedArtifact(job->outputPath, job->request, verificationError)) {
      QFile::remove(job->outputPath);
      finishFailure(job, verificationError);
      return;
    }
    finishSuccess(job);
  });
}

void CiPanel::downloadReport(const QSharedPointer<Job>& job,
                             const QString& failureMessage) {
  if (!job || job->terminal || job->remoteJobId.isEmpty()) {
    if (job && !job->terminal) finishFailure(job, failureMessage);
    return;
  }
  auto* reply = directRequest(job, QStringLiteral("GET"),
    QStringLiteral("/api/v1/jobs/%1/log").arg(job->remoteJobId));
  connect(reply, &QNetworkReply::finished, this,
          [this, job, reply, failureMessage]() {
            const QByteArray body = reply->readAll();
            QString reportPath;
            if (reply->error() == QNetworkReply::NoError) {
              const QString reportName = QStringLiteral("%1-%2-%3-%4.log")
                .arg(safeFilePart(job->request.windowTitle), job->builder.platform,
                     job->builder.architecture, safeFilePart(job->builder.id));
              reportPath = QDir(QFileInfo(job->outputPath).absolutePath())
                             .filePath(reportName);
              QSaveFile report(reportPath);
              if (!report.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                  report.write(body) != body.size() || !report.commit()) {
                reportPath.clear();
              }
            }
            reply->deleteLater();
            if (!reportPath.isEmpty()) {
              QFile report(reportPath);
              if (report.open(QIODevice::ReadOnly)) {
                reportView_->setPlainText(QString::fromUtf8(report.readAll()));
                reportView_->moveCursor(QTextCursor::End);
              }
            }
            finishFailure(job, failureMessage, reportPath);
          });
}

void CiPanel::finishSuccess(const QSharedPointer<Job>& job) {
  if (!job || job->terminal) return;
  job->terminal = true;
  job->status = QStringLiteral("Succeeded");
  job->detail = job->outputPath;
  updateJobTable(job);
  cleanupJobFiles(job);
  emit logLine(QStringLiteral("CI artifact created: %1").arg(job->outputPath));
  emit buildCompleted(job->id, job->outputPath);
}

void CiPanel::finishFailure(const QSharedPointer<Job>& job,
                            const QString& message,
                            const QString& reportPath) {
  if (!job || job->terminal) return;
  job->terminal = true;
  job->status = QStringLiteral("Failed");
  job->detail = message;
  job->reportPath = reportPath;
  updateJobTable(job);
  cleanupJobFiles(job);
  emit logLine(QStringLiteral("CI ERROR: %1").arg(message));
  emit buildFailed(job->id, message, reportPath);
}

void CiPanel::finishCancelled(const QSharedPointer<Job>& job) {
  if (!job || job->terminal) return;
  job->terminal = true;
  job->status = QStringLiteral("Cancelled");
  job->detail.clear();
  updateJobTable(job);
  cleanupJobFiles(job);
  emit buildCancelled(job->id);
}

void CiPanel::cleanupJobFiles(const QSharedPointer<Job>& job) {
  if (!job) return;
  QFile::remove(job->bundlePath);
  if (!job->repositoryPath.isEmpty() &&
      job->repositoryPath.contains(QStringLiteral("psxrecomp-ci-sources"))) {
    QDir(job->repositoryPath).removeRecursively();
  }
}

void CiPanel::cancelAll() {
  for (const auto& job : std::as_const(jobs_)) {
    if (!job || job->terminal) continue;
    job->cancelRequested = true;
    if (job->bundleProcess && job->bundleProcess->state() != QProcess::NotRunning) {
      job->bundleProcess->kill();
    }
    if (!job->remoteJobId.isEmpty()) {
      job->status = QStringLiteral("Cancelling");
      updateJobTable(job);
      auto* reply = directRequest(job, QStringLiteral("DELETE"),
        QStringLiteral("/api/v1/jobs/%1").arg(job->remoteJobId));
      connect(reply, &QNetworkReply::finished, this, [this, job, reply]() {
        reply->deleteLater();
        finishCancelled(job);
      });
    } else {
      finishCancelled(job);
    }
  }
}

QNetworkReply* CiPanel::directRequest(const QSharedPointer<Job>& job,
                                      const QString& method,
                                      const QString& path,
                                      const QJsonObject& body) {
  QNetworkRequest request{ QUrl(joinEndpoint(job->builder.endpoint, path)) };
  request.setRawHeader("Authorization", "Pair " + job->pairToken.toUtf8());
  request.setRawHeader("X-NeoGeo-Pair-ID", job->pairId.toUtf8());
  request.setRawHeader("Accept", "application/json");
  request.setTransferTimeout(45 * 1000);
  if (method == QStringLiteral("POST")) {
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    return directNetwork_->post(request,
      QJsonDocument(body).toJson(QJsonDocument::Compact));
  }
  if (method == QStringLiteral("DELETE")) {
    return directNetwork_->deleteResource(request);
  }
  return directNetwork_->get(request);
}

void CiPanel::setStatus(const QString& text, bool error) {
  statusLabel_->setText(text);
  statusLabel_->setStyleSheet(error ? QStringLiteral("color:#d9384e") : QString());
}

} // namespace psxstudio::ci
