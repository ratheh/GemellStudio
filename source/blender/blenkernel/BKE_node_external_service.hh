/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * External Node Service Manager
 *
 * Manages discovery, configuration, and lifecycle of external geometry node services.
 * External services run as separate processes and communicate via REST API and shared memory.
 */

#include <memory>
#include <optional>
#include <string>

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender::io::serialize {
class DictionaryValue;
class Value;
}  // namespace blender::io::serialize

namespace blender::bke {

/* -------------------------------------------------------------------- */
/** \name External Service Data Structures
 * \{ */

/**
 * Represents a service manifest file from the system-level directory.
 * These are written by external service installers.
 */
struct ExternalServiceManifest {
  /** Manifest schema version (e.g., "1.0") */
  std::string manifest_version;

  /** Service identification */
  std::string service_id;      /* Unique identifier (e.g., "gemell.geo.nodes") */
  std::string service_name;    /* Display name */
  std::string service_version; /* Version string */
  std::string description;
  std::string vendor;

  /** Installation information */
  std::string install_root;         /* Root installation directory */
  std::string executable_path;      /* Full path to service executable */
  std::string working_directory;    /* Working directory for process */

  /** Service configuration */
  Vector<std::string> launch_args;  /* Command line arguments */
  std::string api_protocol;         /* "http" */
  std::string api_host;             /* "127.0.0.1" */
  int api_port;                     /* 0 for dynamic allocation */
  std::string api_base_path;        /* "/api/v1/geonodes" */
  int startup_timeout_ms;
  std::string health_check_endpoint;

  /** UI configuration */
  std::string menu_name;  /* Menu category name in Blender */
  std::string menu_icon;  /* Icon name */

  /** IPC configuration */
  std::string shared_memory_format;     /* "apache_arrow" */
  int shared_memory_alignment;          /* 64 */

  /** Lifecycle configuration */
  bool auto_start;
  bool restart_on_failure;
  int max_restart_attempts;

  /** Source file path (for tracking) */
  std::string manifest_file_path;
  int64_t manifest_file_mtime;  /* Modification time (unix timestamp) */

  ExternalServiceManifest() = default;
};

/**
 * Runtime representation of an external service.
 * Contains the service manifest and runtime state.
 */
struct ExternalNodeService {
  /** Service manifest loaded from system directory */
  ExternalServiceManifest manifest;

  /** Service runtime state */
  bool is_running;          /* Currently running */
  void *process_handle;     /* Platform-specific process handle */
  std::string api_base_url; /* Full base URL (e.g., "http://127.0.0.1:5000") */
  int restart_count;        /* Number of times service has been restarted */

  ExternalNodeService() : is_running(false), process_handle(nullptr), restart_count(0) {}
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name External Node Service Manager
 * \{ */

/**
 * Singleton manager for external node services.
 *
 * Responsibilities:
 * - Service discovery and manifest parsing
 * - Process lifecycle management and health monitoring
 * - Node registration and execution
 */
class ExternalNodeServiceManager {
 public:
  /**
   * Get the singleton instance.
   */
  static ExternalNodeServiceManager &get_instance();

  /**
   * Initialize the service manager (called from node_system_init).
   * Discovers and loads service manifests from system directory.
   */
  void discover_and_initialize();

  /**
   * Cleanup the service manager (called from node_system_exit).
   * Shuts down running services and releases resources.
   */
  void cleanup();

  /**
   * Get all discovered services.
   */
  Span<ExternalNodeService *> get_services() const;

  /**
   * Find a service by ID.
   */
  ExternalNodeService *find_service(StringRef service_id) const;

  /**
   * Launch an external service process.
   * (Delegates to external_service_launch in node_external_service_process.cc)
   */
  bool launch_service(ExternalNodeService &service);

  /**
   * Shutdown an external service process.
   * (Delegates to external_service_shutdown in node_external_service_process.cc)
   */
  bool shutdown_service(ExternalNodeService &service);

  /**
   * Shutdown all running services.
   * (Delegates to external_service_shutdown_all in node_external_service_process.cc)
   */
  void shutdown_all_services();

  /**
   * Check health of all running services and restart if needed.
   * (Delegates to external_service_check_health in node_external_service_process.cc)
   */
  void check_service_health();

 private:
  ExternalNodeServiceManager() = default;
  ~ExternalNodeServiceManager() = default;

  /* Prevent copying */
  ExternalNodeServiceManager(const ExternalNodeServiceManager &) = delete;
  ExternalNodeServiceManager &operator=(const ExternalNodeServiceManager &) = delete;

  /* -------------------------------------------------------------------- */
  /** \name Manifest Discovery
   * \{ */

  /**
   * Discover service manifests from system directory.
   * Returns list of parsed manifests.
   */
  Vector<ExternalServiceManifest> discover_manifests();

  /**
   * Get platform-specific manifest directory path.
   * Windows: %PROGRAMDATA%/GemellStudio/ExternalServices/
   * Linux: /usr/share/GemellStudio/external_services/ or /opt/GemellStudio/external_services/
   * macOS: /Library/Application Support/GemellStudio/ExternalServices/
   */
  std::optional<std::string> get_manifest_directory() const;

  /**
   * Scan manifest directory for .json files.
   */
  Vector<std::string> scan_manifest_directory(const std::string &directory) const;

  /**
   * Parse a single manifest file.
   * Returns nullopt if parsing fails.
   */
  std::optional<ExternalServiceManifest> parse_manifest_file(const std::string &file_path);

  /**
   * Validate manifest schema (check required fields).
   */
  bool validate_manifest(const ExternalServiceManifest &manifest) const;

  /**
   * Parse JSON dictionary value into manifest structure.
   */
  bool parse_manifest_from_json(const blender::io::serialize::DictionaryValue &dict,
                                 ExternalServiceManifest &manifest);

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Data Members
   * \{ */

  /** All discovered services (indexed storage) */
  Vector<std::unique_ptr<ExternalNodeService>> services_;

  /** Fast lookup by service ID */
  Map<std::string, ExternalNodeService *> service_map_;

  /** Temporary storage for raw service pointers (for Span return) */
  mutable Vector<ExternalNodeService *> service_ptr_cache_;

  /** \} */
};

/** \} */

}  // namespace blender::bke
