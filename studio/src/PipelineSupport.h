#pragma once

#include "PipelineTypes.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QSet>

#include <functional>

namespace psxstudio {

QString sha256File(const QString& path, QString& error);
/* Stable QSettings-safe identity for one catalogued disc across rescans/runs. */
QString batchEntrySettingsId(const QString& sourcePath);
/* Parse the major version from `java --version` / `java -version` output. */
int javaMajorVersion(const QString& versionOutput);
QString findJava21Home();
QString sanitizedFileStem(const QString& value);
QString sanitizedBundleIdentifier(const QString& serial, const QString& title);
/* Human-readable package / .app / exe stem from a window title.
 * Keeps spaces and letters so Finder names stay readable, but strips
 * shell- and filesystem-hostile characters (notably apostrophes) that
 * break CMake/Ninja /bin/sh -c rules on macOS paths. */
QString cleanBundleName(const QString& title);
/* Final file/directory name selected for one concrete platform export. */
QString exportOutputName(const PipelineRequest& request);
/* Root manifest embedded beside the launch item in every package ZIP. */
QJsonObject gameManifestForRequest(const PipelineRequest& request);
/* Create a parenthesis-free symlink name for Apple binary-inspection tools.
 * otool-classic interprets a trailing `(name)` in a Mach-O path as archive
 * member syntax, even when the path is passed as one process argument. */
QString createMacosInspectionAlias(const QString& sourcePath,
                                   const QString& aliasDirectory,
                                   QString& error);
QString cmakeQuoted(const QString& value);
QString tomlQuoted(const QString& value);
QString hex32(quint32 value);
QString runtimeBootToml(bool skipBiosBoot);
/* Emit the automatic per-game controller policy. The runtime presents a real
 * D-Pad first and promotes the port to Hybrid only when the game's SIO
 * negotiation demonstrates that the D-Pad was not accepted. */
QString defaultControllerToml();
QString macosGipCmakeOption(bool enabled);
bool nmOutputHasMacosGipBackend(const QByteArray& output);
bool writeBytes(const QString& path, const QByteArray& bytes, QString& error);
bool writeText(const QString& path, const QString& text, QString& error);
bool writeJson(const QString& path, const QJsonObject& object, QString& error);
bool copyFileReplacing(const QString& source, const QString& destination, QString& error);
bool createMacosIconset(const QString& sourceIcon,
                        const QString& iconsetPath,
                        QString& error);
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
/* Stream and verify a package ZIP. An empty rootDirectoryName places the
 * source directory's contents at ZIP root; a non-empty name places that
 * directory itself at ZIP root under the supplied name. */
bool createPackageArchive(
  const QString& sourceDirectory,
  const QString& rootDirectoryName,
  const QString& outputPath,
  QString& error,
  const QMap<QString, QByteArray>& rootFiles = {},
  const std::function<bool()>& cancellationRequested = {});
QList<QPair<quint32, quint32>> parseCodeRanges(const QString& path, QString& error);
QSet<quint32> parseFunctionEntries(const QString& path, QString& error);
bool addressInRanges(quint32 address, const QList<QPair<quint32, quint32>>& ranges);
QString findExecutable(const QString& name);
QString ghidraAnalyzeHeadlessPath(const QString& ghidraHome);
/* Portable generated app CMake used by source exports, local builds, and CI. */
QString generatedProjectCMake(const PipelineRequest& request,
                              const GameDescription& game,
                              const QString& bundleName,
                              const QString& bundleId,
                              quint32 expectedBiosCrc);

} // namespace psxstudio
