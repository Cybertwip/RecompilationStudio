#include "MainWindow.h"

#include "PipelineSupport.h"
#include "PipelineWorker.h"

#include <QApplication>
#include <QColor>
#include <QCheckBox>
#include <QComboBox>

#include <oclero/qlementine.hpp>
#include <oclero/qlementine/icons/Icons16.hpp>
#include <oclero/qlementine/style/ThemeManager.hpp>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QResizeEvent>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace psxstudio {

namespace {

QFrame* makeCard(const QString& objectName, QWidget* parent) {
  auto* frame = new QFrame(parent);
  frame->setFrameShape(QFrame::StyledPanel);
  frame->setObjectName(objectName);
  frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  return frame;
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  auto font = label->font();
  font.setBold(true);
  font.setPointSizeF(font.pointSizeF() + 1.0);
  label->setFont(font);
  return label;
}

bool verifyOutputDirectoryAccess(const QString& path, QString& error) {
  const QFileInfo directoryInfo(path);
  if (!directoryInfo.exists() || !directoryInfo.isDir()) {
    error = QStringLiteral("The output path is not an existing directory: %1").arg(path);
    return false;
  }

  /* Exercise the operations used by delivery rather than trusting permission
   * bits alone. On macOS this also forces any protected-folder denial to be
   * reported while the user is still in the UI, before the build starts. */
  QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".psxrecomp-access-XXXXXX")));
  probe.setAutoRemove(false);
  if (!probe.open()) {
    error = QStringLiteral("Could not create a file in %1: %2")
              .arg(path, probe.errorString());
    return false;
  }
  probe.close();

  const QString renamedPath = probe.fileName() + QStringLiteral(".moved");
  if (!probe.rename(renamedPath)) {
    const QString reason = probe.errorString();
    probe.remove();
    error = QStringLiteral("Could not rename an item in %1: %2").arg(path, reason);
    return false;
  }
  if (!probe.remove()) {
    error = QStringLiteral("Could not remove a temporary item from %1: %2")
              .arg(path, probe.errorString());
    return false;
  }
  return true;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
