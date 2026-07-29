#include "MainWindow.h"

#include "CiPanel.h"
#include "DiscCatalog.h"
#include "GbaSupport.h"
#include "PipelineSupport.h"
#include "PipelineWorker.h"

#include <QApplication>
#include <QAbstractItemView>
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
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QResizeEvent>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTabWidget>
#include <QThread>
#include <QUrl>
#include <QSysInfo>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace psxstudio {

namespace {

QString batchIconSettingKey(const QString& id) {
  return QStringLiteral("batch/icons/%1").arg(id);
}

QString batchNameSettingKey(const QString& id) {
  return QStringLiteral("batch/names/%1").arg(id);
}

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

void configureReadOnlyComboBox(QComboBox* comboBox) {
  /* A parent card stylesheet causes Qt's stylesheet proxy to skip
   * CE_ComboBoxLabel with Qlementine on Windows. The editable paint path
   * renders its foreground through a QLineEdit instead, while read-only mode
   * keeps the control restricted to its predefined choices. */
  comboBox->setEditable(true);
  comboBox->setInsertPolicy(QComboBox::NoInsert);
  comboBox->lineEdit()->setReadOnly(true);
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
  headerTitle_ = new QLabel(QStringLiteral("Build a native PlayStation app"), headerText);
  auto titleFont = headerTitle_->font();
  titleFont.setBold(true);
  titleFont.setPointSizeF(titleFont.pointSizeF() + 7.0);
  headerTitle_->setFont(titleFont);
  headerSubtitle_ = new QLabel(
    QStringLiteral("One workflow for disc analysis, evidence-backed source generation, native compilation, "
                   "and platform packaging."),
    headerText);
  headerSubtitle_->setWordWrap(true);
  headerTextLayout->addWidget(headerTitle_);
  headerTextLayout->addWidget(headerSubtitle_);
  themeButton_ = new QPushButton(header);
  themeButton_->setMinimumWidth(104);
  themeButton_->setToolTip(QStringLiteral("Switch between dark and light themes"));
  headerLayout->addWidget(headerText, 1);
  headerLayout->addWidget(themeButton_, 0, Qt::AlignTop);
  root->addWidget(header);

  tabs_ = new QTabWidget(central);
  tabs_->setObjectName(QStringLiteral("studioTabs"));
  tabs_->setDocumentMode(true);
  tabs_->setMovable(false);
  tabs_->setUsesScrollButtons(false);
  auto* exportPage = new QWidget(tabs_);
  auto* exportRoot = new QVBoxLayout(exportPage);
  exportRoot->setContentsMargins(0, 10, 0, 0);
  exportRoot->setSpacing(14);

  formsContainer_ = new QWidget(exportPage);
  formsLayout_ = new QGridLayout(formsContainer_);
  formsLayout_->setContentsMargins(0, 0, 0, 0);
  formsLayout_->setHorizontalSpacing(14);
  formsLayout_->setVerticalSpacing(14);

  inputCard_ = makeCard(QStringLiteral("inputCard"), formsContainer_);
  auto* inputLayout = new QVBoxLayout(inputCard_);
  inputLayout->setContentsMargins(18, 16, 18, 18);
  inputLayout->setSpacing(10);
  inputLayout->addWidget(makeSectionTitle(QStringLiteral("App inputs"), inputCard_));

  auto* systemRow = new QWidget(inputCard_);
  systemRow->setMinimumHeight(34);
  auto* systemLayout = new QHBoxLayout(systemRow);
  systemLayout->setContentsMargins(0, 0, 0, 0);
  auto* systemLabel = new QLabel(QStringLiteral("System"), systemRow);
  systemLabel->setMinimumWidth(142);
  systemCombo_ = new QComboBox(systemRow);
  systemCombo_->addItem(QStringLiteral("PlayStation"), systemKindKey(SystemKind::PlayStation));
  systemCombo_->addItem(QStringLiteral("Game Boy Advance"), systemKindKey(SystemKind::GameBoyAdvance));
  configureReadOnlyComboBox(systemCombo_);
  systemLayout->addWidget(systemLabel);
  systemLayout->addWidget(systemCombo_, 1);
  inputLayout->addWidget(systemRow);

  auto* platformRow = new QWidget(inputCard_);
  platformRow->setMinimumHeight(34);
  auto* platformLayout = new QHBoxLayout(platformRow);
  platformLayout->setContentsMargins(0, 0, 0, 0);
  auto* platformLabel = new QLabel(QStringLiteral("Platform"), platformRow);
  platformLabel->setMinimumWidth(142);
  platformCombo_ = new QComboBox(platformRow);
  platformCombo_->addItem(QStringLiteral("All"), targetPlatformKey(TargetPlatform::All));
  platformCombo_->addItem(QStringLiteral("macOS"), targetPlatformKey(TargetPlatform::MacOS));
  platformCombo_->addItem(QStringLiteral("Windows"), targetPlatformKey(TargetPlatform::Windows));
  platformCombo_->addItem(QStringLiteral("Linux"), targetPlatformKey(TargetPlatform::Linux));
  platformCombo_->addItem(QStringLiteral("Virtua ARM"), targetPlatformKey(TargetPlatform::VirtuaArm));
  configureReadOnlyComboBox(platformCombo_);
  platformCombo_->setCurrentIndex(0);
  platformLayout->addWidget(platformLabel);
  platformLayout->addWidget(platformCombo_, 1);
  inputLayout->addWidget(platformRow);
  platformRow->setVisible(true);

  useCi_ = new QCheckBox(QStringLiteral("Use CI"), inputCard_);
  useCi_->setObjectName(QStringLiteral("useCiCheckBox"));
  useCi_->setToolTip(
    QStringLiteral("Build mode only. Keep one compatible build local and dispatch remaining platform or queued work to authenticated CI workers."));
  inputLayout->addWidget(useCi_);

  batchCheck_ = new QCheckBox(QStringLiteral("Batch"), inputCard_);
  batchCheck_->setToolTip(
    QStringLiteral("Scan a directory recursively and queue one export for every PlayStation CUE or standalone BIN image."));
  inputLayout->addWidget(batchCheck_);

  discEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("Disc BIN/CUE"),
                         QStringLiteral("One .cue and all referenced .bin files"), SLOT(chooseDisc()));
  discLabel_ = qobject_cast<QLabel*>(discEdit_->parentWidget()->layout()->itemAt(0)->widget());
  batchDirectoryEdit_ = addPathRow(
    inputCard_, inputLayout, QStringLiteral("Game directory"),
    QStringLiteral("Folder containing PlayStation BIN/CUE files"), SLOT(chooseBatchDirectory()));
  batchSummaryLabel_ = new QLabel(
    QStringLiteral("Choose a directory to create the game list. Icons are optional."), inputCard_);
  batchSummaryLabel_->setWordWrap(true);
  batchSummaryLabel_->setObjectName(QStringLiteral("secondaryText"));
  inputLayout->addWidget(batchSummaryLabel_);
  batchList_ = new QListWidget(inputCard_);
  batchList_->setObjectName(QStringLiteral("batchGameList"));
  batchList_->setFrameShape(QFrame::NoFrame);
  batchList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  batchList_->setSelectionMode(QAbstractItemView::NoSelection);
  batchList_->setSpacing(6);
  batchList_->setMinimumHeight(190);
  batchList_->setMaximumHeight(270);
  inputLayout->addWidget(batchList_);
  biosEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("PlayStation BIOS"),
                         QStringLiteral("Canonical SCPH1001.BIN only"), SLOT(chooseBios()));
  biosLabel_ = qobject_cast<QLabel*>(biosEdit_->parentWidget()->layout()->itemAt(0)->widget());
  iconEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("App icon"),
                         QStringLiteral("Optional PNG, SVG, or ICNS"), SLOT(chooseIcon()));
  if (auto* iconRow = qobject_cast<QHBoxLayout*>(iconEdit_->parentWidget()->layout())) {
    auto* clearIcon = new QPushButton(QStringLiteral("Clear"), iconEdit_->parentWidget());
    clearIcon->setToolTip(QStringLiteral("Use the built-in PSXRecomp icon"));
    iconRow->insertWidget(std::max(0, iconRow->count() - 1), clearIcon);
    connect(clearIcon, &QPushButton::clicked, iconEdit_, &QLineEdit::clear);
  }

  skipBiosBoot_ = new QCheckBox(
    QStringLiteral("Skip BIOS intro and boot directly to the game"), inputCard_);
  skipBiosBoot_->setToolTip(
    QStringLiteral("Keeps the recompiled BIOS linked, but skips its visible shell/intro and proceeds directly to disc boot."));
  inputLayout->addWidget(skipBiosBoot_);

  padPolicyLabel_ = new QLabel(
    QStringLiteral("Controller: automatic D-Pad first, Hybrid fallback"), inputCard_);
  padPolicyLabel_->setObjectName(QStringLiteral("secondaryText"));
  padPolicyLabel_->setWordWrap(true);
  padPolicyLabel_->setToolTip(
    QStringLiteral("Each game negotiates its controller at runtime. A real D-Pad is "
                   "presented first; Hybrid is enabled only if the game rejects it."));
  inputLayout->addWidget(padPolicyLabel_);

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
  exportAsZip_ = new QCheckBox(QStringLiteral("Export as zip"), inputCard_);
  exportAsZip_->setChecked(true);
  exportAsZip_->setToolTip(
    QStringLiteral("Create one ZIP per export. Source ZIPs retain their initialized Git repository; build ZIPs contain the verified native package."));
  inputLayout->addWidget(exportAsZip_);
  inputLayout->addStretch(1);

  toolsCard_ = makeCard(QStringLiteral("toolsCard"), formsContainer_);
  auto* toolsLayout = new QVBoxLayout(toolsCard_);
  toolsLayout->setContentsMargins(18, 16, 18, 18);
  toolsLayout->setSpacing(10);
  toolsLayout->addWidget(makeSectionTitle(QStringLiteral("Analysis and export tools"), toolsCard_));
  ghidraEdit_ = addPathRow(toolsCard_, toolsLayout, QStringLiteral("Ghidra home"),
                           QStringLiteral("Ghidra 11.3.2 installation"), SLOT(chooseGhidraHome()));
  powerEngineEdit_ = addPathRow(
    toolsCard_, toolsLayout, QStringLiteral("PowerEngine root"),
    QStringLiteral("PowerEngine directory containing External/Virtua and OS/MVII"),
    SLOT(choosePowerEngineRoot()));
  powerEngineEdit_->setToolTip(
    QStringLiteral("Canonical PowerEngine source checkout used directly for Dash, POSIX headers,\n"
                   "the Virtua packager, and MVII ABI headers. These files are not copied into PSXRecomp."));
  llvmEdit_ = addPathRow(toolsCard_, toolsLayout, QStringLiteral("LLVM toolchain"),
                         QStringLiteral("PowerEngine compiler bundle containing bin/compiler or bin/clang"),
                         SLOT(chooseLlvmRoot()));
  llvmEdit_->setToolTip(
    QStringLiteral("PowerEngine LLVM/compiler bundle used for Virtua ARM. The matching ARM\n"
                   "llvm-libc/libc++ sysroot and compiler-rt are resolved from the same PowerEngine build."));
  signingEnabled_ = new QCheckBox(QStringLiteral("Sign macOS app with PFX"), toolsCard_);
  signingEnabled_->setToolTip(
    QStringLiteral("Optional. The PFX is read directly and is never imported into a Keychain."));
  toolsLayout->addWidget(signingEnabled_);
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
    QStringLiteral("Signing is optional. Direct PFX signing never imports the identity into a Keychain."),
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

  formsScroll_ = new QScrollArea(exportPage);
  formsScroll_->setFrameShape(QFrame::NoFrame);
  formsScroll_->setWidgetResizable(true);
  formsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  formsScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  formsScroll_->setMinimumHeight(250);
  formsScroll_->setMaximumHeight(390);
  formsScroll_->setWidget(formsContainer_);
  exportRoot->addWidget(formsScroll_);

  statusCard_ = makeCard(QStringLiteral("statusCard"), exportPage);
  auto* statusLayout = new QVBoxLayout(statusCard_);
  statusLayout->setContentsMargins(18, 14, 18, 14);
  statusLayout->setSpacing(9);
  auto* statusHeader = new QHBoxLayout();
  stageLabel_ = new QLabel(QStringLiteral("Ready"), statusCard_);
  auto* statusHint = new QLabel(QStringLiteral("Proof artifacts are embedded in every export"), statusCard_);
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
  exportRoot->addWidget(statusCard_, 1);

  auto* actions = new QHBoxLayout();
  revealButton_ = new QPushButton(themedIcon(oclero::qlementine::icons::Icons16::File_FolderOpen),
                                  QStringLiteral("Reveal Output"), exportPage);
  revealButton_->setEnabled(false);
  cancelButton_ = new QPushButton(QStringLiteral("Cancel"), exportPage);
  cancelButton_->setEnabled(false);
  exportModeCombo_ = new QComboBox(exportPage);
  exportModeCombo_->setObjectName(QStringLiteral("exportModeComboBox"));
  exportModeCombo_->addItem(QStringLiteral("Source"), exportModeKey(ExportMode::Source));
  exportModeCombo_->addItem(QStringLiteral("Build"), exportModeKey(ExportMode::Build));
  configureReadOnlyComboBox(exportModeCombo_);
  exportModeCombo_->setMinimumWidth(116);
  exportModeCombo_->setToolTip(
    QStringLiteral("Source exports an initialized, portable Git repository. Build compiles and packages it locally or with CI."));
  buildButton_ = new QPushButton(themedIcon(oclero::qlementine::icons::Icons16::Action_Build),
                                 QStringLiteral("Export"), exportPage);
  buildButton_->setObjectName(QStringLiteral("exportButton"));
  buildButton_->setDefault(true);
  buildButton_->setMinimumWidth(132);
  actions->addWidget(revealButton_);
  actions->addStretch(1);
  actions->addWidget(cancelButton_);
  actions->addWidget(exportModeCombo_);
  actions->addWidget(buildButton_);
  exportRoot->addLayout(actions);

  ciPanel_ = new ci::CiPanel(tabs_);
  tabs_->addTab(exportPage, QStringLiteral("Export"));
  tabs_->addTab(ciPanel_, QStringLiteral("CI"));
  root->addWidget(tabs_, 1);

  setCentralWidget(central);
  reflowForms();

  workerThread_ = new QThread(this);
  worker_ = new PipelineWorker();
  worker_->moveToThread(workerThread_);
  connect(workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(this, &MainWindow::runRequested, worker_, &PipelineWorker::run, Qt::QueuedConnection);
  connect(worker_, &PipelineWorker::stageChanged, this,
          [this](const QString& name, int index, int total) {
            const QString exportPrefix = totalRequestCount_ > 1
              ? QStringLiteral("Export %1 of %2  ·  ")
                  .arg(activeRequestIndex_).arg(totalRequestCount_)
              : QString();
            stageLabel_->setText(
              QStringLiteral("%1%2  ·  Step %3 of %4")
                .arg(exportPrefix, name).arg(index).arg(total));
          });
  connect(worker_, &PipelineWorker::logLine, this, [this](const QString& line) {
    logView_->appendPlainText(line);
  });
  connect(worker_, &PipelineWorker::completed, this, &MainWindow::onCompleted);
  connect(worker_, &PipelineWorker::ciSourcePrepared,
          this, &MainWindow::onCiSourcePrepared);
  connect(worker_, &PipelineWorker::failed, this, &MainWindow::onFailed);
  connect(worker_, &PipelineWorker::cancelled, this, [this]() {
    workerActive_ = false;
    pendingRequests_.clear();
    if (cancelling_) {
      finishIfIdle();
      return;
    }
    onFailed(QStringLiteral("The export was cancelled unexpectedly."), {});
  });
  workerThread_->start();

  connect(buildButton_, &QPushButton::clicked, this, &MainWindow::startBuild);
  connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelBuild);
  connect(revealButton_, &QPushButton::clicked, this, &MainWindow::revealOutput);
  connect(themeButton_, &QPushButton::clicked, this, &MainWindow::toggleTheme);
  connect(systemCombo_, &QComboBox::currentIndexChanged,
          this, &MainWindow::updateSystemControls);
  connect(platformCombo_, &QComboBox::currentIndexChanged,
          this, &MainWindow::updatePlatformControls);
  connect(batchCheck_, &QCheckBox::toggled, this, &MainWindow::updateBatchMode);
  connect(signingEnabled_, &QCheckBox::toggled, this, &MainWindow::updatePlatformControls);
  connect(biosPatchEnabled_, &QCheckBox::toggled, this, &MainWindow::updateBiosPatchControls);
  connect(skipBiosBoot_, &QCheckBox::toggled, this, &MainWindow::updateBuildButton);
  connect(macosGipGamepad_, &QCheckBox::toggled, this, &MainWindow::updateBuildButton);
  connect(exportAsZip_, &QCheckBox::toggled, this, &MainWindow::updatePlatformControls);
  connect(exportModeCombo_, &QComboBox::currentIndexChanged,
          this, &MainWindow::updateExportMode);
  connect(useCi_, &QCheckBox::toggled, this, &MainWindow::updateBuildButton);
  connect(ciPanel_, &ci::CiPanel::authenticationStateChanged,
          this, &MainWindow::updateBuildButton);
  connect(ciPanel_, &ci::CiPanel::buildersChanged,
          this, &MainWindow::updateBuildButton);
  connect(ciPanel_, &ci::CiPanel::logLine, this, [this](const QString& line) {
    logView_->appendPlainText(line);
  });
  connect(ciPanel_, &ci::CiPanel::buildProgress, this,
          [this](const QString&, const QString& text) {
            if (!workerActive_) stageLabel_->setText(QStringLiteral("CI · %1").arg(text));
          });
  connect(ciPanel_, &ci::CiPanel::buildCompleted,
          this, &MainWindow::onCiBuildCompleted);
  connect(ciPanel_, &ci::CiPanel::buildFailed,
          this, &MainWindow::onCiBuildFailed);
  connect(ciPanel_, &ci::CiPanel::buildCancelled,
          this, &MainWindow::onCiBuildCancelled);
  connect(themeManager_, &oclero::qlementine::ThemeManager::currentThemeChanged,
          this, [this]() {
            QSettings().setValue(QStringLiteral("app/theme"), themeManager_->currentTheme());
            applyTheme();
          });
  for (auto* edit : { discEdit_, batchDirectoryEdit_, biosEdit_, iconEdit_, titleEdit_, outputEdit_,
                      certificateEdit_, certificatePasswordEdit_, ghidraEdit_, powerEngineEdit_, llvmEdit_,
                      biosInitialSplashEdit_, biosHandoffImageEdit_ }) {
    connect(edit, &QLineEdit::textChanged, this, &MainWindow::updateBuildButton);
  }

  loadSettings();
  updateSystemControls();
  updateBatchMode();
  updateExportMode();
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

