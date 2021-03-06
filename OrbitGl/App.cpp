// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
#include "OrbitAsio.h"
// clang-format on

#include "App.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <utility>
#include <outcome.hpp>

#include "OrbitBase/Logging.h"
#include "OrbitBase/Tracing.h"

#include "CallStackDataView.h"
#include "Callstack.h"
#include "Capture.h"
#include "CaptureSerializer.h"
#include "CaptureWindow.h"
#include "ConnectionManager.h"
#include "Debugger.h"
#include "Disassembler.h"
#include "EventTracer.h"
#include "FunctionsDataView.h"
#include "GlCanvas.h"
#include "GlobalsDataView.h"
#include "ImGuiOrbit.h"
#include "Injection.h"
#include "Introspection.h"
#include "KeyAndString.h"
#include "LinuxCallstackEvent.h"
#include "LiveFunctionsDataView.h"
#include "Log.h"
#include "LogDataView.h"
#include "ModulesDataView.h"
#include "OrbitAsm.h"
#include "OrbitSession.h"
#include "Params.h"
#include "Pdb.h"
#include "PluginManager.h"
#include "PrintVar.h"
#include "ProcessesDataView.h"
#include "SamplingProfiler.h"
#include "SamplingReport.h"
#include "ScopeTimer.h"
#include "Serialization.h"
#include "SessionsDataView.h"
#include "StringManager.h"
#include "SymbolsClient.h"
#include "Systrace.h"
#include "Tcp.h"
#include "TcpClient.h"
#include "TcpServer.h"
#include "TestRemoteMessages.h"
#include "TextRenderer.h"
#include "TimerManager.h"
#include "TypesDataView.h"
#include "Utils.h"
#include "Version.h"

#define GLUT_DISABLE_ATEXIT_HACK
#include "GL/freeglut.h"

#if __linux__
#include <OrbitLinuxTracing/OrbitTracing.h>
#endif

std::unique_ptr<OrbitApp> GOrbitApp;
float GFontSize;
bool DoZoom = false;

//-----------------------------------------------------------------------------
OrbitApp::OrbitApp(ApplicationOptions&& options)
    : options_(std::move(options)) {
  main_thread_executor_ = MainThreadExecutor::Create();
#ifdef _WIN32
  m_Debugger = std::make_unique<Debugger>();
#endif
}

//-----------------------------------------------------------------------------
OrbitApp::~OrbitApp() {
#ifdef _WIN32
  oqpi_tk::stop_scheduler();
#endif
}

//-----------------------------------------------------------------------------
std::string OrbitApp::FindFile(const std::string& caption,
                               const std::string& dir,
                               const std::string& filter) {
  if (m_FindFileCallback) {
    return m_FindFileCallback(caption, dir, filter);
  }

  return std::string();
}

