#pragma once

#include "PipelineTypes.h"

#include <QObject>

#include <atomic>

class QProcess;
class QProcessEnvironment;

namespace psxstudio {

class PipelineWorker final : public QObject {
  Q_OBJECT

public:
  explicit PipelineWorker(QObject* parent = nullptr);

  void requestCancel();

public slots:
  void run(psxstudio::PipelineRequest request);

signals:
  void stageChanged(const QString& name, int index, int total);
  void logLine(const QString& line);
  void completed(const QString& appPath);
  void ciSourcePrepared(psxstudio::PipelineRequest request,
                        const QString& repositoryPath,
                        const QString& commit);
  void failed(const QString& message, const QString& workspacePath);
  void cancelled();

private:
  bool runCommand(const QString& program,
                  const QStringList& arguments,
                  const QString& workingDirectory,
                  const QString& displayCommand,
                  int timeoutMs,
                  QByteArray* capturedOutput = nullptr,
                  const QProcessEnvironment* environmentOverride = nullptr);
  void emitProcessOutput(const QByteArray& bytes, QByteArray& pending);
  bool cancellationRequested() const;

  std::atomic_bool cancelRequested_{ false };
};

} // namespace psxstudio
