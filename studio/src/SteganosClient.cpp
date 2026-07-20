// Adapted from Cybertwip NeoGeo Hub with authorization for PSXRecomp Studio.
#include "SteganosClient.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QHostAddress>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <limits>

namespace psxstudio::ci {
namespace {
QString normalizedPath(const QString& path) {
  return path.startsWith('/') ? path : QStringLiteral("/") + path;
}

QString replyError(QNetworkReply* reply, const QString& fallback) {
  const auto document = QJsonDocument::fromJson(reply->readAll());
  if (document.isObject()) {
    const auto object = document.object();
    const auto message = object.value(QStringLiteral("message")).toString();
    if (!message.isEmpty()) {
      return message;
    }
    const auto error = object.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
      return error;
    }
  }
  return reply->errorString().isEmpty() ? fallback : reply->errorString();
}

QString apiErrorFromPayload(const QByteArray& payload, const QString& networkError, const QString& fallback) {
  const auto object = QJsonDocument::fromJson(payload).object();
  const auto message = object.value(QStringLiteral("message")).toString();
  if (!message.isEmpty()) return message;
  const auto error = object.value(QStringLiteral("error")).toString();
  if (!error.isEmpty()) return error;
  return networkError.isEmpty() ? fallback : networkError;
}
} // namespace

SteganosClient::SteganosClient(QObject* parent)
  : QObject(parent) {
  _refreshTimer.setSingleShot(true);
  connect(&_refreshTimer, &QTimer::timeout, this, &SteganosClient::refreshAccessToken);
  _authenticationTimeoutTimer.setSingleShot(true);
  connect(&_authenticationTimeoutTimer, &QTimer::timeout, this, [this]() {
    failAuthenticationAttempt(tr("Microsoft sign-in timed out. Please try again."));
  });
  _authenticationPollTimer.setInterval(2000);
  connect(&_authenticationPollTimer, &QTimer::timeout, this, &SteganosClient::pollNativeAuthentication);
}

SteganosClient::SteganosClient(const QUrl& baseUrl, QObject* parent)
  : SteganosClient(parent) {
  setBaseUrl(baseUrl);
}

SteganosClient::~SteganosClient() {
  finishAuthenticationAttempt();
}

void SteganosClient::setBaseUrl(const QUrl& url) {
  auto normalized = url.adjusted(QUrl::StripTrailingSlash);
  if (normalized == _baseUrl) {
    return;
  }
  const bool replacingExistingServer = _baseUrl.isValid() && !_baseUrl.isEmpty();
  finishAuthenticationAttempt();
  clearTokens(true, replacingExistingServer);
  _baseUrl = normalized;
  restorePersistedSession();
}

QUrl SteganosClient::baseUrl() const {
  return _baseUrl;
}

QUrl SteganosClient::apiUrl(const QString& path) const {
  if (!_baseUrl.isValid()) {
    return {};
  }
  auto result = _baseUrl;
  result.setPath(_baseUrl.path().chopped(_baseUrl.path().endsWith('/') ? 1 : 0) + normalizedPath(path));
  result.setQuery(QString{});
  result.setFragment({});
  return result;
}

bool SteganosClient::isAuthenticated() const {
  return !_accessToken.isEmpty();
}

bool SteganosClient::authenticationInProgress() const {
  return _authenticationInProgress;
}

QString SteganosClient::sessionRole() const {
  return _sessionRole;
}

QString SteganosClient::userId() const {
  return _userId;
}

QString SteganosClient::userEmail() const {
  return _userEmail;
}

QString SteganosClient::displayName() const {
  return _displayName;
}

QStringList SteganosClient::userRoles() const {
  return _userRoles;
}

void SteganosClient::signIn(const QString& role) {
  if (!_baseUrl.isValid() || _baseUrl.host().isEmpty()) {
    emit authenticationFailed(tr("Configure a valid Steganos server URL first."));
    return;
  }
  if (role != QStringLiteral("player") && role != QStringLiteral("operator")) {
    emit authenticationFailed(tr("Unsupported Steganos account role."));
    return;
  }

  finishAuthenticationAttempt();
  _requestedRole = role;
  _codeVerifier = base64Url(randomBytes(48));
  const auto challenge = base64Url(QCryptographicHash::hash(_codeVerifier.toUtf8(), QCryptographicHash::Sha256));
  _redirectUri = QUrl(QStringLiteral("urn:steganos:native-poll"));

  setAuthenticationInProgress(true);
  _authenticationTimeoutTimer.start(9 * 60 * 1000);
  emit authenticationStarted();
  beginNativeAuthentication(role, challenge);
}