: QMainWindow(parent) {
  setWindowTitle(QStringLiteral("PSXRecomp Studio"));
  resize(1060, 760);
  setMinimumSize(720, 600);

  auto* qlementine = qobject_cast<oclero::qlementine::QlementineStyle*>(qApp->style());
  themeManager_ = new oclero::qlementine::ThemeManager(qlementine, this);
  themeManager_->loadDirectory(QStringLiteral(":/psxrecomp/studio/resources/themes"));
  QSettings initialSettings;
  themeManager_->setCurrentTheme(initialSettings.value(QStringLiteral("app/theme"),
                                                        QStringLiteral("Dark")).toString());
  if (themeManager_->currentTheme().isEmpty()) {
    themeManager_->setCurrentTheme(QStringLiteral("Dark"));
  }

  auto themedIcon = [qlementine](oclero::qlementine::icons::Icons16 icon) {
    return qlementine
      ? qlementine->makeThemedIcon(
          QString::fromLatin1(oclero::qlementine::icons::iconPath(icon)), QSize(16, 16))
      : QIcon();
  };
  setWindowIcon(themedIcon(oclero::qlementine::icons::Icons16::Action_Build));

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(22, 18, 22, 18);
  root->setSpacing(14);

  auto* header = new QWidget(central);
  auto* headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(0, 0, 0, 0);
  headerLayout->setSpacing(18);
  auto* headerText = new QWidget(header);
  auto* headerTextLayout = new QVBoxLayout(headerText);
  headerTextLayout->setContentsMargins(0, 0, 0, 0);
  headerTextLayout->setSpacing(4);
  auto* title = new QLabel(QStringLiteral("Build a native PlayStation app"), headerText);
  auto titleFont = title->font();
  titleFont.setBold(true);
  titleFont.setPointSizeF(titleFont.pointSizeF() + 7.0);
  title->setFont(titleFont);
  auto* subtitle = new QLabel(
    QStringLiteral("One workflow for disc analysis, evidence-backed source generation, native compilation, "
                   "and platform packaging."),
    headerText);
  subtitle->setWordWrap(true);
  headerTextLayout->addWidget(title);
  headerTextLayout->addWidget(subtitle);
  themeButton_ = new QPushButton(header);
  themeButton_->setMinimumWidth(104);
  themeButton_->setToolTip(QStringLiteral("Switch between dark and light themes"));
  headerLayout->addWidget(headerText, 1);
  headerLayout->addWidget(themeButton_, 0, Qt::AlignTop);
  root->addWidget(header);

  formsContainer_ = new QWidget(central);
  formsLayout_ = new QGridLayout(formsContainer_);
  formsLayout_->setContentsMargins(0, 0, 0, 0);
  formsLayout_->setHorizontalSpacing(14);
  formsLayout_->setVerticalSpacing(14);

  inputCard_ = makeCard(QStringLiteral("inputCard"), formsContainer_);
  auto* inputLayout = new QVBoxLayout(inputCard_);
  inputLayout->setContentsMargins(18, 16, 18, 18);
  inputLayout->setSpacing(10);
  inputLayout->addWidget(makeSectionTitle(QStringLiteral("App inputs"), inputCard_));

  auto* platformRow = new QWidget(inputCard_);
  platformRow->setMinimumHeight(34);
  auto* platformLayout = new QHBoxLayout(platformRow);
  platformLayout->setContentsMargins(0, 0, 0, 0);
  auto* platformLabel = new QLabel(QStringLiteral("Platform"), platformRow);
  platformLabel->setMinimumWidth(142);
  platformCombo_ = new QComboBox(platformRow);
#if defined(Q_OS_MACOS)
  platformCombo_->addItem(QStringLiteral("macOS"), targetPlatformKey(TargetPlatform::MacOS));
  platformCombo_->addItem(QStringLiteral("Windows"), targetPlatformKey(TargetPlatform::Windows));
  platformCombo_->addItem(QStringLiteral("Linux"), targetPlatformKey(TargetPlatform::Linux));
#else
  const auto hostPlatform = hostTargetPlatform();
  platformCombo_->addItem(targetPlatformDisplayName(hostPlatform),
                          targetPlatformKey(hostPlatform));
#endif
  platformCombo_->setCurrentIndex(0);
  platformLayout->addWidget(platformLabel);
  platformLayout->addWidget(platformCombo_, 1);
  inputLayout->addWidget(platformRow);
  platformRow->setVisible(hostCanSelectTargetPlatform());

  discEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("Disc BIN/CUE"),
                         QStringLiteral("One .cue and all referenced .bin files"), SLOT(chooseDisc()));
  biosEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("PlayStation BIOS"),
                         QStringLiteral("Canonical SCPH1001.BIN only"), SLOT(chooseBios()));
  iconEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("App icon"),
                         QStringLiteral("PNG, SVG, or ICNS"), SLOT(chooseIcon()));

  skipBiosBoot_ = new QCheckBox(
    QStringLiteral("Skip BIOS intro and boot directly to the game"), inputCard_);
  skipBiosBoot_->setToolTip(
    QStringLiteral("Keeps the recompiled BIOS linked, but skips its visible shell/intro and proceeds directly to disc boot."));
  inputLayout->addWidget(skipBiosBoot_);

  auto* padModeRow = new QWidget(inputCard_);
  padModeRow->setMinimumHeight(34);
  auto* padModeLayout = new QHBoxLayout(padModeRow);
  padModeLayout->setContentsMargins(0, 0, 0, 0);
  auto* padModeLabel = new QLabel(QStringLiteral("Controller pad type"), padModeRow);
  padModeLabel->setMinimumWidth(142);
  padModeCombo_ = new QComboBox(padModeRow);
  padModeCombo_->addItem(QStringLiteral("Hybrid (auto digital / analog)"),
                         QStringLiteral("hybrid"));
  padModeCombo_->addItem(QStringLiteral("Analog (DualShock)"),
                         QStringLiteral("analog"));
  padModeCombo_->addItem(QStringLiteral("D-Pad (digital)"),
                         QStringLiteral("digital"));
  padModeCombo_->setCurrentIndex(0);
  padModeCombo_->setToolTip(
    QStringLiteral("Fixed pad type baked into the packaged game. The in-game "
                   "settings menu will not offer Hybrid / Analog / D-Pad switching."));
  padModeLayout->addWidget(padModeLabel);
  padModeLayout->addWidget(padModeCombo_, 1);
  inputLayout->addWidget(padModeRow);

  macosGipGamepad_ = new QCheckBox(
    QStringLiteral("Enable wired Xbox/PDP controllers on macOS"), inputCard_);
  macosGipGamepad_->setChecked(true);
  macosGipGamepad_->setToolTip(
    QStringLiteral("Builds the native libusb Xbox GIP backend for wired PDP, Microsoft Xbox One/Series, and PowerA controllers that SDL cannot expose on macOS."));
  inputLayout->addWidget(macosGipGamepad_);

  auto* titleRow = new QWidget(inputCard_);
  titleRow->setMinimumHeight(34);
  auto* titleLayout = new QHBoxLayout(titleRow);
  titleLayout->setContentsMargins(0, 0, 0, 0);
  auto* titleLabel = new QLabel(QStringLiteral("Window title"), titleRow);
  titleLabel->setMinimumWidth(142);
  titleEdit_ = new QLineEdit(titleRow);
  titleEdit_->setPlaceholderText(QStringLiteral("Example: Evil Zone Recompiled"));
  titleLayout->addWidget(titleLabel);
  titleLayout->addWidget(titleEdit_, 1);
  inputLayout->addWidget(titleRow);

  outputEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("Output directory"),
                           QStringLiteral("Destination for the signed .app"), SLOT(chooseOutputDirectory()));
  inputLayout->addStretch(1);

  toolsCard_ = makeCard(QStringLiteral("toolsCard"), formsContainer_);
  auto* toolsLayout = new QVBoxLayout(toolsCard_);
  toolsLayout->setContentsMargins(18, 16, 18, 18);
  toolsLayout->setSpacing(10);
  toolsLayout->addWidget(makeSectionTitle(QStringLiteral("Analysis and export tools"), toolsCard_));
  ghidraEdit_ = addPathRow(toolsCard_, toolsLayout, QStringLiteral("Ghidra home"),
                           QStringLiteral("Ghidra 11.3.2 installation"), SLOT(chooseGhidraHome()));
  certificateEdit_ = addPathRow(toolsCard_, toolsLayout, QStringLiteral("Certificate"),
                                QStringLiteral("Password-protected .pfx or .p12"), SLOT(chooseCertificate()));

  auto* passwordRow = new QWidget(toolsCard_);
  passwordRow->setMinimumHeight(34);
  auto* passwordLayout = new QHBoxLayout(passwordRow);
  passwordLayout->setContentsMargins(0, 0, 0, 0);
  auto* passwordLabel = new QLabel(QStringLiteral("PFX password"), passwordRow);
  passwordLabel->setMinimumWidth(142);
  certificatePasswordEdit_ = new QLineEdit(passwordRow);
  certificatePasswordEdit_->setEchoMode(QLineEdit::Password);
  certificatePasswordEdit_->setPlaceholderText(QStringLiteral("Never persisted; used only for signing"));
  passwordLayout->addWidget(passwordLabel);
  passwordLayout->addWidget(certificatePasswordEdit_, 1);
  toolsLayout->addWidget(passwordRow);
  signingNote_ = new QLabel(
    QStringLiteral("The certificate is imported into an isolated temporary keychain and removed after verification."),
    toolsCard_);
  signingNote_->setWordWrap(true);
  signingNote_->setObjectName(QStringLiteral("secondaryText"));
  toolsLayout->addWidget(signingNote_);
  toolsLayout->addStretch(1);

  brandingCard_ = makeCard(QStringLiteral("brandingCard"), formsContainer_);
  auto* brandingLayout = new QVBoxLayout(brandingCard_);
  brandingLayout->setContentsMargins(18, 16, 18, 18);
  brandingLayout->setSpacing(10);
  biosPatchEnabled_ = new QCheckBox(QStringLiteral("Patch BIOS branding for this app"), brandingCard_);
  auto brandingFont = biosPatchEnabled_->font();
  brandingFont.setBold(true);
  biosPatchEnabled_->setFont(brandingFont);
  brandingLayout->addWidget(biosPatchEnabled_);
  auto* brandingDescription = new QLabel(
    QStringLiteral("Replace the initial SONY splash and PlayStation handoff wordmark in the bundled BIOS. "
                   "The canonical source BIOS is never modified."), brandingCard_);
  brandingDescription->setWordWrap(true);
  brandingDescription->setObjectName(QStringLiteral("secondaryText"));
  brandingLayout->addWidget(brandingDescription);
  biosInitialSplashEdit_ = addPathRow(brandingCard_, brandingLayout,
    QStringLiteral("Initial splash"), QStringLiteral("Microsoft-style first boot image"),
    SLOT(chooseBiosInitialSplash()));
  biosHandoffImageEdit_ = addPathRow(brandingCard_, brandingLayout,
    QStringLiteral("Handoff image"), QStringLiteral("Image shown before game handoff"),
    SLOT(chooseBiosHandoffImage()));
  auto* brandingOptions = new QHBoxLayout();
  biosMuteAudio_ = new QCheckBox(QStringLiteral("Mute BIOS boot audio"), brandingCard_);
  biosMuteAudio_->setChecked(true);
  biosRemovePsGlyph_ = new QCheckBox(QStringLiteral("Remove stock PS glyph"), brandingCard_);
  biosRemovePsGlyph_->setChecked(true);
  brandingOptions->addWidget(biosMuteAudio_);
  brandingOptions->addWidget(biosRemovePsGlyph_);
  brandingOptions->addStretch(1);
  brandingLayout->addLayout(brandingOptions);

  formsScroll_ = new QScrollArea(central);
  formsScroll_->setFrameShape(QFrame::NoFrame);
  formsScroll_->setWidgetResizable(true);
  formsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  formsScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  formsScroll_->setMinimumHeight(250);
  formsScroll_->setMaximumHeight(390);
  formsScroll_->setWidget(formsContainer_);
  root->addWidget(formsScroll_);

  statusCard_ = makeCard(QStringLiteral("statusCard"), central);
  auto* statusLayout = new QVBoxLayout(statusCard_);
  statusLayout->setContentsMargins(18, 14, 18, 14);
  statusLayout->setSpacing(9);
  auto* statusHeader = new QHBoxLayout();
  stageLabel_ = new QLabel(QStringLiteral("Ready"), statusCard_);
  auto* statusHint = new QLabel(QStringLiteral("Proof artifacts are embedded in the generated app"), statusCard_);
  statusHint->setObjectName(QStringLiteral("secondaryText"));
  statusHeader->addWidget(stageLabel_);
  statusHeader->addStretch(1);
  statusHeader->addWidget(statusHint);
  progressBar_ = new QProgressBar(statusCard_);
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  progressBar_->setTextVisible(true);
  logView_ = new QPlainTextEdit(statusCard_);
  logView_->setReadOnly(true);
  logView_->setPlaceholderText(QStringLiteral("Analysis and build output appears here."));
  logView_->setMaximumBlockCount(20000);
  logView_->setMinimumHeight(150);
  logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  statusLayout->addLayout(statusHeader);
  statusLayout->addWidget(progressBar_);
  statusLayout->addWidget(logView_, 1);
  root->addWidget(statusCard_, 1);

  auto* actions = new QHBoxLayout();
  revealButton_ = new QPushButton(themedIcon(oclero::qlementine::icons::Icons16::File_FolderOpen),
                                  QStringLiteral("Reveal Output"), central);
  revealButton_->setEnabled(false);
  cancelButton_ = new QPushButton(QStringLiteral("Cancel"), central);
  cancelButton_->setEnabled(false);
  buildButton_ = new QPushButton(themedIcon(oclero::qlementine::icons::Icons16::Action_Build),
                                 QStringLiteral("Build Signed .app"), central);
  buildButton_->setDefault(true);
  buildButton_->setMinimumWidth(164);
  actions->addWidget(revealButton_);
  actions->addStretch(1);
  actions->addWidget(cancelButton_);
  actions->addWidget(buildButton_);
  root->addLayout(actions);

  setCentralWidget(central);
  reflowForms();

  workerThread_ = new QThread(this);
  worker_ = new PipelineWorker();
  worker_->moveToThread(workerThread_);
  connect(workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(this, &MainWindow::runRequested, worker_, &PipelineWorker::run, Qt::QueuedConnection);
  connect(worker_, &PipelineWorker::stageChanged, this,
          [this](const QString& name, int index, int total) {
            stageLabel_->setText(QStringLiteral("%1  ·  Step %2 of %3").arg(name).arg(index).arg(total));
          });
  connect(worker_, &PipelineWorker::logLine, this, [this](const QString& line) {
    logView_->appendPlainText(line);
  });
  connect(worker_, &PipelineWorker::completed, this, &MainWindow::onCompleted);
  connect(worker_, &PipelineWorker::failed, this, &MainWindow::onFailed);
  connect(worker_, &PipelineWorker::cancelled, this, [this]() {
    stageLabel_->setText(QStringLiteral("Cancelled"));
    logView_->appendPlainText(QStringLiteral("Build cancelled."));
    setBusy(false);
  });
  workerThread_->start();

  connect(buildButton_, &QPushButton::clicked, this, &MainWindow::startBuild);
  connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelBuild);
  connect(revealButton_, &QPushButton::clicked, this, &MainWindow::revealOutput);
  connect(themeButton_, &QPushButton::clicked, this, &MainWindow::toggleTheme);
  connect(platformCombo_, &QComboBox::currentIndexChanged,
          this, &MainWindow::updatePlatformControls);
  connect(biosPatchEnabled_, &QCheckBox::toggled, this, &MainWindow::updateBiosPatchControls);
  connect(skipBiosBoot_, &QCheckBox::toggled, this, &MainWindow::updateBuildButton);
  connect(macosGipGamepad_, &QCheckBox::toggled, this, &MainWindow::updateBuildButton);
  connect(themeManager_, &oclero::qlementine::ThemeManager::currentThemeChanged,
          this, [this]() {
            QSettings().setValue(QStringLiteral("app/theme"), themeManager_->currentTheme());
            applyTheme();
          });
  for (auto* edit : { discEdit_, biosEdit_, iconEdit_, titleEdit_, outputEdit_,
                      certificateEdit_, certificatePasswordEdit_, ghidraEdit_,
                      biosInitialSplashEdit_, biosHandoffImageEdit_ }) {
    connect(edit, &QLineEdit::textChanged, this, &MainWindow::updateBuildButton);
  }

  loadSettings();
  updatePlatformControls();
  updateBiosPatchControls();
  applyTheme();
  updateBuildButton();
}

