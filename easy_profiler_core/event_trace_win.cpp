/************************************************************************
* file name         : event_trace_win.cpp
* ----------------- :
* creation time     : 2016/09/04
* author            : Victor Zarubkin
* email             : v.s.zarubkin@gmail.com
* ----------------- :
* description       : The file contains implementation of EasyEventTracer class used for tracing
*                   : Windows system events to get context switches.
* ----------------- :
* change log        : * 2016/09/04 Victor Zarubkin: initial commit.
*                   :
*                   : * 2016/09/13 Victor Zarubkin: get process id and process name
*                   :        of the owner of thread with id == CSwitch::NewThreadId.
*                   : 
*                   : * 2016/09/17 Victor Zarubkin: added log messages printing.
* ----------------- :
* license           : Lightweight profiler library for c++
*                   : Copyright(C) 2016  Sergey Yagovtsev, Victor Zarubkin
*                   :
*                   :
*                   : Licensed under the Apache License, Version 2.0 (the "License");
*                   : you may not use this file except in compliance with the License.
*                   : You may obtain a copy of the License at
*                   :
*                   : http://www.apache.org/licenses/LICENSE-2.0
*                   :
*                   : Unless required by applicable law or agreed to in writing, software
*                   : distributed under the License is distributed on an "AS IS" BASIS,
*                   : WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*                   : See the License for the specific language governing permissions and
*                   : limitations under the License.
*                   :
*                   :
*                   : GNU General Public License Usage
*                   : Alternatively, this file may be used under the terms of the GNU
*                   : General Public License as published by the Free Software Foundation,
*                   : either version 3 of the License, or (at your option) any later version.
*                   :
*                   : This program is distributed in the hope that it will be useful,
*                   : but WITHOUT ANY WARRANTY; without even the implied warranty of
*                   : MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*                   : GNU General Public License for more details.
*                   :
*                   : You should have received a copy of the GNU General Public License
*                   : along with this program.If not, see <http://www.gnu.org/licenses/>.
************************************************************************/

#ifdef _WIN32
#include <memory.h>
#include <chrono>
#include <unordered_map>
#include "easy/profiler.h"
#include "profile_manager.h"
#include "current_time.h"

#include "event_trace_win.h"
#include <Psapi.h>

#if EASY_OPTION_LOG_ENABLED != 0
# include <iostream>
# ifndef EASY_ETW_LOG
#  define EASY_ETW_LOG ::std::cerr
# endif
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//extern ProfileManager& MANAGER;
#define MANAGER ProfileManager::instance()

::std::atomic_uint64_t TRACING_END_TIME = ATOMIC_VAR_INIT(~0ULL);

namespace profiler {

    const decltype(EVENT_DESCRIPTOR::Opcode) SWITCH_CONTEXT_OPCODE = 36;
    const int RAW_TIMESTAMP_TIME_TYPE = 1;

    //////////////////////////////////////////////////////////////////////////

    struct ProcessInfo {
        std::string      name;
        processid_t    id = 0;
        int8_t      valid = 0;
    };

    //////////////////////////////////////////////////////////////////////////

    // CSwitch class
    // See https://msdn.microsoft.com/en-us/library/windows/desktop/aa964744(v=vs.85).aspx
    // EventType = 36
    struct CSwitch
    {
        uint32_t                 NewThreadId;
        uint32_t                 OldThreadId;
        int8_t             NewThreadPriority;
        int8_t             OldThreadPriority;
        uint8_t               PreviousCState;
        int8_t                     SpareByte;
        int8_t           OldThreadWaitReason;
        int8_t             OldThreadWaitMode;
        int8_t                OldThreadState;
        int8_t   OldThreadWaitIdealProcessor;
        uint32_t           NewThreadWaitTime;
        uint32_t                    Reserved;
    };

    //////////////////////////////////////////////////////////////////////////

    typedef ::std::unordered_map<decltype(CSwitch::NewThreadId), ProcessInfo*, ::profiler::do_not_calc_hash> thread_process_info_map;
    typedef ::std::unordered_map<processid_t, ProcessInfo, ::profiler::do_not_calc_hash> process_info_map;

    // Using static is safe because processTraceEvent() is called from one thread
    process_info_map PROCESS_INFO_TABLE;
    thread_process_info_map THREAD_PROCESS_INFO_TABLE = ([](){ thread_process_info_map initial; initial[0U] = nullptr; return ::std::move(initial); })();

    //////////////////////////////////////////////////////////////////////////

