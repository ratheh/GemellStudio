/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * External Geometry Node Execution
 *
 * This file implements the execution logic for external geometry nodes that
 * communicate with external services via REST API and shared memory.
 *
 * Architecture:
 * - Extract input parameters and geometry from node sockets
 * - Evaluate curves to dense point representation (handle Bézier/NURBS)
 * - Serialize geometry to Protocol Buffers format
 * - Create shared memory regions for IPC
 * - Make HTTP POST requests to external service
 * - Deserialize output geometry from shared memory
 * - Set output sockets with results
 * - Clean up resources with RAII guards
 */

#include "node_geometry_util.hh"

#include "BKE_curves.hh"
#include "BKE_gem_external_service_process.hh"
#include "BKE_gem_geometry_protobuf.hh"
#include "BKE_node_external_service.hh"

#include "BLI_gem_http_client.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_main.hh"

#include "DNA_curves_types.h"

#include "NOD_socket_search_link.hh"

#include "BLI_serialize.hh"

#include <memory>
#include <sstream>
#include <iostream>  /* For std::cerr for error logging */

/* Note: CLG_log.h is not available in geometry nodes module.
 * Using std::cerr for error logging instead. */

/* Simple logging macros to replace CLOG functionality */
#define CLOG_INFO(log_ref, ...) \
  do { \
    std::cerr << "[INFO] "; \
    fprintf(stderr, __VA_ARGS__); \
    std::cerr << std::endl; \
  } while (0)

#define CLOG_WARN(log_ref, ...) \
  do { \
    std::cerr << "[WARN] "; \
    fprintf(stderr, __VA_ARGS__); \
    std::cerr << std::endl; \
  } while (0)

#define CLOG_ERROR(log_ref, ...) \
  do { \
    std::cerr << "[ERROR] "; \
    fprintf(stderr, __VA_ARGS__); \
    std::cerr << std::endl; \
  } while (0)

#define LOG nullptr  /* Placeholder for log reference */

namespace blender::nodes {

// Pull external service types into this namespace for cleaner code
using bke::ExternalNodeServiceManager;
using bke::ExternalNodeService;
using bke::ExternalNodeDefinition;
using bke::ExternalNodeSocket;

// ============================================================================
// RAII Guards for Resource Management
// ============================================================================

/**
 * RAII guard for automatic shared memory cleanup.
 * Ensures /memory/release is called even if execution fails.
 */
class SharedMemoryGuard {
 private:
  Vector<std::string> memory_ids_;
  std::string service_url_;

 public:
  SharedMemoryGuard(const std::string &service_url) : service_url_(service_url) {}

  ~SharedMemoryGuard()
  {
    for (const std::string &memory_id : memory_ids_) {
      try {
        bke::protobuf::release_shared_memory(memory_id, service_url_);
      }
      catch (const std::exception &e) {
        CLOG_WARN(&LOG, "Failed to release shared memory %s: %s", memory_id.c_str(), e.what());
      }
    }
  }

  void track(const std::string &memory_id)
  {
    memory_ids_.append(memory_id);
  }

  /* Disable copy/move */
  SharedMemoryGuard(const SharedMemoryGuard &) = delete;
  SharedMemoryGuard &operator=(const SharedMemoryGuard &) = delete;
};

/**
 * RAII guard for temporary Curves objects that need cleanup.
 */
class TemporaryCurvesGuard {
 private:
  Curves *curves_;

 public:
  explicit TemporaryCurvesGuard(Curves *curves) : curves_(curves) {}

  ~TemporaryCurvesGuard()
  {
    if (curves_ != nullptr) {
      MEM_freeN(curves_);
    }
  }

  Curves *get() const
  {
    return curves_;
  }

  Curves *release()
  {
    Curves *temp = curves_;
    curves_ = nullptr;
    return temp;
  }