void SteganosClient::beginNativeAuthentication(const QString& role, const QString& codeChallenge) {
  QJsonObject payload{
    { QStringLiteral("role"), role },
    { QStringLiteral("code_challenge"), codeChallenge },
  };
  QNetworkRequest request(apiUrl(QStringLiteral("/api/v1/auth/native/start")));
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setTransferTimeout(30 * 1000);
  auto* reply = _networkManager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
  _authenticationReply = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (_authenticationReply != reply) {
      reply->deleteLater();
      return;
    }
    _authenticationReply.clear();
    const auto body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
      const auto message = apiErrorFromPayload(body, reply->errorString(), tr("Steganos could not start native sign-in."));
      reply->deleteLater();
      failAuthenticationAttempt(message);
      return;
    }
    reply->deleteLater();
    const auto object = QJsonDocument::fromJson(body).object();
    const QUrl loginUrl(object.value(QStringLiteral("login_url")).toString());
    _nativePollToken = object.value(QStringLiteral("poll_token")).toString();
    const auto pollInterval = qBound<qint64>(qint64{1}, object.value(QStringLiteral("poll_interval")).toInteger(2), qint64{10});
    if (!loginUrl.isValid() || _nativePollToken.isEmpty()) {
      failAuthenticationAttempt(tr("Steganos returned an invalid native sign-in response."));
      return;
    }
    _authenticationPollTimer.start(static_cast<int>(pollInterval * 1000));
    if (!QDesktopServices::openUrl(loginUrl)) {
      failAuthenticationAttempt(tr("Could not open the Microsoft sign-in page in the system browser."));
      return;
    }
    QTimer::singleShot(250, this, &SteganosClient::pollNativeAuthentication);
  });
}

void SteganosClient::pollNativeAuthentication() {
  if (!_authenticationInProgress || _nativePollToken.isEmpty() || _authenticationPollReply) {
    return;
  }
  QJsonObject payload{ { QStringLiteral("poll_token"), _nativePollToken } };
  QNetworkRequest request(apiUrl(QStringLiteral("/api/v1/auth/native/poll")));
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setTransferTimeout(15 * 1000);
  auto* reply = _networkManager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
  _authenticationPollReply = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (_authenticationPollReply != reply) {
      reply->deleteLater();
      return;
    }
    _authenticationPollReply.clear();
    const auto body = reply->readAll();
    const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto object = QJsonDocument::fromJson(body).object();
    if (reply->error() == QNetworkReply::NoError && statusCode == 200
        && object.value(QStringLiteral("status")).toString() == QStringLiteral("ready")) {
      const auto code = object.value(QStringLiteral("code")).toString();
      const auto redirect = object.value(QStringLiteral("redirect_uri")).toString();
      reply->deleteLater();
      if (code.isEmpty() || redirect.isEmpty()) {
        failAuthenticationAttempt(tr("Steganos returned an incomplete native authorization result."));
        return;
      }
      _authenticationPollTimer.stop();
      _redirectUri = QUrl(redirect);
      _authenticationTimeoutTimer.start(30 * 1000);
      exchangeAuthorizationCode(code);
      return;
    }
    if (statusCode == 202 || statusCode == 429 || statusCode == 0) {
      reply->deleteLater();
      return;
    }
    const auto message = apiErrorFromPayload(body, reply->errorString(), tr("The native sign-in request expired or was rejected."));
    reply->deleteLater();
    failAuthenticationAttempt(message);
  });
}

void SteganosClient::cancelSignIn() {
  if (_authenticationInProgress) {
    failAuthenticationAttempt(tr("Microsoft sign-in was canceled."));
  }
}

void SteganosClient::handleLoopbackConnection() {
  if (!_loopbackServer) {
    return;
  }
  while (_loopbackServer->hasPendingConnections()) {
    auto* socket = _loopbackServer->nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { processLoopbackSocket(socket); });
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    // A browser can send the complete HTTP request before newConnection is delivered.
    // Process already-buffered bytes even if readyRead will not fire again.
    QTimer::singleShot(0, socket, [this, socket]() { processLoopbackSocket(socket); });
  }
}

