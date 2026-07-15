#include "MainWindow.h"
#include "PipelineTypes.h"

#include <oclero/QtAppInstanceManager.hpp>
#include <oclero/qlementine.hpp>
#include <oclero/qlementine/icons/QlementineIcons.hpp>

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
  QGuiApplication::setApplicationName(QStringLiteral("PSXRecompStudio"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("PSXRecomp Studio"));
  QGuiApplication::setOrganizationName(QStringLiteral("PSXRecomp"));
  QGuiApplication::setOrganizationDomain(QStringLiteral("psxrecomp.org"));
  QGuiApplication::setApplicationVersion(QString::fromUtf8(PSX_STUDIO_VERSION));
  QGuiApplication::setDesktopFileName(QStringLiteral("org.psxrecomp.studio"));

  QApplication app(argc, argv);
  qRegisterMetaType<psxstudio::PipelineRequest>("psxstudio::PipelineRequest");

  auto* style = new oclero::qlementine::QlementineStyle(&app);
  style->setAnimationsEnabled(true);
  style->setAutoIconColor(oclero::qlementine::AutoIconColor::TextColor);
  style->setIconPathGetter(oclero::qlementine::icons::fromFreeDesktop);
  QApplication::setStyle(style);
  oclero::qlementine::icons::initializeIconTheme();
  QIcon::setThemeName(QStringLiteral("qlementine"));

  oclero::QtAppInstanceManager instanceManager;
  instanceManager.setMode(oclero::QtAppInstanceManager::Mode::SingleInstance);

  psxstudio::MainWindow window;
  window.show();
  QObject::connect(&instanceManager,
                   &oclero::QtAppInstanceManager::secondaryInstanceMessageReceived,
                   &window,
                   [&window](unsigned int, const QByteArray&) {
                     window.showNormal();
                     window.raise();
                     window.activateWindow();
                   });

  return app.exec();
}
