/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Windows Process Management Implementation
 */

#ifdef WIN32

#  include "BLI_gem_process.hh"

#  include <sstream>

#  include "BLI_winstuff.h"

namespace blender::process {

/* -------------------------------------------------------------------- */
/** \name Process Handle Implementation
 * \{ */

struct ProcessHandle {
  HANDLE process_handle;
  DWORD process_id;

  ProcessHandle() : process_handle(nullptr), process_id(0) {}

  ~ProcessHandle()
  {
    if (process_handle && process_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(process_handle);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper Functions
 * \{ */

/**
 * Build command line string from executable path and arguments.
 * Properly quotes arguments containing spaces.
 */
static std::string build_command_line(const std::string &executable_path,
                                      const Vector<std::string> &args)
{
  std::ostringstream cmd;

  /* Quote executable path if it contains spaces */
  if (executable_path.find(' ') != std::string::npos) {
    cmd << "\"" << executable_path << "\"";
  }
  else {
    cmd << executable_path;
  }

  /* Add arguments */
  for (const std::string &arg : args) {
    cmd << " ";
    /* Quote argument if it contains spaces */
    if (arg.find(' ') != std::string::npos) {
      cmd << "\"" << arg << "\"";
    }
    else {
      cmd << arg;
    }
  }

  return cmd.str();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

ProcessHandle *launch_process(const LaunchOptions &options)
{
  if (options.executable_path.empty()) {
    return nullptr;
  }

  /* Build command line */
  std::string command_line = build_command_line(options.executable_path, options.args);

  /* Convert to wide string for Windows API */
  int wide_len = MultiByteToWideChar(
      CP_UTF8, 0, command_line.c_str(), -1, nullptr, 0);
  if (wide_len == 0) {
    return nullptr;
  }

  wchar_t *wide_cmd = new wchar_t[wide_len];
  MultiByteToWideChar(CP_UTF8, 0, command_line.c_str(), -1, wide_cmd, wide_len);

  /* Convert working directory to wide string if provided */
  wchar_t *wide_workdir = nullptr;
  if (!options.working_directory.empty()) {
    int workdir_len = MultiByteToWideChar(
        CP_UTF8, 0, options.working_directory.c_str(), -1, nullptr, 0);
    if (workdir_len > 0) {
      wide_workdir = new wchar_t[workdir_len];
      MultiByteToWideChar(
          CP_UTF8, 0, options.working_directory.c_str(), -1, wide_workdir, workdir_len);
    }
  }

  /* Setup process startup info */
  STARTUPINFOW startup_info;
  ZeroMemory(&startup_info, sizeof(startup_info));
  startup_info.cb = sizeof(startup_info);

  /* Setup process information */
  PROCESS_INFORMATION process_info;
  ZeroMemory(&process_info, sizeof(process_info));

  /* Create the process */
  BOOL success = CreateProcessW(
      nullptr,         /* lpApplicationName */
      wide_cmd,        /* lpCommandLine */
      nullptr,         /* lpProcessAttributes */
      nullptr,         /* lpThreadAttributes */
      FALSE,           /* bInheritHandles */
      0,               /* dwCreationFlags */
      nullptr,         /* lpEnvironment */
      wide_workdir,    /* lpCurrentDirectory */
      &startup_info,   /* lpStartupInfo */
      &process_info);  /* lpProcessInformation */

  /* Cleanup wide strings */
  delete[] wide_cmd;
  if (wide_workdir) {
    delete[] wide_workdir;
  }

  if (!success) {
    return nullptr;
  }

  /* Close thread handle - we don't need it */
  CloseHandle(process_info.hThread);

  /* Create handle wrapper */
  ProcessHandle *handle = new ProcessHandle();
  handle->process_handle = process_info.hProcess;
  handle->process_id = process_info.dwProcessId;

  return handle;
}

bool is_process_running(ProcessHandle *handle)
{
  if (!handle || !handle->process_handle || handle->process_handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD exit_code;
  if (!GetExitCodeProcess(handle->process_handle, &exit_code)) {
    return false;
  }

  /* STILL_ACTIVE (259) means the process is still running */
  return (exit_code == STILL_ACTIVE);
}

bool terminate_process(ProcessHandle *handle, bool force)
{
  if (!handle || !handle->process_handle || handle->process_handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  if (force) {
    /* Force termination */
    return TerminateProcess(handle->process_handle, 1) != 0;
  }
  else {
    /* Graceful termination not easily possible on Windows without window handle */
    /* For console applications, we'll use TerminateProcess as well */
    /* The external service should handle the /shutdown REST endpoint for graceful shutdown */
    return TerminateProcess(handle->process_handle, 0) != 0;
  }
}

int get_process_id(ProcessHandle *handle)
{
  if (!handle) {
    return 0;
  }
  return static_cast<int>(handle->process_id);
}

void free_process_handle(ProcessHandle *handle)
{
  if (handle) {
    delete handle;  /* Destructor will close the HANDLE */
  }
}

/** \} */

}  // namespace blender::process

#endif /* WIN32 */
