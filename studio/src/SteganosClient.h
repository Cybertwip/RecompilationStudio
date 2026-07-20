// Adapted from Cybertwip NeoGeo Hub with authorization for PSXRecomp Studio.
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>

class QHttpMultiPart;
class QNetworkReply;
class QNetworkRequest;
class QTcpServer;
class QTcpSocket;

namespace psxstudio::ci {

class SteganosClient : public QObject {
  Q_OBJECT

public:
  explicit SteganosClient(QObject* parent = nullptr);
  SteganosClient(const QUrl& baseUrl, QObject* parent);
  ~SteganosClient() override;

  void setBaseUrl(const QUrl& url);
  QUrl baseUrl() const;
  QUrl apiUrl(const QString& path) const;

  bool isAuthenticated() const;
  bool authenticationInProgress() const;
  QString sessionRole() const;
  QString userId() const;
  QString userEmail() const;
  QString displayName() const;
  QStringList userRoles() const;

  void signIn(const QString& role = QStringLiteral("player"));
  void cancelSignIn();
  void signOut();
  void refreshAccessToken();
  void checkConnection();

  QNetworkReply* get(const QString& path, int transferTimeoutMilliseconds = 30000);
  QNetworkReply* getUrl(const QUrl& url);
  QNetworkReply* postJson(const QString& path, const QJsonObject& payload = {});
  QNetworkReply* postMultipart(const QString& path, QHttpMultiPart* multipart);
  void authorize(QNetworkRequest& request) const;

signals:
  void authenticationStarted();
  void authenticationSucceeded();
  void authenticationFailed(const QString& message);
  void authenticationChanged(bool authenticated);
  void authenticationInProgressChanged(bool inProgress);
  void signedOut();
  void connectionCheckStarted();
  void connectionCheckFinished(bool connected, const QString& message);

private:
  void clearTokens(bool emitSignals = true, bool removePersistedSession = false);
  QString sessionFilePath() const;
  void persistSession() const;
  void restorePersistedSession();
  void deletePersistedSession() const;
  void beginNativeAuthentication(const QString& role, const QString& codeChallenge);
  void pollNativeAuthentication();
  void exchangeAuthorizationCode(const QString& code);
  void acceptTokenResponse(const QByteArray& payload);
  void scheduleRefresh(qint64 expiresInSeconds);
  void handleLoopbackConnection();
  void processLoopbackSocket(QTcpSocket* socket);
  void finishAuthenticationAttempt();
  void failAuthenticationAttempt(const QString& message);
  void setAuthenticationInProgress(bool inProgress);
  void writeLoopbackResponse(QTcpSocket* socket, bool success, const QString& message);
  QByteArray randomBytes(int size) const;
  QString base64Url(const QByteArray& bytes) const;

private:
  QNetworkAccessManager _networkManager;
  QTcpServer* _loopbackServer{ nullptr };
  QTimer _refreshTimer;
  QTimer _authenticationTimeoutTimer;
  QTimer _authenticationPollTimer;
  QPointer<QNetworkReply> _authenticationReply;
  QPointer<QNetworkReply> _authenticationPollReply;
  QPointer<QNetworkReply> _connectionReply;
  QUrl _baseUrl;
  QUrl _redirectUri;
  QString _codeVerifier;
  QString _requestedRole;
  QString _nativePollToken;
  QString _accessToken;
  QString _refreshToken;
  QDateTime _accessTokenExpiry;
  QString _sessionRole;
  QString _userId;
  QString _userEmail;
  QString _displayName;
  QStringList _userRoles;
  bool _refreshInProgress{ false };
  bool _authenticationInProgress{ false };
};

} // namespace psxstudio::ci