void MainWindow::reflowForms(bool force) {
  if (!formsLayout_ || !inputCard_ || !toolsCard_ || !brandingCard_ || !formsContainer_) {
    return;
  }
  const bool columns = width() >= 1040;
  if (!force && formsLayout_->count() > 0 && columns == formsAreColumns_) {
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
    formsScroll_->setMaximumHeight(batchCheck_ && batchCheck_->isChecked() ? 540 : 455);
  } else {
    formsLayout_->addWidget(inputCard_, 0, 0);
    formsLayout_->addWidget(toolsCard_, 1, 0);
    formsLayout_->addWidget(brandingCard_, 2, 0);
    formsLayout_->setColumnStretch(0, 1);
    formsLayout_->setColumnStretch(1, 0);
    formsContainer_->setMinimumHeight(inputCard_->sizeHint().height() +
                                      toolsCard_->sizeHint().height() +
                                      brandingCard_->sizeHint().height() + 28);
    formsScroll_->setMaximumHeight(batchCheck_ && batchCheck_->isChecked() ? 500 : 390);
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
  for (const QString& objectName : { QStringLiteral("ciAuthCard"),
                                     QStringLiteral("ciBuildersCard"),
                                     QStringLiteral("ciJobsCard") }) {
    if (auto* frame = findChild<QFrame*>(objectName)) {
      frame->setStyleSheet(cardStyle.arg(objectName,
                                         theme.backgroundColorMain2.name(QColor::HexArgb),
                                         theme.borderColor.name(QColor::HexArgb)));
    }
  }
  logView_->setStyleSheet(QStringLiteral(
    "QPlainTextEdit { background-color: %1; border: 1px solid %2; border-radius: 7px; padding: 7px; }")
    .arg(theme.backgroundColorWorkspace.name(QColor::HexArgb),
         theme.borderColor.name(QColor::HexArgb)));
  batchList_->setStyleSheet(QStringLiteral(
    "QListWidget#batchGameList { background: transparent; border: 0; } "
    "QFrame#batchGameRow { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
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
  if (!ghidraAnalyzeHeadlessPath(environment).isEmpty()) {
    return environment;
  }
  const QString toolsDir = QDir::home().filePath(QStringLiteral("Tools"));
  QDir tools(toolsDir);
  const auto candidates = tools.entryList({ QStringLiteral("ghidra_*_PUBLIC") }, QDir::Dirs, QDir::Name | QDir::Reversed);
  for (const auto& candidate : candidates) {
    const QString path = tools.filePath(candidate);
    if (!ghidraAnalyzeHeadlessPath(path).isEmpty()) {
      return path;
    }
  }
  return {};
}

QString MainWindow::detectPowerEngineRoot() const {
  const auto usable = [](const QString& root) {
    if (root.isEmpty()) return false;
    const QDir directory(root);
    return QFileInfo::exists(directory.filePath(QStringLiteral("External/Virtua/Dash/CMakeLists.txt"))) &&
           QFileInfo::exists(directory.filePath(QStringLiteral("External/Virtua/binary/virtua.go"))) &&
           QFileInfo::exists(directory.filePath(
             QStringLiteral("OS/MVII/Kernel/Shared/posix-shim/include/pthread.h")));
  };
  for (const auto& environmentName : { "POWERENGINE_ROOT", "POWER_ENGINE_ROOT" }) {
    const QString environment = qEnvironmentVariable(environmentName);
    if (usable(environment)) return QDir::cleanPath(environment);
  }
  for (const auto& candidate : {
         QDir::home().filePath(QStringLiteral("Projects/PowerEngineV3/PowerEngine")),
         QDir::home().filePath(QStringLiteral("Projects/PowerEngine")) }) {
    if (usable(candidate)) return QDir::cleanPath(candidate);
  }
  return {};
}

QString MainWindow::detectLlvmRoot() const {
  const auto usable = [](const QString& root) {
    if (root.isEmpty()) return false;
    const QDir bin(QDir(root).filePath(QStringLiteral("bin")));
    return QFileInfo::exists(bin.filePath(QStringLiteral("compiler"))) ||
           QFileInfo::exists(bin.filePath(QStringLiteral("clang")));
  };
  const QString environment = qEnvironmentVariable("VIRTUA_LLVM_ROOT");
  if (usable(environment)) return QDir::cleanPath(environment);

  const QString powerEngine = powerEngineEdit_ && !powerEngineEdit_->text().isEmpty()
    ? powerEngineEdit_->text() : detectPowerEngineRoot();
  if (!powerEngine.isEmpty()) {
    const QDir root(powerEngine);
    for (const auto& relative : {
           QStringLiteral("build/Release/package/bin/Release/bundles/compiler"),
           QStringLiteral("build/Release/stage/External/xbox/host/compiler"),
           QStringLiteral("build/Debug/package/bin/Debug/bundles/compiler"),
           QStringLiteral("build/Debug/stage/External/xbox/host/compiler") }) {
      const QString candidate = root.filePath(relative);
      if (usable(candidate)) return QDir::cleanPath(candidate);
    }
  }
  for (const auto& candidate : { QStringLiteral("/usr/local/opt/llvm"),
                                 QStringLiteral("/opt/homebrew/opt/llvm") }) {
    if (usable(candidate)) return candidate;
  }
  return {};
}

void MainWindow::loadSettings() {
  QSettings settings;
  const QString systemKey = settings.value(
    QStringLiteral("app/system"), systemKindKey(SystemKind::PlayStation)).toString();
  const int systemIndex = systemCombo_->findData(systemKey);
  {
    const QSignalBlocker blocker(systemCombo_);
    systemCombo_->setCurrentIndex(systemIndex >= 0 ? systemIndex : 0);
  }
  currentSystem_ = systemKindFromKey(systemCombo_->currentData().toString());
  psxInputPath_ = settings.value(QStringLiteral("paths/psx_disc"),
                                 settings.value(QStringLiteral("paths/disc"))).toString();
  psxBiosPath_ = settings.value(QStringLiteral("paths/psx_bios"),
                                settings.value(QStringLiteral("paths/bios"))).toString();
  psxBatchDirectory_ = settings.value(QStringLiteral("batch/psx_directory"),
                                      settings.value(QStringLiteral("batch/directory"))).toString();
  gbaInputPath_ = settings.value(QStringLiteral("paths/gba_rom")).toString();
  gbaBiosPath_ = settings.value(QStringLiteral("paths/gba_bios")).toString();
  gbaBatchDirectory_ = settings.value(QStringLiteral("batch/gba_directory")).toString();
  const QString legacyIcon = settings.value(QStringLiteral("paths/icon")).toString();
  const QString legacyName = settings.value(QStringLiteral("app/window_title")).toString();
  psxIconPath_ = settings.value(
    QStringLiteral("paths/psx_icon"),
    currentSystem_ == SystemKind::PlayStation ? legacyIcon : QString()).toString();
  gbaIconPath_ = settings.value(
    QStringLiteral("paths/gba_icon"),
    currentSystem_ == SystemKind::GameBoyAdvance ? legacyIcon : QString()).toString();
  psxName_ = settings.value(
    QStringLiteral("app/psx_name"),
    currentSystem_ == SystemKind::PlayStation ? legacyName : QString()).toString();
  gbaName_ = settings.value(
    QStringLiteral("app/gba_name"),
    currentSystem_ == SystemKind::GameBoyAdvance ? legacyName : QString()).toString();
  discEdit_->setText(currentSystem_ == SystemKind::GameBoyAdvance ? gbaInputPath_ : psxInputPath_);
  biosEdit_->setText(currentSystem_ == SystemKind::GameBoyAdvance ? gbaBiosPath_ : psxBiosPath_);
  batchCheck_->setChecked(settings.value(QStringLiteral("batch/enabled"), false).toBool());
  batchDirectoryEdit_->setText(currentSystem_ == SystemKind::GameBoyAdvance
    ? gbaBatchDirectory_ : psxBatchDirectory_);
  const QString platformKey = settings.value(
    QStringLiteral("app/platform"), targetPlatformKey(hostTargetPlatform())).toString();
  const int platformIndex = platformCombo_->findData(platformKey);
  platformCombo_->setCurrentIndex(platformIndex >= 0 ? platformIndex : 0);
  iconEdit_->setText(currentSystem_ == SystemKind::GameBoyAdvance
    ? gbaIconPath_ : psxIconPath_);
  outputEdit_->setText(settings.value(QStringLiteral("paths/output"),
                                      QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).toString());
  certificateEdit_->setText(settings.value(QStringLiteral("paths/certificate")).toString());
  signingEnabled_->setChecked(settings.value(QStringLiteral("signing/enabled"), false).toBool());
  ghidraEdit_->setText(settings.value(QStringLiteral("paths/ghidra"), detectGhidraHome()).toString());
  powerEngineEdit_->setText(
    settings.value(QStringLiteral("paths/powerengine"), detectPowerEngineRoot()).toString());
  llvmEdit_->setText(settings.value(QStringLiteral("paths/llvm"), detectLlvmRoot()).toString());
  titleEdit_->setText(currentSystem_ == SystemKind::GameBoyAdvance
    ? gbaName_ : psxName_);
  biosPatchEnabled_->setChecked(settings.value(QStringLiteral("bios_patch/enabled"), false).toBool());
  biosInitialSplashEdit_->setText(settings.value(QStringLiteral("bios_patch/initial_image")).toString());
  biosHandoffImageEdit_->setText(settings.value(QStringLiteral("bios_patch/handoff_image")).toString());
  biosMuteAudio_->setChecked(settings.value(QStringLiteral("bios_patch/mute_audio"), true).toBool());
  biosRemovePsGlyph_->setChecked(settings.value(QStringLiteral("bios_patch/remove_ps_glyph"), true).toBool());
  skipBiosBoot_->setChecked(settings.value(QStringLiteral("runtime/skip_bios_boot"), false).toBool());
  macosGipGamepad_->setChecked(
    settings.value(QStringLiteral("runtime/macos_gip_gamepad"), true).toBool());
  exportAsZip_->setChecked(settings.value(QStringLiteral("export/as_zip"), true).toBool());
  const QString exportMode = settings.value(
    QStringLiteral("export/mode"), exportModeKey(ExportMode::Build)).toString();
  const int exportModeIndex = exportModeCombo_->findData(exportMode);
  exportModeCombo_->setCurrentIndex(exportModeIndex >= 0 ? exportModeIndex : 1);
  useCi_->setChecked(settings.value(QStringLiteral("export/use_ci"), false).toBool());

  if (batchCheck_->isChecked() && QFileInfo(batchDirectoryEdit_->text()).isDir()) {
    populateBatchDirectory(batchDirectoryEdit_->text(), false);
  }
}

void MainWindow::saveSettings() const {
  QSettings settings;
  QString psxInput = psxInputPath_;
  QString psxBios = psxBiosPath_;
  QString psxBatch = psxBatchDirectory_;
  QString gbaInput = gbaInputPath_;
  QString gbaBios = gbaBiosPath_;
  QString gbaBatch = gbaBatchDirectory_;
  QString psxIcon = psxIconPath_;
  QString psxName = psxName_;
  QString gbaIcon = gbaIconPath_;
  QString gbaName = gbaName_;
  if (currentSystem_ == SystemKind::GameBoyAdvance) {
    gbaInput = discEdit_->text();
    gbaBios = biosEdit_->text();
    gbaBatch = batchDirectoryEdit_->text();
    gbaIcon = iconEdit_->text();
    gbaName = titleEdit_->text();
  } else {
    psxInput = discEdit_->text();
    psxBios = biosEdit_->text();
    psxBatch = batchDirectoryEdit_->text();
    psxIcon = iconEdit_->text();
    psxName = titleEdit_->text();
  }
  settings.setValue(QStringLiteral("app/system"), systemCombo_->currentData().toString());
  settings.setValue(QStringLiteral("app/platform"), platformCombo_->currentData().toString());
  settings.setValue(QStringLiteral("batch/enabled"), batchCheck_->isChecked());
  settings.setValue(QStringLiteral("batch/psx_directory"), psxBatch);
  settings.setValue(QStringLiteral("batch/gba_directory"), gbaBatch);
  settings.setValue(QStringLiteral("paths/psx_disc"), psxInput);
  settings.setValue(QStringLiteral("paths/psx_bios"), psxBios);
  settings.setValue(QStringLiteral("paths/gba_rom"), gbaInput);
  settings.setValue(QStringLiteral("paths/gba_bios"), gbaBios);
  settings.setValue(QStringLiteral("paths/psx_icon"), psxIcon);
  settings.setValue(QStringLiteral("paths/gba_icon"), gbaIcon);
  settings.setValue(QStringLiteral("app/psx_name"), psxName);
  settings.setValue(QStringLiteral("app/gba_name"), gbaName);
  // Keep the legacy keys synchronized for older Studio builds.
  settings.setValue(QStringLiteral("paths/icon"), iconEdit_->text());
  settings.setValue(QStringLiteral("paths/output"), outputEdit_->text());
  settings.setValue(QStringLiteral("paths/certificate"), certificateEdit_->text());
  settings.setValue(QStringLiteral("signing/enabled"), signingEnabled_->isChecked());
  settings.setValue(QStringLiteral("paths/ghidra"), ghidraEdit_->text());
  settings.setValue(QStringLiteral("paths/powerengine"), powerEngineEdit_->text());
  settings.setValue(QStringLiteral("paths/llvm"), llvmEdit_->text());
  settings.setValue(QStringLiteral("app/window_title"), titleEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/enabled"), biosPatchEnabled_->isChecked());
  settings.setValue(QStringLiteral("bios_patch/initial_image"), biosInitialSplashEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/handoff_image"), biosHandoffImageEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/mute_audio"), biosMuteAudio_->isChecked());
  settings.setValue(QStringLiteral("bios_patch/remove_ps_glyph"), biosRemovePsGlyph_->isChecked());
  settings.setValue(QStringLiteral("runtime/skip_bios_boot"), skipBiosBoot_->isChecked());
  settings.remove(QStringLiteral("runtime/pad_mode"));
  settings.setValue(QStringLiteral("runtime/macos_gip_gamepad"), macosGipGamepad_->isChecked());
  settings.remove(QStringLiteral("runtime/gba_virtua_native"));
  settings.setValue(QStringLiteral("export/as_zip"), exportAsZip_->isChecked());
  settings.setValue(QStringLiteral("export/mode"), exportModeCombo_->currentData().toString());
  settings.setValue(QStringLiteral("export/use_ci"), useCi_->isChecked());
  settings.sync();
}

void MainWindow::chooseDisc() {
  if (currentSystem_ == SystemKind::GameBoyAdvance) {
    const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Select Game Boy Advance ROM"),
      discEdit_->text().isEmpty() ? QDir::homePath()
                                  : QFileInfo(discEdit_->text()).absolutePath(),
      QStringLiteral("Game Boy Advance ROMs (*.gba *.GBA);;All files (*)"));
    if (path.isEmpty()) return;
    GbaDescription game;
    QString error;
    if (!inspectGbaRom(path, game, error)) {
      QMessageBox::warning(this, QStringLiteral("GBA ROM selection"), error);
      return;
    }
    if (!game.warning.isEmpty()) {
      QMessageBox::warning(this, QStringLiteral("Unusual GBA header"), game.warning);
    }
    discEdit_->setText(path);
    selectedBins_.clear();
    if (titleEdit_->text().isEmpty())
      titleEdit_->setText(game.title + QStringLiteral(" Recompiled"));
    return;
  }

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
  QString sourcePath = cue;
  if (sourcePath.isEmpty() && bins.size() == 1) {
    sourcePath = bins.constFirst();
  } else if (sourcePath.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Disc selection"),
                         QStringLiteral("Select one CUE set or one standalone BIN image."));
    return;
  }
  discEdit_->setText(sourcePath);
  selectedBins_ = bins;
  if (titleEdit_->text().isEmpty()) {
    titleEdit_->setText(QFileInfo(sourcePath).completeBaseName() + QStringLiteral(" Recompiled"));
  }
}
void MainWindow::chooseBatchDirectory() {
  const bool gba = currentSystem_ == SystemKind::GameBoyAdvance;
  const QString path = QFileDialog::getExistingDirectory(
    this, gba ? QStringLiteral("Select Game Boy Advance game directory")
              : QStringLiteral("Select PlayStation game directory"),
    batchDirectoryEdit_->text().isEmpty() ? QDir::homePath()
                                          : batchDirectoryEdit_->text());
  if (!path.isEmpty()) {
    populateBatchDirectory(path, true);
  }
}
void MainWindow::populateBatchDirectory(const QString& path, bool showDialogs) {
  if (currentSystem_ == SystemKind::GameBoyAdvance) {
    QList<GbaCatalogEntry> catalog;
    QStringList warnings;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool scanned = scanGbaDirectory(path, catalog, warnings, error);
    QApplication::restoreOverrideCursor();
    batchDirectoryEdit_->setText(path);
    batchEntries_.clear();
    if (!scanned) {
      batchSummaryLabel_->setText(error);
      rebuildBatchList();
      if (showDialogs)
        QMessageBox::warning(this, QStringLiteral("No GBA games found"), error);
      updateBuildButton();
      return;
    }
    QSettings settings;
    for (const auto& game : catalog) {
      const QString id = batchEntrySettingsId(game.sourcePath);
      const QString iconKey = batchIconSettingKey(id);
      QString savedIcon = settings.value(iconKey).toString();
      if (!savedIcon.isEmpty() && !QFileInfo(savedIcon).isFile()) {
        settings.remove(iconKey);
        savedIcon.clear();
      }
      const QString savedName = settings.value(
        batchNameSettingKey(id), game.suggestedTitle).toString();
      batchEntries_.append({ id, game.sourcePath, {}, savedName,
                             savedIcon, game.gameCode, QStringLiteral("GBA") });
    }
    batchSummaryLabel_->setText(
      warnings.isEmpty()
        ? QStringLiteral("%1 GBA game%2 found. Edit names and choose optional icons below.")
            .arg(batchEntries_.size()).arg(batchEntries_.size() == 1 ? QString() : QStringLiteral("s"))
        : QStringLiteral("%1 GBA game%2 found; %3 header warning%4 recorded.")
            .arg(batchEntries_.size()).arg(batchEntries_.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(warnings.size()).arg(warnings.size() == 1 ? QString() : QStringLiteral("s")));
    rebuildBatchList();
    if (showDialogs && !warnings.isEmpty()) {
      QMessageBox warning(QMessageBox::Warning, QStringLiteral("GBA header warnings"),
                          QStringLiteral("Studio found %1 buildable GBA image%2 with %3 warning%4.")
                            .arg(batchEntries_.size())
                            .arg(batchEntries_.size() == 1 ? QString() : QStringLiteral("s"))
                            .arg(warnings.size())
                            .arg(warnings.size() == 1 ? QString() : QStringLiteral("s")),
                          QMessageBox::Ok, this);
      warning.setDetailedText(warnings.join(QStringLiteral("\n")));
      warning.exec();
    }
    updateBuildButton();
    reflowForms(true);
    return;
  }

  QList<DiscCatalogEntry> catalog;
  QStringList warnings;
  QString error;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool scanned = DiscCatalog::scanDirectory(path, catalog, warnings, error);
  QApplication::restoreOverrideCursor();
  if (!scanned) {
    batchEntries_.clear();
    batchDirectoryEdit_->setText(path);
    batchSummaryLabel_->setText(error);
    rebuildBatchList();
    if (showDialogs) {
      QMessageBox::warning(this, QStringLiteral("No PlayStation games found"), error);
    }
    updateBuildButton();
    return;
  }

  batchDirectoryEdit_->setText(path);
  batchEntries_.clear();
  QSettings settings;
  for (const auto& disc : catalog) {
    const QString id = batchEntrySettingsId(disc.sourcePath);
    const QString iconKey = batchIconSettingKey(id);
    QString savedIcon = settings.value(iconKey).toString();
    if (!savedIcon.isEmpty() && !QFileInfo(savedIcon).isFile()) {
      settings.remove(iconKey);
      savedIcon.clear();
    }
    const QString savedName = settings.value(
      batchNameSettingKey(id), disc.suggestedTitle).toString();
    batchEntries_.append({
      id,
      disc.sourcePath,
      disc.selectedBinPaths,
      savedName,
      savedIcon,
      disc.serial,
      disc.volumeId,
    });
  }
  batchSummaryLabel_->setText(
    warnings.isEmpty()
      ? QStringLiteral("%1 game%2 found. Edit names and choose optional icons below.")
          .arg(batchEntries_.size())
          .arg(batchEntries_.size() == 1 ? QString() : QStringLiteral("s"))
      : QStringLiteral("%1 game%2 found; %3 unsupported item%4 skipped.")
          .arg(batchEntries_.size())
          .arg(batchEntries_.size() == 1 ? QString() : QStringLiteral("s"))
          .arg(warnings.size())
          .arg(warnings.size() == 1 ? QString() : QStringLiteral("s")));
  rebuildBatchList();
  if (showDialogs && !warnings.isEmpty()) {
    QMessageBox warning(QMessageBox::Warning, QStringLiteral("Some files were skipped"),
                        QStringLiteral("Studio found %1 buildable game%2 and skipped %3 unsupported item%4.")
                          .arg(batchEntries_.size())
                          .arg(batchEntries_.size() == 1 ? QString() : QStringLiteral("s"))
                          .arg(warnings.size())
                          .arg(warnings.size() == 1 ? QString() : QStringLiteral("s")),
                        QMessageBox::Ok, this);
    warning.setDetailedText(warnings.join(QStringLiteral("\n")));
    warning.exec();
  }
  updateBuildButton();
  reflowForms(true);
}

void MainWindow::rebuildBatchList() {
  batchList_->clear();
  const QIcon fallbackIcon(QStringLiteral(":/psxrecomp/studio/resources/psxrecomp-studio.svg"));
  for (const auto& entry : batchEntries_) {
    auto* item = new QListWidgetItem(batchList_);
    item->setSizeHint(QSize(0, 78));

    auto* row = new QFrame(batchList_);
    row->setObjectName(QStringLiteral("batchGameRow"));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(10);

    auto* thumbnail = new QLabel(row);
    thumbnail->setFixedSize(52, 52);
    thumbnail->setAlignment(Qt::AlignCenter);
    const QIcon selectedIcon = entry.iconPath.isEmpty() ? fallbackIcon : QIcon(entry.iconPath);
    thumbnail->setPixmap(selectedIcon.pixmap(48, 48));
    thumbnail->setToolTip(entry.iconPath.isEmpty()
      ? QStringLiteral("Built-in icon") : entry.iconPath);
    layout->addWidget(thumbnail);

    auto* text = new QWidget(row);
    auto* textLayout = new QVBoxLayout(text);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(3);
    auto* nameEdit = new QLineEdit(entry.title, text);
    nameEdit->setPlaceholderText(QStringLiteral("Game name"));
    QStringList details;
    if (!entry.serial.isEmpty()) details.append(entry.serial);
    if (!entry.volumeId.isEmpty() &&
        entry.volumeId.compare(entry.serial, Qt::CaseInsensitive) != 0) {
      details.append(entry.volumeId);
    }
    details.append(QFileInfo(entry.sourcePath).fileName());
    auto* detailLabel = new QLabel(details.join(QStringLiteral("  ·  ")), text);
    detailLabel->setObjectName(QStringLiteral("secondaryText"));
    detailLabel->setToolTip(entry.sourcePath);
    textLayout->addWidget(nameEdit);
    textLayout->addWidget(detailLabel);
    layout->addWidget(text, 1);

    auto* iconButton = new QPushButton(
      entry.iconPath.isEmpty() ? QStringLiteral("Choose Icon…") : QStringLiteral("Change Icon…"), row);
    iconButton->setToolTip(QStringLiteral("Optional PNG, SVG, or ICNS icon"));
    layout->addWidget(iconButton);
    QPushButton* clearButton = nullptr;
    if (!entry.iconPath.isEmpty()) {
      clearButton = new QPushButton(QStringLiteral("Use Default"), row);
      layout->addWidget(clearButton);
    }
    auto* removeButton = new QPushButton(QStringLiteral("Remove"), row);
    layout->addWidget(removeButton);

    const QString id = entry.id;
    connect(nameEdit, &QLineEdit::textChanged, this, [this, id](const QString& value) {
      for (auto& candidate : batchEntries_) {
        if (candidate.id == id) {
          candidate.title = value;
          QSettings settings;
          settings.setValue(batchNameSettingKey(id), value);
          settings.sync();
          break;
        }
      }
      updateBuildButton();
    });
    connect(iconButton, &QPushButton::clicked, this, [this, id]() { chooseBatchIcon(id); });
    if (clearButton) {
      connect(clearButton, &QPushButton::clicked, this, [this, id]() { clearBatchIcon(id); });
    }
    connect(removeButton, &QPushButton::clicked, this, [this, id]() { removeBatchEntry(id); });
    batchList_->setItemWidget(item, row);
  }
  applyTheme();
}

void MainWindow::chooseBatchIcon(const QString& id) {
  for (auto& entry : batchEntries_) {
    if (entry.id != id) {
      continue;
    }
    const QString start = entry.iconPath.isEmpty()
      ? QFileInfo(entry.sourcePath).absolutePath()
      : QFileInfo(entry.iconPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Select icon for %1").arg(entry.title), start,
      QStringLiteral("App icons (*.png *.svg *.icns);;All files (*)"));
    if (!path.isEmpty()) {
      entry.iconPath = path;
      QSettings settings;
      settings.setValue(batchIconSettingKey(entry.id), path);
      settings.sync();
      rebuildBatchList();
      updateBuildButton();
    }
    return;
  }
}

void MainWindow::clearBatchIcon(const QString& id) {
  for (auto& entry : batchEntries_) {
    if (entry.id == id) {
      entry.iconPath.clear();
      QSettings settings;
      settings.remove(batchIconSettingKey(entry.id));
      settings.sync();
      rebuildBatchList();
      updateBuildButton();
      return;
    }
  }
}

void MainWindow::removeBatchEntry(const QString& id) {
  for (qsizetype index = 0; index < batchEntries_.size(); ++index) {
    if (batchEntries_.at(index).id == id) {
      batchEntries_.removeAt(index);
      break;
    }
  }
  batchSummaryLabel_->setText(QStringLiteral("%1 game%2 queued. Icons are optional.")
    .arg(batchEntries_.size())
    .arg(batchEntries_.size() == 1 ? QString() : QStringLiteral("s")));
  rebuildBatchList();
  updateBuildButton();
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

void MainWindow::updateSystemControls() {
  const SystemKind selected = systemKindFromKey(systemCombo_->currentData().toString());
  if (selected != currentSystem_) {
    if (currentSystem_ == SystemKind::GameBoyAdvance) {
      gbaInputPath_ = discEdit_->text();
      gbaBiosPath_ = biosEdit_->text();
      gbaBatchDirectory_ = batchDirectoryEdit_->text();
      gbaIconPath_ = iconEdit_->text();
      gbaName_ = titleEdit_->text();
    } else {
      psxInputPath_ = discEdit_->text();
      psxBiosPath_ = biosEdit_->text();
      psxBatchDirectory_ = batchDirectoryEdit_->text();
      psxIconPath_ = iconEdit_->text();
      psxName_ = titleEdit_->text();
    }
    currentSystem_ = selected;
    {
      const QSignalBlocker inputBlocker(discEdit_);
      const QSignalBlocker biosBlocker(biosEdit_);
      const QSignalBlocker batchBlocker(batchDirectoryEdit_);
      const QSignalBlocker iconBlocker(iconEdit_);
      const QSignalBlocker nameBlocker(titleEdit_);
      discEdit_->setText(selected == SystemKind::GameBoyAdvance ? gbaInputPath_ : psxInputPath_);
      biosEdit_->setText(selected == SystemKind::GameBoyAdvance ? gbaBiosPath_ : psxBiosPath_);
      batchDirectoryEdit_->setText(selected == SystemKind::GameBoyAdvance
        ? gbaBatchDirectory_ : psxBatchDirectory_);
      iconEdit_->setText(selected == SystemKind::GameBoyAdvance ? gbaIconPath_ : psxIconPath_);
      titleEdit_->setText(selected == SystemKind::GameBoyAdvance ? gbaName_ : psxName_);
    }
    selectedBins_.clear();
    batchEntries_.clear();
    rebuildBatchList();
    if (batchCheck_->isChecked() && QFileInfo(batchDirectoryEdit_->text()).isDir())
      populateBatchDirectory(batchDirectoryEdit_->text(), false);
  }

  const bool gba = selected == SystemKind::GameBoyAdvance;
  headerTitle_->setText(gba ? QStringLiteral("Build a native Game Boy Advance app")
                            : QStringLiteral("Build a native PlayStation app"));
  headerSubtitle_->setText(gba
    ? QStringLiteral("One workflow for Ghidra-seeded ARM/THUMB static recompilation, native compilation, and platform packaging.")
    : QStringLiteral("One workflow for disc analysis, evidence-backed source generation, native compilation, and platform packaging."));
  if (discLabel_) discLabel_->setText(gba ? QStringLiteral("GBA ROM") : QStringLiteral("Disc BIN/CUE"));
  discEdit_->setPlaceholderText(gba ? QStringLiteral("One .gba cartridge image")
                                    : QStringLiteral("One .cue and all referenced .bin files"));
  if (biosLabel_) biosLabel_->setText(gba ? QStringLiteral("GBA BIOS (required)") : QStringLiteral("PlayStation BIOS"));
  biosEdit_->setPlaceholderText(gba ? QStringLiteral("Canonical 16 KiB dump; statically recompiled and dispatched")
                                    : QStringLiteral("Canonical SCPH1001.BIN only"));
  batchCheck_->setToolTip(gba
    ? QStringLiteral("Scan a directory recursively and queue one export for every .gba image.")
    : QStringLiteral("Scan a directory recursively and queue one export for every PlayStation CUE or standalone BIN image."));
  batchDirectoryEdit_->setPlaceholderText(gba
    ? QStringLiteral("Folder containing .gba cartridge images")
    : QStringLiteral("Folder containing PlayStation BIN/CUE files"));
  batchSummaryLabel_->setText(gba
    ? QStringLiteral("Choose a directory to create the GBA game list. Icons are optional.")
    : QStringLiteral("Choose a directory to create the game list. Icons are optional."));
  titleEdit_->setPlaceholderText(gba ? QStringLiteral("Example: Final Fantasy VI Advance Recompiled")
                                     : QStringLiteral("Example: Evil Zone Recompiled"));

  // Both systems now statically recompile from Ghidra-seeded discovery.
  ghidraEdit_->parentWidget()->setVisible(true);
  brandingCard_->setVisible(!gba);
  skipBiosBoot_->setVisible(!gba);
  padPolicyLabel_->setVisible(!gba);
  macosGipGamepad_->setVisible(
    targetPlatformFromKey(platformCombo_->currentData().toString()) == TargetPlatform::MacOS ||
     targetPlatformFromKey(platformCombo_->currentData().toString()) == TargetPlatform::All);
  if (gba) biosPatchEnabled_->setChecked(false);
  updateBatchMode();
  updatePlatformControls();
  reflowForms(true);
  updateBuildButton();
}

void MainWindow::updateBiosPatchControls() {
  const bool enabled = biosPatchEnabled_ && biosPatchEnabled_->isChecked();
  if (biosInitialSplashEdit_) biosInitialSplashEdit_->parentWidget()->setEnabled(enabled);
  if (biosHandoffImageEdit_) biosHandoffImageEdit_->parentWidget()->setEnabled(enabled);
  if (biosMuteAudio_) biosMuteAudio_->setEnabled(enabled);
  if (biosRemovePsGlyph_) biosRemovePsGlyph_->setEnabled(enabled);
  updateBuildButton();
}

void MainWindow::updateBatchMode() {
  const bool batch = batchCheck_->isChecked();
  discEdit_->parentWidget()->setVisible(!batch);
  iconEdit_->parentWidget()->setVisible(!batch);
  titleEdit_->parentWidget()->setVisible(!batch);
  batchDirectoryEdit_->parentWidget()->setVisible(batch);
  batchSummaryLabel_->setVisible(batch);
  batchList_->setVisible(batch);
  updatePlatformControls();
  reflowForms(true);
}

void MainWindow::updateExportMode() {
  const bool buildMode = exportModeFromKey(exportModeCombo_->currentData().toString()) ==
                         ExportMode::Build;
  useCi_->setEnabled(buildMode && !cancelButton_->isEnabled());
  updatePlatformControls();
}

void MainWindow::chooseBios() {
  const bool gba = currentSystem_ == SystemKind::GameBoyAdvance;
  const auto path = QFileDialog::getOpenFileName(
    this, gba ? QStringLiteral("Select 16 KiB GBA BIOS (required for package)")
              : QStringLiteral("Select SCPH1001.BIN"),
    QFileInfo(biosEdit_->text()).absolutePath(),
    gba ? QStringLiteral("GBA BIOS (*.bin *.BIN);;All files (*)")
        : QStringLiteral("SCPH1001 BIOS (SCPH1001.BIN);;BIN files (*.BIN *.bin)"));
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

void MainWindow::choosePowerEngineRoot() {
  const auto path = QFileDialog::getExistingDirectory(
    this, QStringLiteral("Select PowerEngine source directory"), powerEngineEdit_->text());
  if (!path.isEmpty()) {
    powerEngineEdit_->setText(path);
    if (llvmEdit_->text().isEmpty()) llvmEdit_->setText(detectLlvmRoot());
  }
}

void MainWindow::chooseLlvmRoot() {
  const auto path = QFileDialog::getExistingDirectory(
    this, QStringLiteral("Select PowerEngine LLVM/compiler bundle"), llvmEdit_->text());
  if (!path.isEmpty()) {
    llvmEdit_->setText(path);
  }
}

PipelineRequest MainWindow::requestFromUi(bool overwrite) const {
  PipelineRequest request;
  request.system = systemKindFromKey(systemCombo_->currentData().toString());
  request.targetPlatform = targetPlatformFromKey(platformCombo_->currentData().toString());
  request.exportMode = exportModeFromKey(exportModeCombo_->currentData().toString());
  request.buildBackend = BuildBackend::Local;
  request.useCi = request.exportMode == ExportMode::Build && useCi_->isChecked();
  if (request.system == SystemKind::GameBoyAdvance) {
    request.romPath = discEdit_->text();
  } else {
    request.cuePath = discEdit_->text();
    request.selectedBinPaths = selectedBins_;
  }
  request.biosPath = biosEdit_->text();
  request.iconPath = iconEdit_->text();
  request.windowTitle = titleEdit_->text();
  request.outputDirectory = outputEdit_->text();
  if (request.exportMode == ExportMode::Build && signingEnabled_->isChecked()) {
    request.certificatePath = certificateEdit_->text();
    request.certificatePassword = certificatePasswordEdit_->text();
  }
  request.ghidraHome = ghidraEdit_->text();
  request.llvmRoot = llvmEdit_->text();
  request.powerEngineRoot = powerEngineEdit_->text();
  request.frameworkRoot = QString::fromUtf8(PSXRECOMP_SOURCE_ROOT);
  request.patchBiosBranding = request.system == SystemKind::PlayStation &&
                               biosPatchEnabled_->isChecked();
  request.biosInitialSplashPath = biosInitialSplashEdit_->text();
  request.biosHandoffImagePath = biosHandoffImageEdit_->text();
  request.biosMuteBootAudio = biosMuteAudio_->isChecked();
  request.biosRemoveStockPsGlyph = biosRemovePsGlyph_->isChecked();
  request.skipBiosBoot = request.system == SystemKind::PlayStation &&
                         skipBiosBoot_->isChecked();
  request.macosGipGamepad = macosGipGamepad_->isChecked();
  request.exportAsZip = exportAsZip_->isChecked();
  request.overwriteOutput = overwrite;
  return request;
}

QList<PipelineRequest> MainWindow::requestsFromUi(bool overwrite) const {
  QList<PipelineRequest> requests;
  const PipelineRequest base = requestFromUi(overwrite);
  const auto targets = concreteTargetPlatforms(base.targetPlatform);
  if (batchCheck_->isChecked()) {
    for (const auto& entry : batchEntries_) {
      for (const auto target : targets) {
        PipelineRequest request = base;
        request.targetPlatform = target;
        if (target != TargetPlatform::MacOS) {
          request.certificatePath.clear();
          request.certificatePassword.clear();
        }
        if (request.system == SystemKind::GameBoyAdvance) {
          request.romPath = entry.sourcePath;
          request.cuePath.clear();
          request.selectedBinPaths.clear();
        } else {
          request.cuePath = entry.sourcePath;
          request.romPath.clear();
          request.selectedBinPaths = entry.selectedBinPaths;
        }
        request.iconPath = entry.iconPath;
        request.windowTitle = entry.title.trimmed();
        requests.append(request);
      }
    }
  } else {
    for (const auto target : targets) {
      PipelineRequest request = base;
      request.targetPlatform = target;
      if (target != TargetPlatform::MacOS) {
        request.certificatePath.clear();
        request.certificatePassword.clear();
      }
      requests.append(request);
    }
  }
  return requests;
}

QString MainWindow::outputPathForRequest(const PipelineRequest& request) const {
  return QDir(request.outputDirectory).filePath(exportOutputName(request));
}

void MainWindow::planBuildBackends(QList<PipelineRequest>& requests) {
  for (auto& request : requests) {
    request.buildBackend = BuildBackend::Local;
    request.ciBuilderId.clear();
    request.ciBuilderName.clear();
    request.ciBuilderEndpoint.clear();
    request.ciArchitecture.clear();
  }
  if (requests.isEmpty() || requests.constFirst().exportMode != ExportMode::Build ||
      !requests.constFirst().useCi || !ciPanel_ || !ciPanel_->canScheduleBuilds()) {
    return;
  }

  int reservedLocal = -1;
  for (int index = 0; index < requests.size(); ++index) {
    if (requests.at(index).targetPlatform == hostTargetPlatform() &&
        targetPlatformSupportedOnHost(requests.at(index).targetPlatform)) {
      reservedLocal = index;
      break;
    }
  }
  if (reservedLocal < 0) {
    for (int index = 0; index < requests.size(); ++index) {
      if (targetPlatformSupportedOnHost(requests.at(index).targetPlatform)) {
        reservedLocal = index;
        break;
      }
    }
  }

  QString hostArchitecture = QSysInfo::currentCpuArchitecture().toLower();
  if (hostArchitecture == QStringLiteral("amd64") ||
      hostArchitecture == QStringLiteral("x64") ||
      hostArchitecture == QStringLiteral("x86-64")) {
    hostArchitecture = QStringLiteral("x86_64");
  } else if (hostArchitecture == QStringLiteral("aarch64")) {
    hostArchitecture = QStringLiteral("arm64");
  }
  for (int index = 0; index < requests.size(); ++index) {
    auto& request = requests[index];
    const bool requiresLocalSigning = request.targetPlatform == TargetPlatform::MacOS &&
                                      !request.certificatePath.isEmpty();
    if (index == reservedLocal || requiresLocalSigning) continue;
    const QString preferredArchitecture =
      request.targetPlatform == TargetPlatform::Windows ||
      request.targetPlatform == TargetPlatform::Linux
        ? QStringLiteral("x86_64")
        : request.targetPlatform == hostTargetPlatform()
            ? hostArchitecture : QString();
    const auto builder = ciPanel_->chooseBuilder(request.targetPlatform,
                                                  preferredArchitecture);
    if (!builder.isValid() ||
        (request.targetPlatform == hostTargetPlatform() &&
         builder.architecture != hostArchitecture) ||
        ((request.targetPlatform == TargetPlatform::Windows ||
          request.targetPlatform == TargetPlatform::Linux) &&
         builder.architecture != QStringLiteral("x86_64"))) {
      continue;
    }
    request.buildBackend = BuildBackend::RemoteCi;
    request.ciBuilderId = builder.id;
    request.ciBuilderName = builder.name;
    request.ciBuilderEndpoint = builder.endpoint.toString(QUrl::FullyEncoded);
    request.ciArchitecture = builder.architecture;
  }

  std::stable_sort(requests.begin(), requests.end(), [](const PipelineRequest& left,
                                                        const PipelineRequest& right) {
    return left.buildBackend == BuildBackend::RemoteCi &&
           right.buildBackend != BuildBackend::RemoteCi;
  });
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

  auto requests = requestsFromUi(false);
  if (requests.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Nothing to export"),
                         QStringLiteral("Add at least one game to the batch list."));
    return;
  }
  if (requests.constFirst().useCi && !requests.constFirst().exportAsZip) {
    QMessageBox::warning(this, QStringLiteral("CI builds are ZIP artifacts"),
      QStringLiteral("Enable Export as zip before using CI. Steganos builders return verified ZIP artifacts."));
    return;
  }

  planBuildBackends(requests);
  QStringList unavailableTargets;
  for (const auto& request : requests) {
    if (request.exportMode == ExportMode::Build &&
        request.buildBackend == BuildBackend::Local &&
        !targetPlatformSupportedOnHost(request.targetPlatform)) {
      unavailableTargets.append(targetPlatformDisplayName(request.targetPlatform));
    }
  }
  unavailableTargets.removeDuplicates();
  if (!unavailableTargets.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("CI builder unavailable"),
      QStringLiteral("This host cannot build %1 locally, and no matching authenticated CI worker is available.")
        .arg(unavailableTargets.join(QStringLiteral(", "))));
    return;
  }

  QSet<QString> uniqueOutputs;
  QStringList duplicateOutputs;
  QStringList existingOutputs;
  for (const auto& request : requests) {
    const QString outputPath = outputPathForRequest(request);
    const QString folded = QDir::cleanPath(outputPath).toCaseFolded();
    if (uniqueOutputs.contains(folded)) duplicateOutputs.append(outputPath);
    else uniqueOutputs.insert(folded);
    if (QFileInfo::exists(outputPath)) existingOutputs.append(outputPath);
  }
  duplicateOutputs.removeDuplicates();
  if (!duplicateOutputs.isEmpty()) {
    QMessageBox::warning(
      this, QStringLiteral("Duplicate game names"),
      QStringLiteral("Two or more queued exports resolve to the same output path. Rename the duplicate games before exporting.\n\n%1")
        .arg(duplicateOutputs.join(QStringLiteral("\n"))));
    return;
  }

  bool overwrite = false;
  if (!existingOutputs.isEmpty()) {
    const auto answer = QMessageBox::question(
      this, QStringLiteral("Replace existing outputs?"),
      QStringLiteral("%1 queued output%2 already exist%3. Replace %4 with the new export%5?")
        .arg(existingOutputs.size())
        .arg(existingOutputs.size() == 1 ? QString() : QStringLiteral("s"))
        .arg(existingOutputs.size() == 1 ? QStringLiteral("s") : QString())
        .arg(existingOutputs.size() == 1 ? QStringLiteral("it") : QStringLiteral("them"))
        .arg(existingOutputs.size() == 1 ? QString() : QStringLiteral("s")),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    overwrite = true;
  }
  for (auto& request : requests) request.overwriteOutput = overwrite;

  saveSettings();
  pendingRequests_ = requests;
  completedOutputs_.clear();
  activeCiJobs_.clear();
  totalRequestCount_ = pendingRequests_.size();
  activeRequestIndex_ = 0;
  outputAppPath_.clear();
  workerActive_ = false;
  exportFailed_ = false;
  cancelling_ = false;
  revealButton_->setEnabled(false);
  logView_->clear();

  int ciCount = 0;
  for (const auto& request : std::as_const(pendingRequests_)) {
    if (request.buildBackend == BuildBackend::RemoteCi) ++ciCount;
    logView_->appendPlainText(QStringLiteral("Plan · %1 · %2 · %3")
      .arg(request.windowTitle,
           targetPlatformDisplayName(request.targetPlatform),
           request.buildBackend == BuildBackend::RemoteCi
             ? QStringLiteral("CI: %1/%2").arg(request.ciBuilderName, request.ciArchitecture)
             : request.exportMode == ExportMode::Source
                 ? QStringLiteral("local source export")
                 : QStringLiteral("local build")));
  }
  if (ciCount > 0) {
    logView_->appendPlainText(QStringLiteral("%1 build%2 will be prepared for CI; one compatible slot remains local.")
      .arg(ciCount).arg(ciCount == 1 ? QString() : QStringLiteral("s")));
  }
  setBusy(true);
  startNextRequest();
}

void MainWindow::startNextRequest() {
  if (workerActive_) return;
  if (pendingRequests_.isEmpty()) {
    finishIfIdle();
    return;
  }
  activeRequest_ = pendingRequests_.takeFirst();
  ++activeRequestIndex_;
  workerActive_ = true;
  if (totalRequestCount_ > 1) {
    logView_->appendPlainText(
      QStringLiteral("\n##### Export %1 of %2 — %3 (%4) #####")
        .arg(activeRequestIndex_).arg(totalRequestCount_)
        .arg(activeRequest_.windowTitle,
             targetPlatformDisplayName(activeRequest_.targetPlatform)));
  }
  emit runRequested(activeRequest_);
}

void MainWindow::cancelBuild() {
  if (cancelling_) return;
  cancelling_ = true;
  pendingRequests_.clear();
  cancelButton_->setEnabled(false);
  stageLabel_->setText(QStringLiteral("Cancelling local and CI work…"));
  if (workerActive_ && worker_) worker_->requestCancel();
  if (ciPanel_) ciPanel_->cancelAll();
  finishIfIdle();
}

void MainWindow::revealOutput() {
  if (outputAppPath_.isEmpty()) {
    return;
  }
#if defined(Q_OS_MACOS)
  QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                          { QStringLiteral("-R"), outputAppPath_ });
#elif defined(Q_OS_WIN)
  if (QFileInfo(outputAppPath_).isDir()) {
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            { QDir::toNativeSeparators(outputAppPath_) });
  } else {
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            { QStringLiteral("/select,%1")
                                .arg(QDir::toNativeSeparators(outputAppPath_)) });
  }
#else
  const QString directory = QFileInfo(outputAppPath_).isDir()
    ? outputAppPath_ : QFileInfo(outputAppPath_).absolutePath();
  QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
#endif
}