//-----------------------------------------------------------------------------
void OrbitApp::SetCommandLineArguments(const std::vector<std::string>& a_Args) {
  m_Arguments = a_Args;

  for (const std::string& arg : a_Args) {
    if (absl::StrContains(arg, "preset:")) {
      std::vector<std::string> vec = absl::StrSplit(arg, ":");
      if (vec.size() > 1) {
        Capture::GPresetToLoad = vec[1];
      }
    } else if (absl::StrContains(arg, "inject:")) {
      std::vector<std::string> vec = absl::StrSplit(arg, ":");
      if (vec.size() > 1) {
        Capture::GProcessToInject = vec[1];
      }
    } else if (absl::StrContains(arg, "systrace:")) {
      m_PostInitArguments.push_back(arg);
    }
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::ProcessTimer(const Timer& timer) {
  GCurrentTimeGraph->ProcessTimer(timer);
}

//-----------------------------------------------------------------------------
void OrbitApp::ProcessSamplingCallStack(LinuxCallstackEvent& a_CallStack) {
  Capture::GSamplingProfiler->AddCallStack(a_CallStack.callstack_);
  GEventTracer.GetEventBuffer().AddCallstackEvent(
      a_CallStack.time_, a_CallStack.callstack_.m_Hash,
      a_CallStack.callstack_.m_ThreadId);
}

//-----------------------------------------------------------------------------
void OrbitApp::ProcessHashedSamplingCallStack(CallstackEvent& a_CallStack) {
  if (Capture::GSamplingProfiler == nullptr) {
    ERROR("GSamplingProfiler is null, ignoring callstack event.");
    return;
  }
  Capture::GSamplingProfiler->AddHashedCallStack(a_CallStack);
  GEventTracer.GetEventBuffer().AddCallstackEvent(
      a_CallStack.m_Time, a_CallStack.m_Id, a_CallStack.m_TID);
}

//-----------------------------------------------------------------------------
void OrbitApp::AddAddressInfo(LinuxAddressInfo address_info) {
  uint64_t address = address_info.address;
  Capture::GAddressInfos.emplace(address, std::move(address_info));
}

//-----------------------------------------------------------------------------
void OrbitApp::AddKeyAndString(uint64_t key, std::string_view str) {
  string_manager_->AddIfNotPresent(key, str);
}

//-----------------------------------------------------------------------------
void OrbitApp::UpdateThreadName(uint32_t thread_id,
                                const std::string& thread_name) {
  Capture::GTargetProcess->SetThreadName(thread_id, thread_name);
}

//-----------------------------------------------------------------------------
void OrbitApp::LoadSystrace(const std::string& a_FileName) {
  SystraceManager::Get().Clear();
  Capture::ClearCaptureData();
  GCurrentTimeGraph->Clear();
  if (Capture::GClearCaptureDataFunc) {
    Capture::GClearCaptureDataFunc();
  }

  std::shared_ptr<Systrace> systrace =
      std::make_shared<Systrace>(a_FileName.c_str());

  for (Function& func : systrace->GetFunctions()) {
    Capture::GSelectedFunctionsMap[func.GetVirtualAddress()] = &func;
  }
  Capture::GVisibleFunctionsMap = Capture::GSelectedFunctionsMap;

  for (const auto& timer : systrace->GetTimers()) {
    GCurrentTimeGraph->ProcessTimer(timer);
    ++Capture::GFunctionCountMap[timer.m_FunctionAddress];
  }

  for (const auto& pair : systrace->GetThreadNames()) {
    Capture::GTargetProcess->SetThreadName(pair.first, pair.second);
  }

  SystraceManager::Get().Add(systrace);
  GOrbitApp->FireRefreshCallbacks();
  GOrbitApp->StopCapture();
  DoZoom = true;  // TODO: remove global, review logic
}

//-----------------------------------------------------------------------------
void OrbitApp::AppendSystrace(const std::string& a_FileName,
                              uint64_t a_TimeOffset) {
  std::shared_ptr<Systrace> systrace =
      std::make_shared<Systrace>(a_FileName.c_str(), a_TimeOffset);

  for (Function& func : systrace->GetFunctions()) {
    Capture::GSelectedFunctionsMap[func.GetVirtualAddress()] = &func;
  }
  Capture::GVisibleFunctionsMap = Capture::GSelectedFunctionsMap;

  for (const auto& timer : systrace->GetTimers()) {
    GCurrentTimeGraph->ProcessTimer(timer);
    Capture::GFunctionCountMap[timer.m_FunctionAddress];
  }

  for (const auto& pair : systrace->GetThreadNames()) {
    Capture::GTargetProcess->SetThreadName(pair.first, pair.second);
  }

  SystraceManager::Get().Add(systrace);
  GOrbitApp->FireRefreshCallbacks();
  GOrbitApp->StopCapture();
  DoZoom = true;  // TODO: remove global, review logic
}

//-----------------------------------------------------------------------------
bool OrbitApp::Init(ApplicationOptions&& options) {
  GOrbitApp = std::make_unique<OrbitApp>(std::move(options));
  GCoreApp = GOrbitApp.get();

  GTimerManager = std::make_unique<TimerManager>();
  GTcpServer = std::make_unique<TcpServer>();

  Path::Init();

  Capture::Init();

#ifdef _WIN32
  oqpi_tk::start_default_scheduler();
#endif

  GPluginManager.Initialize();

  GParams.Load();
  GFontSize = GParams.m_FontSize;
  GOrbitApp->LoadFileMapping();

  return true;
}

//-----------------------------------------------------------------------------
void OrbitApp::PostInit() {
  if (!options_.asio_server_address.empty()) {
    GTcpClient = std::make_unique<TcpClient>();
    GTcpClient->AddMainThreadCallback(
        Msg_RemoteProcess,
        [=](const Message& a_Msg) { GOrbitApp->OnRemoteProcess(a_Msg); });
    ConnectionManager::Get().ConnectToRemote(options_.asio_server_address);
    SetIsRemote(true);
  }

  if (!options_.grpc_server_address.empty()) {
    grpc_channel_ = grpc::CreateChannel(options_.grpc_server_address,
                                        grpc::InsecureChannelCredentials());
    if (!grpc_channel_) {
      ERROR("Unable to create GRPC channel to %s",
            options_.grpc_server_address);
    }

    // TODO: Replace refresh_timeout with config option. Let users to modify it.
    process_manager_ =
        ProcessManager::Create(grpc_channel_, absl::Milliseconds(1000));

    auto callback = [&](ProcessManager* process_manager) {
      main_thread_executor_->Schedule([&, process_manager]() {
        m_ProcessesDataView->SetProcessList(process_manager->GetProcessList());
        FireRefreshCallbacks(DataViewType::PROCESSES);
      });
    };

    process_manager_->SetProcessListUpdateListener(callback);
  }

  ListSessions();

  string_manager_ = std::make_shared<StringManager>();
  GCurrentTimeGraph->SetStringManager(string_manager_);

  for (std::string& arg : m_PostInitArguments) {
    if (absl::StrContains(arg, "systrace:")) {
      std::string command = Replace(arg, "systrace:", "");
      std::vector<std::string> tokens = absl::StrSplit(command, ",");
      if (!tokens.empty()) {
        GoToCapture();
        LoadSystrace(tokens[0]);
      }
      for (size_t i = 1; i + 1 < tokens.size(); i += 2) {
        AppendSystrace(tokens[i], std::stoull(tokens[i + 1]));
      }
      SystraceManager::Get().Dump();
    }
  }

  GOrbitApp->InitializeClientTransactions();
}

//-----------------------------------------------------------------------------
void OrbitApp::LoadFileMapping() {
  m_FileMapping.clear();
  std::string fileName = Path::GetFileMappingFileName();
  if (!Path::FileExists(fileName)) {
    std::ofstream outfile(fileName);
    outfile << "//-------------------" << std::endl
            << "// Orbit File Mapping" << std::endl
            << "//-------------------" << std::endl
            << "// If the file path in the pdb is \"D:\\NoAccess\\File.cpp\""
            << std::endl
            << "// and File.cpp is locally available in \"C:\\Available\\\""
            << std::endl
            << "// then enter a file mapping on its own line like so:"
            << std::endl
            << "// \"D:\\NoAccess\\File.cpp\" \"C:\\Available\\\"" << std::endl
            << std::endl
            << "\"D:\\NoAccess\" \"C:\\Available\"" << std::endl;

    outfile.close();
  }

  std::fstream infile(fileName);
  if (!infile.fail()) {
    std::string line;
    while (std::getline(infile, line)) {
      if (absl::StartsWith(line, "//")) continue;

      bool containsQuotes = absl::StrContains(line, "\"");

      std::vector<std::string> tokens = absl::StrSplit(line, ' ');

      if (tokens.size() == 2 && !containsQuotes) {
        m_FileMapping[ToLower(tokens[0])] = ToLower(tokens[1]);
      } else {
        std::vector<std::string> validTokens;
        std::vector<std::string> tokens = absl::StrSplit(line, '"');
        for (const std::string& token : tokens) {
          if (!IsBlank(token)) {
            validTokens.push_back(token);
          }
        }

        if (validTokens.size() > 1) {
          m_FileMapping[ToLower(validTokens[0])] = ToLower(validTokens[1]);
        }
      }
    }
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::ListSessions() {
  std::vector<std::string> sessionFileNames =
      Path::ListFiles(Path::GetPresetPath(), ".opr");
  std::vector<std::shared_ptr<Session>> sessions;
  for (std::string& filename : sessionFileNames) {
    std::ifstream file(filename, std::ios::binary);
    if (file.fail()) {
      ERROR("Loading session from \"%s\": %s", filename, "file.fail()");
      continue;
    }

    try {
      auto session = std::make_shared<Session>();
      cereal::BinaryInputArchive archive(file);
      archive(*session);
      file.close();
      session->m_FileName = filename;
      sessions.push_back(session);
    } catch (std::exception& e) {
      ERROR("Loading session from \"%s\": %s", filename, e.what());
    }
  }

  m_SessionsDataView->SetSessions(sessions);
}

//-----------------------------------------------------------------------------
void OrbitApp::RefreshCaptureView() {
  NeedsRedraw();
  GOrbitApp->FireRefreshCallbacks();
  DoZoom = true;  // TODO: remove global, review logic
}

//-----------------------------------------------------------------------------
void OrbitApp::AddWatchedVariable(Variable* a_Variable) {
#ifdef _WIN32
  for (WatchCallback& callback : m_AddToWatchCallbacks) {
    callback(a_Variable);
  }
#else
  UNUSED(a_Variable);
#endif
}

//-----------------------------------------------------------------------------
void OrbitApp::UpdateVariable(Variable* a_Variable) {
  for (WatchCallback& callback : m_UpdateWatchCallbacks) {
    callback(a_Variable);
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::ClearWatchedVariables() {
  if (Capture::GTargetProcess) {
    Capture::GTargetProcess->ClearWatchedVariables();
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::RefreshWatch() {
  if (Capture::Connect(options_.asio_server_address)) {
    Capture::GTargetProcess->RefreshWatchedVariables();
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::Disassemble(const std::string& a_FunctionName,
                           uint64_t a_VirtualAddress,
                           const uint8_t* a_MachineCode, size_t a_Size) {
  Disassembler disasm;
  disasm.LOGF(absl::StrFormat("asm: /* %s */\n", a_FunctionName.c_str()));
  disasm.Disassemble(a_MachineCode, a_Size, a_VirtualAddress,
                     Capture::GTargetProcess->GetIs64Bit());
  SendToUi(disasm.GetResult());
}

//-----------------------------------------------------------------------------
void OrbitApp::OnExit() {
  if (GTimerManager && GTimerManager->m_IsRecording) {
    StopCapture();
  }

  GParams.Save();

  ConnectionManager::Get().Stop();
  GTcpClient->Stop();

  if (HasTcpServer()) {
    GTcpServer->Stop();
  }

  process_manager_->Shutdown();

  GTimerManager = nullptr;
  GCoreApp = nullptr;
  GOrbitApp = nullptr;
  Orbit_ImGui_Shutdown();
}

//-----------------------------------------------------------------------------
Timer GMainTimer;

//-----------------------------------------------------------------------------
// TODO: make it non-static
void OrbitApp::MainTick() {
  ORBIT_SCOPE_FUNC;
  TRACE_VAR(GMainTimer.QueryMillis());

  if (GTcpServer) GTcpServer->ProcessMainThreadCallbacks();
  if (GTcpClient) GTcpClient->ProcessMainThreadCallbacks();

  GOrbitApp->main_thread_executor_->ConsumeActions();

  // Tick Transaction manager only from client (OrbitApp is client only);
  auto transaction_manager = GOrbitApp->GetTransactionClient();

  // Note that MainTick could be called before OrbitApp::PostInit() was complete
  // in which case translaction namager is not yet initialized - check that it
  // is not null before calling it.
  if (transaction_manager != nullptr) {
    transaction_manager->Tick();
  }

  GMainTimer.Reset();
  GTcpServer->MainThreadTick();

  if (!Capture::GProcessToInject.empty()) {
    LOG("Injecting into %s",Capture::GTargetProcess->GetFullPath());
    LOG("Orbit host: %s", GOrbitApp->options_.asio_server_address);
    GOrbitApp->SelectProcess(Capture::GProcessToInject);
    Capture::InjectRemote(GOrbitApp->options_.asio_server_address);
    exit(0);
  }

#ifdef _WIN32
  GOrbitApp->m_Debugger->MainTick();
#endif

  ++GOrbitApp->m_NumTicks;

  if (DoZoom) {
    GCurrentTimeGraph->SortTracks();
    GOrbitApp->m_CaptureWindow->ZoomAll();
    GOrbitApp->NeedsRedraw();
    DoZoom = false;
  }
}

//-----------------------------------------------------------------------------
std::string OrbitApp::GetVersion() { return OrbitVersion::GetVersion(); }

//-----------------------------------------------------------------------------
void OrbitApp::RegisterCaptureWindow(CaptureWindow* a_Capture) {
  assert(m_CaptureWindow == nullptr);
  m_CaptureWindow = a_Capture;
}

//-----------------------------------------------------------------------------
void OrbitApp::NeedsRedraw() { m_CaptureWindow->NeedsUpdate(); }

//-----------------------------------------------------------------------------
void OrbitApp::AddSamplingReport(
    std::shared_ptr<SamplingProfiler>& sampling_profiler) {
  auto report = std::make_shared<SamplingReport>(sampling_profiler);

  for (SamplingReportCallback& callback : m_SamplingReportsCallbacks) {
    DataView* callstack_data_view =
        GetOrCreateDataView(DataViewType::CALLSTACK);
    callback(callstack_data_view, report);
  }

  sampling_report_ = report;
}

//-----------------------------------------------------------------------------
void OrbitApp::AddSelectionReport(
    std::shared_ptr<SamplingProfiler>& a_SamplingProfiler) {
  auto report = std::make_shared<SamplingReport>(a_SamplingProfiler);

  for (SamplingReportCallback& callback : m_SelectionReportCallbacks) {
    DataView* callstack_data_view =
        GetOrCreateDataView(DataViewType::CALLSTACK);
    callback(callstack_data_view, report);
  }

  selection_report_ = report;
}

//-----------------------------------------------------------------------------
void OrbitApp::GoToCode(DWORD64 a_Address) {
  m_CaptureWindow->FindCode(a_Address);
  SendToUi("gotocode");
}

//-----------------------------------------------------------------------------
void OrbitApp::GoToCallstack() { SendToUi("gotocallstack"); }

//-----------------------------------------------------------------------------
void OrbitApp::GoToCapture() { SendToUi("gotocapture"); }

//-----------------------------------------------------------------------------
void OrbitApp::OnLaunchProcess(const std::string& process_name,
                               const std::string& working_dir,
                               const std::string& args) {
#ifdef _WIN32
  m_Debugger->LaunchProcess(process_name, working_dir, args);
#else
  UNUSED(process_name);
  UNUSED(working_dir);
  UNUSED(args);
#endif
}

//-----------------------------------------------------------------------------
std::string OrbitApp::GetCaptureFileName() {
  CHECK(Capture::GTargetProcess != nullptr);
  time_t timestamp =
      std::chrono::system_clock::to_time_t(Capture::GCaptureTimePoint);
  std::string result;
  result.append(Path::StripExtension(Capture::GTargetProcess->GetName()));
  result.append("_");
  result.append(OrbitUtils::FormatTime(timestamp));
  result.append(".orbit");
  return result;
}

//-----------------------------------------------------------------------------
std::string OrbitApp::GetSessionFileName() {
  return Capture::GSessionPresets ? Capture::GSessionPresets->m_FileName : "";
}

//-----------------------------------------------------------------------------
std::string OrbitApp::GetSaveFile(const std::string& extension) {
  if (!m_SaveFileCallback) return "";
  return m_SaveFileCallback(extension);
}

//-----------------------------------------------------------------------------
void OrbitApp::SetClipboard(const std::string& text) {
  if (m_ClipboardCallback) m_ClipboardCallback(text);
}

//-----------------------------------------------------------------------------
outcome::result<void, std::string> OrbitApp::OnSaveSession(
    const std::string& file_name) {
  OUTCOME_TRY(Capture::SaveSession(file_name));
  ListSessions();
  Refresh(DataViewType::SESSIONS);
  return outcome::success();
}

//-----------------------------------------------------------------------------
outcome::result<void, std::string> OrbitApp::OnLoadSession(
    const std::string& file_name) {
  std::string file_path = file_name;

  if (Path::GetDirectory(file_name).empty()) {
    file_path = Path::JoinPath({Path::GetPresetPath(), file_name});
  }

  std::ifstream file(file_path);
  if (file.fail()) {
    ERROR("Loading session from \"%s\": %s", file_path, "file.fail()");
    return outcome::failure("Error opening the file for reading");
  }

  try {
    auto session = std::make_shared<Session>();
    cereal::BinaryInputArchive archive(file);
    archive(*session);
    file.close();
    session->m_FileName = file_path;
    LoadSession(session);
    return outcome::success();
  } catch (std::exception& e) {
    ERROR("Loading session from \"%s\": %s", file_path, e.what());
    return outcome::failure("Error parsing the session");
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::LoadSession(const std::shared_ptr<Session>& session) {
  if (SelectProcess(Path::GetFileName(session->m_ProcessFullPath))) {
    Capture::GSessionPresets = session;
  }
}

//-----------------------------------------------------------------------------
outcome::result<void, std::string> OrbitApp::OnSaveCapture(
    const std::string& file_name) {
  CaptureSerializer ar;
  ar.time_graph_ = GCurrentTimeGraph;
  return ar.Save(file_name);
}

//-----------------------------------------------------------------------------
outcome::result<void, std::string> OrbitApp::OnLoadCapture(
    const std::string& file_name) {
  Capture::ClearCaptureData();
  GCurrentTimeGraph->Clear();
  if (Capture::GClearCaptureDataFunc) {
    Capture::GClearCaptureDataFunc();
  }

  CaptureSerializer ar;
  ar.time_graph_ = GCurrentTimeGraph;
  OUTCOME_TRY(ar.Load(file_name));

  m_ModulesDataView->SetProcess(Capture::GTargetProcess);
  DoZoom = true;  // TODO: remove global, review logic
  return outcome::success();
}

//-----------------------------------------------------------------------------
void OrbitApp::OnDisconnect() { GTcpServer->Send(Msg_Unload); }

//-----------------------------------------------------------------------------
void OrbitApp::OnPdbLoaded() {
  FireRefreshCallbacks();

  if (m_ModulesToLoad.empty()) {
    SendToUi("pdbloaded");
  } else {
    LoadModules();
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::FireRefreshCallbacks(DataViewType a_Type) {
  for (DataView* panel : m_Panels) {
    if (a_Type == DataViewType::ALL || a_Type == panel->GetType()) {
      panel->OnDataChanged();
    }
  }

  // UI callbacks
  for (RefreshCallback& callback : m_RefreshCallbacks) {
    callback(a_Type);
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::AddUiMessageCallback(
    std::function<void(const std::string&)> a_Callback) {
  GTcpServer->SetUiCallback(a_Callback);
  m_UiCallback = a_Callback;
}

bool OrbitApp::StartCapture() {
  if (Capture::IsCapturing()) {
    LOG("Ignoring Start Capture - already capturing...");
    return false;
  }

  outcome::result<void, std::string> result =
      Capture::StartCapture(options_.asio_server_address);
  if (result.has_error()) {
    SendErrorToUi("Error starting capture", result.error());
    return false;
  }

  if (m_NeedsThawing) {
#ifdef _WIN32
    m_Debugger->SendThawMessage();
#endif
    m_NeedsThawing = false;
  }

  for (const CaptureStartedCallback& callback : capture_started_callbacks_) {
    callback();
  }

  return true;
}

//-----------------------------------------------------------------------------
void OrbitApp::StopCapture() {
  Capture::StopCapture();

  for (const CaptureStopRequestedCallback& callback :
       capture_stop_requested_callbacks_) {
    callback();
  }
  FireRefreshCallbacks();
}

void OrbitApp::OnCaptureStopped() {
  Capture::FinalizeCapture();

  AddSamplingReport(Capture::GSamplingProfiler);

  for (const CaptureStopRequestedCallback& callback :
       capture_stopped_callbacks_) {
    callback();
  }
  FireRefreshCallbacks();
}

//-----------------------------------------------------------------------------
void OrbitApp::ToggleCapture() {
  if (!GTimerManager) {
    return;
  }

  if (Capture::IsCapturing()) {
    StopCapture();
  } else {
    StartCapture();
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::Unregister(DataView* a_Model) {
  for (size_t i = 0; i < m_Panels.size(); ++i) {
    if (m_Panels[i] == a_Model) {
      m_Panels.erase(m_Panels.begin() + i);
    }
  }
}

//-----------------------------------------------------------------------------
bool OrbitApp::SelectProcess(const std::string& a_Process) {
  if (m_ProcessesDataView) {
    return m_ProcessesDataView->SelectProcess(a_Process);
  }

  return false;
}

//-----------------------------------------------------------------------------
bool OrbitApp::SelectProcess(uint32_t a_ProcessID) {
  if (m_ProcessesDataView) {
    return m_ProcessesDataView->SelectProcess(a_ProcessID);
  }

  return false;
}

//-----------------------------------------------------------------------------
bool OrbitApp::Inject(unsigned long a_ProcessId) {
  if (SelectProcess(a_ProcessId)) {
    return Capture::Inject(options_.asio_server_address);
  }

  return false;
}

//-----------------------------------------------------------------------------
void OrbitApp::SetCallStack(std::shared_ptr<CallStack> a_CallStack) {
  m_CallStackDataView->SetCallStack(std::move(a_CallStack));
  FireRefreshCallbacks(DataViewType::CALLSTACK);
}

//-----------------------------------------------------------------------------
void OrbitApp::SendToUi(const std::string& message) {
  main_thread_executor_->Schedule([&, message] {
    if (m_UiCallback) {
      m_UiCallback(message);
    }
  });
}

//-----------------------------------------------------------------------------
void OrbitApp::SendInfoToUi(const std::string& title, const std::string& text) {
  std::string message = "info:" + title + "\n" + text;
  SendToUi(message);
}

//-----------------------------------------------------------------------------
void OrbitApp::SendErrorToUi(const std::string& title,
                             const std::string& text) {
  std::string message = "error:" + title + "\n" + text;
  SendToUi(message);
}

//-----------------------------------------------------------------------------
void OrbitApp::EnqueueModuleToLoad(const std::shared_ptr<Module>& a_Module) {
  m_ModulesToLoad.push_back(a_Module);
}

//-----------------------------------------------------------------------------
void OrbitApp::LoadModules() {
  if (!m_ModulesToLoad.empty()) {
    LoadRemoteModules();
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::LoadRemoteModules() {
  GetSymbolsClient()->LoadSymbolsFromModules(Capture::GTargetProcess.get(),
                                             m_ModulesToLoad, nullptr);
  m_ModulesToLoad.clear();
  // This is a bit counterintuitive. LoadSymbols generates request to
  // the service if symbols cannot be loaded locally. In which case
  // the UI update happens in OnRemoteModuleDebugInfo. But if all symbols
  // are loaded locally it will not call remote and OnRemoteModuleDebugInfo
  // are never called. For this case we need to make sure sampling report is
  // updated here as well.
  // TODO: LoadSymbolsFromModules should always call a callback even when
  // symbols are loaded locally. Remove this call after this is done.
  UpdateSamplingReport();
  GOrbitApp->FireRefreshCallbacks();
}

//-----------------------------------------------------------------------------
bool OrbitApp::IsLoading() { return GPdbDbg->IsLoading(); }

//-----------------------------------------------------------------------------
void OrbitApp::SetTrackContextSwitches(bool a_Value) {
  GParams.m_TrackContextSwitches = a_Value;
}

//-----------------------------------------------------------------------------
bool OrbitApp::GetTrackContextSwitches() {
  return GParams.m_TrackContextSwitches;
}

//-----------------------------------------------------------------------------
void OrbitApp::EnableUnrealSupport(bool a_Value) {
  GParams.m_UnrealSupport = a_Value;
}

//-----------------------------------------------------------------------------
bool OrbitApp::GetUnrealSupportEnabled() { return GParams.m_UnrealSupport; }

//-----------------------------------------------------------------------------
void OrbitApp::EnableUnsafeHooking(bool a_Value) {
  GParams.m_AllowUnsafeHooking = a_Value;
}

//-----------------------------------------------------------------------------
bool OrbitApp::GetUnsafeHookingEnabled() {
  return GParams.m_AllowUnsafeHooking;
}

//-----------------------------------------------------------------------------
bool OrbitApp::GetOutputDebugStringEnabled() {
  return GParams.m_HookOutputDebugString;
}

//-----------------------------------------------------------------------------
void OrbitApp::EnableOutputDebugString(bool a_Value) {
  GParams.m_HookOutputDebugString = a_Value;
}

//-----------------------------------------------------------------------------
void OrbitApp::EnableSampling(bool a_Value) {
  GParams.m_TrackSamplingEvents = a_Value;
}

//-----------------------------------------------------------------------------
bool OrbitApp::GetSamplingEnabled() { return GParams.m_TrackSamplingEvents; }

//-----------------------------------------------------------------------------

void OrbitApp::OnProcessSelected(uint32_t pid) {
  Message msg(Msg_RemoteProcessRequest);
  msg.m_Header.m_GenericHeader.m_Address = pid;
  GTcpClient->Send(msg);

  std::shared_ptr<Process> process = FindProcessByPid(pid);

  if (process) {
    m_ModulesDataView->SetProcess(process);
    Capture::SetTargetProcess(process);
    FireRefreshCallbacks();
  }
}

//-----------------------------------------------------------------------------
bool OrbitApp::GetUploadDumpsToServerEnabled() const {
  return GParams.m_UploadDumpsToServer;
}

//-----------------------------------------------------------------------------
void OrbitApp::EnableUploadDumpsToServer(bool a_Value) {
  GParams.m_UploadDumpsToServer = a_Value;
  GParams.Save();
}

//-----------------------------------------------------------------------------
void OrbitApp::OnRemoteProcess(const Message& a_Message) {
  std::istringstream buffer(a_Message.GetDataAsString());
  cereal::JSONInputArchive inputAr(buffer);
  std::shared_ptr<Process> remote_process = std::make_shared<Process>();
  inputAr(*remote_process);
  remote_process->SetIsRemote(true);
  PRINT_VAR(remote_process->GetName());

  UpdateProcess(remote_process);

  if (remote_process->GetID() == m_ProcessesDataView->GetSelectedProcessId()) {
    m_ModulesDataView->SetProcess(remote_process);
    // Is this needed?
    Capture::SetTargetProcess(remote_process);
    FireRefreshCallbacks();
  }

  // Trigger session loading if needed.
  std::shared_ptr<Session> session = Capture::GSessionPresets;
  if (session) {
    GetSymbolsClient()->LoadSymbolsFromSession(remote_process.get(), session);
    GParams.m_ProcessPath = session->m_ProcessFullPath;
    GParams.m_Arguments = session->m_Arguments;
    GParams.m_WorkingDirectory = session->m_WorkingDirectory;
    GCoreApp->SendToUi("SetProcessParams");
    Capture::GSessionPresets = nullptr;
  }
}

//-----------------------------------------------------------------------------
void OrbitApp::ApplySession(const Session& session) {
  for (const auto& pair : session.m_Modules) {
    const std::string& name = pair.first;
    std::shared_ptr<Module> module =
        Capture::GTargetProcess->GetModuleFromName(Path::GetFileName(name));
    if (module && module->m_Pdb) module->m_Pdb->ApplyPresets(session);
  }

  FireRefreshCallbacks();
}

std::shared_ptr<Process> OrbitApp::FindProcessByPid(uint32_t pid) {
  absl::MutexLock lock(&process_map_mutex_);
  auto it = process_map_.find(pid);
  if (it == process_map_.end()) {
    return nullptr;
  }

  return it->second;
}

void OrbitApp::UpdateProcess(const std::shared_ptr<Process>& process) {
  absl::MutexLock lock(&process_map_mutex_);
  // We want to move function map to new process object
  auto old_process_it = process_map_.find(process->GetID());
  if (old_process_it != process_map_.end()) {
    // TODO: all the following needs to be addressed by
    // separating client-only part of information to
    // separate structures.
    std::shared_ptr<Process> old_process = old_process_it->second;
    std::map<uint64_t, std::shared_ptr<Module>>& old_modules =
        old_process->GetModules();
    std::map<uint64_t, std::shared_ptr<Module>>& new_modules =
        process->GetModules();

    // Move 'loaded' flag to new modules if possible, together with m_Pdb,
    // needed when loading sessions as Pdb::ApplyPresets is called.
    for (auto module_entry : old_modules) {
      uint64_t key = module_entry.first;
      auto new_module_it = new_modules.find(key);
      if (new_module_it != new_modules.end()) {
        new_module_it->second->SetLoaded(module_entry.second->IsLoaded());
        new_module_it->second->m_Pdb = module_entry.second->m_Pdb;
      }
    }

    process->SetFunctions(old_process->GetFunctions());
  }
  process_map_.insert_or_assign(process->GetID(), process);
}

//-----------------------------------------------------------------------------
void OrbitApp::OnRemoteModuleDebugInfo(
    const std::vector<ModuleDebugInfo>& remote_module_debug_infos) {
  for (const ModuleDebugInfo& module_info : remote_module_debug_infos) {
    std::shared_ptr<Module> module =
        Capture::GTargetProcess->GetModuleFromName(module_info.m_Name);

    if (!module) {
      ERROR("Could not find module %s", module_info.m_Name.c_str());
      continue;
    }

    if (module_info.m_Functions.empty()) {
      ERROR("Remote did not send any symbols for module %s",
            module_info.m_Name.c_str());
      continue;
    }
    symbol_helper_.LoadSymbolsFromDebugInfo(module, module_info);
    LOG("Received %lu function symbols from remote service for module %s",
        module_info.m_Functions.size(), module_info.m_Name.c_str());
  }

  UpdateSamplingReport();
  GOrbitApp->FireRefreshCallbacks();
}

void OrbitApp::UpdateSamplingReport() {
  if (sampling_report_ != nullptr) {
    sampling_report_->UpdateReport();
  }

  if (selection_report_ != nullptr) {
    selection_report_->UpdateReport();
  }
}

//-----------------------------------------------------------------------------
DataView* OrbitApp::GetOrCreateDataView(DataViewType type) {
  switch (type) {
    case DataViewType::FUNCTIONS:
      if (!m_FunctionsDataView) {
        m_FunctionsDataView = std::make_unique<FunctionsDataView>();
        m_Panels.push_back(m_FunctionsDataView.get());
      }
      return m_FunctionsDataView.get();

    case DataViewType::TYPES:
      if (!m_TypesDataView) {
        m_TypesDataView = std::make_unique<TypesDataView>();
        m_Panels.push_back(m_TypesDataView.get());
      }
      return m_TypesDataView.get();

    case DataViewType::LIVE_FUNCTIONS:
      if (!m_LiveFunctionsDataView) {
        m_LiveFunctionsDataView = std::make_unique<LiveFunctionsDataView>();
        m_Panels.push_back(m_LiveFunctionsDataView.get());
      }
      return m_LiveFunctionsDataView.get();

    case DataViewType::CALLSTACK:
      if (!m_CallStackDataView) {
        m_CallStackDataView = std::make_unique<CallStackDataView>();
        m_Panels.push_back(m_CallStackDataView.get());
      }
      return m_CallStackDataView.get();

    case DataViewType::GLOBALS:
      if (!m_GlobalsDataView) {
        m_GlobalsDataView = std::make_unique<GlobalsDataView>();
        m_Panels.push_back(m_GlobalsDataView.get());
      }
      return m_GlobalsDataView.get();

    case DataViewType::MODULES:
      if (!m_ModulesDataView) {
        m_ModulesDataView = std::make_unique<ModulesDataView>();
        m_Panels.push_back(m_ModulesDataView.get());
      }
      return m_ModulesDataView.get();

    case DataViewType::PROCESSES:
      if (!m_ProcessesDataView) {
        m_ProcessesDataView = std::make_unique<ProcessesDataView>();
        m_ProcessesDataView->SetSelectionListener(
            [&](uint32_t pid) { OnProcessSelected(pid); });
        m_Panels.push_back(m_ProcessesDataView.get());
      }
      return m_ProcessesDataView.get();

    case DataViewType::SESSIONS:
      if (!m_SessionsDataView) {
        m_SessionsDataView = std::make_unique<SessionsDataView>();
        m_Panels.push_back(m_SessionsDataView.get());
      }
      return m_SessionsDataView.get();

    case DataViewType::LOG:
      if (!m_LogDataView) {
        m_LogDataView = std::make_unique<LogDataView>();
        m_Panels.push_back(m_LogDataView.get());
      }
      return m_LogDataView.get();

    case DataViewType::SAMPLING:
      FATAL(
          "DataViewType::SAMPLING Data View construction is not supported by"
          "the factory.");

    case DataViewType::ALL:
      FATAL("DataViewType::ALL should not be used with the factory.");

    case DataViewType::INVALID:
      FATAL("DataViewType::INVALID should not be used with the factory.");
  }

  FATAL("Unreachable");
}

//-----------------------------------------------------------------------------
void OrbitApp::InitializeClientTransactions() {
  transaction_client_ = std::make_unique<TransactionClient>(GTcpClient.get());
  symbols_client_ =
      std::make_unique<SymbolsClient>(this, transaction_client_.get());
  frame_pointer_validator_client_ =
      std::make_unique<FramePointerValidatorClient>(this,
                                                    transaction_client_.get());
  process_memory_client_ =
      std::make_unique<ProcessMemoryClient>(transaction_client_.get());
}

//-----------------------------------------------------------------------------
void OrbitApp::FilterFunctions(const std::string& filter) {
  m_LiveFunctionsDataView->OnFilter(filter);
}