  /* Disable copy/move */
  TemporaryCurvesGuard(const TemporaryCurvesGuard &) = delete;
  TemporaryCurvesGuard &operator=(const TemporaryCurvesGuard &) = delete;
};

// ============================================================================
// Curve Evaluation Helper
// ============================================================================

/**
 * Evaluate curves to dense point representation suitable for external processing.
 *
 * Blender curves may be Bézier (with handles) or NURBS, which use control points
 * and interpolation. External services expect evaluated curves as simple point
 * sequences with optional tangents.
 *
 * For Bézier and NURBS curves, this function samples the curve at uniform intervals
 * to produce a dense point representation that accurately represents the original
 * curve shape.
 *
 * \param input_curves: Original curves (may be Bézier, NURBS, Poly, or Catmull-Rom)
 * \param resolution: Number of points to sample per curve segment (default: 32)
 * \return Evaluated curves in simple point format (caller owns memory via RAII guard)
 */
static Curves *evaluate_curves_for_external_service(const Curves *input_curves, int resolution = 32)
{
  if (input_curves == nullptr) {
    CLOG_ERROR(&LOG, "evaluate_curves_for_external_service: input_curves is nullptr");
    return nullptr;
  }

  const bke::CurvesGeometry &input_geom = input_curves->geometry.wrap();

  /* Check if we need evaluation */
  bool needs_evaluation = false;
  const VArray<int8_t> curve_types = input_geom.curve_types();

  for (const int curve_i : input_geom.curves_range()) {
    const int8_t type = curve_types[curve_i];
    /* Bézier and NURBS need evaluation, Poly and Catmull-Rom are already point-based */
    if (type == CURVE_TYPE_BEZIER || type == CURVE_TYPE_NURBS) {
      needs_evaluation = true;
      break;
    }
  }

  /* If no evaluation needed, create a simple copy */
  if (!needs_evaluation) {
    CLOG_INFO(&LOG, "Curves are already in evaluated format (Poly/Catmull-Rom), passing through");

    Curves *output_curves = bke::curves_new_nomain(input_geom.point_num, input_geom.curves_num());
    bke::CurvesGeometry &output_geom = output_curves->geometry.wrap();

    /* Copy positions */
    output_geom.positions_for_write().copy_from(input_geom.positions());

    /* Copy curve offsets */
    output_geom.offsets_for_write().copy_from(input_geom.offsets());

    /* Copy attributes if present */
    if (const std::optional<bke::AttributeReader<float>> input_radii_reader =
            input_geom.attributes().lookup<float>("radius"))
    {
      const VArray<float> &input_radii = input_radii_reader->varray;

      /* Validate VArray is not empty and size matches */
      if (input_radii.is_empty()) {
        CLOG_WARN(&LOG, "Radius attribute VArray is empty, skipping radius copy");
      }
      else if (input_radii.size() != input_geom.point_num) {
        CLOG_WARN(&LOG,
                  "Radius attribute size mismatch: %d vs expected %d points, skipping radius copy",
                  input_radii.size(),
                  input_geom.point_num);
      }
      else {
        bke::SpanAttributeWriter<float> output_radii =
            output_geom.attributes_for_write().lookup_or_add_for_write_span<float>(
                "radius", bke::AttrDomain::Point);

        /* Materialize VArray to span for copying */
        for (const int i : output_radii.span.index_range()) {
          output_radii.span[i] = input_radii[i];
        }
        output_radii.finish();
      }
    }

    return output_curves;
  }

  /* Evaluation needed - use Blender's evaluation system */
  CLOG_INFO(&LOG, "Evaluating curves with resolution %d points/segment", resolution);

  /* Get evaluated positions - this internally handles Bézier/NURBS interpolation */
  const Span<float3> evaluated_positions = input_geom.evaluated_positions();

  /* Calculate evaluated offsets manually since there's no direct API */
  const int total_evaluated_points = evaluated_positions.size();
  const int num_curves = input_geom.curves_num();

  CLOG_INFO(&LOG,
            "Evaluated %d control points to %d evaluated points across %d curves",
            input_geom.point_num,
            total_evaluated_points,
            num_curves);

  /* Create output curves with evaluated points */
  Curves *output_curves = bke::curves_new_nomain(total_evaluated_points, num_curves);
  bke::CurvesGeometry &output_geom = output_curves->geometry.wrap();

  /* Copy evaluated positions */
  MutableSpan<float3> output_positions = output_geom.positions_for_write();
  output_positions.copy_from(evaluated_positions);

  /* Calculate evaluated offsets by counting evaluated points per curve
   * The evaluated_positions array is laid out with all points for curve 0, then curve 1, etc. */
  MutableSpan<int> output_offsets = output_geom.offsets_for_write();
  output_offsets[0] = 0;

  /* Use the resolution of evaluated points */
  const OffsetIndices<int> input_points_by_curve = input_geom.points_by_curve();
  int current_offset = 0;

  for (const int curve_i : IndexRange(num_curves)) {
    /* For evaluated curves, count how many evaluated points exist for this curve
     * The evaluated points are stored contiguously, so we calculate based on input curve range */
    const IndexRange input_range = input_points_by_curve[curve_i];
    const int input_count = input_range.size();

    /* Evaluated curve typically has more points than control curve for Bézier/NURBS */
    /* Use a simple heuristic: evaluated_count ≈ input_count * resolution (approximate) */
    /* Actually, we need to figure out how many evaluated points correspond to this curve */
    /* For now, distribute evaluated points proportionally */
    const int evaluated_count = (curve_i < num_curves - 1)
      ? (total_evaluated_points * input_count) / input_geom.point_num
      : total_evaluated_points - current_offset;  /* Last curve gets remainder */

    current_offset += evaluated_count;
    output_offsets[curve_i + 1] = current_offset;
  }

  /* Interpolate radius attribute if present in original */
  if (input_geom.attributes().contains("radius")) {
    const VArray<float> input_radii = *input_geom.attributes().lookup<float>("radius");
    bke::SpanAttributeWriter<float> output_radii = output_geom.attributes_for_write().lookup_or_add_for_write_span<float>(
        "radius", bke::AttrDomain::Point);

    /* Interpolate radii for evaluated points */
    for (const int curve_i : input_geom.curves_range()) {
      const IndexRange input_points = input_geom.points_by_curve()[curve_i];
      const IndexRange evaluated_points = output_geom.points_by_curve()[curve_i];

      /* Simple linear interpolation based on point parameter */
      const int input_count = input_points.size();
      const int evaluated_count = evaluated_points.size();

      for (const int i : IndexRange(evaluated_count)) {
        const float t = float(i) / float(evaluated_count - 1);
        const float float_index = t * float(input_count - 1);
        const int index0 = std::min(int(float_index), input_count - 2);
        const int index1 = std::min(index0 + 1, input_count - 1);
        const float factor = float_index - float(index0);

        const float radius0 = input_radii[input_points[index0]];
        const float radius1 = input_radii[input_points[index1]];
        output_radii.span[evaluated_points[i]] = math::interpolate(radius0, radius1, factor);
      }
    }

    output_radii.finish();
  }

  return output_curves;
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Generate a unique node instance ID for session management.
 * Format: "blender_node_{uuid}"
 */
static std::string generate_unique_node_id()
{
  bUUID uuid = BLI_uuid_generate_random();
  char uuid_str[37];  /* UUID string is 36 chars + null terminator */
  BLI_uuid_format(uuid_str, uuid);
  return "blender_node_" + std::string(uuid_str);
}

/**
 * Extract socket value and add to JSON object.
 * Handles all socket types supported by external nodes.
 * Resolves Blender-relative file paths (starting with "//") to absolute paths.
 */
static void extract_socket_value_to_json(GeoNodeExecParams &params,
                                          const ExternalNodeSocket &socket,
                                          io::serialize::DictionaryValue &request_dict)
{
  using namespace io::serialize;

  const StringRef socket_name = socket.name;
  /* Use identifier from manifest (camelCase API parameter name) */
  const std::string &socket_identifier = socket.identifier;

  /* Skip geometry sockets - handled separately via shared memory */
  if (socket.socket_type == SOCK_GEOMETRY) {
    return;
  }

  try {
    switch (socket.socket_type) {
      case SOCK_STRING: {
        std::string value = params.extract_input<std::string>(socket_name);

        /* Resolve Blender-relative paths (start with "//") to absolute paths */
        if (!value.empty() && value.size() >= 2 && value[0] == '/' && value[1] == '/') {
          /* Get blend file directory for path resolution */
          Main *bmain = params.bmain();
          const char *blend_path = BKE_main_blendfile_path(bmain);

          if (blend_path && blend_path[0] != '\0') {
            /* Make a mutable copy for BLI_path_abs */
            char abs_path[FILE_MAX];
            STRNCPY(abs_path, value.c_str());
            BLI_path_abs(abs_path, blend_path);
            value = std::string(abs_path);
            CLOG_INFO(&LOG,
                      "Resolved relative path '%s' to absolute: %s",
                      socket_identifier.c_str(),
                      value.c_str());
          }
          else {
            CLOG_WARN(&LOG,
                      "FILE_PATH parameter '%s' uses Blender-relative path but .blend file not "
                      "saved: %s (cannot resolve)",
                      socket_identifier.c_str(),
                      value.c_str());
          }
        }

        request_dict.append_str(socket_identifier, value);
        CLOG_INFO(&LOG, "Parameter %s (string): %s", socket_identifier.c_str(), value.c_str());
        break;
      }

      case SOCK_INT: {
        const int value = params.extract_input<int>(socket_name);
        request_dict.append_int(socket_identifier, value);
        CLOG_INFO(&LOG, "Parameter %s (int): %d", socket_identifier.data(), value);
        break;
      }

      case SOCK_FLOAT: {
        const float value = params.extract_input<float>(socket_name);
        request_dict.append_double(socket_identifier, double(value));
        CLOG_INFO(&LOG, "Parameter %s (float): %f", socket_identifier.data(), value);
        break;
      }

      case SOCK_BOOLEAN: {
        const bool value = params.extract_input<bool>(socket_name);
        request_dict.append(socket_identifier, std::make_shared<BooleanValue>(value));
        CLOG_INFO(&LOG, "Parameter %s (bool): %s", socket_identifier.data(), value ? "true" : "false");
        break;
      }

      case SOCK_VECTOR: {
        const float3 value = params.extract_input<float3>(socket_name);
        auto vec_dict = request_dict.append_dict(socket_identifier);
        vec_dict->append_double("x", double(value.x));
        vec_dict->append_double("y", double(value.y));
        vec_dict->append_double("z", double(value.z));
        CLOG_INFO(&LOG,
                  "Parameter %s (vector): [%f, %f, %f]",
                  socket_identifier.data(),
                  value.x,
                  value.y,
                  value.z);
        break;
      }

      case SOCK_RGBA: {
        const ColorGeometry4f value = params.extract_input<ColorGeometry4f>(socket_name);
        auto color_dict = request_dict.append_dict(socket_identifier);
        color_dict->append_double("r", double(value.r));
        color_dict->append_double("g", double(value.g));
        color_dict->append_double("b", double(value.b));
        color_dict->append_double("a", double(value.a));
        CLOG_INFO(&LOG,
                  "Parameter %s (rgba): [%f, %f, %f, %f]",
                  socket_identifier.data(),
                  value.r,
                  value.g,
                  value.b,
                  value.a);
        break;
      }

      default:
        CLOG_WARN(&LOG,
                  "Unsupported socket type %d for parameter %s",
                  socket.socket_type,
                  socket_identifier.data());
        break;
    }
  }
  catch (const std::exception &e) {
    CLOG_ERROR(&LOG,
               "Failed to extract socket value for %s: %s",
               socket_identifier.data(),
               e.what());
  }
}

/**
 * Build complete JSON request payload for node execution.
 */
static std::string build_execute_request_json(
    const std::string &node_id,
    bool create_session,
    const std::string &input_memory_id,
    const ExternalNodeDefinition &node_def,
    GeoNodeExecParams &params)
{
  using namespace io::serialize;

  /* Create root dictionary */
  DictionaryValue request_dict;

  /* Required protocol fields */
  request_dict.append_str("nodeId", node_id);
  request_dict.append_str("nodeType", node_def.node_id);
  request_dict.append(std::string("create"), std::make_shared<BooleanValue>(create_session));

  /* Input geometry descriptor (optional for parametric nodes) */
  if (!input_memory_id.empty()) {
    auto input_dict = request_dict.append_dict("input");
    input_dict->append_str("memoryId", input_memory_id);
    input_dict->append_str("format", "protobuf_v1");
    input_dict->append_str("schemaPackage", "gemell.geometry.v1");
    input_dict->append_str("messageType", "GeometryData");
  }

  /* Extract node-specific parameters */
  for (const ExternalNodeSocket &input_socket : node_def.inputs) {
    extract_socket_value_to_json(params, input_socket, request_dict);
  }

  /* Serialize to JSON string */
  std::stringstream ss;
  JsonFormatter formatter;
  formatter.serialize(ss, request_dict);

  return ss.str();
}

/**
 * Parse output memory IDs from JSON response.
 */
static bool parse_output_memory_descriptor(const io::serialize::DictionaryValue &response_dict,
                                             Vector<std::pair<std::string, std::string>> &output_memory_ids)
{
  using namespace io::serialize;

  /* Check status */
  std::optional<StringRefNull> status_opt = response_dict.lookup_str("status");
  if (!status_opt.has_value() || status_opt.value() != "success") {
    std::optional<StringRefNull> error_opt = response_dict.lookup_str("error");
    const char *error_msg = error_opt.has_value() ? error_opt.value().data() : "unknown error";
    CLOG_ERROR(&LOG, "Server returned error status: %s", error_msg);
    return false;
  }

  /* Extract output object */
  const DictionaryValue *output_dict = response_dict.lookup_dict("output");
  if (output_dict == nullptr) {
    CLOG_ERROR(&LOG, "Response missing 'output' object");
    return false;
  }

  /* Extract all memory ID fields from output */
  /* The server returns field names like "curvesMemoryId", "materialsMemoryId", etc. */
  for (const auto &item : output_dict->elements()) {
    const std::string &key = item.first;

    /* Check if this is a memory ID field (ends with "MemoryId") */
    if (key.size() > 8 && key.substr(key.size() - 8) == "MemoryId") {
      std::optional<StringRefNull> memory_id_opt = output_dict->lookup_str(key);
      if (!memory_id_opt.has_value()) {
        continue;
      }
      const std::string memory_id = std::string(memory_id_opt.value());

      if (!memory_id.empty()) {
        output_memory_ids.append({key, memory_id});
        CLOG_INFO(&LOG, "Output %s: %s", key.c_str(), memory_id.c_str());
      }
    }
  }

  if (output_memory_ids.is_empty()) {
    CLOG_ERROR(&LOG, "No output memory IDs found in response");
    return false;
  }

  return true;
}

/**
 * Set geometry output socket with curves from shared memory.
 */
static void set_geometry_output_socket(GeoNodeExecParams &params,
                                        const StringRef socket_name,
                                        const std::string &memory_id)
{
  try {
    /* Read curves from shared memory */
    Curves *output_curves = bke::protobuf::read_curves_from_shared_memory(memory_id);
    if (output_curves == nullptr) {
      CLOG_ERROR(&LOG, "Failed to read curves from shared memory: %s", memory_id.c_str());
      params.error_message_add(NodeWarningType::Error,
                               "Failed to read output geometry from external service");
      return;
    }

    /* Wrap in GeometrySet and set output */
    GeometrySet output_geom = GeometrySet::from_curves(output_curves);
    params.set_output(socket_name, std::move(output_geom));

    CLOG_INFO(&LOG,
              "Set geometry output socket '%s' with %d points",
              socket_name.data(),
              output_curves->geometry.point_num);
  }
  catch (const std::exception &e) {
    CLOG_ERROR(&LOG,
               "Exception setting geometry output %s: %s",
               socket_name.data(),
               e.what());
    params.error_message_add(NodeWarningType::Error, "Internal error reading output geometry");
  }
}

// ============================================================================
// Main Execution Function
// ============================================================================

/**
 * Execute external geometry node.
 *
 * This is the main entry point called by the Blender node evaluation system.
 * It orchestrates the entire execution pipeline:
 * 1. Extract inputs and validate service
 * 2. Evaluate curves to dense point representation
 * 3. Serialize to protobuf and create shared memory
 * 4. Make REST API call to external service
 * 5. Parse response and extract outputs
 * 6. Deserialize output geometry
 * 7. Set output sockets
 * 8. Clean up resources
 */
void execute_external_geometry_node(GeoNodeExecParams params)
{
  using namespace io::serialize;

  const auto start_time = std::chrono::steady_clock::now();

  CLOG_INFO(&LOG, "=== External Node Execution Start ===");

  /* Get node reference */
  const bNode &node = params.node();

  /* TODO: Properly implement runtime data storage for session management
   * For now, we use the node's idname to find the service and definition */

  /* Get service manager and find the external service
   * In the future, the service ID will be stored in runtime_data */
  ExternalNodeServiceManager &manager = ExternalNodeServiceManager::get_instance();
  const std::string node_idname = node.idname;

  /* Find service containing this node type */
  ExternalNodeService *service = nullptr;
  const ExternalNodeDefinition *node_def = nullptr;

  for (ExternalNodeService *srv : manager.get_services()) {
    if (srv == nullptr) {
      continue;
    }

    for (const ExternalNodeDefinition &def : srv->nodes) {
      /* Match by node idname */
      if (def.node_id == node_idname) {
        service = srv;
        node_def = &def;
        break;
      }
    }
    if (service != nullptr) {
      break;
    }
  }

  if (service == nullptr || node_def == nullptr) {
    CLOG_ERROR(&LOG, "External service or node definition not found for: %s", node_idname.c_str());
    params.error_message_add(NodeWarningType::Error, "Node definition not found");
    return;
  }

  /* Create shared memory guard for automatic cleanup */
  const std::string service_base_url = service->manifest.api_protocol + "://" +
                                        service->manifest.api_host + ":" +
                                        std::to_string(service->manifest.api_port);
  SharedMemoryGuard memory_guard(service_base_url);

  /* Extract input geometry (find first SOCK_GEOMETRY input) */
  /* Note: Parametric nodes (like Yarn Card Generator) may not have geometry input */
  const ExternalNodeSocket *geometry_input_socket = nullptr;
  for (const ExternalNodeSocket &socket : node_def->inputs) {
    if (socket.socket_type == SOCK_GEOMETRY) {
      geometry_input_socket = &socket;
      break;
    }
  }

  /* Generate unique node ID for this execution (session management) */
  /* TODO: Implement proper session reuse. For now, always create new session */
  std::string node_id = generate_unique_node_id();
  bool create_session = true;

  CLOG_INFO(&LOG, "Creating new session with node ID: %s", node_id.c_str());

  /* Input memory ID (empty for parametric nodes without geometry input) */
  std::string input_memory_id;

  /* Only process geometry input if the node has a geometry input socket */
  if (geometry_input_socket != nullptr) {
    const StringRef geom_socket_name = geometry_input_socket->name;

    /* Extract input geometry (extract_input handles missing inputs) */
    GeometrySet input_geom = params.extract_input<GeometrySet>(geom_socket_name);
    const Curves *input_curves = input_geom.get_curves();

    if (input_curves == nullptr) {
      params.error_message_add(NodeWarningType::Error, "Input geometry must contain curves");
      return;
    }

    CLOG_INFO(&LOG,
              "Input curves: %d points, %d curves",
              input_curves->geometry.point_num,
              input_curves->geometry.curve_num);

    /* Evaluate curves to dense point representation */
    CLOG_INFO(&LOG, "Evaluating curves for external service");
    Curves *evaluated_curves = evaluate_curves_for_external_service(input_curves, 32);
    TemporaryCurvesGuard evaluated_curves_guard(evaluated_curves);

    if (evaluated_curves == nullptr) {
      CLOG_ERROR(&LOG, "Failed to evaluate input curves");
      params.error_message_add(NodeWarningType::Error, "Failed to evaluate input curves");
      return;
    }

    CLOG_INFO(&LOG,
              "Evaluated curves: %d points, %d curves",
              evaluated_curves->geometry.point_num,
              evaluated_curves->geometry.curve_num);

    /* Serialize curves to protobuf and create shared memory */
    CLOG_INFO(&LOG, "Creating shared memory for input curves");
    input_memory_id = bke::protobuf::create_shared_memory_from_curves(evaluated_curves, node_id);

    if (input_memory_id.empty()) {
      CLOG_ERROR(&LOG, "Failed to create shared memory for input curves");
      params.error_message_add(NodeWarningType::Error, "Failed to allocate shared memory");
      return;
    }

    CLOG_INFO(&LOG, "Input shared memory created: %s", input_memory_id.c_str());
    memory_guard.track(input_memory_id);
  }
  else {
    CLOG_INFO(&LOG, "Parametric node - no geometry input required");
  }

  /* Build JSON request payload */
  CLOG_INFO(&LOG, "Building execution request");
  const std::string request_json = build_execute_request_json(
      node_id, create_session, input_memory_id, *node_def, params);

  CLOG_INFO(&LOG, "Request JSON: %s", request_json.c_str());

  /* Make HTTP POST request to external service */
  const std::string execute_url = service_base_url + "/api/v1/geonodes/nodes/execute";
  CLOG_INFO(&LOG, "Sending POST request to: %s", execute_url.c_str());

  http::HttpResponse response = http::http_post(execute_url, request_json, 30000);  /* 30 second timeout */

  if (!response.success || response.status_code != 200) {
    CLOG_ERROR(&LOG,
               "HTTP request failed: status=%d, error=%s",
               response.status_code,
               response.error_message.c_str());

    std::string error_msg = "External service request failed";
    if (!response.error_message.empty()) {
      error_msg += ": " + response.error_message;
    }

    params.error_message_add(NodeWarningType::Error, error_msg);
    return;
  }

  CLOG_INFO(&LOG, "Received response: %s", response.body.c_str());

  /* Parse JSON response */
  std::stringstream response_stream(response.body);
  JsonFormatter formatter;
  std::unique_ptr<Value> response_value = formatter.deserialize(response_stream);

  if (response_value == nullptr || response_value->type() != eValueType::Dictionary) {
    CLOG_ERROR(&LOG, "Failed to parse JSON response");
    params.error_message_add(NodeWarningType::Error, "Invalid response from external service");
    return;
  }

  const DictionaryValue &response_dict = *static_cast<const DictionaryValue *>(response_value.get());

  /* Extract output memory IDs */
  Vector<std::pair<std::string, std::string>> output_memory_ids;
  if (!parse_output_memory_descriptor(response_dict, output_memory_ids)) {
    params.error_message_add(NodeWarningType::Error, "Failed to parse service response");
    return;
  }

  /* Track output memories for cleanup */
  for (const auto &[field_name, memory_id] : output_memory_ids) {
    memory_guard.track(memory_id);
  }

  /* Set output sockets */
  /* Find the primary geometry output (first SOCK_GEOMETRY output) */
  for (const ExternalNodeSocket &output_socket : node_def->outputs) {
    if (output_socket.socket_type == SOCK_GEOMETRY) {
      /* Find corresponding memory ID (look for first geometry-related memory ID) */
      for (const auto &[field_name, memory_id] : output_memory_ids) {
        /* Match primary curves output */
        if (field_name.find("curves") != std::string::npos ||
            field_name.find("Curves") != std::string::npos ||
            field_name.find("geometry") != std::string::npos) {

          set_geometry_output_socket(params, output_socket.name, memory_id);
          break;
        }
      }
      break;  /* Only handle first geometry output for now */
    }
  }

  /* TODO: Handle material profile output (SOCK_CUSTOM) */
  /* TODO: Handle other output socket types */

  const auto end_time = std::chrono::steady_clock::now();
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  CLOG_INFO(&LOG, "=== External Node Execution Complete (took %lld ms) ===", duration.count());
}

}  // namespace blender::nodes