void MainWindow::onCompleted(const QString& appPath) {
  workerActive_ = false;
  if (exportFailed_ || cancelling_) {
    finishIfIdle();
    return;
  }
  completedOutputs_.append(appPath);
  startNextRequest();
}

void MainWindow::onCiSourcePrepared(PipelineRequest request,
                                    const QString& repositoryPath,
                                    const QString& commit) {
  workerActive_ = false;
  if (exportFailed_ || cancelling_) {
    QDir(repositoryPath).removeRecursively();
    finishIfIdle();
    return;
  }
  ci::BuilderInfo builder;
  builder.id = request.ciBuilderId;
  builder.name = request.ciBuilderName;
  builder.endpoint = QUrl(request.ciBuilderEndpoint);
  builder.platform = targetPlatformKey(request.targetPlatform);
  builder.architecture = request.ciArchitecture;
  builder.online = true;
  const QString jobId = ciPanel_->queueBuild(
    request, builder, repositoryPath, commit, outputPathForRequest(request));
  if (jobId.isEmpty()) {
    QDir(repositoryPath).removeRecursively();
    onFailed(QStringLiteral("The generated source repository could not be queued with the selected CI builder."),
             repositoryPath);
    return;
  }
  activeCiJobs_.insert(jobId);
  startNextRequest();
}