void SteganosClient::processLoopbackSocket(QTcpSocket* socket) {
  if (!socket || socket->property("steganos_request_processed").toBool()) {
    return;
  }
  auto request = socket->property("steganos_request_buffer").toByteArray();
  request.append(socket->readAll());
  if (request.size() > 16 * 1024) {
    socket->setProperty("steganos_request_processed", true);
    writeLoopbackResponse(socket, false, tr("The local callback request was too large."));
    failAuthenticationAttempt(tr("The local browser callback was invalid."));
    return;
  }
  const auto lineEnd = request.indexOf("\r\n");
  if (lineEnd < 0) {
    socket->setProperty("steganos_request_buffer", request);
    return;
  }
  socket->setProperty("steganos_request_processed", true);
  socket->setProperty("steganos_request_buffer", {});
  const auto firstLine = request.left(lineEnd);
  const auto parts = firstLine.split(' ');
  if (parts.size() < 2 || parts.at(0) != "GET") {
    writeLoopbackResponse(socket, false, tr("The local callback request was invalid."));
    failAuthenticationAttempt(tr("The local browser callback was invalid."));
    return;
  }
  const auto callback = QUrl(QStringLiteral("http://127.0.0.1") + QString::fromUtf8(parts.at(1)));
  const QUrlQuery query(callback);
  const auto code = query.queryItemValue(QStringLiteral("code"));
  const auto error = query.queryItemValue(QStringLiteral("error"));
  if (!error.isEmpty() || code.isEmpty()) {
    const auto message = error.isEmpty() ? tr("Authorization code was missing.") : error;
    writeLoopbackResponse(socket, false, message);
    failAuthenticationAttempt(message);
    return;
  }
  writeLoopbackResponse(socket, true, tr("Sign-in complete. You can return to NeoGeoHub."));
  if (_loopbackServer) {
    _loopbackServer->close();
  }
  // The browser portion completed. Give the token exchange a bounded 30 seconds.
  _authenticationTimeoutTimer.start(30 * 1000);
  exchangeAuthorizationCode(code);
}

void SteganosClient::finishAuthenticationAttempt() {
  _authenticationTimeoutTimer.stop();
  _authenticationPollTimer.stop();
  if (_authenticationPollReply) {
    auto* reply = _authenticationPollReply.data();
    _authenticationPollReply.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  if (_authenticationReply) {
    auto* reply = _authenticationReply.data();
    _authenticationReply.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  if (_loopbackServer) {
    _loopbackServer->close();
  }
  _nativePollToken.clear();
  setAuthenticationInProgress(false);
}

void SteganosClient::failAuthenticationAttempt(const QString& message) {
  finishAuthenticationAttempt();
  emit authenticationFailed(message);
}

void SteganosClient::setAuthenticationInProgress(bool inProgress) {
  if (_authenticationInProgress == inProgress) {
    return;
  }
  _authenticationInProgress = inProgress;
  emit authenticationInProgressChanged(inProgress);
}

void SteganosClient::writeLoopbackResponse(QTcpSocket* socket, bool success, const QString& message) {
  const auto title = success ? tr("NeoGeoHub sign-in complete") : tr("NeoGeoHub sign-in failed");
  const auto color = success ? QStringLiteral("#50d890") : QStringLiteral("#ff6b78");
  const auto html = QStringLiteral(
    "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
    "<title>%1</title></head><body style=\"background:#090c12;color:#edf2fa;font-family:system-ui;padding:3rem\">"
    "<main style=\"max-width:42rem;margin:auto;background:#131925;border:1px solid #2a3549;border-radius:16px;padding:2rem\">"
    "<h1 style=\"color:%2\">%1</h1><p>%3</p></main></body></html>")
                      .arg(title.toHtmlEscaped(), color, message.toHtmlEscaped());
  const auto payload = html.toUtf8();
  socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: ");
  socket->write(QByteArray::number(payload.size()));
  socket->write("\r\n\r\n");
  socket->write(payload);
  socket->disconnectFromHost();
}

void SteganosClient::exchangeAuthorizationCode(const QString& code) {
  QJsonObject payload{
    { QStringLiteral("code"), code },
    { QStringLiteral("code_verifier"), _codeVerifier },
    { QStringLiteral("redirect_uri"), _redirectUri.toString(QUrl::FullyEncoded) },
  };
  QNetworkRequest request(apiUrl(QStringLiteral("/api/v1/auth/exchange")));
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setTransferTimeout(30 * 1000);
  auto* reply = _networkManager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
  _authenticationReply = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (_authenticationReply != reply) {
      reply->deleteLater();
      return;
    }
    _authenticationReply.clear();
    if (reply->error() != QNetworkReply::NoError) {
      const auto message = replyError(reply, tr("Steganos rejected the authorization code."));
      reply->deleteLater();
      clearTokens(false);
      failAuthenticationAttempt(message);
      emit authenticationChanged(false);
      return;
    }
    const auto payload = reply->readAll();
    reply->deleteLater();
    acceptTokenResponse(payload);
  });
}

