/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * External Node Service Process Management Implementation
 *
 * All Phase 2 process lifecycle code is contained in this file to minimize
 * changes to core Blender files and reduce forward integration conflicts.
 */

#include "BKE_gem_external_service_process.hh"
#include "BKE_node_external_service.hh"

#include <chrono>
#include <sstream>
#include <thread>

#include "CLG_log.h"

#include "BKE_appdir.hh"

#include "BLI_fileops.h"
#include "BLI_gem_http_client.hh"
#include "BLI_gem_process.hh"
#include "BLI_serialize.hh"
#include "BLI_string.h"

static CLG_LogRef LOG = {"bke.node_external_service"};

namespace blender::bke {

/* -------------------------------------------------------------------- */
/** \name Process Lifecycle Implementation
 * \{ */

bool external_service_launch(ExternalNodeService &service)
{
  CLOG_INFO(&LOG,
            "Launching service '%s' (ID: %s)",
            service.manifest.service_name.c_str(),
            service.manifest.service_id.c_str());

  /* Validate executable exists */
  if (!BLI_exists(service.manifest.executable_path.c_str())) {
    CLOG_ERROR(&LOG,
               "Executable not found: %s",
               service.manifest.executable_path.c_str());
    return false;
  }

  /* Substitute variables in launch args */
  Vector<std::string> args = external_service_substitute_launch_args(service.manifest);

  /* Log launch command for debugging */
  CLOG_INFO(&LOG, "Executable: %s", service.manifest.executable_path.c_str());
  CLOG_INFO(&LOG, "Working directory: %s", service.manifest.working_directory.c_str());
  std::string args_str;
  for (const std::string &arg : args) {
    if (!args_str.empty()) {
      args_str += " ";
    }
    args_str += "\"" + arg + "\"";
  }
  CLOG_INFO(&LOG, "Launch arguments: %s", args_str.c_str());

  /* Build process launch options */
  process::LaunchOptions options;
  options.executable_path = service.manifest.executable_path;
  options.args = std::move(args);
  options.working_directory = service.manifest.working_directory;

  /* Launch process */
  process::ProcessHandle *handle = process::launch_process(options);
  if (!handle) {
    CLOG_ERROR(&LOG, "Failed to launch service process");
    return false;
  }

  /* Store process handle */
  service.process_handle = handle;
  int pid = process::get_process_id(handle);
  CLOG_INFO(&LOG, "Service process launched (PID: %d)", pid);

  /* Build API base URL */
  char base_url[256];
  BLI_snprintf(base_url,
               sizeof(base_url),
               "%s://%s:%d%s",
               service.manifest.api_protocol.c_str(),
               service.manifest.api_host.c_str(),
               service.manifest.api_port,
               service.manifest.api_base_path.c_str());
  service.api_base_url = std::string(base_url);

  CLOG_INFO(&LOG, "Service API base URL: %s", service.api_base_url.c_str());

  /* Wait for health check */
  if (!external_service_wait_for_health(service)) {
    CLOG_ERROR(&LOG, "Service health check failed or timed out");
    /* Terminate the process */
    process::terminate_process(handle, true);
    process::free_process_handle(handle);
    service.process_handle = nullptr;
    return false;
  }

  /* Mark as running */
  service.is_running = true;
  CLOG_INFO(&LOG, "Service '%s' is running and healthy", service.manifest.service_name.c_str());

  return true;
}

bool external_service_shutdown(ExternalNodeService &service)
{
  if (!service.is_running || !service.process_handle) {
    return true; /* Already stopped */
  }

  CLOG_INFO(&LOG, "Shutting down service '%s'", service.manifest.service_name.c_str());

  process::ProcessHandle *handle = static_cast<process::ProcessHandle *>(service.process_handle);

  /* Try graceful shutdown via REST API */
  if (!service.api_base_url.empty()) {
    std::string shutdown_url = service.api_base_url + "/shutdown";
    http::HttpResponse response = http::http_post(shutdown_url, "", 2000);

    if (response.success) {
      CLOG_INFO(&LOG, "Sent graceful shutdown request");

      /* Wait 2 seconds for graceful shutdown */
      std::this_thread::sleep_for(std::chrono::seconds(2));

      /* Check if process exited */
      if (!process::is_process_running(handle)) {
        CLOG_INFO(&LOG, "Service shut down gracefully");
        process::free_process_handle(handle);
        service.process_handle = nullptr;
        service.is_running = false;
        return true;
      }
    }
  }

  /* Force termination */
  CLOG_INFO(&LOG, "Force terminating service process");
  process::terminate_process(handle, true);
  process::free_process_handle(handle);
  service.process_handle = nullptr;
  service.is_running = false;

  return true;
}

void external_service_auto_start_all(Vector<std::unique_ptr<ExternalNodeService>> &services)
{
  CLOG_INFO(&LOG, "Auto-starting external services");

  /* Auto-start services if configured */
  for (std::unique_ptr<ExternalNodeService> &service : services) {
    if (service->manifest.auto_start) {
      CLOG_INFO(&LOG,
                "Auto-starting service '%s' (auto_start: true)",
                service->manifest.service_name.c_str());

      if (!external_service_launch(*service)) {
        CLOG_ERROR(&LOG,
                   "Failed to auto-start service '%s'",
                   service->manifest.service_name.c_str());
      }
    }
    else {
      CLOG_INFO(&LOG,
                "Service '%s' will be launched on demand (auto_start: false)",
                service->manifest.service_name.c_str());
    }
  }
}

void external_service_shutdown_all(Vector<std::unique_ptr<ExternalNodeService>> &services)
{
  CLOG_INFO(&LOG, "Shutting down all external services");

  for (std::unique_ptr<ExternalNodeService> &service : services) {
    if (service->is_running) {
      external_service_shutdown(*service);
    }
  }
}

void external_service_check_health(Vector<std::unique_ptr<ExternalNodeService>> &services)
{
  for (std::unique_ptr<ExternalNodeService> &service : services) {
    if (!service->is_running) {
      continue;
    }

    process::ProcessHandle *handle = static_cast<process::ProcessHandle *>(
        service->process_handle);
    if (!handle) {
      continue;
    }

    /* Check if process is still alive */
    if (!process::is_process_running(handle)) {
      CLOG_WARN(&LOG,
                "Service '%s' (PID: %d) has crashed",
                service->manifest.service_name.c_str(),
                process::get_process_id(handle));

      /* Mark as not running */
      service->is_running = false;
      process::free_process_handle(handle);
      service->process_handle = nullptr;

      /* Attempt restart if configured */
      if (service->manifest.restart_on_failure) {
        external_service_restart(*service);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper Functions Implementation
 * \{ */

bool external_service_wait_for_health(ExternalNodeService &service)
{
  const int timeout_ms = service.manifest.startup_timeout_ms > 0 ?
                             service.manifest.startup_timeout_ms :
                             10000;
  const std::string health_url = service.api_base_url + service.manifest.health_check_endpoint;

  CLOG_INFO(&LOG, "Waiting for service health check: %s", health_url.c_str());

  int elapsed_ms = 0;
  int delay_ms = 100; /* Start with 100ms delay */

  while (elapsed_ms < timeout_ms) {
    /* Make HTTP GET request to health endpoint */
    http::HttpResponse response = http::http_get(health_url, 1000);

    CLOG_INFO(&LOG,
              "Health check attempt (elapsed: %d ms / %d ms): HTTP %d %s",
              elapsed_ms,
              timeout_ms,
              response.status_code,
              response.success ? "success" : "failed");

    if (response.success && response.status_code == 200) {
      /* Parse JSON to check status */
      std::stringstream ss(response.body);
      io::serialize::JsonFormatter formatter;
      std::unique_ptr<io::serialize::Value> value = formatter.deserialize(ss);
      if (value) {
        const io::serialize::DictionaryValue *dict = value->as_dictionary_value();
        if (dict) {
          if (auto status = dict->lookup_str("status")) {
            if (*status == "healthy") {
              CLOG_INFO(&LOG, "Service health check passed");
              return true;
            }
          }
        }
      }
    }

    /* Wait with exponential backoff */
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    elapsed_ms += delay_ms;

    /* Exponential backoff (double delay each time, max 1600ms) */
    delay_ms = std::min(delay_ms * 2, 1600);
  }

  CLOG_WARN(&LOG, "Service health check timed out after %d ms", timeout_ms);
  return false;
}

Vector<std::string> external_service_substitute_launch_args(
    const ExternalServiceManifest &manifest)
{
  Vector<std::string> substituted_args;

  /* Get substitution values */
  char port_str[16];
  BLI_snprintf(port_str, sizeof(port_str), "%d", manifest.api_port);

  const char *temp_dir = BKE_tempdir_session();
  std::optional<std::string> config_dir_opt = BKE_appdir_folder_id(BLENDER_USER_CONFIG, nullptr);

  for (const std::string &arg : manifest.launch_args) {
    std::string substituted = arg;

    /* Replace {port} */
    size_t pos = substituted.find("{port}");
    if (pos != std::string::npos) {
      substituted.replace(pos, 6, port_str);
    }

    /* Replace {temp} */
    pos = substituted.find("{temp}");
    if (pos != std::string::npos && temp_dir) {
      substituted.replace(pos, 6, temp_dir);
    }

    /* Replace {config_dir} */
    pos = substituted.find("{config_dir}");
    if (pos != std::string::npos && config_dir_opt.has_value()) {
      substituted.replace(pos, 12, config_dir_opt->c_str());
    }

    substituted_args.append(substituted);
  }

  return substituted_args;
}

bool external_service_restart(ExternalNodeService &service)
{
  /* Check if we've exceeded max restart attempts */
  if (service.restart_count >= service.manifest.max_restart_attempts) {
    CLOG_ERROR(&LOG,
               "Service '%s' has exceeded max restart attempts (%d)",
               service.manifest.service_name.c_str(),
               service.manifest.max_restart_attempts);
    return false;
  }

  /* Increment restart count */
  service.restart_count++;

  /* Exponential backoff delay: 1s, 2s, 4s, ... */
  int delay_seconds = 1 << (service.restart_count - 1); /* 2^(count-1) */
  delay_seconds = std::min(delay_seconds, 8);           /* Cap at 8 seconds */

  CLOG_INFO(&LOG,
            "Restarting service '%s' (attempt %d/%d) after %d second delay",
            service.manifest.service_name.c_str(),
            service.restart_count,
            service.manifest.max_restart_attempts,
            delay_seconds);

  std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));

  /* Attempt to launch */
  if (external_service_launch(service)) {
    CLOG_INFO(&LOG,
              "Service '%s' restarted successfully",
              service.manifest.service_name.c_str());
    return true;
  }

  CLOG_ERROR(&LOG, "Failed to restart service '%s'", service.manifest.service_name.c_str());
  return false;
}

/** \} */

}  // namespace blender::bke