MainWindow::~MainWindow() {
  saveSettings();
  if (worker_) {
    worker_->requestCancel();
  }
  workerThread_->quit();
  workerThread_->wait(5000);
}

QLineEdit* MainWindow::addPathRow(QWidget* parent,
                                  QVBoxLayout* layout,
                                  const QString& label,
                                  const QString& placeholder,
                                  const char* slot) {
  auto* row = new QWidget(parent);
  row->setMinimumHeight(34);
  auto* rowLayout = new QHBoxLayout(row);
  rowLayout->setContentsMargins(0, 0, 0, 0);
  auto* fieldLabel = new QLabel(label, row);
  fieldLabel->setMinimumWidth(142);
  auto* edit = new QLineEdit(row);
  edit->setReadOnly(true);
  edit->setPlaceholderText(placeholder);
  auto* browse = new QPushButton(QStringLiteral("Browse…"), row);
  if (auto* style = qobject_cast<oclero::qlementine::QlementineStyle*>(qApp->style())) {
    browse->setIcon(style->makeThemedIcon(QString::fromLatin1(
      oclero::qlementine::icons::iconPath(oclero::qlementine::icons::Icons16::File_FolderOpen))));
  }
  connect(browse, SIGNAL(clicked()), this, slot);
  rowLayout->addWidget(fieldLabel);
  rowLayout->addWidget(edit, 1);
  rowLayout->addWidget(browse);
  layout->addWidget(row);
  return edit;
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  reflowForms();
}