void SteganosClient::acceptTokenResponse(const QByteArray& payload) {
  const auto document = QJsonDocument::fromJson(payload);
  if (!document.isObject()) {
    clearTokens(false, true);
    failAuthenticationAttempt(tr("Steganos returned an invalid token response."));
    emit authenticationChanged(false);
    return;
  }
  const auto object = document.object();
  const auto accessToken = object.value(QStringLiteral("access_token")).toString();
  const auto refreshToken = object.value(QStringLiteral("refresh_token")).toString();
  const auto expiresIn = object.value(QStringLiteral("expires_in")).toInteger();
  const auto user = object.value(QStringLiteral("user")).toObject();
  if (accessToken.isEmpty() || refreshToken.isEmpty() || expiresIn <= 0 || user.isEmpty()) {
    clearTokens(false, true);
    failAuthenticationAttempt(tr("Steganos returned an incomplete token response."));
    emit authenticationChanged(false);
    return;
  }

  const auto wasAuthenticated = isAuthenticated();
  _accessToken = accessToken;
  _refreshToken = refreshToken;
  _accessTokenExpiry = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
  _userId = user.value(QStringLiteral("id")).toString();
  _userEmail = user.value(QStringLiteral("email")).toString();
  _displayName = user.value(QStringLiteral("display_name")).toString();
  _userRoles.clear();
  for (const auto& role : user.value(QStringLiteral("roles")).toArray()) {
    _userRoles.append(role.toString());
  }
  if (_requestedRole == QStringLiteral("operator")) {
    _sessionRole = _userRoles.contains(QStringLiteral("admin")) ? QStringLiteral("admin") : QStringLiteral("operator");
  } else {
    _sessionRole = QStringLiteral("player");
  }
  _refreshInProgress = false;
  persistSession();
  scheduleRefresh(expiresIn);
  finishAuthenticationAttempt();
  emit authenticationSucceeded();
  if (!wasAuthenticated) {
    emit authenticationChanged(true);
  }
}

void SteganosClient::scheduleRefresh(qint64 expiresInSeconds) {
  const auto delay = qMax<qint64>(30, expiresInSeconds - 60);
  _refreshTimer.start(static_cast<int>(qMin<qint64>(delay * 1000, std::numeric_limits<int>::max())));
}

void SteganosClient::refreshAccessToken() {
  if (_refreshToken.isEmpty() || _refreshInProgress) {
    return;
  }
  _refreshInProgress = true;
  QJsonObject payload{ { QStringLiteral("refresh_token"), _refreshToken } };
  auto request = QNetworkRequest(apiUrl(QStringLiteral("/api/v1/auth/refresh")));
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setTransferTimeout(30 * 1000);
  auto* reply = _networkManager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (reply->error() != QNetworkReply::NoError) {
      const auto message = replyError(reply, tr("The Steganos session expired."));
      const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      reply->deleteLater();
      clearTokens(true, statusCode == 400 || statusCode == 401);
      finishAuthenticationAttempt();
      emit authenticationFailed(message);
      return;
    }
    const auto payload = reply->readAll();
    reply->deleteLater();
    acceptTokenResponse(payload);
  });
}

void SteganosClient::checkConnection() {
  if (_connectionReply) return;
  emit connectionCheckStarted();
  QNetworkRequest request(apiUrl(isAuthenticated() ? QStringLiteral("/api/v1/me") : QStringLiteral("/healthz")));
  request.setTransferTimeout(15 * 1000);
  authorize(request);
  auto* reply = _networkManager.get(request);
  _connectionReply = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (_connectionReply != reply) { reply->deleteLater(); return; }
    _connectionReply.clear();
    const auto body = reply->readAll();
    const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool connected = reply->error() == QNetworkReply::NoError && statusCode >= 200 && statusCode < 300;
    const auto message = connected ? tr("Connected to Steganos.")
      : apiErrorFromPayload(body, reply->errorString(), tr("Could not reach Steganos."));
    reply->deleteLater();
    emit connectionCheckFinished(connected, message);
    if (!connected && isAuthenticated() && statusCode == 401) refreshAccessToken();
  });
}

