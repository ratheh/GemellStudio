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

#include "CLG_log.h"

#include "BKE_appdir.hh"
#include "BKE_node.hh"

#include "BLI_fileops.hh"
#include "BLI_map.hh"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "NOD_geometry.hh"
#include "NOD_geometry_exec.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "DNA_node_types.h"

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
}

void ExternalNodeServiceManager::cleanup()
{
  /* Shutdown all running services (delegates to process helper) */
  external_service_shutdown_all(services_);

  /* Unregister and free dynamically allocated node types */
  CLOG_INFO(&LOG, "Unregistering %d external node type(s)...", int(g_external_node_types.size()));
  for (bke::bNodeType *ntype : g_external_node_types) {
    if (ntype) {
      /* Unregister the node type from Blender's node system */
      node_unregister_type(*ntype);
      /* Free the allocated node type */
      MEM_delete(ntype);
    }
  }
  g_external_node_types.clear();

  /* Note: We don't manually free RNA structs - Blender's RNA system manages their lifecycle.
   * Attempting to free them here causes "freed while holding a Python reference" warnings. */

  /* Clear node registries */
  g_external_node_registry.clear();
  g_external_node_rna_structs.clear();

  /* Release resources */
  services_.clear();
  service_map_.clear();
  service_ptr_cache_.clear();

  CLOG_INFO(&LOG, "External Node Service Manager cleaned up");
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
    if (auto port = api_dict->lookup_int("port")) {
      manifest.api_port = int(*port);
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
  return external_service_launch(service);
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
 * Stub execution function for external nodes.
 * TODO: Implement actual execution by calling the external service via REST API.
 */
static void external_node_execute_stub(nodes::GeoNodeExecParams /* params */)
{
  CLOG_WARN(&LOG, "External node execution not yet implemented");
  /* For now, just pass through geometry if there's a geometry input */
  /* This prevents crashes when the node is evaluated */
}

void ExternalNodeServiceManager::register_all_pending_nodes()
{
  CLOG_INFO(&LOG, "Registering all pending external nodes...");

  for (const std::unique_ptr<ExternalNodeService> &service : services_) {
    if (!service->nodes_registered && !service->nodes.is_empty()) {
      CLOG_INFO(&LOG,
                "Registering %d node(s) from service '%s'",
                service->nodes.size(),
                service->manifest.service_name.c_str());
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
    CLOG_INFO(&LOG, "    node_type_base completed");

    /* Create RNA struct for this node type (required by node_register_type) */
    /* We need to allocate the RNA struct dynamically for external nodes */
    CLOG_INFO(&LOG, "    Creating RNA struct...");
    StructRNA *srna = RNA_def_struct_ptr(&BLENDER_RNA, node_def.node_id.c_str(), &RNA_Node);
    if (srna) {
      ntype->rna_ext.srna = srna;
      RNA_def_struct_ui_text(srna, node_def.name.c_str(), node_def.description.c_str());
      RNA_def_struct_sdna(srna, "bNode");
      /* Store the RNA struct for cleanup later */
      g_external_node_rna_structs.append(srna);
      /* Note: Icon will be set to default geometry node icon by the UI system */
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
    ntype->geometry_node_execute = external_node_execute_stub;

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

/** \} */

}  // namespace blender::bke