void MainWindow::reflowForms() {
  if (!formsLayout_ || !inputCard_ || !toolsCard_ || !brandingCard_ || !formsContainer_) {
    return;
  }
  const bool columns = width() >= 1040;
  if (formsLayout_->count() > 0 && columns == formsAreColumns_) {
    return;
  }

  formsLayout_->removeWidget(inputCard_);
  formsLayout_->removeWidget(toolsCard_);
  formsLayout_->removeWidget(brandingCard_);
  if (columns) {
    formsLayout_->addWidget(inputCard_, 0, 0);
    formsLayout_->addWidget(toolsCard_, 0, 1);
    formsLayout_->addWidget(brandingCard_, 1, 0, 1, 2);
    formsLayout_->setColumnStretch(0, 1);
    formsLayout_->setColumnStretch(1, 1);
    formsContainer_->setMinimumHeight(std::max(inputCard_->sizeHint().height(),
                                               toolsCard_->sizeHint().height()) +
                                      brandingCard_->sizeHint().height() + 14);
    formsScroll_->setMaximumHeight(455);
  } else {
    formsLayout_->addWidget(inputCard_, 0, 0);
    formsLayout_->addWidget(toolsCard_, 1, 0);
    formsLayout_->addWidget(brandingCard_, 2, 0);
    formsLayout_->setColumnStretch(0, 1);
    formsLayout_->setColumnStretch(1, 0);
    formsContainer_->setMinimumHeight(inputCard_->sizeHint().height() +
                                      toolsCard_->sizeHint().height() +
                                      brandingCard_->sizeHint().height() + 28);
    formsScroll_->setMaximumHeight(390);
  }
  formsAreColumns_ = columns;
  formsContainer_->updateGeometry();
}