void SteganosClient::signOut() {
  finishAuthenticationAttempt();
  if (isAuthenticated()) {
    QJsonObject payload{ { QStringLiteral("refresh_token"), _refreshToken } };
    auto* reply = postJson(QStringLiteral("/api/v1/auth/logout"), payload);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
  }
  clearTokens(true, true);
  emit signedOut();
}

void SteganosClient::clearTokens(bool emitSignals, bool removePersisted) {
  const auto wasAuthenticated = isAuthenticated();
  _refreshTimer.stop();
  _accessToken.clear();
  _refreshToken.clear();
  _accessTokenExpiry = {};
  _sessionRole.clear();
  _userId.clear();
  _userEmail.clear();
  _displayName.clear();
  _userRoles.clear();
  _refreshInProgress = false;
  if (removePersisted) deletePersistedSession();
  if (emitSignals && wasAuthenticated) emit authenticationChanged(false);
}

QString SteganosClient::sessionFilePath() const {
  const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return QDir(directory).absoluteFilePath(QStringLiteral("psxrecomp-ci-session.json"));
}

void SteganosClient::persistSession() const {
  if (_refreshToken.isEmpty() || !_baseUrl.isValid()) return;
  const auto path = sessionFilePath();
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  const QJsonObject session{
    { QStringLiteral("base_url"), _baseUrl.toString(QUrl::FullyEncoded) },
    { QStringLiteral("refresh_token"), _refreshToken },
    { QStringLiteral("role"), _requestedRole },
  };
  file.write(QJsonDocument(session).toJson(QJsonDocument::Compact));
  if (file.commit()) QFile(path).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void SteganosClient::restorePersistedSession() {
  if (!_baseUrl.isValid()) return;
  QFile file(sessionFilePath());
  if (!file.open(QIODevice::ReadOnly)) return;
  const auto object = QJsonDocument::fromJson(file.readAll()).object();
  const QUrl storedUrl(object.value(QStringLiteral("base_url")).toString());
  const auto refreshToken = object.value(QStringLiteral("refresh_token")).toString();
  const auto role = object.value(QStringLiteral("role")).toString();
  if (storedUrl.adjusted(QUrl::StripTrailingSlash) != _baseUrl || refreshToken.isEmpty()
      || (role != QStringLiteral("player") && role != QStringLiteral("operator"))) return;
  _refreshToken = refreshToken;
  _requestedRole = role;
  setAuthenticationInProgress(true);
  _authenticationTimeoutTimer.start(30 * 1000);
  refreshAccessToken();
}

void SteganosClient::deletePersistedSession() const {
  QFile::remove(sessionFilePath());
}

void SteganosClient::authorize(QNetworkRequest& request) const {
  if (!_accessToken.isEmpty()) {
    request.setRawHeader("Authorization", "Bearer " + _accessToken.toUtf8());
  }
  request.setRawHeader("Accept", "application/json");
}

QNetworkReply* SteganosClient::get(const QString& path, int transferTimeoutMilliseconds) {
  QNetworkRequest request(apiUrl(path));
  if (transferTimeoutMilliseconds > 0) request.setTransferTimeout(transferTimeoutMilliseconds);
  authorize(request);
  return _networkManager.get(request);
}

QNetworkReply* SteganosClient::getUrl(const QUrl& url) {
  QNetworkRequest request(url);
  authorize(request);
  return _networkManager.get(request);
}

QNetworkReply* SteganosClient::postJson(const QString& path, const QJsonObject& payload) {
  QNetworkRequest request(apiUrl(path));
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  authorize(request);
  return _networkManager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

QNetworkReply* SteganosClient::postMultipart(const QString& path, QHttpMultiPart* multipart) {
  QNetworkRequest request(apiUrl(path));
  authorize(request);
  auto* reply = _networkManager.post(request, multipart);
  multipart->setParent(reply);
  return reply;
}

QByteArray SteganosClient::randomBytes(int size) const {
  QByteArray bytes(size, '\0');
  auto* generator = QRandomGenerator::system();
  for (int i = 0; i < size; ++i) {
    bytes[i] = static_cast<char>(generator->generate() & 0xff);
  }
  return bytes;
}

QString SteganosClient::base64Url(const QByteArray& bytes) const {
  return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace psxstudio::ci