void MainWindow::onCiBuildCompleted(const QString& jobId, const QString& outputPath) {
  if (!activeCiJobs_.remove(jobId) || exportFailed_) return;
  completedOutputs_.append(outputPath);
  finishIfIdle();
}

void MainWindow::onCiBuildFailed(const QString& jobId,
                                 const QString& message,
                                 const QString& reportPath) {
  if (!activeCiJobs_.remove(jobId) || exportFailed_ || cancelling_) return;
  QString detail = message;
  if (!reportPath.isEmpty()) {
    detail += QStringLiteral("\n\nCI report: %1").arg(reportPath);
  }
  onFailed(detail, {});
}

void MainWindow::onCiBuildCancelled(const QString& jobId) {
  activeCiJobs_.remove(jobId);
  finishIfIdle();
}

void MainWindow::finishIfIdle() {
  if (workerActive_ || !pendingRequests_.isEmpty() || !activeCiJobs_.isEmpty()) return;
  if (cancelling_) {
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    stageLabel_->setText(QStringLiteral("Cancelled"));
    logView_->appendPlainText(QStringLiteral("Export cancelled."));
    cancelling_ = false;
    setBusy(false);
    return;
  }
  if (!exportFailed_) finishSuccessfulExport();
}

void MainWindow::finishSuccessfulExport() {
  outputAppPath_ = completedOutputs_.size() == 1
    ? completedOutputs_.constFirst() : outputEdit_->text();
  progressBar_->setRange(0, 100);
  progressBar_->setValue(100);
  const bool sourceMode = exportModeFromKey(exportModeCombo_->currentData().toString()) ==
                          ExportMode::Source;
  const QString noun = sourceMode ? QStringLiteral("source repository")
                                  : QStringLiteral("package");
  stageLabel_->setText(completedOutputs_.size() == 1
    ? QStringLiteral("Complete — %1 verified").arg(noun)
    : QStringLiteral("Complete — %1 exports verified").arg(completedOutputs_.size()));
  revealButton_->setEnabled(!completedOutputs_.isEmpty());
  setBusy(false);

  QStringList displayedOutputs = completedOutputs_;
  if (displayedOutputs.size() > 12) {
    const int omitted = displayedOutputs.size() - 12;
    displayedOutputs = displayedOutputs.mid(0, 12);
    displayedOutputs.append(QStringLiteral("…and %1 more in %2")
                              .arg(omitted).arg(outputEdit_->text()));
  }
  QMessageBox::information(
    this, completedOutputs_.size() == 1 ? QStringLiteral("Export complete")
                                        : QStringLiteral("Batch export complete"),
    QStringLiteral("%1 %2%3 created and verified:\n\n%4")
      .arg(completedOutputs_.size())
      .arg(noun)
      .arg(completedOutputs_.size() == 1 ? QString() : QStringLiteral("s"))
      .arg(displayedOutputs.join(QStringLiteral("\n"))));
}

