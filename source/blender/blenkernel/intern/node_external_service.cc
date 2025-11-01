/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * External Node Service Manager Implementation
 */

#include "BKE_node_external_service.hh"
#include "BKE_gem_external_service_process.hh"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "CLG_log.h"

#include "BKE_appdir.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_fileops.hh"
#include "BLI_gem_sse_client.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_string.h"
#include "BLI_timer.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"

#include "NOD_geometry.hh"
#include "NOD_geometry_exec.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "DNA_ID.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#ifdef WIN32
#  include "BLI_winstuff.h"
#  include <shlobj.h>
#else
#  include <sys/stat.h>
#endif

/* Forward declaration of external node execution function (implemented in node_geo_external.cc) */
namespace blender::nodes {
void execute_external_geometry_node(GeoNodeExecParams params);
}  // namespace blender::nodes

static CLG_LogRef LOG = {"bke.node_external_service"};

namespace blender::bke {

namespace fs = std::filesystem;

/* -------------------------------------------------------------------- */
/** \name Global Registries
 * \{ */

/**
 * Global registry to store external node definitions.
 * Maps node idname to its definition.
 */
static Map<std::string, ExternalNodeDefinition> g_external_node_registry;

/**
 * Storage for dynamically created RNA structs.
 * These need to be freed during cleanup.
 */
static Vector<StructRNA *> g_external_node_rna_structs;

/**
 * Storage for dynamically allocated node types.
 * These need to be unregistered and freed during cleanup.
 */
static Vector<bke::bNodeType *> g_external_node_types;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Singleton Access
 * \{ */

ExternalNodeServiceManager &ExternalNodeServiceManager::get_instance()
{
  static ExternalNodeServiceManager instance;
  return instance;
}

/* Destructor must be defined in .cc file where SseClient is complete (PIMPL with unique_ptr) */
ExternalNodeServiceManager::~ExternalNodeServiceManager()
{
  /* cleanup() is called before this destructor, so members are already cleaned up */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

void ExternalNodeServiceManager::discover_and_initialize()
{
  CLOG_INFO(&LOG, "Initializing External Node Service Manager...");

  /* Discover manifests from system directory */
  Vector<ExternalServiceManifest> manifests = discover_manifests();

  if (manifests.is_empty()) {
    CLOG_INFO(&LOG, "No external service manifests found");
    return;
  }

  CLOG_INFO(&LOG, "Discovered %d service manifest(s)", int(manifests.size()));

  /* Load manifests directly into services */
  for (ExternalServiceManifest &manifest : manifests) {
    auto service = std::make_unique<ExternalNodeService>();
    service->manifest = std::move(manifest);

    const std::string &service_id = service->manifest.service_id;
    service_map_.add(service_id, service.get());
    services_.append(std::move(service));
  }

  CLOG_INFO(&LOG, "Loaded %d external service(s)", int(services_.size()));

  /* Auto-start services if configured (delegates to process helper) */
  external_service_auto_start_all(services_);

  /* Register timer for SSE event checking (runs every 1 second) */
  sse_check_timer_uuid_ = reinterpret_cast<uintptr_t>(this);
  BLI_timer_register(
      sse_check_timer_uuid_,
      [](uintptr_t /*uuid*/, void *user_data) -> double {
        ExternalNodeServiceManager *manager = static_cast<ExternalNodeServiceManager *>(user_data);
        manager->check_and_invalidate_nodes();
        return 1.0;  /* Call again in 1 second */
      },
      this,
      nullptr,  /* No free function needed (singleton) */
      1.0,      /* First check after 1 second */
      true);    /* Persistent */

  CLOG_INFO(&LOG, "SSE event check timer registered");
}

void ExternalNodeServiceManager::cleanup()
{
  /* Unregister SSE check timer */
  if (sse_check_timer_uuid_ != 0) {
    BLI_timer_unregister(sse_check_timer_uuid_);
    sse_check_timer_uuid_ = 0;
    CLOG_INFO(&LOG, "SSE event check timer unregistered");
  }

  /* Clear SSE connections (destructors will handle disconnect automatically)
   * IMPORTANT: Do NOT manually call disconnect() here - it will be called by
   * ~SseClient() destructors when unique_ptrs are destroyed. Double-disconnect
   * causes crash at INVALID_HANDLE_VALUE (0xFFFFFFFFFFFFFFFF). */
  {
    std::lock_guard<std::mutex> lock(sse_connections_mutex_);
    sse_connections_.clear();
    CLOG_INFO(&LOG, "All SSE connections closed");
  }

  /* Shutdown all running services (delegates to process helper) */
  external_service_shutdown_all(services_);

  /* Unregister and free dynamically allocated node types */
  CLOG_INFO(&LOG, "Unregistering %d external node type(s)...", int(g_external_node_types.size()));
  int index = 0;
  for (bke::bNodeType *ntype : g_external_node_types) {
    if (ntype) {
      CLOG_INFO(&LOG, "  [%d] Unregistering node type: idname='%s', ui_name='%s'",
                index, ntype->idname.c_str(), ntype->ui_name.c_str());

      /* Unregister the node type from Blender's node system */
      CLOG_INFO(&LOG, "  [%d] Calling node_unregister_type...", index);
      node_unregister_type(*ntype);
      CLOG_INFO(&LOG, "  [%d] node_unregister_type completed", index);

      /* Free the allocated node type */
      CLOG_INFO(&LOG, "  [%d] Freeing node type memory...", index);
      MEM_delete(ntype);
      CLOG_INFO(&LOG, "  [%d] Node type freed", index);
    }
    index++;
  }
  g_external_node_types.clear();
  CLOG_INFO(&LOG, "All node types unregistered successfully");

  /* Note: We don't manually free RNA structs - Blender's RNA system manages their lifecycle.
   * Attempting to free them here causes "freed while holding a Python reference" warnings. */

  /* Clear node registries */
  g_external_node_registry.clear();
  g_external_node_rna_structs.clear();

  /* Release resources */
  CLOG_INFO(&LOG, "Clearing services vector...");
  services_.clear();
  CLOG_INFO(&LOG, "Clearing service map...");
  service_map_.clear();
  CLOG_INFO(&LOG, "Clearing service pointer cache...");
  service_ptr_cache_.clear();

  CLOG_INFO(&LOG, "External Node Service Manager cleanup completed successfully");
}

Span<ExternalNodeService *> ExternalNodeServiceManager::get_services() const
{
  /* Rebuild pointer cache if needed */
  service_ptr_cache_.clear();
  for (const std::unique_ptr<ExternalNodeService> &service : services_) {
    service_ptr_cache_.append(service.get());
  }
  return service_ptr_cache_;
}

ExternalNodeService *ExternalNodeServiceManager::find_service(StringRef service_id) const
{
  const ExternalNodeService *const *service_ptr = service_map_.lookup_ptr(service_id);
  if (service_ptr) {
    return const_cast<ExternalNodeService *>(*service_ptr);
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Manifest Discovery
 * \{ */

Vector<ExternalServiceManifest> ExternalNodeServiceManager::discover_manifests()
{
  Vector<ExternalServiceManifest> manifests;

  /* Get manifest directory path */
  std::optional<std::string> manifest_dir = get_manifest_directory();
  if (!manifest_dir.has_value()) {
    CLOG_INFO(&LOG, "Manifest directory not configured or not found");
    return manifests;
  }

  /* Check if directory exists */
  if (!BLI_is_dir(manifest_dir->c_str())) {
    CLOG_INFO(&LOG, "Manifest directory does not exist: %s", manifest_dir->c_str());
    return manifests;
  }

  CLOG_INFO(&LOG, "Scanning manifest directory: %s", manifest_dir->c_str());

  /* Scan for .json files */
  Vector<std::string> manifest_files = scan_manifest_directory(*manifest_dir);

  CLOG_INFO(&LOG, "Found %d manifest file(s)", int(manifest_files.size()));

  /* Parse each manifest */
  for (const std::string &file_path : manifest_files) {
    std::optional<ExternalServiceManifest> manifest = parse_manifest_file(file_path);
    if (manifest.has_value()) {
      if (validate_manifest(*manifest)) {
        manifests.append(std::move(*manifest));
        CLOG_INFO(&LOG,
                  "Successfully parsed manifest: %s (service: %s)",
                  file_path.c_str(),
                  manifest->service_name.c_str());
      }
      else {
        CLOG_WARN(&LOG, "Invalid manifest schema: %s", file_path.c_str());
      }
    }
    else {
      CLOG_WARN(&LOG, "Failed to parse manifest: %s", file_path.c_str());
    }
  }

  return manifests;
}

std::optional<std::string> ExternalNodeServiceManager::get_manifest_directory() const
{
#ifdef WIN32
  /* Windows: %PROGRAMDATA%/GemellStudio/ExternalServices/ */
  char programdata[FILE_MAX];
  if (SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programdata) == S_OK) {
    char manifest_dir[FILE_MAX];
    BLI_path_join(manifest_dir, sizeof(manifest_dir), programdata, "GemellStudio", "ExternalServices");
    return std::string(manifest_dir);
  }
#elif defined(__APPLE__)
  /* macOS: /Library/Application Support/GemellStudio/ExternalServices/ */
  return std::string("/Library/Application Support/GemellStudio/ExternalServices");
#else
  /* Linux: Try /usr/share first, fallback to /opt */
  const char *try_paths[] = {
      "/usr/share/GemellStudio/external_services",
      "/opt/GemellStudio/external_services",
  };

  for (const char *path : try_paths) {
    if (BLI_is_dir(path)) {
      return std::string(path);
    }
  }

  /* If neither exists, return the preferred path anyway */
  return std::string(try_paths[0]);
#endif

  return std::nullopt;
}

Vector<std::string> ExternalNodeServiceManager::scan_manifest_directory(
    const std::string &directory) const
{
  Vector<std::string> manifest_files;

  try {
    for (const auto &entry : fs::directory_iterator(directory)) {
      if (entry.is_regular_file() && entry.path().extension() == ".json") {
        manifest_files.append(entry.path().string());
      }
    }
  }
  catch (const fs::filesystem_error &e) {
    CLOG_WARN(&LOG, "Error scanning manifest directory: %s", e.what());
  }

  return manifest_files;
}

std::optional<ExternalServiceManifest> ExternalNodeServiceManager::parse_manifest_file(
    const std::string &file_path)
{
  /* Read JSON file */
  std::shared_ptr<io::serialize::Value> value = io::serialize::read_json_file(file_path);
  if (!value) {
    CLOG_ERROR(&LOG, "Failed to read JSON from: %s", file_path.c_str());
    return std::nullopt;
  }

  const io::serialize::DictionaryValue *dict = value->as_dictionary_value();
  if (!dict) {
    CLOG_ERROR(&LOG, "Manifest is not a JSON object: %s", file_path.c_str());
    return std::nullopt;
  }

  ExternalServiceManifest manifest;
  if (!parse_manifest_from_json(*dict, manifest)) {
    return std::nullopt;
  }

  /* Store source file info */
  manifest.manifest_file_path = file_path;

  /* Get file modification time */
  try {
    auto ftime = fs::last_write_time(file_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    manifest.manifest_file_mtime = std::chrono::system_clock::to_time_t(sctp);
  }
  catch (...) {
    manifest.manifest_file_mtime = 0;
  }

  return manifest;
}

bool ExternalNodeServiceManager::parse_manifest_from_json(
    const io::serialize::DictionaryValue &dict, ExternalServiceManifest &manifest)
{
  /* Parse manifest_version */
  if (auto version = dict.lookup_str("manifest_version")) {
    manifest.manifest_version = std::string(*version);
  }

  /* Parse service section */
  const io::serialize::DictionaryValue *service_dict = dict.lookup_dict("service");
  if (!service_dict) {
    CLOG_ERROR(&LOG, "Manifest missing 'service' section");
    return false;
  }

  if (auto id = service_dict->lookup_str("id")) {
    manifest.service_id = std::string(*id);
  }
  if (auto name = service_dict->lookup_str("name")) {
    manifest.service_name = std::string(*name);
  }
  if (auto version = service_dict->lookup_str("version")) {
    manifest.service_version = std::string(*version);
  }
  if (auto desc = service_dict->lookup_str("description")) {
    manifest.description = std::string(*desc);
  }
  if (auto vendor = service_dict->lookup_str("vendor")) {
    manifest.vendor = std::string(*vendor);
  }

  /* Parse installation section */
  const io::serialize::DictionaryValue *install_dict = dict.lookup_dict("installation");
  if (install_dict) {
    if (auto install_root = install_dict->lookup_str("install_root")) {
      manifest.install_root = std::string(*install_root);
    }
    if (auto exe_path = install_dict->lookup_str("executable_path")) {
      manifest.executable_path = std::string(*exe_path);
    }
    if (auto work_dir = install_dict->lookup_str("working_directory")) {
      manifest.working_directory = std::string(*work_dir);
    }
  }

  /* Parse service_config section */
  const io::serialize::DictionaryValue *config_dict = dict.lookup_dict("service_config");
  if (!config_dict) {
    CLOG_ERROR(&LOG, "Manifest missing 'service_config' section");
    return false;
  }

  /* Parse executable args */
  const io::serialize::DictionaryValue *exec_dict = config_dict->lookup_dict("executable");
  if (exec_dict) {
    const io::serialize::ArrayValue *args_array = exec_dict->lookup_array("args");
    if (args_array) {
      for (const auto &arg_value : args_array->elements()) {
        if (const io::serialize::StringValue *arg_str = arg_value->as_string_value()) {
          manifest.launch_args.append(arg_str->value());
        }
      }
    }
  }

  /* Parse API config */
  const io::serialize::DictionaryValue *api_dict = config_dict->lookup_dict("api");
  if (api_dict) {
    if (auto protocol = api_dict->lookup_str("protocol")) {
      manifest.api_protocol = std::string(*protocol);
    }
    if (auto host = api_dict->lookup_str("host")) {
      manifest.api_host = std::string(*host);
    }
    /* Parse port_file_path for service discovery */
    if (auto port_file = api_dict->lookup_str("port_file_path")) {
      manifest.port_file_path = std::string(*port_file);
    }
    /* Parse preferred_port (with fallback to "port" for backward compatibility) */
    if (auto preferred = api_dict->lookup_int("preferred_port")) {
      manifest.preferred_port = int(*preferred);
      manifest.api_port = int(*preferred);  /* Initialize api_port with preferred */
    } else if (auto port = api_dict->lookup_int("port")) {
      manifest.preferred_port = int(*port);
      manifest.api_port = int(*port);
    }
    /* Parse port range for dynamic port selection */
    if (auto range_start = api_dict->lookup_int("port_range_start")) {
      manifest.port_range_start = int(*range_start);
    }
    if (auto range_end = api_dict->lookup_int("port_range_end")) {
      manifest.port_range_end = int(*range_end);
    }
    if (auto base_path = api_dict->lookup_str("base_path")) {
      manifest.api_base_path = std::string(*base_path);
    }
    if (auto timeout = api_dict->lookup_int("startup_timeout_ms")) {
      manifest.startup_timeout_ms = int(*timeout);
    }
    if (auto health = api_dict->lookup_str("health_check_endpoint")) {
      manifest.health_check_endpoint = std::string(*health);
    }
  }

  /* Parse node_collection */
  const io::serialize::DictionaryValue *node_coll_dict = config_dict->lookup_dict(
      "node_collection");
  if (node_coll_dict) {
    if (auto menu_name = node_coll_dict->lookup_str("menu_name")) {
      manifest.menu_name = std::string(*menu_name);
    }
    if (auto icon = node_coll_dict->lookup_str("icon")) {
      manifest.menu_icon = std::string(*icon);
    }
  }

  /* Parse shared_memory */
  const io::serialize::DictionaryValue *shmem_dict = config_dict->lookup_dict("shared_memory");
  if (shmem_dict) {
    if (auto format = shmem_dict->lookup_str("format")) {
      manifest.shared_memory_format = std::string(*format);
    }
    if (auto alignment = shmem_dict->lookup_int("alignment")) {
      manifest.shared_memory_alignment = int(*alignment);
    }
  }

  /* Parse lifecycle flags */
  const io::serialize::DictionaryValue *root_config = config_dict;

  /* Default values */
  manifest.auto_start = false;
  manifest.restart_on_failure = false;
  manifest.max_restart_attempts = 3;

  /* Helper lambda to read boolean from either int (0/1) or boolean (true/false) */
  auto lookup_bool = [](const io::serialize::DictionaryValue *dict,
                        const char *key) -> std::optional<bool> {
    /* Try to lookup as integer first (for backward compatibility with 0/1) */
    if (auto int_val = dict->lookup_int(key)) {
      return *int_val != 0;
    }
    /* Try to lookup as boolean (for true/false) */
    if (const std::shared_ptr<io::serialize::Value> *value = dict->lookup(key)) {
      if (const io::serialize::BooleanValue *bool_val = (*value)->as_boolean_value()) {
        return bool_val->value();
      }
    }
    return std::nullopt;
  };

  if (auto auto_start = lookup_bool(root_config, "auto_start")) {
    manifest.auto_start = *auto_start;
  }
  if (auto restart = lookup_bool(root_config, "restart_on_failure")) {
    manifest.restart_on_failure = *restart;
  }
  if (auto max_attempts = root_config->lookup_int("max_restart_attempts")) {
    manifest.max_restart_attempts = int(*max_attempts);
  }

  return true;
}

bool ExternalNodeServiceManager::validate_manifest(const ExternalServiceManifest &manifest) const
{
  /* Required fields */
  if (manifest.service_id.empty()) {
    CLOG_WARN(&LOG, "Manifest missing required field: service.id");
    return false;
  }
  if (manifest.service_name.empty()) {
    CLOG_WARN(&LOG, "Manifest missing required field: service.name");
    return false;
  }
  if (manifest.executable_path.empty()) {
    CLOG_WARN(&LOG, "Manifest missing required field: installation.executable_path");
    return false;
  }

  /* Validate executable exists */
  if (!BLI_exists(manifest.executable_path.c_str())) {
    CLOG_WARN(&LOG,
              "Service '%s' executable not found: %s",
              manifest.service_name.c_str(),
              manifest.executable_path.c_str());
    /* Not a hard error - service might be temporarily unavailable */
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Service Lifecycle Management (Delegates to node_external_service_process.cc)
 * \{ */

bool ExternalNodeServiceManager::launch_service(ExternalNodeService &service)
{
  return external_service_discover_or_launch(service);
}

bool ExternalNodeServiceManager::shutdown_service(ExternalNodeService &service)
{
  return external_service_shutdown(service);
}

void ExternalNodeServiceManager::shutdown_all_services()
{
  external_service_shutdown_all(services_);
}

void ExternalNodeServiceManager::check_service_health()
{
  external_service_check_health(services_);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Registration
 * \{ */

/* -------------------------------------------------------------------- */
/** \name External Node Storage Management
 * \{ */

/**
 * Initialize storage for external node.
 * Called when node is created.
 */
static void external_node_init_storage(bNodeTree * /*ntree*/, bNode *node)
{
  ExternalNodeStorage *storage = MEM_new<ExternalNodeStorage>(__func__);
  node->storage = storage;
}

/**
 * Free storage for external node.
 * Called when node is deleted.
 */
static void external_node_free_storage(bNode *node)
{
  if (node->storage) {
    MEM_delete(static_cast<ExternalNodeStorage *>(node->storage));
    node->storage = nullptr;
  }
}

/**
 * Copy storage for external node.
 * Called when node is duplicated or loaded from .blend file.
 *
 * Note: ExternalNodeStorage is runtime-only (not serialized to DNA).
 * When loading old .blend files, src_node->storage will be null.
 */
static void external_node_copy_storage(bNodeTree * /*tree*/, bNode *dst_node, const bNode *src_node)
{
  const ExternalNodeStorage *src_storage = static_cast<const ExternalNodeStorage *>(src_node->storage);

  /* Handle case where source storage is null (e.g., loading old .blend files).
   * Since ExternalNodeStorage is not serialized to DNA (registered with empty struct name),
   * nodes loaded from .blend files will have null storage until initfunc runs. */
  if (src_storage == nullptr) {
    /* Allocate fresh storage - initfunc will be called later to fully initialize */
    dst_node->storage = MEM_new<ExternalNodeStorage>(__func__);
    return;
  }

  /* Copy existing storage and reset session state for copied node */
  ExternalNodeStorage *dst_storage = MEM_dupallocN<ExternalNodeStorage>(__func__, *src_storage);
  dst_node->storage = dst_storage;

  /* Reset session state for copied node - it should create its own session */
  dst_storage->session_initialized = false;
  dst_storage->session_id[0] = '\0';
  dst_storage->document_session_id[0] = '\0';
  dst_storage->sse_stream_url[0] = '\0';
  dst_storage->last_execution_time = 0.0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name External Node Declaration
 * \{ */

/**
 * Build node declaration (sockets) from external node definition.
 */
static void build_external_node_declaration(const ExternalNodeDefinition &node_def,
                                             nodes::NodeDeclarationBuilder &b)
{
  using namespace blender::nodes::decl;

  /* Add input sockets */
  for (const ExternalNodeSocket &input : node_def.inputs) {
    switch (input.socket_type) {
      case SOCK_GEOMETRY: {
        auto &socket = b.add_input<Geometry>(input.name.c_str());
        if (!input.description.empty()) {
          socket.description(input.description.c_str());
        }
        break;
      }
      case SOCK_STRING: {
        auto &socket = b.add_input<String>(input.name.c_str());
        if (!input.description.empty()) {
          socket.description(input.description.c_str());
        }
        if (!input.default_string_value.empty()) {
          socket.default_value(input.default_string_value);
        }
        /* Set subtype for special string types (e.g., file paths) */
        if (input.subtype != 0) {
          socket.subtype(PropertySubType(input.subtype));
        }
        break;
      }
      case SOCK_INT: {
        auto &socket = b.add_input<Int>(input.name.c_str());
        if (!input.description.empty()) {
          socket.description(input.description.c_str());
        }
        socket.default_value(input.default_int_value);
        if (input.min_value.has_value()) {
          socket.min(int(*input.min_value));
        }
        if (input.max_value.has_value()) {
          socket.max(int(*input.max_value));
        }
        break;
      }
      case SOCK_FLOAT: {
        auto &socket = b.add_input<Float>(input.name.c_str());
        if (!input.description.empty()) {
          socket.description(input.description.c_str());
        }
        socket.default_value(input.default_float_value);
        if (input.min_value.has_value()) {
          socket.min(*input.min_value);
        }
        if (input.max_value.has_value()) {
          socket.max(*input.max_value);
        }
        break;
      }
      case SOCK_BOOLEAN: {
        auto &socket = b.add_input<Bool>(input.name.c_str());
        if (!input.description.empty()) {
          socket.description(input.description.c_str());
        }
        socket.default_value(input.default_bool_value);
        break;
      }
      case SOCK_VECTOR: {
        auto &socket = b.add_input<blender::nodes::decl::Vector>(input.name.c_str());
        if (!input.description.empty()) {
          socket.description(input.description.c_str());
        }
        break;
      }
      case SOCK_RGBA: {
        auto &socket = b.add_input<Color>(input.name.c_str());
        if (!input.description.empty()) {
          socket.description(input.description.c_str());
        }
        break;
      }
      default:
        CLOG_WARN(&LOG, "Unsupported input socket type: %d", input.socket_type);
        break;
    }
  }

  /* Add output sockets */
  for (const ExternalNodeSocket &output : node_def.outputs) {
    switch (output.socket_type) {
      case SOCK_GEOMETRY: {
        auto &socket = b.add_output<Geometry>(output.name.c_str());
        if (!output.description.empty()) {
          socket.description(output.description.c_str());
        }
        break;
      }
      case SOCK_INT: {
        auto &socket = b.add_output<Int>(output.name.c_str());
        if (!output.description.empty()) {
          socket.description(output.description.c_str());
        }
        break;
      }
      case SOCK_FLOAT: {
        auto &socket = b.add_output<Float>(output.name.c_str());
        if (!output.description.empty()) {
          socket.description(output.description.c_str());
        }
        break;
      }
      case SOCK_VECTOR: {
        auto &socket = b.add_output<blender::nodes::decl::Vector>(output.name.c_str());
        if (!output.description.empty()) {
          socket.description(output.description.c_str());
        }
        break;
      }
      default:
        CLOG_WARN(&LOG, "Unsupported output socket type: %d", output.socket_type);
        break;
    }
  }
}

/**
 * Wrapper function for node declaration that looks up the node definition from registry.
 * This is needed because we cannot use lambda captures in the declare function pointer.
 */
static void external_node_declare_wrapper(nodes::NodeDeclarationBuilder &b)
{
  /* Get the node being declared */
  const bNode *node = b.node_or_null();
  if (node == nullptr ||  node->typeinfo == nullptr) {
    return;
  }

  /* Look up the node definition in the registry using the idname */
  const ExternalNodeDefinition *node_def = g_external_node_registry.lookup_ptr(node->typeinfo->idname);
  if (node_def == nullptr) {
    CLOG_ERROR(&LOG,
               "External node definition not found in registry: %s",
               node->typeinfo->idname);
    return;
  }

  /* Build the node declaration from the stored definition */
  build_external_node_declaration(*node_def, b);
}

/**
 * Poll function to check if external nodes can be added to a tree.
 * Only allow external geometry nodes in geometry node trees.
 */
static bool external_node_poll(const bke::bNodeType * /*ntype*/,
                                const bNodeTree *ntree,
                                const char **r_disabled_hint)
{
  if (ntree && ntree->idname != std::string("GeometryNodeTree")) {
    if (r_disabled_hint) {
      *r_disabled_hint = "Not a geometry node tree";
    }
    return false;
  }
  return true;
}

/**
 * Execution wrapper for external nodes.
 * Delegates to the actual implementation in node_geo_external.cc
 */
static void external_node_execute_wrapper(nodes::GeoNodeExecParams params)
{
  nodes::execute_external_geometry_node(params);
}

void ExternalNodeServiceManager::register_all_pending_nodes()
{
  CLOG_INFO(&LOG, "Registering all pending external nodes...");

  for (const std::unique_ptr<ExternalNodeService> &service : services_) {
    if (!service->nodes_registered && !service->nodes.is_empty()) {
      /* register_nodes_from_service will log its own message */
      register_nodes_from_service(*service);
    }
  }
}

void ExternalNodeServiceManager::register_nodes_from_service(ExternalNodeService &service)
{
  if (service.nodes_registered) {
    CLOG_INFO(&LOG, "Nodes already registered for service '%s'", service.manifest.service_name.c_str());
    return;
  }

  CLOG_INFO(&LOG,
            "Registering %d node(s) from service '%s'",
            service.nodes.size(),
            service.manifest.service_name.c_str());

  for (const ExternalNodeDefinition &node_def : service.nodes) {
    /* Store node definition in global registry for lookup in declare function */
    g_external_node_registry.add_overwrite(node_def.node_id, node_def);

    CLOG_INFO(&LOG, "  Registering external node: '%s'", node_def.node_id.c_str());

    /* Allocate node type on heap (will be freed by Blender's node system during shutdown) */
    bke::bNodeType *ntype = MEM_new<bke::bNodeType>(__func__);

    /* Initialize with node_type_base using NODE_CUSTOM to skip pre-compiled RNA lookup */
    /* This sets default values, poll functions, and generates a unique legacy type */
    CLOG_INFO(&LOG, "    Calling node_type_base with NODE_CUSTOM...");
    node_type_base(*ntype, node_def.node_id, NODE_CUSTOM);
    CLOG_INFO(&LOG, "    node_type_base completed, idname='%s'", ntype->idname.c_str());

    /* Create RNA struct for this node type (required by node_register_type) */
    /* We need to allocate the RNA struct dynamically for external nodes */
    /* Each node gets its own RNA struct with a name matching the node ID */
    CLOG_INFO(&LOG, "    Creating RNA struct with name '%s'...", node_def.node_id.c_str());
    StructRNA *srna = RNA_def_struct_ptr(&BLENDER_RNA, node_def.node_id.c_str(), &RNA_Node);
    if (srna) {
      ntype->rna_ext.srna = srna;
      RNA_def_struct_ui_text(srna, node_def.name.c_str(), node_def.description.c_str());
      RNA_def_struct_sdna(srna, "bNode");

      /* CRITICAL: Establish bidirectional link between RNA struct and bNodeType */
      /* This allows Blender to find the node type when loading .blend files */
      RNA_struct_blender_type_set(srna, ntype);

      /* Store the RNA struct for cleanup later */
      g_external_node_rna_structs.append(srna);
      CLOG_INFO(&LOG, "    RNA struct created successfully");
    }
    else {
      CLOG_ERROR(&LOG, "    Failed to create RNA struct for node: %s", node_def.node_id.c_str());
      MEM_delete(ntype);
      continue;
    }

    /* Set custom node information */
    ntype->ui_name = node_def.name;
    ntype->ui_description = node_def.description;
    ntype->nclass = NODE_CLASS_GEOMETRY;

    /* Override poll function for geometry nodes */
    ntype->poll = external_node_poll;

    /* Set our custom functions */
    ntype->declare = external_node_declare_wrapper;
    ntype->geometry_node_execute = external_node_execute_wrapper;

    /* Register storage management functions */
    /* Note: We don't pass a struct name because ExternalNodeStorage is not a DNA struct */
    /* It's runtime-only storage managed by our init/free/copy functions */
    node_type_storage(*ntype,
                      "",  /* Empty string = no DNA serialization */
                      external_node_free_storage,
                      external_node_copy_storage);
    ntype->initfunc = external_node_init_storage;

    /* Register the node type with Blender's node system */
    CLOG_INFO(&LOG, "    Calling node_register_type...");
    node_register_type(*ntype);
    CLOG_INFO(&LOG, "    node_register_type completed");

    /* Store the node type pointer for cleanup later */
    g_external_node_types.append(ntype);

    CLOG_INFO(&LOG, "  Successfully registered: %s (%s)",
              node_def.name.c_str(),
              node_def.node_id.c_str());
  }

  /* Mark nodes as registered */
  service.nodes_registered = true;
  CLOG_INFO(&LOG, "Completed node registration for service '%s'", service.manifest.service_name.c_str());
}

Vector<std::string> ExternalNodeServiceManager::get_registered_external_node_ids() const
{
  Vector<std::string> node_ids;

  for (const std::unique_ptr<ExternalNodeService> &service : services_) {
    if (service->nodes_registered) {
      for (const ExternalNodeDefinition &node_def : service->nodes) {
        node_ids.append(node_def.node_id);
      }
    }
  }

  return node_ids;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SSE Event Subscription
 * \{ */

/* ExternalNodeSseConnection constructor and destructor implementations */
ExternalNodeSseConnection::ExternalNodeSseConnection()
    : node_ptr(nullptr), tree_ptr(nullptr), requires_update(false)
{
}

/* Destructor must be defined in .cc file where SseClient is complete (PIMPL with unique_ptr) */
ExternalNodeSseConnection::~ExternalNodeSseConnection()
{
}

/* Move constructor - manually implemented because std::atomic<bool> cannot be moved with = default */
ExternalNodeSseConnection::ExternalNodeSseConnection(ExternalNodeSseConnection &&other) noexcept
    : client(std::move(other.client)),
      node_id(std::move(other.node_id)),
      node_ptr(other.node_ptr),
      tree_ptr(other.tree_ptr),
      requires_update(other.requires_update.load())
{
  other.node_ptr = nullptr;
  other.tree_ptr = nullptr;
  other.requires_update.store(false);
}

/* Move assignment - manually implemented because std::atomic<bool> cannot be moved with = default */
ExternalNodeSseConnection &ExternalNodeSseConnection::operator=(ExternalNodeSseConnection &&other) noexcept
{
  if (this != &other) {
    client = std::move(other.client);
    node_id = std::move(other.node_id);
    node_ptr = other.node_ptr;
    tree_ptr = other.tree_ptr;
    requires_update.store(other.requires_update.load());

    other.node_ptr = nullptr;
    other.tree_ptr = nullptr;
    other.requires_update.store(false);
  }
  return *this;
}

void ExternalNodeServiceManager::start_sse_connection(const std::string &node_id,
                                                        const std::string &sse_stream_url,
                                                        const std::string &service_id,
                                                        ::bNode *node,
                                                        ::bNodeTree *tree)
{
  std::lock_guard<std::mutex> lock(sse_connections_mutex_);

  /* Find service by service_id */
  ExternalNodeService *service = find_service(service_id);
  if (service == nullptr) {
    CLOG_WARN(&LOG, "Service '%s' not found for SSE connection", service_id.c_str());
    return;
  }

  /* Extract protocol+host+port from api_base_url */
  /* api_base_url is like "http://127.0.0.1:8567/api/v1/geonodes" */
  /* We need just "http://127.0.0.1:8567" because sse_stream_url already has the full path */
  std::string api_base_url = service->api_base_url;

  /* Find the third slash (after "http://") to extract protocol+host+port */
  size_t protocol_end = api_base_url.find("://");
  std::string protocol_and_host;
  if (protocol_end != std::string::npos) {
    size_t path_start = api_base_url.find('/', protocol_end + 3);
    if (path_start != std::string::npos) {
      protocol_and_host = api_base_url.substr(0, path_start);
    } else {
      protocol_and_host = api_base_url;  /* No path, entire URL is protocol+host */
    }
  } else {
    protocol_and_host = api_base_url;  /* Fallback */
  }

  std::string full_url = protocol_and_host + sse_stream_url;

  CLOG_INFO(&LOG, "Starting SSE connection for node %s: %s", node_id.c_str(), full_url.c_str());

  /* Create connection object */
  auto connection = std::make_unique<ExternalNodeSseConnection>();
  connection->node_id = node_id;
  connection->node_ptr = node;
  connection->tree_ptr = tree;

  /* Event callback (runs on background thread) */
  auto event_callback = [node_id, this](const sse::SseEvent &event) {
    /* Log all SSE events for debugging */
    CLOG_INFO(&LOG,
              "SSE event received for node %s: type='%s' requiresReexecution=%d data='%s'",
              node_id.c_str(),
              event.event_type.c_str(),
              event.requires_reexecution,
              event.data.c_str());

    /* Check if this event requires re-execution.
     * Protocol: SSE events use "event: geonode" header with JSON payload containing
     * "type" field (nodeDataChanged, nodeError, etc.) and "requiresReexecution" boolean.
     * See Protocol-External-Geometry-Nodes.md section 3.4 for full specification. */
    if (event.requires_reexecution) {
      /* Set flag for invalidation (thread-safe atomic operation) */
      std::lock_guard<std::mutex> cb_lock(sse_connections_mutex_);
      std::unique_ptr<ExternalNodeSseConnection> *conn_ptr = sse_connections_.lookup_ptr(node_id);
      if (conn_ptr && *conn_ptr) {
        (*conn_ptr)->requires_update = true;
        CLOG_INFO(&LOG, "Marking node %s for re-execution (event type: %s)",
                  node_id.c_str(), event.event_type.c_str());
      }
    }
  };

  /* Error callback */
  auto error_callback = [node_id](const std::string &error) {
    CLOG_WARN(&LOG, "SSE error for node %s: %s", node_id.c_str(), error.c_str());
  };

  /* Connect SSE client */
  connection->client = sse::SseClient::connect(full_url, event_callback, error_callback);

  if (connection->client) {
    /* Store connection */
    sse_connections_.add_new(node_id, std::move(connection));
    CLOG_INFO(&LOG, "SSE connection established for node %s", node_id.c_str());
  }
  else {
    CLOG_ERROR(&LOG, "Failed to establish SSE connection for node %s", node_id.c_str());
  }
}

void ExternalNodeServiceManager::stop_sse_connection(const std::string &node_id)
{
  std::lock_guard<std::mutex> lock(sse_connections_mutex_);

  std::unique_ptr<ExternalNodeSseConnection> *conn_ptr = sse_connections_.lookup_ptr(node_id);
  if (conn_ptr && *conn_ptr) {
    if ((*conn_ptr)->client) {
      (*conn_ptr)->client->disconnect();
    }
    sse_connections_.remove(node_id);
    CLOG_INFO(&LOG, "Stopped SSE connection for node %s", node_id.c_str());
  }
}

void ExternalNodeServiceManager::check_and_invalidate_nodes()
{
  std::lock_guard<std::mutex> lock(sse_connections_mutex_);

  for (auto item : sse_connections_.items()) {
    const std::string &node_id = item.key;
    std::unique_ptr<ExternalNodeSseConnection> &connection = item.value;

    if (connection->requires_update) {
      /* Invalidate objects using this node tree (triggers geometry re-execution)
       *
       * Key insight: Geometry nodes are evaluated as part of the object's modifier stack,
       * NOT directly from the tree. Therefore, we must tag the OBJECTS, not the tree.
       * Tagging the tree alone does not trigger modifier evaluation.
       */
      if (connection->tree_ptr) {
        CLOG_INFO(&LOG, "Invalidating objects for node %s: tree_ptr=%p tree_id='%s'",
                  node_id.c_str(),
                  static_cast<void*>(connection->tree_ptr),
                  connection->tree_ptr->id.name);

        /* Tag all objects that use this node tree via Geometry Nodes modifiers.
         * This is the ONLY action that actually triggers geometry node re-execution. */
        int objects_tagged = 0;

        if (!G_MAIN) {
          CLOG_WARN(&LOG, "G_MAIN is NULL, cannot invalidate objects for node %s", node_id.c_str());
        }
        else {
          /* Iterate through all objects */
          LISTBASE_FOREACH (Object *, ob, &G_MAIN->objects) {
            /* Check each Geometry Nodes modifier */
            LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
              if (md->type == eModifierType_Nodes) {
                NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);

                /* Check if this modifier uses our node tree.
                 * IMPORTANT: Compare by tree NAME, not pointer! Objects use evaluated copies
                 * of the original tree, so pointer comparison fails. Both the original tree
                 * (connection->tree_ptr) and evaluated copy (nmd->node_group) have the same
                 * ID name (e.g., "NTGeometry Nodes"). */
                if (nmd->node_group &&
                    strcmp(nmd->node_group->id.name, connection->tree_ptr->id.name) == 0) {
                  /* Tag this object for re-evaluation (triggers modifier stack) */
                  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
                  objects_tagged++;
                }
              }
            }
          }

          CLOG_INFO(&LOG, "Tagged %d objects for re-evaluation (node %s)",
                    objects_tagged, node_id.c_str());
        }
      }
      else {
        CLOG_WARN(&LOG, "Cannot invalidate node %s: tree_ptr is null",
                  node_id.c_str());
      }

      /* Reset flag */
      connection->requires_update = false;
    }
  }
}

/** \} */

}  // namespace blender::bke
