/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Unix Process Management Implementation (Linux, macOS)
 */

#ifndef WIN32

#  include "BLI_gem_process.hh"

#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <cstring>
#  include <vector>

namespace blender::process {

/* -------------------------------------------------------------------- */
/** \name Process Handle Implementation
 * \{ */

struct ProcessHandle {
  pid_t process_id;

  ProcessHandle() : process_id(0) {}
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

ProcessHandle *launch_process(const LaunchOptions &options)
{
  if (options.executable_path.empty()) {
    return nullptr;
  }

  /* Fork the current process */
  pid_t pid = fork();

  if (pid < 0) {
    /* Fork failed */
    return nullptr;
  }

  if (pid == 0) {
    /* Child process */

    /* Change working directory if specified */
    if (!options.working_directory.empty()) {
      if (chdir(options.working_directory.c_str()) != 0) {
        /* Failed to change directory - exit child process */
        _exit(1);
      }
    }

    /* Build argument array for execv */
    /* Format: [executable_path, arg1, arg2, ..., nullptr] */
    std::vector<char *> argv;

    /* Add executable path as argv[0] */
    argv.push_back(const_cast<char *>(options.executable_path.c_str()));

    /* Add arguments */
    for (const std::string &arg : options.args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }

    /* Null terminator */
    argv.push_back(nullptr);

    /* Execute the new program */
    execv(options.executable_path.c_str(), argv.data());

    /* If execv returns, it failed */
    _exit(1);
  }

  /* Parent process */
  ProcessHandle *handle = new ProcessHandle();
  handle->process_id = pid;
  return handle;
}

bool is_process_running(ProcessHandle *handle)
{
  if (!handle || handle->process_id <= 0) {
    return false;
  }

  /* Send signal 0 to check if process exists */
  /* This doesn't actually send a signal, just checks if we can */
  if (kill(handle->process_id, 0) == 0) {
    return true;
  }

  /* Process doesn't exist or we don't have permission */
  if (errno == ESRCH) {
    /* Process not found */
    return false;
  }

  /* Other error (e.g., permission denied) - assume running */
  return true;
}

bool terminate_process(ProcessHandle *handle, bool force)
{
  if (!handle || handle->process_id <= 0) {
    return false;
  }

  int signal_num = force ? SIGKILL : SIGTERM;

  /* Send termination signal */
  if (kill(handle->process_id, signal_num) != 0) {
    return false;
  }

  return true;
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
    delete handle;
  }
}

/** \} */

}  // namespace blender::process

#endif /* !WIN32 */
