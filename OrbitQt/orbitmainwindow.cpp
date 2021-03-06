// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "orbitmainwindow.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressDialog>
#include <QTimer>
#include <QToolTip>
#include <utility>

#include "../OrbitCore/Path.h"
#include "../OrbitCore/PrintVar.h"
#include "../OrbitCore/Utils.h"
#include "../OrbitCore/Version.h"
#include "../OrbitGl/App.h"
#include "../OrbitGl/PluginManager.h"
#include "../OrbitGl/SamplingReport.h"
#include "../OrbitPlugin/OrbitSDK.h"
#include "../third_party/concurrentqueue/concurrentqueue.h"
#include "absl/flags/flag.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "orbitaboutdialog.h"
#include "orbitcodeeditor.h"
#include "orbitdisassemblydialog.h"
#include "orbitsamplingreport.h"
#include "outputdialog.h"
#include "showincludesdialog.h"
#include "ui_orbitmainwindow.h"

#ifdef _WIN32
#include <shellapi.h>
#endif

ABSL_DECLARE_FLAG(bool, enable_stale_features);
ABSL_DECLARE_FLAG(bool, devmode);

//-----------------------------------------------------------------------------
OrbitMainWindow* GMainWindow;
extern QMenu* GContextMenu;

//-----------------------------------------------------------------------------
OrbitMainWindow::OrbitMainWindow(QApplication* a_App,
                                 ApplicationOptions&& options)
    : QMainWindow(nullptr),
      m_App(a_App),
      ui(new Ui::OrbitMainWindow),
      m_Headless(false),
      m_IsDev(false) {
  OrbitApp::Init(std::move(options));

  DataViewFactory* data_view_factory = GOrbitApp.get();

  ui->setupUi(this);

  ui->ProcessesList->SetDataView(
      data_view_factory->GetOrCreateDataView(DataViewType::PROCESSES));
  ui->ProcessesList->SetProcessParams();

  QList<int> sizes;
  sizes.append(5000);
  sizes.append(5000);
  ui->HomeVerticalSplitter->setSizes(sizes);
  ui->HomeHorizontalSplitter->setSizes(sizes);
  ui->splitter_2->setSizes(sizes);

  GOrbitApp->AddCaptureStartedCallback([this] {
    ui->actionOpen_Capture->setDisabled(true);
    ui->actionSave_Capture->setDisabled(true);
    ui->actionOpen_Session->setDisabled(true);
    ui->actionSave_Session->setDisabled(true);
    ui->actionSave_Session_As->setDisabled(true);
    ui->HomeTab->setDisabled(true);
  });

  auto finalizing_capture_dialog = new QProgressDialog(
      "Waiting for the remaining capture data...", "OK", 0, 0, this, Qt::Tool);
  finalizing_capture_dialog->setWindowTitle("Finalizing capture");
  finalizing_capture_dialog->setModal(true);
  finalizing_capture_dialog->setWindowFlags(
      (finalizing_capture_dialog->windowFlags() | Qt::CustomizeWindowHint) &
      ~Qt::WindowCloseButtonHint & ~Qt::WindowSystemMenuHint);
  finalizing_capture_dialog->setFixedSize(finalizing_capture_dialog->size());
  finalizing_capture_dialog->close();

  GOrbitApp->AddCaptureStopRequestedCallback(
      [finalizing_capture_dialog] { finalizing_capture_dialog->show(); });
  GOrbitApp->AddCaptureStoppedCallback([this, finalizing_capture_dialog] {
    finalizing_capture_dialog->close();
    ui->actionOpen_Capture->setDisabled(false);
    ui->actionSave_Capture->setDisabled(false);
    ui->actionOpen_Session->setDisabled(false);
    ui->actionSave_Session->setDisabled(false);
    ui->actionSave_Session_As->setDisabled(false);
    ui->HomeTab->setDisabled(false);
  });

  GOrbitApp->AddRefreshCallback(
      [this](DataViewType a_Type) { this->OnRefreshDataViewPanels(a_Type); });
  GOrbitApp->AddSamplingReportCallback(
      [this](DataView* callstack_data_view,
             std::shared_ptr<SamplingReport> report) {
        this->OnNewSamplingReport(callstack_data_view, std::move(report));
      });
  GOrbitApp->AddSelectionReportCallback(
      [this](DataView* callstack_data_view,
             std::shared_ptr<SamplingReport> report) {
        this->OnNewSelectionReport(callstack_data_view, std::move(report));
      });
  GOrbitApp->AddUiMessageCallback([this](const std::string& a_Message) {
    this->OnReceiveMessage(a_Message);
  });
  GOrbitApp->SetFindFileCallback([this](const std::string& caption,
                                        const std::string& dir,
                                        const std::string& filter) {
    return this->FindFile(caption, dir, filter);
  });
  GOrbitApp->AddWatchCallback(
      [this](const Variable* a_Variable) { this->OnAddToWatch(a_Variable); });
  GOrbitApp->SetSaveFileCallback([this](const std::string& extension) {
    return this->OnGetSaveFileName(extension);
  });
  GOrbitApp->SetClipboardCallback(
      [this](const std::string& text) { this->OnSetClipboard(text); });

  ParseCommandlineArguments();

  ui->DebugGLWidget->Initialize(GlPanel::DEBUG, this);
  ui->CaptureGLWidget->Initialize(GlPanel::CAPTURE, this);
  ui->VisualizeGLWidget->Initialize(GlPanel::VISUALIZE, this);

  ui->ModulesList->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::MODULES),
      SelectionType::kExtended, FontType::kDefault);
  ui->FunctionsList->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::FUNCTIONS),
      SelectionType::kExtended, FontType::kDefault);
  ui->LiveFunctionsList->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::LIVE_FUNCTIONS),
      SelectionType::kExtended, FontType::kDefault);
  ui->CallStackView->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::CALLSTACK),
      SelectionType::kExtended, FontType::kDefault);
  ui->TypesList->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::TYPES),
      SelectionType::kDefault, FontType::kDefault);
  ui->GlobalsList->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::GLOBALS),
      SelectionType::kDefault, FontType::kDefault);
  ui->SessionList->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::SESSIONS),
      SelectionType::kDefault, FontType::kDefault);
  ui->OutputView->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::LOG),
      SelectionType::kDefault, FontType::kFixed);

  SetupCodeView();

  if (!m_IsDev) {
    HideTab(ui->MainTabWidget, "debug");
    HideTab(ui->MainTabWidget, "visualize");
  }

  if (!absl::GetFlag(FLAGS_enable_stale_features)) {
    ui->MainTabWidget->removeTab(ui->MainTabWidget->indexOf(ui->OutputTab));
    ui->MainTabWidget->removeTab(ui->MainTabWidget->indexOf(ui->WatchTab));

    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->TypesTab));
    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->GlobalsType));
    ui->RightTabWidget->removeTab(
        ui->RightTabWidget->indexOf(ui->CallStackTab));
    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->CodeTab));
    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->outputTab));

    ui->actionLaunch_Process->setVisible(false);
    ui->actionDisconnect->setVisible(false);

    ui->actionEnable_Sampling->setVisible(false);
    ui->actionEnable_Context_Switches->setVisible(false);
    ui->actionEnable_Unreal_Support->setVisible(false);
    ui->actionAllow_Unsafe_Hooking->setVisible(false);
    ui->actionOutputDebugString->setVisible(false);
    ui->actionUploadDumpsToServer->setVisible(false);

    ui->actionShow_Includes_Util->setVisible(false);
    ui->menuTools->menuAction()->setVisible(false);
    ui->menuDev->menuAction()->setVisible(!ui->menuDev->isEmpty());
  }

  if (!absl::GetFlag(FLAGS_devmode)) {
    ui->menuDebug->menuAction()->setVisible(false);
  }

  // Output window
  this->ui->plainTextEdit->SetIsOutputWindow();

  StartMainTimer();

  m_OutputDialog = new OutputDialog(this);
  m_OutputDialog->setWindowTitle("Orbit - Loading Pdb...");

  ui->actionEnable_Context_Switches->setChecked(
      GOrbitApp->GetTrackContextSwitches());
  ui->actionEnable_Unreal_Support->setChecked(
      GOrbitApp->GetUnrealSupportEnabled());
  ui->actionAllow_Unsafe_Hooking->setChecked(
      GOrbitApp->GetUnsafeHookingEnabled());
  ui->actionEnable_Sampling->setChecked(GOrbitApp->GetSamplingEnabled());
  ui->actionOutputDebugString->setChecked(
      GOrbitApp->GetOutputDebugStringEnabled());
  ui->actionUploadDumpsToServer->setChecked(
      GOrbitApp->GetUploadDumpsToServerEnabled());

  CreateSamplingTab();
  CreateSelectionTab();
  CreatePluginTabs();

  SetTitle({});
  std::string iconFileName = Path::GetExecutablePath() + "orbit.ico";
  this->setWindowIcon(QIcon(iconFileName.c_str()));

  GMainWindow = this;

  GOrbitApp->PostInit();
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::SetupCodeView() {
  ui->CodeTextEdit->SetEditorType(OrbitCodeEditor::CODE_VIEW);
  ui->FileMappingTextEdit->SetEditorType(OrbitCodeEditor::FILE_MAPPING);
  ui->FileMappingTextEdit->SetSaveButton(ui->SaveFileMapping);
  ui->CodeTextEdit->SetFindLineEdit(ui->lineEdit);
  ui->FileMappingWidget->hide();
  OrbitCodeEditor::setFileMappingWidget(ui->FileMappingWidget);
}