void MainWindow::onFailed(const QString& message, const QString& workspacePath) {
  if (exportFailed_) return;
  exportFailed_ = true;
  workerActive_ = false;
  pendingRequests_.clear();
  if (worker_) worker_->requestCancel();
  if (ciPanel_) ciPanel_->cancelAll();
  activeCiJobs_.clear();
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  stageLabel_->setText(QStringLiteral("Export failed"));
  setBusy(false);
  QString detail = message;
  if (!workspacePath.isEmpty()) {
    detail += QStringLiteral("\n\nProof and intermediate artifacts were retained at:\n%1").arg(workspacePath);
  }
  if (!completedOutputs_.isEmpty()) {
    detail += QStringLiteral("\n\n%1 earlier export%2 completed before this failure:\n%3")
      .arg(completedOutputs_.size())
      .arg(completedOutputs_.size() == 1 ? QString() : QStringLiteral("s"))
      .arg(completedOutputs_.join(QStringLiteral("\n")));
  }
  QMessageBox::critical(this, QStringLiteral("Export failed"), detail);
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
  for (auto* edit : { discEdit_, batchDirectoryEdit_, biosEdit_, iconEdit_, titleEdit_, outputEdit_,
                      certificateEdit_, certificatePasswordEdit_, ghidraEdit_, powerEngineEdit_, llvmEdit_,
                      biosInitialSplashEdit_, biosHandoffImageEdit_ }) {
    edit->parentWidget()->setEnabled(!busy);
  }
  biosPatchEnabled_->setEnabled(!busy);
  batchCheck_->setEnabled(!busy);
  batchList_->setEnabled(!busy);
  signingEnabled_->setEnabled(!busy);
  systemCombo_->setEnabled(!busy);
  platformCombo_->setEnabled(!busy);
  skipBiosBoot_->setEnabled(!busy);
  macosGipGamepad_->setEnabled(!busy);
  exportAsZip_->setEnabled(!busy);
  exportModeCombo_->setEnabled(!busy);
  useCi_->setEnabled(!busy &&
    exportModeFromKey(exportModeCombo_->currentData().toString()) == ExportMode::Build);
  biosMuteAudio_->setEnabled(!busy && biosPatchEnabled_->isChecked());
  biosRemovePsGlyph_->setEnabled(!busy && biosPatchEnabled_->isChecked());
  if (!busy) {
    updatePlatformControls();
    updateBiosPatchControls();
  }
  updateBuildButton();
}