    void WINAPI processTraceEvent(PEVENT_RECORD _traceEvent)
    {
        if (_traceEvent->EventHeader.EventDescriptor.Opcode != SWITCH_CONTEXT_OPCODE)
            return;

        if (sizeof(CSwitch) != _traceEvent->UserDataLength)
            return;

        EASY_FUNCTION(::profiler::colors::White, ::profiler::OFF);

        auto _contextSwitchEvent = reinterpret_cast<CSwitch*>(_traceEvent->UserData);
        const auto time = static_cast<::profiler::timestamp_t>(_traceEvent->EventHeader.TimeStamp.QuadPart);
        if (time > TRACING_END_TIME.load(::std::memory_order_acquire))
            return;

        processid_t pid = 0;
        const char* process_name = "";

        // Trying to get target process name and id
        auto it = THREAD_PROCESS_INFO_TABLE.find(_contextSwitchEvent->NewThreadId);
        if (it == THREAD_PROCESS_INFO_TABLE.end())
        {
            auto hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, _contextSwitchEvent->NewThreadId);
            if (hThread != nullptr)
            {
                pid = GetProcessIdOfThread(hThread);
                auto pinfo = &PROCESS_INFO_TABLE[pid];

                if (pinfo->valid == 0)
                {
                    if (pinfo->name.empty())
                    {
                        static char numbuf[128] = {};
                        sprintf(numbuf, "%u", pid);
                        pinfo->name = numbuf;
                        pinfo->id = pid;
                    }

                    /*
                    According to documentation, using GetModuleBaseName() requires
                    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ access rights.
                    But it works fine with PROCESS_QUERY_LIMITED_INFORMATION instead of PROCESS_QUERY_INFORMATION.
                    
                    See https://msdn.microsoft.com/en-us/library/windows/desktop/ms683196(v=vs.85).aspx
                    */

                    //auto hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                    //if (hProc == nullptr)
                    auto hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                    if (hProc != nullptr)
                    {
                        static TCHAR buf[MAX_PATH] = {}; // Using static is safe because processTraceEvent() is called from one thread
                        auto len = GetModuleBaseName(hProc, 0, buf, MAX_PATH);

                        if (len != 0)
                        {
                            pinfo->name.reserve(pinfo->name.size() + 2 + len);
                            pinfo->name.append(" ", 1);
                            pinfo->name.append(buf, len);
                            pinfo->valid = 1;
                        }

                        CloseHandle(hProc);
                    }
                    else
                    {
                        //auto err = GetLastError();
                        //printf("OpenProcess(%u) fail: GetLastError() == %u\n", pid, err);
                        pinfo->valid = -1;

                        if (pid == 4) {
                            pinfo->name.reserve(pinfo->name.size() + 8);
                            pinfo->name.append(" System", 7);
                        }
                    }
                }

                process_name = pinfo->name.c_str();
                THREAD_PROCESS_INFO_TABLE[_contextSwitchEvent->NewThreadId] = pinfo;

                CloseHandle(hThread);
            }
            else
            {
                //printf("Can not OpenThread(%u);\n", _contextSwitchEvent->NewThreadId);
                THREAD_PROCESS_INFO_TABLE[_contextSwitchEvent->NewThreadId] = nullptr;
            }
        }
        else
        {
            auto pinfo = it->second;
            if (pinfo != nullptr)
                process_name = pinfo->name.c_str();
            else if (it->first == 0)
                process_name = "System Idle";
            else if (it->first == 4)
                process_name = "System";
        }

        MANAGER.beginContextSwitch(_contextSwitchEvent->OldThreadId, time, _contextSwitchEvent->NewThreadId, process_name);
        MANAGER.endContextSwitch(_contextSwitchEvent->NewThreadId, pid, time);
    }

    //////////////////////////////////////////////////////////////////////////

#ifndef EASY_MAGIC_STATIC_CPP11
    class EasyEventTracerInstance {
        friend EasyEventTracer;
        EasyEventTracer instance;
    } EASY_EVENT_TRACER;