void MainWindow::toggleTheme() {
  if (!themeManager_) {
    return;
  }
  themeManager_->setCurrentTheme(
    themeManager_->currentTheme() == QStringLiteral("Dark")
      ? QStringLiteral("Light")
      : QStringLiteral("Dark"));
}

void MainWindow::applyTheme() {
  auto* style = qobject_cast<oclero::qlementine::QlementineStyle*>(qApp->style());
  if (!style || !themeManager_) {
    return;
  }
  const auto& theme = style->theme();
  const QString cardStyle = QStringLiteral(
    "QFrame#%1 { background-color: %2; border: 1px solid %3; border-radius: 10px; }");
  inputCard_->setStyleSheet(cardStyle.arg(QStringLiteral("inputCard"),
                                           theme.backgroundColorMain2.name(QColor::HexArgb),
                                           theme.borderColor.name(QColor::HexArgb)));
  toolsCard_->setStyleSheet(cardStyle.arg(QStringLiteral("toolsCard"),
                                           theme.backgroundColorMain2.name(QColor::HexArgb),
                                           theme.borderColor.name(QColor::HexArgb)));
  brandingCard_->setStyleSheet(cardStyle.arg(QStringLiteral("brandingCard"),
                                              theme.backgroundColorMain2.name(QColor::HexArgb),
                                              theme.borderColor.name(QColor::HexArgb)));
  statusCard_->setStyleSheet(cardStyle.arg(QStringLiteral("statusCard"),
                                            theme.backgroundColorMain2.name(QColor::HexArgb),
                                            theme.borderColor.name(QColor::HexArgb)));
  logView_->setStyleSheet(QStringLiteral(
    "QPlainTextEdit { background-color: %1; border: 1px solid %2; border-radius: 7px; padding: 7px; }")
    .arg(theme.backgroundColorWorkspace.name(QColor::HexArgb),
         theme.borderColor.name(QColor::HexArgb)));
  const QString secondary = theme.secondaryAlternativeColor.name(QColor::HexArgb);
  for (auto* label : findChildren<QLabel*>(QStringLiteral("secondaryText"))) {
    label->setStyleSheet(QStringLiteral("color: %1;").arg(secondary));
  }

  const bool dark = themeManager_->currentTheme() == QStringLiteral("Dark");
  const auto icon = dark
    ? oclero::qlementine::icons::Icons16::Misc_Sun
    : oclero::qlementine::icons::Icons16::Misc_Moon;
  themeButton_->setText(dark ? QStringLiteral("Light") : QStringLiteral("Dark"));
  themeButton_->setIcon(style->makeThemedIcon(
    QString::fromLatin1(oclero::qlementine::icons::iconPath(icon))));
  themeButton_->setToolTip(dark ? QStringLiteral("Switch to light theme")
                                : QStringLiteral("Switch to dark theme"));
  style->triggerCompleteRepaint();
}

QString MainWindow::detectGhidraHome() const {
  const QString environment = qEnvironmentVariable("GHIDRA_HOME");
  if (QFileInfo(QDir(environment).filePath(QStringLiteral("support/analyzeHeadless"))).isExecutable()) {
    return environment;
  }
  const QString toolsDir = QDir::home().filePath(QStringLiteral("Tools"));
  QDir tools(toolsDir);
  const auto candidates = tools.entryList({ QStringLiteral("ghidra_*_PUBLIC") }, QDir::Dirs, QDir::Name | QDir::Reversed);
  for (const auto& candidate : candidates) {
    const QString path = tools.filePath(candidate);
    if (QFileInfo(QDir(path).filePath(QStringLiteral("support/analyzeHeadless"))).isExecutable()) {
      return path;
    }
  }
  return {};
}

void MainWindow::loadSettings() {
  QSettings settings;
  const QString platformKey = hostCanSelectTargetPlatform()
    ? settings.value(QStringLiteral("app/platform"),
                     targetPlatformKey(hostTargetPlatform())).toString()
    : targetPlatformKey(hostTargetPlatform());
  const int platformIndex = platformCombo_->findData(platformKey);
  platformCombo_->setCurrentIndex(platformIndex >= 0 ? platformIndex : 0);
  biosEdit_->setText(settings.value(QStringLiteral("paths/bios")).toString());
  iconEdit_->setText(settings.value(QStringLiteral("paths/icon")).toString());
  outputEdit_->setText(settings.value(QStringLiteral("paths/output"),
                                      QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).toString());
  certificateEdit_->setText(settings.value(QStringLiteral("paths/certificate")).toString());
  ghidraEdit_->setText(settings.value(QStringLiteral("paths/ghidra"), detectGhidraHome()).toString());
  titleEdit_->setText(settings.value(QStringLiteral("app/window_title")).toString());
  biosPatchEnabled_->setChecked(settings.value(QStringLiteral("bios_patch/enabled"), false).toBool());
  biosInitialSplashEdit_->setText(settings.value(QStringLiteral("bios_patch/initial_image")).toString());
  biosHandoffImageEdit_->setText(settings.value(QStringLiteral("bios_patch/handoff_image")).toString());
  biosMuteAudio_->setChecked(settings.value(QStringLiteral("bios_patch/mute_audio"), true).toBool());
  biosRemovePsGlyph_->setChecked(settings.value(QStringLiteral("bios_patch/remove_ps_glyph"), true).toBool());
  skipBiosBoot_->setChecked(settings.value(QStringLiteral("runtime/skip_bios_boot"), false).toBool());
  {
    const QString padMode = settings.value(QStringLiteral("runtime/pad_mode"),
                                            QStringLiteral("hybrid")).toString();
    const int padIndex = padModeCombo_->findData(padMode);
    padModeCombo_->setCurrentIndex(padIndex >= 0 ? padIndex : 0);
  }
  macosGipGamepad_->setChecked(
    settings.value(QStringLiteral("runtime/macos_gip_gamepad"), true).toBool());

  if (certificateEdit_->text().isEmpty()) {
    const QString candidate = QDir::home().filePath(QStringLiteral("Projects/neogeo-hub/certificate.pfx"));
    if (QFileInfo(candidate).isFile()) {
      certificateEdit_->setText(candidate);
    }
  }
}