void MainWindow::updatePlatformControls() {
  // A GBA package has no Virtua ARM form yet, so the entry is greyed out rather
  // than left selectable and failing once the pipeline has already started.
  {
    const int virtuaIndex =
      platformCombo_->findData(targetPlatformKey(TargetPlatform::VirtuaArm));
    if (virtuaIndex >= 0) {
      const bool selectable = currentSystem_ != SystemKind::GameBoyAdvance;
      platformCombo_->setItemData(
        virtuaIndex,
        selectable ? QVariant(Qt::ItemIsSelectable | Qt::ItemIsEnabled) : QVariant(0),
        Qt::UserRole - 1);
      if (!selectable && platformCombo_->currentIndex() == virtuaIndex) {
        const QSignalBlocker blocker(platformCombo_);
        platformCombo_->setCurrentIndex(
          platformCombo_->findData(targetPlatformKey(hostTargetPlatform())));
      }
    }
  }
  const auto selectedPlatform =
    targetPlatformFromKey(platformCombo_->currentData().toString());
  const ExportMode exportMode =
    exportModeFromKey(exportModeCombo_->currentData().toString());
  const bool buildMode = exportMode == ExportMode::Build;
  const bool includesMacos = selectedPlatform == TargetPlatform::MacOS ||
                             selectedPlatform == TargetPlatform::All;
  const bool busy = cancelButton_ && cancelButton_->isEnabled();
  const bool signingRequested = buildMode && includesMacos &&
                                signingEnabled_->isChecked();
  const bool zip = exportAsZip_->isChecked();

  signingEnabled_->setVisible(buildMode && includesMacos);
  signingEnabled_->setEnabled(buildMode && includesMacos && !busy);
  certificateEdit_->parentWidget()->setVisible(signingRequested);
  certificatePasswordEdit_->parentWidget()->setVisible(signingRequested);
  certificateEdit_->parentWidget()->setEnabled(signingRequested && !busy);
  certificatePasswordEdit_->parentWidget()->setEnabled(signingRequested && !busy);
  macosGipGamepad_->setVisible(includesMacos);
  macosGipGamepad_->setEnabled(includesMacos && !busy);
  // Virtua consumes PowerEngine in place and uses its LLVM/compiler bundle;
  // neither dependency is duplicated into the generated framework.
  const bool needsVirtuaRoots = selectedPlatform == TargetPlatform::VirtuaArm ||
                                selectedPlatform == TargetPlatform::All;
  powerEngineEdit_->parentWidget()->setVisible(needsVirtuaRoots);
  powerEngineEdit_->parentWidget()->setEnabled(needsVirtuaRoots && !busy);
  llvmEdit_->parentWidget()->setVisible(needsVirtuaRoots);
  llvmEdit_->parentWidget()->setEnabled(needsVirtuaRoots && !busy);
  biosEdit_->parentWidget()->setVisible(true);
  useCi_->setEnabled(buildMode && selectedPlatform != TargetPlatform::VirtuaArm && !busy);
  if (selectedPlatform == TargetPlatform::VirtuaArm && useCi_->isChecked())
    useCi_->setChecked(false);
  exportAsZip_->setText(exportMode == ExportMode::Source
    ? QStringLiteral("Export source as zip") : QStringLiteral("Export as zip"));

  if (!buildMode) {
    signingNote_->setText(QStringLiteral(
      "Source export creates a portable, initialized Git repository with generated C, "
      "runtime sources, package resources, CMake build rules, and proof artifacts. CI is not used."));
    outputEdit_->setPlaceholderText(zip
      ? QStringLiteral("Destination for source repository ZIPs")
      : QStringLiteral("Destination for source Git repositories"));
  } else if (selectedPlatform == TargetPlatform::All) {
    signingNote_->setText(signingRequested
      ? QStringLiteral("Signed macOS builds stay local. Unsigned Windows/Linux work can be dispatched to CI in parallel.")
      : QStringLiteral("Local and authenticated CI builders can co-operate on the three platform packages."));
    outputEdit_->setPlaceholderText(zip
      ? QStringLiteral("Destination for per-game macOS, Windows, and Linux ZIPs")
      : QStringLiteral("Destination for macOS, Windows, and Linux packages"));
  } else if (selectedPlatform == TargetPlatform::MacOS) {
    signingNote_->setText(signingRequested
      ? QStringLiteral("The PFX is read directly by rcodesign. Signed macOS builds are kept on this host.")
      : QStringLiteral("Signing is optional. Additional batch builds can use an authenticated macOS CI worker."));
    outputEdit_->setPlaceholderText(zip
      ? QStringLiteral("Destination for the macOS ZIP")
      : QStringLiteral("Destination for the macOS .app"));
  } else if (selectedPlatform == TargetPlatform::Windows) {
    signingNote_->setText(QStringLiteral(
      "Windows builds use the local native/cross toolchain or an authenticated Windows CI worker."));
    outputEdit_->setPlaceholderText(zip
      ? QStringLiteral("Destination for the Windows ZIP")
      : QStringLiteral("Destination for the Windows app folder"));
  } else if (selectedPlatform == TargetPlatform::VirtuaArm) {
    signingNote_->setText(currentSystem_ == SystemKind::GameBoyAdvance
      ? QStringLiteral(
          "Game Boy Advance titles cannot target Virtua ARM yet: the bundled Virtua SDL "
          "surface does not cover the gbarecomp host layer.")
      : QStringLiteral(
          "Virtua ARM builds link the recompiled PlayStation program into a cooperative "
          "ARMv7 .virtua executable."));
    outputEdit_->setPlaceholderText(zip
      ? QStringLiteral("Destination for the Virtua ARM ZIP")
      : QStringLiteral("Destination for the Virtua ARM package folder"));
  } else {
    signingNote_->setText(QStringLiteral(
      "Linux builds use the local native/cross toolchain or an authenticated Linux CI worker."));
    outputEdit_->setPlaceholderText(zip
      ? QStringLiteral("Destination for the Linux ZIP")
      : QStringLiteral("Destination for the Linux app folder"));
  }
  updateBuildButton();
}

