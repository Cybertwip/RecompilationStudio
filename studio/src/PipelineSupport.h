#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QSet>

namespace psxstudio {

QString sha256File(const QString& path, QString& error);
QString sanitizedFileStem(const QString& value);
QString sanitizedBundleIdentifier(const QString& serial, const QString& title);
/* Human-readable package / .app / exe stem from a window title.
 * Keeps spaces and letters so Finder names stay readable, but strips
 * shell- and filesystem-hostile characters (notably apostrophes) that
 * break CMake/Ninja /bin/sh -c rules on macOS paths. */
QString cleanBundleName(const QString& title);
QString cmakeQuoted(const QString& value);
QString tomlQuoted(const QString& value);
QString hex32(quint32 value);
QString runtimeBootToml(bool skipBiosBoot);
/* Emit [controller] defaults for a packaged game. padMode is one of
 * "hybrid", "analog", or "digital" (D-Pad). The pad type is locked so the
 * in-game settings UI cannot change it after packaging. */
QString defaultControllerToml(const QString& padMode = QStringLiteral("digital"));
QString macosGipCmakeOption(bool enabled);
bool nmOutputHasMacosGipBackend(const QByteArray& output);
bool writeBytes(const QString& path, const QByteArray& bytes, QString& error);
bool writeText(const QString& path, const QString& text, QString& error);
bool writeJson(const QString& path, const QJsonObject& object, QString& error);
bool copyFileReplacing(const QString& source, const QString& destination, QString& error);
bool createIcns(const QString& sourceIcon,
                const QString& workingDirectory,
                const QString& outputPath,
                QString& error);
bool createIco(const QString& sourceIcon,
               const QString& outputPath,
               QString& error);
bool createPngIcon(const QString& sourceIcon,
                   const QString& outputPath,
                   QString& error);
bool copyDirectoryTree(const QString& sourceDirectory,
                       const QString& destinationDirectory,
                       QString& error);
bool createProofArchive(const QString& proofDirectory,
                        const QString& outputPath,
                        QString& error);
QList<QPair<quint32, quint32>> parseCodeRanges(const QString& path, QString& error);
QSet<quint32> parseFunctionEntries(const QString& path, QString& error);
bool addressInRanges(quint32 address, const QList<QPair<quint32, quint32>>& ranges);
QString findExecutable(const QString& name);
QString ghidraAnalyzeHeadlessPath(const QString& ghidraHome);

} // namespace psxstudio
