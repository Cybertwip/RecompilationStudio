#include "MainWindow.h"

#include "DiscCatalog.h"
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
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QUuid>

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
  platformCombo_->addItem(QStringLiteral("All"), targetPlatformKey(TargetPlatform::All));
  platformCombo_->addItem(QStringLiteral("macOS"), targetPlatformKey(TargetPlatform::MacOS));
  platformCombo_->addItem(QStringLiteral("Windows"), targetPlatformKey(TargetPlatform::Windows));
  platformCombo_->addItem(QStringLiteral("Linux"), targetPlatformKey(TargetPlatform::Linux));
#else
  const auto hostPlatform = hostTargetPlatform();
  platformCombo_->addItem(targetPlatformDisplayName(hostPlatform),
                          targetPlatformKey(hostPlatform));
#endif
  configureReadOnlyComboBox(platformCombo_);
  platformCombo_->setCurrentIndex(0);
  platformLayout->addWidget(platformLabel);
  platformLayout->addWidget(platformCombo_, 1);
  inputLayout->addWidget(platformRow);
  platformRow->setVisible(hostCanSelectTargetPlatform());

  batchCheck_ = new QCheckBox(QStringLiteral("Batch"), inputCard_);
  batchCheck_->setToolTip(
    QStringLiteral("Scan a directory recursively and queue one export for every PlayStation CUE or standalone BIN image."));
  inputLayout->addWidget(batchCheck_);

  discEdit_ = addPathRow(inputCard_, inputLayout, QStringLiteral("Disc BIN/CUE"),
                         QStringLiteral("One .cue and all referenced .bin files"), SLOT(chooseDisc()));
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

  auto* padPolicy = new QLabel(
    QStringLiteral("Controller: automatic D-Pad first, Hybrid fallback"), inputCard_);
  padPolicy->setObjectName(QStringLiteral("secondaryText"));
  padPolicy->setWordWrap(true);
  padPolicy->setToolTip(
    QStringLiteral("Each game negotiates its controller at runtime. A real D-Pad is "
                   "presented first; Hybrid is enabled only if the game rejects it."));
  inputLayout->addWidget(padPolicy);

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
  connect(worker_, &PipelineWorker::failed, this, &MainWindow::onFailed);
  connect(worker_, &PipelineWorker::cancelled, this, [this]() {
    pendingRequests_.clear();
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
  connect(batchCheck_, &QCheckBox::toggled, this, &MainWindow::updateBatchMode);
  connect(signingEnabled_, &QCheckBox::toggled, this, &MainWindow::updatePlatformControls);
  connect(biosPatchEnabled_, &QCheckBox::toggled, this, &MainWindow::updateBiosPatchControls);
  connect(skipBiosBoot_, &QCheckBox::toggled, this, &MainWindow::updateBuildButton);
  connect(macosGipGamepad_, &QCheckBox::toggled, this, &MainWindow::updateBuildButton);
  connect(themeManager_, &oclero::qlementine::ThemeManager::currentThemeChanged,
          this, [this]() {
            QSettings().setValue(QStringLiteral("app/theme"), themeManager_->currentTheme());
            applyTheme();
          });
  for (auto* edit : { discEdit_, batchDirectoryEdit_, biosEdit_, iconEdit_, titleEdit_, outputEdit_,
                      certificateEdit_, certificatePasswordEdit_, ghidraEdit_,
                      biosInitialSplashEdit_, biosHandoffImageEdit_ }) {
    connect(edit, &QLineEdit::textChanged, this, &MainWindow::updateBuildButton);
  }

  loadSettings();
  updateBatchMode();
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

void MainWindow::loadSettings() {
  QSettings settings;
  const QString platformKey = hostCanSelectTargetPlatform()
    ? settings.value(QStringLiteral("app/platform"),
                     targetPlatformKey(hostTargetPlatform())).toString()
    : targetPlatformKey(hostTargetPlatform());
  const int platformIndex = platformCombo_->findData(platformKey);
  platformCombo_->setCurrentIndex(platformIndex >= 0 ? platformIndex : 0);
  batchCheck_->setChecked(settings.value(QStringLiteral("batch/enabled"), false).toBool());
  batchDirectoryEdit_->setText(settings.value(QStringLiteral("batch/directory")).toString());
  biosEdit_->setText(settings.value(QStringLiteral("paths/bios")).toString());
  iconEdit_->setText(settings.value(QStringLiteral("paths/icon")).toString());
  outputEdit_->setText(settings.value(QStringLiteral("paths/output"),
                                      QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).toString());
  certificateEdit_->setText(settings.value(QStringLiteral("paths/certificate")).toString());
  signingEnabled_->setChecked(settings.value(QStringLiteral("signing/enabled"), false).toBool());
  ghidraEdit_->setText(settings.value(QStringLiteral("paths/ghidra"), detectGhidraHome()).toString());
  titleEdit_->setText(settings.value(QStringLiteral("app/window_title")).toString());
  biosPatchEnabled_->setChecked(settings.value(QStringLiteral("bios_patch/enabled"), false).toBool());
  biosInitialSplashEdit_->setText(settings.value(QStringLiteral("bios_patch/initial_image")).toString());
  biosHandoffImageEdit_->setText(settings.value(QStringLiteral("bios_patch/handoff_image")).toString());
  biosMuteAudio_->setChecked(settings.value(QStringLiteral("bios_patch/mute_audio"), true).toBool());
  biosRemovePsGlyph_->setChecked(settings.value(QStringLiteral("bios_patch/remove_ps_glyph"), true).toBool());
  skipBiosBoot_->setChecked(settings.value(QStringLiteral("runtime/skip_bios_boot"), false).toBool());
  macosGipGamepad_->setChecked(
    settings.value(QStringLiteral("runtime/macos_gip_gamepad"), true).toBool());

  if (batchCheck_->isChecked() && QFileInfo(batchDirectoryEdit_->text()).isDir()) {
    populateBatchDirectory(batchDirectoryEdit_->text(), false);
  }
}

void MainWindow::saveSettings() const {
  QSettings settings;
  settings.setValue(QStringLiteral("app/platform"), platformCombo_->currentData().toString());
  settings.setValue(QStringLiteral("batch/enabled"), batchCheck_->isChecked());
  settings.setValue(QStringLiteral("batch/directory"), batchDirectoryEdit_->text());
  settings.setValue(QStringLiteral("paths/bios"), biosEdit_->text());
  settings.setValue(QStringLiteral("paths/icon"), iconEdit_->text());
  settings.setValue(QStringLiteral("paths/output"), outputEdit_->text());
  settings.setValue(QStringLiteral("paths/certificate"), certificateEdit_->text());
  settings.setValue(QStringLiteral("signing/enabled"), signingEnabled_->isChecked());
  settings.setValue(QStringLiteral("paths/ghidra"), ghidraEdit_->text());
  settings.setValue(QStringLiteral("app/window_title"), titleEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/enabled"), biosPatchEnabled_->isChecked());
  settings.setValue(QStringLiteral("bios_patch/initial_image"), biosInitialSplashEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/handoff_image"), biosHandoffImageEdit_->text());
  settings.setValue(QStringLiteral("bios_patch/mute_audio"), biosMuteAudio_->isChecked());
  settings.setValue(QStringLiteral("bios_patch/remove_ps_glyph"), biosRemovePsGlyph_->isChecked());
  settings.setValue(QStringLiteral("runtime/skip_bios_boot"), skipBiosBoot_->isChecked());
  settings.remove(QStringLiteral("runtime/pad_mode"));
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
  const QString path = QFileDialog::getExistingDirectory(
    this, QStringLiteral("Select PlayStation game directory"),
    batchDirectoryEdit_->text().isEmpty() ? QDir::homePath()
                                          : batchDirectoryEdit_->text());
  if (!path.isEmpty()) {
    populateBatchDirectory(path, true);
  }
}

void MainWindow::populateBatchDirectory(const QString& path, bool showDialogs) {
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
  for (const auto& disc : catalog) {
    batchEntries_.append({
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      disc.sourcePath,
      disc.selectedBinPaths,
      disc.suggestedTitle,
      {},
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
  if (signingEnabled_->isChecked()) {
    request.certificatePath = certificateEdit_->text();
    request.certificatePassword = certificatePasswordEdit_->text();
  }
  request.ghidraHome = ghidraEdit_->text();
  request.frameworkRoot = QString::fromUtf8(PSXRECOMP_SOURCE_ROOT);
  request.patchBiosBranding = biosPatchEnabled_->isChecked();
  request.biosInitialSplashPath = biosInitialSplashEdit_->text();
  request.biosHandoffImagePath = biosHandoffImageEdit_->text();
  request.biosMuteBootAudio = biosMuteAudio_->isChecked();
  request.biosRemoveStockPsGlyph = biosRemovePsGlyph_->isChecked();
  request.skipBiosBoot = skipBiosBoot_->isChecked();
  request.macosGipGamepad = macosGipGamepad_->isChecked();
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
        request.cuePath = entry.sourcePath;
        request.selectedBinPaths = entry.selectedBinPaths;
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
  const QString bundleName = cleanBundleName(request.windowTitle);
  QString outputName;
  switch (request.targetPlatform) {
    case TargetPlatform::Windows:
      outputName = bundleName + QStringLiteral("-Windows");
      break;
    case TargetPlatform::Linux:
      outputName = bundleName + QStringLiteral("-Linux");
      break;
    case TargetPlatform::MacOS:
      outputName = bundleName + QStringLiteral(".app");
      break;
    case TargetPlatform::All:
      return {};
  }
  return QDir(request.outputDirectory).filePath(outputName);
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

  QSet<QString> uniqueOutputs;
  QStringList duplicateOutputs;
  QStringList existingOutputs;
  for (const auto& request : requests) {
    const QString outputPath = outputPathForRequest(request);
    const QString folded = QDir::cleanPath(outputPath).toCaseFolded();
    if (uniqueOutputs.contains(folded)) {
      duplicateOutputs.append(outputPath);
    } else {
      uniqueOutputs.insert(folded);
    }
    if (QFileInfo::exists(outputPath)) {
      existingOutputs.append(outputPath);
    }
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
      QStringLiteral("%1 queued output%2 already exist%3. Replace %4 with the new build%5?")
        .arg(existingOutputs.size())
        .arg(existingOutputs.size() == 1 ? QString() : QStringLiteral("s"))
        .arg(existingOutputs.size() == 1 ? QStringLiteral("s") : QString())
        .arg(existingOutputs.size() == 1 ? QStringLiteral("it") : QStringLiteral("them"))
        .arg(existingOutputs.size() == 1 ? QString() : QStringLiteral("s")),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      return;
    }
    overwrite = true;
  }

  for (auto& request : requests) {
    request.overwriteOutput = overwrite;
  }

  saveSettings();
  pendingRequests_ = requests;
  completedOutputs_.clear();
  totalRequestCount_ = pendingRequests_.size();
  activeRequestIndex_ = 0;
  outputAppPath_.clear();
  revealButton_->setEnabled(false);
  logView_->clear();
  setBusy(true);
  startNextRequest();
}

void MainWindow::startNextRequest() {
  if (pendingRequests_.isEmpty()) {
    return;
  }
  activeRequest_ = pendingRequests_.takeFirst();
  ++activeRequestIndex_;
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
  if (worker_) {
    pendingRequests_.clear();
    worker_->requestCancel();
    cancelButton_->setEnabled(false);
    stageLabel_->setText(QStringLiteral("Cancelling…"));
  }
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
  completedOutputs_.append(appPath);
  if (!pendingRequests_.isEmpty()) {
    startNextRequest();
    return;
  }

  outputAppPath_ = completedOutputs_.size() == 1
    ? completedOutputs_.constFirst() : outputEdit_->text();
  progressBar_->setRange(0, 100);
  progressBar_->setValue(100);
  stageLabel_->setText(completedOutputs_.size() == 1
    ? QStringLiteral("Complete — package verified")
    : QStringLiteral("Complete — %1 packages verified").arg(completedOutputs_.size()));
  revealButton_->setEnabled(true);
  setBusy(false);
  QStringList displayedOutputs = completedOutputs_;
  if (displayedOutputs.size() > 12) {
    const int omitted = displayedOutputs.size() - 12;
    displayedOutputs = displayedOutputs.mid(0, 12);
    displayedOutputs.append(QStringLiteral("…and %1 more in %2")
                              .arg(omitted).arg(outputEdit_->text()));
  }
  QMessageBox::information(
    this, completedOutputs_.size() == 1 ? QStringLiteral("App created")
                                        : QStringLiteral("Batch export complete"),
    QStringLiteral("%1 package%2 created and verified:\n\n%3")
      .arg(completedOutputs_.size())
      .arg(completedOutputs_.size() == 1 ? QStringLiteral(" was") : QStringLiteral("s were"))
      .arg(displayedOutputs.join(QStringLiteral("\n"))));
}

void MainWindow::onFailed(const QString& message, const QString& workspacePath) {
  pendingRequests_.clear();
  progressBar_->setRange(0, 100);
  progressBar_->setValue(0);
  stageLabel_->setText(QStringLiteral("Build failed"));
  setBusy(false);
  QString detail = message;
  if (!workspacePath.isEmpty()) {
    detail += QStringLiteral("\n\nProof and intermediate artifacts were retained at:\n%1").arg(workspacePath);
  }
  if (!completedOutputs_.isEmpty()) {
    detail += QStringLiteral("\n\n%1 earlier package%2 completed before this failure:\n%3")
      .arg(completedOutputs_.size())
      .arg(completedOutputs_.size() == 1 ? QString() : QStringLiteral("s"))
      .arg(completedOutputs_.join(QStringLiteral("\n")));
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
  for (auto* edit : { discEdit_, batchDirectoryEdit_, biosEdit_, iconEdit_, titleEdit_, outputEdit_,
                      certificateEdit_, certificatePasswordEdit_, ghidraEdit_,
                      biosInitialSplashEdit_, biosHandoffImageEdit_ }) {
    edit->parentWidget()->setEnabled(!busy);
  }
  biosPatchEnabled_->setEnabled(!busy);
  batchCheck_->setEnabled(!busy);
  batchList_->setEnabled(!busy);
  signingEnabled_->setEnabled(!busy);
  platformCombo_->setEnabled(!busy);
  skipBiosBoot_->setEnabled(!busy);
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
  const auto selectedPlatform =
    targetPlatformFromKey(platformCombo_->currentData().toString());
  const bool includesMacos = selectedPlatform == TargetPlatform::MacOS ||
                             selectedPlatform == TargetPlatform::All;
  const bool busy = cancelButton_ && cancelButton_->isEnabled();
  const bool signingRequested = includesMacos && signingEnabled_->isChecked();
  signingEnabled_->setVisible(includesMacos);
  signingEnabled_->setEnabled(includesMacos && !busy);
  certificateEdit_->parentWidget()->setEnabled(signingRequested && !busy);
  certificatePasswordEdit_->parentWidget()->setEnabled(signingRequested && !busy);
  certificateEdit_->parentWidget()->setVisible(signingRequested);
  certificatePasswordEdit_->parentWidget()->setVisible(signingRequested);
  macosGipGamepad_->setVisible(includesMacos);
  macosGipGamepad_->setEnabled(includesMacos && !busy);
  const QString unsignedNote = selectedPlatform == TargetPlatform::Windows
    ? QStringLiteral("Windows exports are currently delivered unsigned")
    : QStringLiteral("Linux exports are currently delivered unsigned");
  certificateEdit_->setToolTip(includesMacos
    ? QStringLiteral("Optional PKCS#12 identity read directly by rcodesign")
    : unsignedNote);
  certificatePasswordEdit_->setToolTip(certificateEdit_->toolTip());
  if (selectedPlatform == TargetPlatform::All) {
    signingNote_->setText(signingRequested
      ? QStringLiteral("macOS uses one-pass direct PFX signing with no Keychain import. Windows and Linux are unsigned.")
      : QStringLiteral("macOS signing is optional; all three platform packages will be verified before delivery."));
    outputEdit_->setPlaceholderText(QStringLiteral("Destination for macOS, Windows, and Linux packages"));
    buildButton_->setText(batchCheck_->isChecked()
      ? QStringLiteral("Batch Export All Platforms") : QStringLiteral("Export All Platforms"));
  } else if (selectedPlatform == TargetPlatform::MacOS) {
    signingNote_->setText(signingRequested
      ? QStringLiteral("The PFX is read directly by rcodesign. No identity is imported into any Keychain.")
      : QStringLiteral("Signing is optional. The unsigned app is hash-verified before and after delivery."));
    outputEdit_->setPlaceholderText(QStringLiteral("Destination for the macOS .app"));
    buildButton_->setText(batchCheck_->isChecked()
      ? (signingRequested ? QStringLiteral("Batch Build Signed Apps")
                          : QStringLiteral("Batch Build macOS Apps"))
      : (signingRequested ? QStringLiteral("Build Signed .app")
                          : QStringLiteral("Build macOS App")));
  } else if (selectedPlatform == TargetPlatform::Windows) {
#if defined(Q_OS_WIN)
    signingNote_->setText(QStringLiteral(
      "Windows exports use the native Visual Studio 2022 MSVC x64 toolchain and are delivered unsigned."));
#else
    signingNote_->setText(QStringLiteral(
      "Windows exports use the installed x86_64-w64-mingw32 MinGW toolchain and are delivered unsigned."));
#endif
    outputEdit_->setPlaceholderText(QStringLiteral("Destination for the Windows app folder"));
    buildButton_->setText(batchCheck_->isChecked()
      ? QStringLiteral("Batch Build Windows Apps") : QStringLiteral("Build Windows App"));
  } else {
#if defined(Q_OS_LINUX)
    signingNote_->setText(QStringLiteral(
      "Linux exports use the native GCC toolchain, bundle SDL2, and are delivered unsigned."));
#else
    signingNote_->setText(QStringLiteral(
      "Linux exports use the installed x86_64-unknown-linux-gnu toolchain, bundle SDL2, and are delivered unsigned."));
#endif
    outputEdit_->setPlaceholderText(QStringLiteral("Destination for the Linux app folder"));
    buildButton_->setText(batchCheck_->isChecked()
      ? QStringLiteral("Batch Build Linux Apps") : QStringLiteral("Build Linux App"));
  }
  updateBuildButton();
}

void MainWindow::updateBuildButton() {
  if (cancelButton_->isEnabled()) {
    return;
  }
  const bool brandingReady = !biosPatchEnabled_->isChecked() ||
    (!biosInitialSplashEdit_->text().isEmpty() && !biosHandoffImageEdit_->text().isEmpty());
  const auto selectedPlatform = targetPlatformFromKey(platformCombo_->currentData().toString());
  const bool includesMacos = selectedPlatform == TargetPlatform::MacOS ||
                             selectedPlatform == TargetPlatform::All;
  const bool signingReady = !includesMacos || !signingEnabled_->isChecked() ||
    (!certificateEdit_->text().isEmpty() && !certificatePasswordEdit_->text().isEmpty());
  bool gameReady = !discEdit_->text().isEmpty() && !titleEdit_->text().trimmed().isEmpty();
  if (batchCheck_->isChecked()) {
    gameReady = !batchEntries_.isEmpty() && std::all_of(
      batchEntries_.cbegin(), batchEntries_.cend(), [](const BatchGameEntry& entry) {
        return !entry.title.trimmed().isEmpty();
      });
  }
  const bool ready = gameReady && !biosEdit_->text().isEmpty() &&
                     !outputEdit_->text().isEmpty() && !ghidraEdit_->text().isEmpty() &&
                     signingReady && brandingReady;
  buildButton_->setEnabled(ready);
}

} // namespace psxstudio