void MainWindow::updateBuildButton() {
  if (cancelButton_->isEnabled()) {
    return;
  }
  const ExportMode exportMode =
    exportModeFromKey(exportModeCombo_->currentData().toString());
  const bool buildMode = exportMode == ExportMode::Build;
  const bool brandingReady = currentSystem_ == SystemKind::GameBoyAdvance ||
    !biosPatchEnabled_->isChecked() ||
    (!biosInitialSplashEdit_->text().isEmpty() && !biosHandoffImageEdit_->text().isEmpty());
  const auto selectedPlatform = targetPlatformFromKey(platformCombo_->currentData().toString());
  const bool includesMacos = selectedPlatform == TargetPlatform::MacOS ||
                             selectedPlatform == TargetPlatform::All;
  const bool signingReady = !buildMode || !includesMacos || !signingEnabled_->isChecked() ||
    (!certificateEdit_->text().isEmpty() && !certificatePasswordEdit_->text().isEmpty());
  bool gameReady = !discEdit_->text().isEmpty() && !titleEdit_->text().trimmed().isEmpty();
  if (batchCheck_->isChecked()) {
    gameReady = !batchEntries_.isEmpty() && std::all_of(
      batchEntries_.cbegin(), batchEntries_.cend(), [](const BatchGameEntry& entry) {
        return !entry.title.trimmed().isEmpty();
      });
  }
  const bool ciReady = !buildMode || !useCi_->isChecked() ||
                       (ciPanel_ && ciPanel_->canScheduleBuilds());
  const auto selectedTargets = concreteTargetPlatforms(selectedPlatform);
  const bool localPlatformReady = !buildMode || useCi_->isChecked() ||
    std::all_of(selectedTargets.cbegin(), selectedTargets.cend(),
                [this](TargetPlatform platform) {
                  if (currentSystem_ == SystemKind::GameBoyAdvance &&
                      platform != TargetPlatform::VirtuaArm) {
                    return platform == hostTargetPlatform();
                  }
                  return targetPlatformSupportedOnHost(platform);
                });
  const bool analysisReady = !ghidraEdit_->text().isEmpty();
  const bool biosReady = !biosEdit_->text().isEmpty();
  const bool needsVirtuaRoots = selectedPlatform == TargetPlatform::VirtuaArm ||
                                selectedPlatform == TargetPlatform::All;
  const bool virtuaRootsReady = !needsVirtuaRoots ||
    (!powerEngineEdit_->text().trimmed().isEmpty() &&
     !llvmEdit_->text().trimmed().isEmpty());
  const bool ready = gameReady && biosReady && virtuaRootsReady &&
                     !outputEdit_->text().isEmpty() && analysisReady &&
                     signingReady && brandingReady && ciReady && localPlatformReady;
  buildButton_->setText(QStringLiteral("Export"));
  buildButton_->setEnabled(ready);
}


} // namespace psxstudio
