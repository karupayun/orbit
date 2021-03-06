// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "Systrace.h"

#include <fstream>

#include "Capture.h"
#include "PrintVar.h"
#include "Profiling.h"
#include "ScopeTimer.h"

//-----------------------------------------------------------------------------
bool ShouldIgnore(const std::string& line) {
  return absl::StartsWith(line, "#") ||
         absl::StrContains(line, "<script class=") ||
         absl::StrContains(line, "</script>") ||
         absl::StrContains(line, "<!-- ");
}

//-----------------------------------------------------------------------------
bool IsBegin(const std::string& line) {
  return absl::StrContains(line, "tracing_mark_write: B");
}

//-----------------------------------------------------------------------------
bool IsEnd(const std::string& line) {
  return absl::StrContains(line, "tracing_mark_write: E");
}

//-----------------------------------------------------------------------------
bool IsTraceBegin(const std::string& line) {
  return absl::StrContains(line, "<!-- BEGIN TRACE -->");
}

//-----------------------------------------------------------------------------
bool IsTraceEnd(const std::string& line) {
  return absl::StrContains(line, "<!-- END TRACE -->");
}

//-----------------------------------------------------------------------------
std::string GetThreadName(const std::string& a_Line) {
  std::vector<std::string> tokens = absl::StrSplit(a_Line, "(");
  return !tokens.empty() ? tokens[0] : "unknown-thread-name";
}

//-----------------------------------------------------------------------------
std::string GetTimeStamp(const std::string& a_Line) {
  std::vector<std::string> tokens = absl::StrSplit(a_Line, "]");
  if (tokens.size() > 1) {
    tokens = absl::StrSplit(tokens[1], ' ');
    if (tokens.size() > 1) return tokens[1];
  }
  return "0";
}

//-----------------------------------------------------------------------------
std::string GetFunction(const std::string& a_Line) {
  std::vector<std::string> tokens = absl::StrSplit(a_Line, "|");
  return !tokens.empty() ? tokens.back() : "unknown-function";
}

//-----------------------------------------------------------------------------
uint32_t Systrace::GetThreadId(const std::string& a_ThreadName) {
  auto it = m_ThreadIDs.find(a_ThreadName);
  if (it == m_ThreadIDs.end()) {
    auto tid = SystraceManager::Get().GetNewThreadID();
    m_ThreadIDs[a_ThreadName] = tid;
    m_ThreadNames[tid] = a_ThreadName;
  }
  return m_ThreadIDs[a_ThreadName];
}

//-----------------------------------------------------------------------------
uint64_t Systrace::ProcessString(const std::string& a_String) {
  auto hash = StringHash(a_String);
  if (m_StringMap.find(hash) == m_StringMap.end()) {
    m_StringMap[hash] = a_String;
  }

  return hash;
}

//-----------------------------------------------------------------------------
uint64_t Systrace::ProcessFunctionName(const std::string& a_String) {
  std::vector<std::string> tokens = absl::StrSplit(a_String, "|");
  if (!tokens.empty()) {
    const std::string& name = tokens.back();
    uint64_t hash = ProcessString(name);
    Function func{name, name, hash, 0, 0, "", 0};
    m_Functions.push_back(func);
    return hash;
  }

  return 0;
}

//-----------------------------------------------------------------------------
const std::string& Systrace::GetFunctionName(uint64_t a_ID) const {
  static std::string defaultName = "";
  auto it = m_StringMap.find(a_ID);
  if (it == m_StringMap.end()) return defaultName;
  return it->second;
}

//-----------------------------------------------------------------------------
void Systrace::UpdateMinMax(const Timer& a_Timer) {
  if (a_Timer.m_Start < m_MinTime) m_MinTime = a_Timer.m_Start;
  if (a_Timer.m_End > m_MaxTime) m_MaxTime = a_Timer.m_End;
}

//-----------------------------------------------------------------------------
Systrace::Systrace(const char* a_FilePath, uint64_t a_TimeOffsetNs) {
  SCOPE_TIMER_LOG("Systrace Parsing");
  m_Name = a_FilePath;
  std::ifstream infile(a_FilePath);
  std::string line;
  m_TimeOffsetNs = a_TimeOffsetNs;
  bool foundBegin = false;
  while (std::getline(infile, line)) {
    if (IsTraceBegin(line)) foundBegin = true;
    if (!foundBegin) continue;
    if (IsTraceEnd(line)) break;

    bool isBegin = IsBegin(line);
    bool isEnd = IsEnd(line);
    if (isBegin || isEnd) {
      const std::string& threadName = GetThreadName(line);
      const std::string& timestamp = GetTimeStamp(line);

      if (isBegin) {
        Timer timer;
        timer.m_TID = GetThreadId(threadName);
        timer.m_Start = TicksFromMicroseconds(GetMicros(timestamp) +
                                              m_TimeOffsetNs * 0.001);

        timer.m_Depth = static_cast<uint8_t>(m_TimerStacks[threadName].size());
        const std::string& function = GetFunction(line);
        timer.m_FunctionAddress = ProcessFunctionName(function);
        m_TimerStacks[threadName].push_back(timer);
      }

      if (isEnd) {
        std::vector<Timer>& timers = m_TimerStacks[threadName];
        if (timers.size()) {
          Timer& timer = timers.back();
          timer.m_End = TicksFromMicroseconds(GetMicros(timestamp) +
                                              m_TimeOffsetNs * 0.001);
          m_Timers.push_back(timer);
          UpdateMinMax(timer);
          timers.pop_back();
        }
      }
    }
  }

  {
    SCOPE_TIMER_LOG("Function Map");
    for (auto& function : m_Functions) {
      // TODO: Should this be an absolute address of a function?
      m_FunctionMap[function.Address()] = &function;
    }
  }

  {
    SCOPE_TIMER_LOG("Update Timers");
    for (auto& timer : m_Timers) {
      m_FunctionMap[timer.m_FunctionAddress]->UpdateStats(timer);
    }
  }
}

//-----------------------------------------------------------------------------
SystraceManager& SystraceManager::Get() {
  static SystraceManager manager;
  return manager;
}

//-----------------------------------------------------------------------------
void SystraceManager::Clear() {
  m_ThreadCount = 0;
  m_Systraces.clear();
}

//-----------------------------------------------------------------------------
void SystraceManager::Dump() const {
  for (auto systrace : m_Systraces) {
    PRINT_VAR(systrace->GetName());
    PRINT_VAR(systrace->GetMinTime());
    PRINT_VAR(systrace->GetMaxTime());
    PRINT_VAR(systrace->GetTimeRange() * 0.001);
  }
}

//-----------------------------------------------------------------------------
void SystraceManager::Add(std::shared_ptr<Systrace> a_Systrace) {
  m_Systraces.push_back(a_Systrace);
}

//-----------------------------------------------------------------------------
const std::string& SystraceManager::GetFunctionName(uint64_t a_ID) const {
  for (auto& systrace : m_Systraces) {
    const std::string& name = systrace->GetFunctionName(a_ID);
    if (!name.empty()) return name;
  }

  static std::string defaultString;
  return defaultString;
}