void MainWindow::saveSettings() const {
  QSettings settings;
  settings.setValue(QStringLiteral("app/platform"), platformCombo_->currentData().toString());
  settings.setValue(QStringLiteral("paths/bios"), biosEdit_->text());
  settings.setValue(QStringLiteral("paths/icon"), iconEdit_->text());
  settings.setValue(QStringLiteral("paths/output"), outputEdit_->text());
  settings.setValue(QStringLiteral("paths/certificate"), certificateEdit_->text());
  settings.setValue(QStringLiteral("paths/ghidra"), ghidraEdit_->text());
  settings.setValue(QStringLiteral("app/window_title"), titleEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/enabled"), biosPatchEnabled_->isChecked());
  settings.setValue(QStringLiteral("bios_patch/initial_image"), biosInitialSplashEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/handoff_image"), biosHandoffImageEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/mute_audio"), biosMuteAudio_->isChecked());
  settings.setValue(QStringLiteral("bios_patch/remove_ps_glyph"), biosRemovePsGlyph_->isChecked());
  settings.setValue(QStringLiteral("runtime/skip_bios_boot"), skipBiosBoot_->isChecked());
  settings.setValue(QStringLiteral("runtime/pad_mode"), padModeCombo_->currentData().toString());
  settings.setValue(QStringLiteral("runtime/macos_gip_gamepad"), macosGipGamepad_->isChecked());
}

void MainWindow::chooseDisc() {
  const auto files = QFileDialog::getOpenFileNames(
    this, QStringLiteral("Select PlayStation BIN/CUE files"),
    discEdit_->text().isEmpty() ? QDir::homePath() : QFileInfo(discEdit_->text()).absolutePath(),
    QStringLiteral("PlayStation disc files (*.cue *.bin);;All files (*)"));
  if (files.isEmpty()) {
    return;
  }
  QString cue;
  QStringList bins;
  for (const auto& path : files) {
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare(QStringLiteral("cue"), Qt::CaseInsensitive) == 0) {
      if (!cue.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Disc selection"),
                             QStringLiteral("Select exactly one CUE file per build."));
        return;
      }
      cue = path;
    } else if (suffix.compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
      bins.append(path);
    }
  }
  if (cue.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Disc selection"),
                         QStringLiteral("The selection must include one .cue file."));
    return;
  }
  discEdit_->setText(cue);
  selectedBins_ = bins;
  if (titleEdit_->text().isEmpty()) {
    titleEdit_->setText(QFileInfo(cue).completeBaseName() + QStringLiteral(" Recompiled"));
  }
}

void MainWindow::chooseBiosInitialSplash() {
  const auto path = QFileDialog::getOpenFileName(
    this, QStringLiteral("Select initial BIOS splash"),
    QFileInfo(biosInitialSplashEdit_->text()).absolutePath(),
    QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*)"));
  if (!path.isEmpty()) biosInitialSplashEdit_->setText(path);
}

void MainWindow::chooseBiosHandoffImage() {
  const auto path = QFileDialog::getOpenFileName(
    this, QStringLiteral("Select BIOS handoff image"),
    QFileInfo(biosHandoffImageEdit_->text()).absolutePath(),
    QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*)"));
  if (!path.isEmpty()) biosHandoffImageEdit_->setText(path);
}

void MainWindow::updateBiosPatchControls() {
  const bool enabled = biosPatchEnabled_ && biosPatchEnabled_->isChecked();
  if (biosInitialSplashEdit_) biosInitialSplashEdit_->setEnabled(enabled);
  if (biosHandoffImageEdit_) biosHandoffImageEdit_->setEnabled(enabled);
  if (biosMuteAudio_) biosMuteAudio_->setEnabled(enabled);
  if (biosRemovePsGlyph_) biosRemovePsGlyph_->setEnabled(enabled);
  updateBuildButton();
}

void MainWindow::chooseBios() {
  const auto path = QFileDialog::getOpenFileName(
    this, QStringLiteral("Select SCPH1001.BIN"), QFileInfo(biosEdit_->text()).absolutePath(),
    QStringLiteral("SCPH1001 BIOS (SCPH1001.BIN);;BIN files (*.BIN *.bin)"));
  if (!path.isEmpty()) {
    biosEdit_->setText(path);
  }
}

void MainWindow::chooseIcon() {
  const auto path = QFileDialog::getOpenFileName(
    this, QStringLiteral("Select app icon"), QFileInfo(iconEdit_->text()).absolutePath(),
    QStringLiteral("App icons (*.png *.svg *.icns);;All files (*)"));
  if (!path.isEmpty()) {
    iconEdit_->setText(path);
  }
}

