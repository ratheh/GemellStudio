/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Platform-independent Process Management
 *
 * Provides a cross-platform interface for launching and managing external processes.
 * Platform-specific implementations are in BLI_process_win.cc (Windows) and
 * BLI_process_unix.cc (Linux/macOS).
 */

#include <string>

#include "BLI_vector.hh"

namespace blender::process {

/* -------------------------------------------------------------------- */
/** \name Process Handle
 * \{ */

/**
 * Opaque handle to a running process.
 * Platform-specific implementation details are hidden.
 */
struct ProcessHandle;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Process Launch Options
 * \{ */

/**
 * Configuration for launching a process.
 */
struct LaunchOptions {
  /** Full path to the executable to launch */
  std::string executable_path;

  /** Command-line arguments (not including the executable itself) */
  Vector<std::string> args;

  /** Working directory for the process (empty = inherit from parent) */
  std::string working_directory;

  LaunchOptions() = default;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Process Management Functions
 * \{ */

/**
 * Launch a new process with the specified options.
 *
 * \param options: Configuration for the process to launch
 * \return Process handle on success, nullptr on failure
 *
 * \note The returned handle must be freed with free_process_handle() when done.
 */
ProcessHandle *launch_process(const LaunchOptions &options);

/**
 * Check if a process is still running.
 *
 * \param handle: Process handle returned from launch_process()
 * \return true if the process is still running, false otherwise
 */
bool is_process_running(ProcessHandle *handle);

/**
 * Terminate a process.
 *
 * \param handle: Process handle returned from launch_process()
 * \param force: If true, forcefully kill the process. If false, request graceful termination.
 * \return true if termination was successful, false otherwise
 *
 * \note On Windows: force=false sends WM_CLOSE, force=true uses TerminateProcess()
 * \note On Unix: force=false sends SIGTERM, force=true sends SIGKILL
 */
bool terminate_process(ProcessHandle *handle, bool force);

/**
 * Get the process ID (PID) of a running process.
 *
 * \param handle: Process handle returned from launch_process()
 * \return Process ID, or 0 if invalid
 */
int get_process_id(ProcessHandle *handle);

/**
 * Free a process handle and release associated resources.
 *
 * \param handle: Process handle returned from launch_process()
 *
 * \note This does NOT terminate the process - call terminate_process() first if needed.
 * \note After calling this function, the handle is invalid and should not be used.
 */
void free_process_handle(ProcessHandle *handle);

/** \} */

}  // namespace blender::process