#endif

    EasyEventTracer& EasyEventTracer::instance()
    {
#ifndef EASY_MAGIC_STATIC_CPP11
        return EASY_EVENT_TRACER.instance;
#else
        static EasyEventTracer tracer;
        return tracer;
#endif
    }

    EasyEventTracer::EasyEventTracer()
    {
        m_lowPriority = ATOMIC_VAR_INIT(EASY_OPTION_LOW_PRIORITY_EVENT_TRACING);
    }

    EasyEventTracer::~EasyEventTracer()
    {
        disable();
    }

    bool EasyEventTracer::isLowPriority() const
    {
        return m_lowPriority.load(::std::memory_order_acquire);
    }

    void EasyEventTracer::setLowPriority(bool _value)
    {
        m_lowPriority.store(_value, ::std::memory_order_release);
    }

    bool setPrivilege(HANDLE hToken, LPCSTR _privelegeName)
    {
        bool success = false;

        if (hToken)
        {
            LUID privilegyId;
            if (LookupPrivilegeValue(NULL, _privelegeName, &privilegyId))
            {
                TOKEN_PRIVILEGES tokenPrivilege;
                tokenPrivilege.PrivilegeCount = 1;
                tokenPrivilege.Privileges[0].Luid = privilegyId;
                tokenPrivilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                success = AdjustTokenPrivileges(hToken, FALSE, &tokenPrivilege, sizeof(TOKEN_PRIVILEGES), NULL, NULL) != FALSE;
            }
        }

#if EASY_OPTION_LOG_ENABLED != 0
        if (!success)
            EASY_ETW_LOG << "Warning: EasyProfiler failed to set " << _privelegeName << " privelege for the application.\n";
#endif

        return success;
    }

    void EasyEventTracer::setProcessPrivileges()
    {
        static bool alreadySet = false;
        if (alreadySet)
            return;

        alreadySet = true;

        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        {
#if EASY_OPTION_LOG_ENABLED != 0
            const bool success = setPrivilege(hToken, SE_DEBUG_NAME);
            if (!success)
                EASY_ETW_LOG << "Warning: Some context switch events could not get process name.\n";
#else
            setPrivilege(hToken, SE_DEBUG_NAME);
#endif
            
            CloseHandle(hToken);
        }
#if EASY_OPTION_LOG_ENABLED != 0
        else
        {
            EASY_ETW_LOG << "Warning: EasyProfiler failed to open process to adjust priveleges.\n";
        }
#endif
    }

    ::profiler::EventTracingEnableStatus EasyEventTracer::startTrace(bool _force, int _step)
    {
        auto startTraceResult = StartTrace(&m_sessionHandle, KERNEL_LOGGER_NAME, props());
        switch (startTraceResult)
        {
            case ERROR_SUCCESS:
                return EVENT_TRACING_LAUNCHED_SUCCESSFULLY;

            case ERROR_ALREADY_EXISTS:
            {
                if (_force)
                {
                    // Try to stop another event tracing session to force launch self session.

                    if (_step == 0)
                    {
                        /*
                        According to https://msdn.microsoft.com/en-us/library/windows/desktop/aa363696(v=vs.85).aspx
                        SessionHandle is ignored (and could be NULL) if SessionName is not NULL,
                        and you only need to set the Wnode.BufferSize, Wnode.Guid, LoggerNameOffset, and LogFileNameOffset
                        in EVENT_TRACE_PROPERTIES structure if ControlCode is EVENT_TRACE_CONTROL_STOP.
                        All data is already set for m_properties to the moment. Simply copy m_properties and use the copy.

                        This method supposed to be faster than launching console window and executing shell command,
                        but if that would not work, return to using shell command "logman stop".
                        */

                        // static is safe because we are guarded by spin-lock m_spin
                        static Properties p = ([]{ Properties prp; strncpy(prp.sessionName, KERNEL_LOGGER_NAME, sizeof(prp.sessionName)); return prp; })();
                        p.base = m_properties.base; // Use copy of m_properties to make sure m_properties will not be changed

                        // Stop another session
                        ControlTrace(NULL, KERNEL_LOGGER_NAME, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(&p), EVENT_TRACE_CONTROL_STOP);

                        // Console window variant:
                        //if (32 >= (int)ShellExecute(NULL, NULL, "logman", "stop \"" KERNEL_LOGGER_NAME "\" -ets", NULL, SW_HIDE))
                        //    return EVENT_TRACING_WAS_LAUNCHED_BY_SOMEBODY_ELSE;
                    }

                    if (_step < 4)
                    {
                        // Command executed successfully. Wait for a few time until tracing session finish.
                        ::std::this_thread::sleep_for(::std::chrono::milliseconds(500));
                        return startTrace(true, ++_step);
                    }
                }

#if EASY_OPTION_LOG_ENABLED != 0
                EASY_ETW_LOG << "Error: EasyProfiler.ETW not launched: ERROR_ALREADY_EXISTS. To stop another session execute cmd: logman stop \"" << KERNEL_LOGGER_NAME << "\" -ets\n";
#endif
                return EVENT_TRACING_WAS_LAUNCHED_BY_SOMEBODY_ELSE;
            }

            case ERROR_ACCESS_DENIED:
#if EASY_OPTION_LOG_ENABLED != 0
                EASY_ETW_LOG << "Error: EasyProfiler.ETW not launched: ERROR_ACCESS_DENIED. Try to launch your application as Administrator.\n";
#endif
                return EVENT_TRACING_NOT_ENOUGH_ACCESS_RIGHTS;

            case ERROR_BAD_LENGTH:
#if EASY_OPTION_LOG_ENABLED != 0
                EASY_ETW_LOG << "Error: EasyProfiler.ETW not launched: ERROR_BAD_LENGTH. It seems that your KERNEL_LOGGER_NAME differs from \"" << m_properties.sessionName << "\". Try to re-compile easy_profiler or contact EasyProfiler developers.\n";
#endif
                return EVENT_TRACING_BAD_PROPERTIES_SIZE;
        }

#if EASY_OPTION_LOG_ENABLED != 0
        EASY_ETW_LOG << "Error: EasyProfiler.ETW not launched: StartTrace() returned " << startTraceResult << ::std::endl;
#endif
        return EVENT_TRACING_MISTERIOUS_ERROR;
    }

    ::profiler::EventTracingEnableStatus EasyEventTracer::enable(bool _force)
    {
        ::profiler::guard_lock<::profiler::spin_lock> lock(m_spin);
        if (m_bEnabled)
            return EVENT_TRACING_LAUNCHED_SUCCESSFULLY;

        /*
        Trying to set debug privilege for current process
        to be able to get other process information (process name).
        */
        EasyEventTracer::setProcessPrivileges();

        // Clear properties
        memset(&m_properties, 0, sizeof(m_properties));
        m_properties.base.Wnode.BufferSize = sizeof(m_properties);
        m_properties.base.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        m_properties.base.Wnode.ClientContext = RAW_TIMESTAMP_TIME_TYPE;
        m_properties.base.Wnode.Guid = SystemTraceControlGuid;
        m_properties.base.LoggerNameOffset = sizeof(m_properties.base);
        m_properties.base.EnableFlags = EVENT_TRACE_FLAG_CSWITCH;
        m_properties.base.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;

        // Start event tracing
        auto res = startTrace(_force);
        if (res != EVENT_TRACING_LAUNCHED_SUCCESSFULLY)
            return res;

        memset(&m_trace, 0, sizeof(m_trace));
        m_trace.LoggerName = KERNEL_LOGGER_NAME;
        m_trace.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        m_trace.EventRecordCallback = ::profiler::processTraceEvent;

        m_openedHandle = OpenTrace(&m_trace);
        if (m_openedHandle == INVALID_PROCESSTRACE_HANDLE)
        {
#if EASY_OPTION_LOG_ENABLED != 0
            EASY_ETW_LOG << "Error: EasyProfiler.ETW not launched: OpenTrace() returned invalid handle.\n";
#endif
            return EVENT_TRACING_OPEN_TRACE_ERROR;
        }

        /*
        Have to launch a thread to process events because according to MSDN documentation:
        
        The ProcessTrace function blocks the thread until it delivers all events, the BufferCallback function returns FALSE,
        or you call CloseTrace. If the consumer is consuming events in real time, the ProcessTrace function returns after
        the controller stops the trace session. (Note that there may be a several-second delay before the function returns.)
        
        https://msdn.microsoft.com/en-us/library/windows/desktop/aa364093(v=vs.85).aspx
        */
        m_processThread = ::std::move(::std::thread([this]()
        {
            EASY_THREAD_SCOPE("EasyProfiler.ETW");
            ProcessTrace(&m_openedHandle, 1, 0, 0);
        }));

        // Set low priority for event tracing thread
        if (m_lowPriority.load(::std::memory_order_acquire))
            SetThreadPriority(m_processThread.native_handle(), THREAD_PRIORITY_LOWEST);

        m_bEnabled = true;

        return EVENT_TRACING_LAUNCHED_SUCCESSFULLY;
    }

    void EasyEventTracer::disable()
    {
        ::profiler::guard_lock<::profiler::spin_lock> lock(m_spin);
        if (!m_bEnabled)
            return;

        TRACING_END_TIME.store(getCurrentTime(), ::std::memory_order_release);

        ControlTrace(m_openedHandle, KERNEL_LOGGER_NAME, props(), EVENT_TRACE_CONTROL_STOP);
        CloseTrace(m_openedHandle);

        // Wait for ProcessTrace to finish to make sure no processTraceEvent() will be called later.
        if (m_processThread.joinable())
            m_processThread.join();

        m_bEnabled = false;

        // processTraceEvent() is not called anymore. Clean static maps is safe.
        PROCESS_INFO_TABLE.clear();
        THREAD_PROCESS_INFO_TABLE.clear();
        THREAD_PROCESS_INFO_TABLE[0U] = nullptr;

        TRACING_END_TIME.store(~0ULL, ::std::memory_order_release);
    }

} // END of namespace profiler.

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif // _WIN32