void MainWindow::chooseOutputDirectory() {
  QFileDialog dialog(this, QStringLiteral("Select output directory"));
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setFileMode(QFileDialog::Directory);
  dialog.setOption(QFileDialog::ShowDirsOnly, true);
  dialog.setOption(QFileDialog::DontUseNativeDialog, false);
  dialog.setDirectory(outputEdit_->text().isEmpty() ? QDir::homePath()
                                                    : outputEdit_->text());
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
    return;
  }

  const QString path = dialog.selectedFiles().constFirst();
  QString error;
  if (!verifyOutputDirectoryAccess(path, error)) {
    QMessageBox::warning(
      this, QStringLiteral("Output directory unavailable"),
      QStringLiteral("PSXRecomp Studio cannot deliver an app to the selected directory.\n\n"
                     "%1\n\n"
                     "Choose the directory again or allow PSXRecomp Studio access in "
                     "System Settings > Privacy & Security > Files & Folders.").arg(error));
    return;
  }
  outputEdit_->setText(path);
}

void MainWindow::chooseCertificate() {
  const auto path = QFileDialog::getOpenFileName(
    this, QStringLiteral("Select signing certificate"), QFileInfo(certificateEdit_->text()).absolutePath(),
    QStringLiteral("PKCS#12 certificates (*.pfx *.p12);;All files (*)"));
  if (!path.isEmpty()) {
    certificateEdit_->setText(path);
  }
}

void MainWindow::chooseGhidraHome() {
  const auto path = QFileDialog::getExistingDirectory(
    this, QStringLiteral("Select Ghidra installation"), ghidraEdit_->text());
  if (!path.isEmpty()) {
    ghidraEdit_->setText(path);
  }
}

PipelineRequest MainWindow::requestFromUi(bool overwrite) const {
  PipelineRequest request;
  request.targetPlatform = targetPlatformFromKey(platformCombo_->currentData().toString());
  request.cuePath = discEdit_->text();
  request.selectedBinPaths = selectedBins_;
  request.biosPath = biosEdit_->text();
  request.iconPath = iconEdit_->text();
  request.windowTitle = titleEdit_->text();
  request.outputDirectory = outputEdit_->text();
  request.certificatePath = certificateEdit_->text();
  request.certificatePassword = certificatePasswordEdit_->text();
  request.ghidraHome = ghidraEdit_->text();
  request.frameworkRoot = QString::fromUtf8(PSXRECOMP_SOURCE_ROOT);
  request.patchBiosBranding = biosPatchEnabled_->isChecked();
  request.biosInitialSplashPath = biosInitialSplashEdit_->text();
  request.biosHandoffImagePath = biosHandoffImageEdit_->text();
  request.biosMuteBootAudio = biosMuteAudio_->isChecked();
  request.biosRemoveStockPsGlyph = biosRemovePsGlyph_->isChecked();
  request.skipBiosBoot = skipBiosBoot_->isChecked();
  request.padMode = padModeCombo_->currentData().toString();
  request.macosGipGamepad = macosGipGamepad_->isChecked();
  request.overwriteOutput = overwrite;
  return request;
}

void MainWindow::startBuild() {
  QString accessError;
  if (!verifyOutputDirectoryAccess(outputEdit_->text(), accessError)) {
    QMessageBox::warning(
      this, QStringLiteral("Output directory unavailable"),
      QStringLiteral("PSXRecomp Studio cannot use the selected output directory.\n\n"
                     "%1\n\n"
                     "Use Browse to select it again and grant Studio access if prompted.").arg(accessError));
    return;
  }

  const QString bundleName = cleanBundleName(titleEdit_->text());
  const auto targetPlatform =
    targetPlatformFromKey(platformCombo_->currentData().toString());
  QString outputName;
  switch (targetPlatform) {
    case TargetPlatform::Windows:
      outputName = bundleName + QStringLiteral("-Windows");
      break;
    case TargetPlatform::Linux:
      outputName = bundleName + QStringLiteral("-Linux");
      break;
    case TargetPlatform::MacOS:
      outputName = bundleName + QStringLiteral(".app");
      break;
  }
  const QString outputPath = QDir(outputEdit_->text()).filePath(outputName);
  bool overwrite = false;
  if (QFileInfo::exists(outputPath)) {
    const auto answer = QMessageBox::question(
      this, QStringLiteral("Replace existing output?"),
      QStringLiteral("%1 already exists. Replace it with the new build?").arg(outputPath),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      return;
    }
    overwrite = true;
  }

  saveSettings();
  outputAppPath_.clear();
  revealButton_->setEnabled(false);
  logView_->clear();
  setBusy(true);
  emit runRequested(requestFromUi(overwrite));
}

void MainWindow::cancelBuild() {
  if (worker_) {
    worker_->requestCancel();
    cancelButton_->setEnabled(false);
    stageLabel_->setText(QStringLiteral("Cancelling…"));
  }
}

void MainWindow::revealOutput() {
  if (outputAppPath_.isEmpty()) {
    return;
  }
  QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                          { QStringLiteral("-R"), outputAppPath_ });
}