//-----------------------------------------------------------------------------
OrbitMainWindow::~OrbitMainWindow() {
  delete m_OutputDialog;
  delete ui;
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::ParseCommandlineArguments() {
  std::vector<std::string> arguments;
  for (const auto& qt_argument : QCoreApplication::arguments()) {
    std::string argument = qt_argument.toStdString();
    if (absl::StrContains(argument, "inject:")) {
      m_Headless = true;
    } else if (argument == "dev") {
      m_IsDev = true;
    }

    arguments.push_back(std::move(argument));
  }

  GOrbitApp->SetCommandLineArguments(arguments);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::PostInit() {}

//-----------------------------------------------------------------------------
bool OrbitMainWindow::HideTab(QTabWidget* a_TabWidget, const char* a_TabName) {
  QTabWidget* tab = a_TabWidget;

  for (int i = 0; i < tab->count(); ++i) {
    std::string tabName = tab->tabText(i).toStdString();

    if (tabName == a_TabName) {
      tab->removeTab(i);
      return true;
    }
  }

  return false;
}

//-----------------------------------------------------------------------------
std::string OrbitMainWindow::FindFile(const std::string& caption,
                                      const std::string& dir,
                                      const std::string& filter) {
  QStringList list = QFileDialog::getOpenFileNames(this, caption.c_str(),
                                                   dir.c_str(), filter.c_str());
  std::string result;
  for (auto& file : list) {
    result = file.toStdString();
    break;
  }

  return result;
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnRefreshDataViewPanels(DataViewType a_Type) {
  if (a_Type == DataViewType::ALL) {
    for (int i = 0; i < static_cast<int>(DataViewType::ALL); ++i) {
      UpdatePanel(static_cast<DataViewType>(i));
    }
  } else {
    UpdatePanel(a_Type);
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::UpdatePanel(DataViewType a_Type) {
  switch (a_Type) {
    case DataViewType::CALLSTACK:
      ui->CallStackView->Refresh();
      break;
    case DataViewType::FUNCTIONS:
      ui->FunctionsList->Refresh();
      break;
    case DataViewType::LIVE_FUNCTIONS:
      ui->LiveFunctionsList->Refresh();
      break;
    case DataViewType::TYPES:
      ui->TypesList->Refresh();
      break;
    case DataViewType::GLOBALS:
      ui->GlobalsList->Refresh();
      break;
    case DataViewType::MODULES:
      ui->ModulesList->Refresh();
      break;
    case DataViewType::LOG:
      ui->OutputView->Refresh();
      break;
    case DataViewType::PROCESSES:
      ui->ProcessesList->Refresh();
      break;
    case DataViewType::SESSIONS:
      ui->SessionList->Refresh();
      break;
    case DataViewType::SAMPLING:
      m_OrbitSamplingReport->RefreshCallstackView();
      m_OrbitSamplingReport->RefreshTabs();
      m_SelectionReport->RefreshCallstackView();
      m_SelectionReport->RefreshTabs();
      break;
    default:
      break;
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::CreateSamplingTab() {
  m_SamplingTab = new QWidget();
  m_SamplingLayout = new QGridLayout(m_SamplingTab);
  m_SamplingLayout->setSpacing(6);
  m_SamplingLayout->setContentsMargins(11, 11, 11, 11);
  m_OrbitSamplingReport = new OrbitSamplingReport(m_SamplingTab);
  m_SamplingLayout->addWidget(m_OrbitSamplingReport, 0, 0, 1, 1);

  ui->RightTabWidget->addTab(m_SamplingTab, QString("sampling"));
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::CreatePluginTabs() {
  for (Orbit::Plugin* plugin : GPluginManager.m_Plugins) {
    QWidget* widget = new QWidget();
    QGridLayout* layout = new QGridLayout(widget);
    layout->setSpacing(6);
    layout->setContentsMargins(11, 11, 11, 11);
    OrbitGLWidget* glWidget = new OrbitGLWidget(widget);
    layout->addWidget(glWidget, 0, 0, 1, 1);
    ui->RightTabWidget->addTab(widget, plugin->GetName());

    glWidget->Initialize(GlPanel::PLUGIN, this, plugin);
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnNewSamplingReport(
    DataView* callstack_data_view,
    std::shared_ptr<SamplingReport> sampling_report) {
  m_SamplingLayout->removeWidget(m_OrbitSamplingReport);
  delete m_OrbitSamplingReport;

  m_OrbitSamplingReport = new OrbitSamplingReport(m_SamplingTab);
  m_OrbitSamplingReport->Initialize(callstack_data_view, sampling_report);
  m_SamplingLayout->addWidget(m_OrbitSamplingReport, 0, 0, 1, 1);

  // Automatically switch to sampling tab if not already in live tab.
  if (ui->RightTabWidget->currentWidget() != ui->LiveTab) {
    ui->RightTabWidget->setCurrentWidget(m_SamplingTab);
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::CreateSelectionTab() {
  m_SelectionTab = new QWidget();
  m_SelectionLayout = new QGridLayout(m_SelectionTab);
  m_SelectionLayout->setSpacing(6);
  m_SelectionLayout->setContentsMargins(11, 11, 11, 11);
  m_SelectionReport = new OrbitSamplingReport(m_SelectionTab);
  m_SelectionLayout->addWidget(m_SelectionReport, 0, 0, 1, 1);
  ui->RightTabWidget->addTab(m_SelectionTab, QString("selection"));
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnNewSelectionReport(
    DataView* callstack_data_view,
    std::shared_ptr<class SamplingReport> sampling_report) {
  m_SelectionLayout->removeWidget(m_SelectionReport);
  delete m_SelectionReport;

  m_SelectionReport = new OrbitSamplingReport(m_SelectionTab);
  m_SelectionReport->Initialize(callstack_data_view, sampling_report);
  m_SelectionLayout->addWidget(m_SelectionReport, 0, 0, 1, 1);

  ui->RightTabWidget->setCurrentWidget(m_SelectionTab);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnReceiveMessage(const std::string& a_Message) {
  if (a_Message == "ScreenShot") {
    QFile file("sshot.png");
    file.open(QIODevice::WriteOnly);
    QPixmap pixMap = this->grab();
    pixMap.save(&file, "PNG");

#ifdef _WIN32
    std::string fileName = file.fileName().toStdString();
    ShellExecuteA(0, 0, fileName.c_str(), 0, 0, SW_SHOW);
#endif
  } else if (absl::StartsWith(a_Message, "code")) {
    ui->FileMappingWidget->hide();

    bool success = ui->CodeTextEdit->loadCode(a_Message);

    if (!success) {
      ui->FileMappingTextEdit->loadFileMap();
      ui->FileMappingWidget->show();
    }
  } else if (absl::StartsWith(a_Message, "tooltip:")) {
    QToolTip::showText(QCursor::pos(),
                       Replace(a_Message, "tooltip:", "").c_str(), this);
  } else if (absl::StartsWith(a_Message, "output")) {
    ui->MainTabWidget->setCurrentWidget(ui->OutputTab);
  } else if (absl::StartsWith(a_Message, "gotocode")) {
    ui->RightTabWidget->setCurrentWidget(ui->CodeTab);
  } else if (absl::StartsWith(a_Message, "gotocallstack")) {
    ui->RightTabWidget->setCurrentWidget(ui->CallStackTab);
  } else if (absl::StartsWith(a_Message, "startcapture")) {
    SetTitle({});
  } else if (absl::StartsWith(a_Message, "gotolive")) {
    ui->RightTabWidget->setCurrentWidget(ui->LiveTab);
  } else if (absl::StartsWith(a_Message, "gotocapture")) {
    ui->MainTabWidget->setCurrentWidget(ui->CaptureTab);
  } else if (absl::StartsWith(a_Message, "opencapture")) {
    on_actionOpen_Capture_triggered();
  } else if (absl::StartsWith(a_Message, "savecapture")) {
    on_actionSave_Capture_triggered();
  } else if (absl::StartsWith(a_Message, "pdb:")) {
    if (GOrbitApp->IsLoading()) {
      m_CurrentPdbName = Replace(a_Message, "pdb:", "");
      m_OutputDialog->Reset();
      m_OutputDialog->SetStatus(m_CurrentPdbName);
      m_OutputDialog->show();
      this->setDisabled(true);
    }
  } else if (absl::StartsWith(a_Message, "pdbloaded")) {
    this->setDisabled(false);
    m_OutputDialog->hide();
  } else if (absl::StartsWith(a_Message, "status:")) {
    m_OutputDialog->SetStatus(Replace(a_Message, "status:", ""));
  } else if (absl::StartsWith(a_Message, "log:")) {
    m_OutputDialog->AddLog(Replace(a_Message, "log:", ""));
  } else if (absl::StartsWith(a_Message, "asm:")) {
    OpenDisassembly(a_Message);
  } else if (absl::StartsWith(a_Message, "UpdateProcessParams")) {
    ui->ProcessesList->UpdateProcessParams();
  } else if (absl::StartsWith(a_Message, "SetProcessParams")) {
    ui->ProcessesList->SetProcessParams();
  } else if (absl::StartsWith(a_Message, "error:")) {
    std::string title_text = Replace(a_Message, "error:", "");
    std::string title = title_text.substr(0, title_text.find('\n'));
    std::string text = title_text.find('\n') != std::string::npos
                           ? title_text.substr(title_text.find('\n'))
                           : title;
    QMessageBox::critical(this, title.c_str(), text.c_str());
  } else if (absl::StartsWith(a_Message, "info:")) {
    std::string title_text = Replace(a_Message, "info:", "");
    std::string title = title_text.substr(0, title_text.find('\n'));
    std::string text = title_text.find('\n') != std::string::npos
                           ? title_text.substr(title_text.find('\n'))
                           : title;
    QMessageBox::information(this, title.c_str(), text.c_str());
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnAddToWatch(const class Variable* a_Variable) {
  ui->WatchWidget->AddToWatch(a_Variable);
  ui->MainTabWidget->setCurrentWidget(ui->WatchTab);
}

//-----------------------------------------------------------------------------
std::string OrbitMainWindow::OnGetSaveFileName(const std::string& extension) {
  std::string filename =
      QFileDialog::getSaveFileName(this, "Specify a file to save...", nullptr,
                                   extension.c_str())
          .toStdString();
  if (!filename.empty() && !absl::EndsWith(filename, extension)) {
    filename += extension;
  }
  return filename;
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnSetClipboard(const std::string& text) {
  QApplication::clipboard()->setText(QString::fromStdString(text));
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionAbout_triggered() {
  OrbitQt::OrbitAboutDialog dialog{this};
  dialog.setWindowTitle(windowTitle());
  dialog.setVersionString(QCoreApplication::applicationVersion());

  QFile licenseFile{QDir{QCoreApplication::applicationDirPath()}.filePath(
      "NOTICE")};
  if (licenseFile.open(QIODevice::ReadOnly)) {
    dialog.setLicenseText(licenseFile.readAll());
  }
  dialog.exec();
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::StartMainTimer() {
  m_MainTimer = new QTimer(this);
  connect(m_MainTimer, SIGNAL(timeout()), this, SLOT(OnTimer()));

  // Update period set to 16ms (~60FPS)
  int msec = 16;
  m_MainTimer->start(msec);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnTimer() {
  OrbitApp::MainTick();

  for (OrbitGLWidget* glWidget : m_GlWidgets) {
    glWidget->update();
  }

  // Output window
  this->ui->plainTextEdit->OnTimer();
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OnHideSearch() { ui->lineEdit->hide(); }

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionSave_Session_triggered() {
  std::string sessionName = GOrbitApp->GetSessionFileName();
  if (sessionName.empty()) {
    on_actionSave_Session_As_triggered();
    return;
  }
  outcome::result<void, std::string> result =
      GOrbitApp->OnSaveSession(sessionName);
  if (result.has_error()) {
    QMessageBox::critical(
        this, "Error saving session",
        absl::StrFormat("Could not save session in \"%s\":\n%s.", sessionName,
                        result.error())
            .c_str());
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionOpen_Session_triggered() {
  QStringList list = QFileDialog::getOpenFileNames(
      this, "Select a file to open...", Path::GetPresetPath().c_str(), "*.opr");
  for (const auto& file : list) {
    outcome::result<void, std::string> result =
        GOrbitApp->OnLoadSession(file.toStdString());
    if (result.has_error()) {
      QMessageBox::critical(
          this, "Error loading session",
          absl::StrFormat("Could not load session from \"%s\":\n%s.",
                          file.toStdString(), result.error())
              .c_str());
    }
    break;
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionDisconnect_triggered() {
  GOrbitApp->OnDisconnect();
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionQuit_triggered() {
  close();
  QApplication::quit();
}

//-----------------------------------------------------------------------------
QPixmap QtGrab(OrbitMainWindow* a_Window) {
  QPixmap pixMap = a_Window->grab();
  if (GContextMenu) {
    QPixmap menuPixMap = GContextMenu->grab();
    pixMap.copy();
  }
  return pixMap;
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionToogleDevMode_toggled(bool a_Toggle) {
  UNUSED(a_Toggle);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionSave_Session_As_triggered() {
  QString file =
      QFileDialog::getSaveFileName(this, "Specify a file to save...",
                                   Path::GetPresetPath().c_str(), "*.opr");
  if (file.isEmpty()) {
    return;
  }

  outcome::result<void, std::string> result =
      GOrbitApp->OnSaveSession(file.toStdString());
  if (result.has_error()) {
    QMessageBox::critical(
        this, "Error saving session",
        absl::StrFormat("Could not save session in \"%s\":\n%s.",
                        file.toStdString(), result.error())
            .c_str());
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionEnable_Context_Switches_triggered() {}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionEnable_Context_Switches_triggered(bool checked) {
  GOrbitApp->SetTrackContextSwitches(checked);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionLaunch_Process_triggered() {
  QStringList list = QFileDialog::getOpenFileNames(
      this, "Select an executable to launch...", "", "*.exe");
  for (auto& file : list) {
    GOrbitApp->OnLaunchProcess(file.toStdString(), "", "");
    break;
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionEnable_Unreal_Support_triggered(bool checked) {
  GOrbitApp->EnableUnrealSupport(checked);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionAllow_Unsafe_Hooking_triggered(bool checked) {
  GOrbitApp->EnableUnsafeHooking(checked);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionEnable_Sampling_triggered(bool checked) {
  GOrbitApp->EnableSampling(checked);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionEnable_Sampling_toggled(bool arg1) {
  UNUSED(arg1);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionSave_Capture_triggered() {
  QString file = QFileDialog::getSaveFileName(
      this, "Save capture...",
      Path::JoinPath({Path::GetCapturePath(), GOrbitApp->GetCaptureFileName()})
          .c_str(),
      "*.orbit");
  if (file.isEmpty()) {
    return;
  }

  outcome::result<void, std::string> result =
      GOrbitApp->OnSaveCapture(file.toStdString());
  if (result.has_error()) {
    QMessageBox::critical(
        this, "Error saving capture",
        absl::StrFormat("Could not save capture in \"%s\":\n%s.",
                        file.toStdString(), result.error())
            .c_str());
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionOpen_Capture_triggered() {
  QString file = QFileDialog::getOpenFileName(
      this, "Open capture...", QString::fromStdString(Path::GetCapturePath()),
      "*.orbit");
  if (file.isEmpty()) {
    return;
  }

  (void)OpenCapture(file.toStdString());
}

outcome::result<void> OrbitMainWindow::OpenCapture(
    const std::string& filepath) {
  outcome::result<void, std::string> result =
      GOrbitApp->OnLoadCapture(filepath);

  if (result.has_error()) {
    SetTitle({});
    QMessageBox::critical(this, "Error loading capture",
                          QString::fromStdString(absl::StrFormat(
                              "Could not load capture from \"%s\":\n%s.",
                              filepath, result.error())));
    return std::errc::no_such_file_or_directory;
  }
  SetTitle(QString::fromStdString(filepath));
  ui->MainTabWidget->setCurrentWidget(ui->CaptureTab);
  return outcome::success();
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionShow_Includes_Util_triggered() {
  ShowIncludesDialog* dialog = new ShowIncludesDialog(this);
  dialog->setWindowTitle("Orbit Show Includes Utility");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowMinimizeButtonHint |
                         Qt::WindowMaximizeButtonHint);
  dialog->show();
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::OpenDisassembly(const std::string& a_String) {
  OrbitDisassemblyDialog* dialog = new OrbitDisassemblyDialog(this);
  dialog->SetText(a_String);
  dialog->setWindowTitle("Orbit Disassembly");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowMinimizeButtonHint |
                         Qt::WindowMaximizeButtonHint);
  dialog->show();
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::SetTitle(const QString& task_description) {
  if (task_description.isEmpty()) {
    setWindowTitle(QString("%1 %2").arg(QApplication::applicationName(),
                                        QApplication::applicationVersion()));
  } else {
    setWindowTitle(QString("%1 %2 - %3")
                       .arg(QApplication::applicationName(),
                            QApplication::applicationVersion(),
                            task_description));
  }
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionOutputDebugString_triggered(bool checked) {
  GOrbitApp->EnableOutputDebugString(checked);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionUploadDumpsToServer_triggered(bool checked) {
  GOrbitApp->EnableUploadDumpsToServer(checked);
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionCheckFalse_triggered() { CHECK(false); }

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionNullPointerDereference_triggered() {
  int* null_pointer = nullptr;
  *null_pointer = 0;
}

//-----------------------------------------------------------------------------
void InfiniteRecursion(int num) {
  if (num != 1) {
    InfiniteRecursion(num);
  }
  LOG("%s", VAR_TO_STR(num).c_str());
}

//-----------------------------------------------------------------------------
void OrbitMainWindow::on_actionStackOverflow_triggered() {
  InfiniteRecursion(0);
}
