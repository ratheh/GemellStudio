/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * External Node Service Process Management
 *
 * Handles process lifecycle (launch, shutdown, health monitoring, restart)
 * for external geometry node services.
 *
 * This is separate from the main service manager to minimize changes to core files
 * and reduce merge conflicts during forward integration.
 */

#include <memory>
#include <string>

#include "BLI_vector.hh"

namespace blender::bke {

struct ExternalNodeService;
struct ExternalServiceManifest;

/* -------------------------------------------------------------------- */
/** \name Process Lifecycle Functions
 * \{ */

/**
 * Launch an external service process.
 *
 * Steps:
 * 1. Validates executable exists
 * 2. Substitutes variables in launch args ({port}, {temp}, {config_dir})
 * 3. Launches process using platform-specific APIs
 * 4. Waits for health check to succeed
 * 5. Marks service as running
 *
 * \param service: Service to launch
 * \return true if successfully launched and health check passed, false otherwise
 */
bool external_service_launch(ExternalNodeService &service);

/**
 * Shutdown an external service process.
 *
 * Steps:
 * 1. Attempts graceful shutdown via REST API POST /shutdown
 * 2. Waits 2 seconds for graceful exit
 * 3. Force terminates if still running
 * 4. Cleans up process handle
 *
 * \param service: Service to shutdown
 * \return true if successfully shut down, false otherwise
 */
bool external_service_shutdown(ExternalNodeService &service);

/**
 * Auto-start all services with auto_start flag enabled.
 * Called during initialization after service discovery.
 *
 * \param services: Vector of all services to check and auto-start
 */
void external_service_auto_start_all(Vector<std::unique_ptr<ExternalNodeService>> &services);

/**
 * Shutdown all running services.
 * Called during cleanup or Blender exit.
 *
 * \param services: Vector of all services to check and shutdown
 */
void external_service_shutdown_all(Vector<std::unique_ptr<ExternalNodeService>> &services);

/**
 * Check health of all running services and restart crashed ones.
 *
 * For each running service:
 * - Checks if process is still alive
 * - If crashed and restart_on_failure=true, attempts restart
 * - Respects max_restart_attempts limit
 *
 * Should be called periodically from Blender main loop.
 *
 * \param services: Vector of all services to monitor
 */
void external_service_check_health(Vector<std::unique_ptr<ExternalNodeService>> &services);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper Functions
 * \{ */

/**
 * Wait for service health check to succeed.
 *
 * Makes HTTP GET requests to health endpoint with exponential backoff.
 * Delays: 100ms, 200ms, 400ms, 800ms, 1600ms (max)
 *
 * \param service: Service to check
 * \return true if health check passed within timeout, false otherwise
 */
bool external_service_wait_for_health(ExternalNodeService &service);

/**
 * Substitute variables in launch arguments.
 *
 * Replaces:
 * - {port} → api_port from manifest
 * - {temp} → BKE_tempdir_session()
 * - {config_dir} → BKE_appdir_folder_id(BLENDER_USER_CONFIG)
 *
 * \param manifest: Service manifest containing launch args
 * \return Substituted arguments ready for process launch
 */
Vector<std::string> external_service_substitute_launch_args(
    const ExternalServiceManifest &manifest);

/**
 * Restart a crashed service (if allowed by configuration).
 *
 * Checks:
 * - restart_count < max_restart_attempts
 * - Applies exponential backoff delay (1s, 2s, 4s, 8s max)
 * - Increments restart_count
 * - Calls external_service_launch()
 *
 * \param service: Service to restart
 * \return true if restart succeeded, false otherwise
 */
bool external_service_restart(ExternalNodeService &service);

/** \} */

}  // namespace blender::bke