void MainWindow::onCompleted(const QString& appPath) {
  outputAppPath_ = appPath;
  progressBar_->setRange(0, 100);
  progressBar_->setValue(100);
  const auto targetPlatform =
    targetPlatformFromKey(platformCombo_->currentData().toString());
  const bool macosTarget = targetPlatform == TargetPlatform::MacOS;
  const QString platformName = targetPlatformDisplayName(targetPlatform);
  stageLabel_->setText(macosTarget
    ? QStringLiteral("Complete — signed app verified")
    : QStringLiteral("Complete — %1 package verified").arg(platformName));
  revealButton_->setEnabled(true);
  setBusy(false);
  QMessageBox::information(
    this, macosTarget ? QStringLiteral("Signed app created")
                      : QStringLiteral("%1 app created").arg(platformName),
    macosTarget
      ? QStringLiteral("The signed macOS app was created and verified:\n\n%1").arg(appPath)
      : QStringLiteral("The %1 app package was created and verified:\n\n%2")
          .arg(platformName, appPath));
}

void MainWindow::onFailed(const QString& message, const QString& workspacePath) {
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  stageLabel_->setText(QStringLiteral("Build failed"));
  setBusy(false);
  QString detail = message;
  if (!workspacePath.isEmpty()) {
    detail += QStringLiteral("\n\nProof and intermediate artifacts were retained at:\n%1").arg(workspacePath);
  }
  QMessageBox::critical(this, QStringLiteral("Build failed"), detail);
}

void MainWindow::setBusy(bool busy) {
  buildButton_->setEnabled(!busy);
  cancelButton_->setEnabled(busy);
  if (busy) {
    progressBar_->setRange(0, 0);
  } else if (progressBar_->maximum() == 0) {
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
  }
  for (auto* edit : { discEdit_, biosEdit_, iconEdit_, titleEdit_, outputEdit_,
                      certificateEdit_, certificatePasswordEdit_, ghidraEdit_,
                      biosInitialSplashEdit_, biosHandoffImageEdit_ }) {
    edit->setEnabled(!busy);
  }
  biosPatchEnabled_->setEnabled(!busy);
  platformCombo_->setEnabled(!busy);
  skipBiosBoot_->setEnabled(!busy);
  padModeCombo_->setEnabled(!busy);
  macosGipGamepad_->setEnabled(!busy);
  biosMuteAudio_->setEnabled(!busy && biosPatchEnabled_->isChecked());
  biosRemovePsGlyph_->setEnabled(!busy && biosPatchEnabled_->isChecked());
  if (!busy) {
    updatePlatformControls();
    updateBiosPatchControls();
  }
  updateBuildButton();
}

void MainWindow::updatePlatformControls() {
  const auto targetPlatform =
    targetPlatformFromKey(platformCombo_->currentData().toString());
  const bool macosTarget = targetPlatform == TargetPlatform::MacOS;
  const bool busy = cancelButton_ && cancelButton_->isEnabled();
  certificateEdit_->setEnabled(macosTarget && !busy);
  certificatePasswordEdit_->setEnabled(macosTarget && !busy);
  certificateEdit_->parentWidget()->setVisible(macosTarget);
  certificatePasswordEdit_->parentWidget()->setVisible(macosTarget);
  macosGipGamepad_->setVisible(macosTarget);
  macosGipGamepad_->setEnabled(macosTarget && !busy);
  const QString unsignedNote = targetPlatform == TargetPlatform::Windows
    ? QStringLiteral("Windows exports are currently delivered unsigned")
    : QStringLiteral("Linux exports are currently delivered unsigned");
  certificateEdit_->setToolTip(macosTarget
    ? QStringLiteral("PKCS#12 identity used to sign the macOS app")
    : unsignedNote);
  certificatePasswordEdit_->setToolTip(certificateEdit_->toolTip());
  if (macosTarget) {
    signingNote_->setText(QStringLiteral(
      "The certificate is imported into an isolated temporary keychain and removed after verification."));
    outputEdit_->setPlaceholderText(QStringLiteral("Destination for the signed .app"));
    buildButton_->setText(QStringLiteral("Build Signed .app"));
  } else if (targetPlatform == TargetPlatform::Windows) {
    signingNote_->setText(QStringLiteral(
      "Windows exports use the installed x86_64-w64-mingw32 MinGW toolchain and are delivered unsigned."));
    outputEdit_->setPlaceholderText(QStringLiteral("Destination for the Windows app folder"));
    buildButton_->setText(QStringLiteral("Build Windows App"));
  } else {
    signingNote_->setText(QStringLiteral(
      "Linux exports use the installed x86_64-unknown-linux-gnu toolchain, bundle SDL2, and are delivered unsigned."));
    outputEdit_->setPlaceholderText(QStringLiteral("Destination for the Linux app folder"));
    buildButton_->setText(QStringLiteral("Build Linux App"));
  }
  updateBuildButton();
}

void MainWindow::updateBuildButton() {
  if (cancelButton_->isEnabled()) {
    return;
  }
  const bool brandingReady = !biosPatchEnabled_->isChecked() ||
    (!biosInitialSplashEdit_->text().isEmpty() && !biosHandoffImageEdit_->text().isEmpty());
  const bool macosTarget =
    targetPlatformFromKey(platformCombo_->currentData().toString()) == TargetPlatform::MacOS;
  const bool signingReady = !macosTarget ||
    (!certificateEdit_->text().isEmpty() && !certificatePasswordEdit_->text().isEmpty());
  const bool ready = !discEdit_->text().isEmpty() && !biosEdit_->text().isEmpty() &&
                     !iconEdit_->text().isEmpty() && !titleEdit_->text().trimmed().isEmpty() &&
                     !outputEdit_->text().isEmpty() && !ghidraEdit_->text().isEmpty() &&
                     signingReady && brandingReady;
  buildButton_->setEnabled(ready);
}

} // namespace psxstudio
