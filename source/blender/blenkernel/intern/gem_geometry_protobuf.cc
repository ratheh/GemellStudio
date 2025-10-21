// SPDX-FileCopyrightText: 2025 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

/** \file
 * \ingroup bke
 * \brief Protocol Buffers geometry serialization implementation
 */

// Define NOMINMAX before including windows.h to prevent min/max macro conflicts
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif

// Include protobuf headers FIRST before any Blender headers to prevent windows.h pollution
#include "gemell/geometry/v1/geometry.pb.h"
#include "gemell/geometry/v1/curves.pb.h"
#include "gemell/geometry/v1/attributes.pb.h"

#include "BKE_gem_geometry_protobuf.hh"

#include "DNA_curves_types.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"

#include "BLI_color_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"

#include "CLG_log.h"

#include "BLI_gem_http_client.hh"

#include <random>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

static CLG_LogRef LOG = {"bke.gem_geometry_protobuf"};

namespace blender::bke::protobuf {

// ============================================================================
// Shared Memory Management (Platform-Specific)
// ============================================================================

#ifdef _WIN32
/**
 * Create Windows security descriptor that allows cross-process access.
 * Returns a heap-allocated descriptor that must be freed by the caller.
 */
static SECURITY_DESCRIPTOR *create_permissive_security_descriptor()
{
  SECURITY_DESCRIPTOR *sd = (SECURITY_DESCRIPTOR *)malloc(sizeof(SECURITY_DESCRIPTOR));
  if (sd == nullptr) {
    return nullptr;
  }

  if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION)) {
    free(sd);
    return nullptr;
  }

  /* NULL DACL = allow all access to all users */
  if (!SetSecurityDescriptorDacl(sd, TRUE, nullptr, FALSE)) {
    free(sd);
    return nullptr;
  }

  return sd;
}
#endif

SharedMemoryHandle create_shared_memory(const std::string &name, size_t size)
{
  SharedMemoryHandle handle;
  handle.memory_id = name;
  handle.size = size;
  handle.is_owner = true;

#ifdef _WIN32
  // Windows implementation using CreateFileMapping with cross-process security
  std::string mapping_name = "Local\\" + name;

  /* Create security descriptor that allows cross-process access */
  SECURITY_DESCRIPTOR *sd = create_permissive_security_descriptor();
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = FALSE;
  sa.lpSecurityDescriptor = sd;

  HANDLE win_handle = CreateFileMappingA(INVALID_HANDLE_VALUE,
                                          sd ? &sa : nullptr,  /* Use secure attrs if available */
                                          PAGE_READWRITE,
                                          (DWORD)(size >> 32),
                                          (DWORD)(size & 0xFFFFFFFF),
                                          mapping_name.c_str());
  handle.handle = win_handle;

  /* Free security descriptor after creating mapping */
  if (sd) {
    free(sd);
  }

  if (win_handle == nullptr) {
    DWORD error = GetLastError();
    CLOG_ERROR(&LOG,
               "Failed to create file mapping '%s': error %lu",
               mapping_name.c_str(),
               error);
    return handle;
  }

  handle.mapped_ptr = MapViewOfFile(win_handle, FILE_MAP_ALL_ACCESS, 0, 0, size);

  if (handle.mapped_ptr == nullptr) {
    DWORD error = GetLastError();
    CLOG_ERROR(&LOG, "Failed to map view of file '%s': error %lu", mapping_name.c_str(), error);
    CloseHandle(win_handle);
    handle.handle = nullptr;
    return handle;
  }

  CLOG_INFO(&LOG,
            "Created shared memory '%s' with cross-process security (size: %zu bytes)",
            mapping_name.c_str(),
            size);

#else
  // Unix/Linux/macOS implementation using POSIX shared memory
  std::string shm_name = "/" + name;

  handle.fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
  if (handle.fd == -1) {
    CLOG_ERROR(&LOG, "Failed to create shared memory '%s': %s", shm_name.c_str(), strerror(errno));
    return handle;
  }

  if (ftruncate(handle.fd, size) == -1) {
    CLOG_ERROR(&LOG, "Failed to set shared memory size '%s': %s", shm_name.c_str(), strerror(errno));
    close(handle.fd);
    handle.fd = -1;
    shm_unlink(shm_name.c_str());
    return handle;
  }

  handle.mapped_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle.fd, 0);

  if (handle.mapped_ptr == MAP_FAILED) {
    CLOG_ERROR(&LOG, "Failed to mmap shared memory '%s': %s", shm_name.c_str(), strerror(errno));
    close(handle.fd);
    handle.fd = -1;
    handle.mapped_ptr = nullptr;
    shm_unlink(shm_name.c_str());
    return handle;
  }
#endif

  CLOG_INFO(&LOG, "Created shared memory '%s' with size %zu bytes", name.c_str(), size);
  return handle;
}

SharedMemoryHandle open_shared_memory(const std::string &name)
{
  SharedMemoryHandle handle;
  handle.memory_id = name;
  handle.is_owner = false;

#ifdef _WIN32
  // Windows implementation
  std::string mapping_name = "Local\\" + name;

  HANDLE win_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.c_str());
  handle.handle = win_handle;

  if (win_handle == nullptr) {
    DWORD error = GetLastError();
    CLOG_ERROR(&LOG, "Failed to open file mapping '%s': error %lu", mapping_name.c_str(), error);
    return handle;
  }

  handle.mapped_ptr = MapViewOfFile(win_handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);

  if (handle.mapped_ptr == nullptr) {
    DWORD error = GetLastError();
    CLOG_ERROR(&LOG, "Failed to map view of file '%s': error %lu", mapping_name.c_str(), error);
    CloseHandle(win_handle);
    handle.handle = nullptr;
    return handle;
  }

  // Query size (Windows doesn't provide direct API, we need to track this externally)
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(handle.mapped_ptr, &info, sizeof(info))) {
    handle.size = info.RegionSize;
  }

#else
  // Unix/Linux/macOS implementation
  std::string shm_name = "/" + name;

  handle.fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
  if (handle.fd == -1) {
    CLOG_ERROR(&LOG, "Failed to open shared memory '%s': %s", shm_name.c_str(), strerror(errno));
    return handle;
  }

  // Get size
  struct stat sb;
  if (fstat(handle.fd, &sb) == -1) {
    CLOG_ERROR(&LOG, "Failed to get shared memory size '%s': %s", shm_name.c_str(), strerror(errno));
    close(handle.fd);
    handle.fd = -1;
    return handle;
  }
  handle.size = sb.st_size;

  handle.mapped_ptr = mmap(nullptr, handle.size, PROT_READ | PROT_WRITE, MAP_SHARED, handle.fd, 0);

  if (handle.mapped_ptr == MAP_FAILED) {
    CLOG_ERROR(&LOG, "Failed to mmap shared memory '%s': %s", shm_name.c_str(), strerror(errno));
    close(handle.fd);
    handle.fd = -1;
    handle.mapped_ptr = nullptr;
    return handle;
  }
#endif

  CLOG_INFO(&LOG, "Opened shared memory '%s' with size %zu bytes", name.c_str(), handle.size);
  return handle;
}

void unmap_shared_memory(SharedMemoryHandle &handle, bool keep_alive)
{
  if (handle.mapped_ptr == nullptr) {
    return;
  }

#ifdef _WIN32
  UnmapViewOfFile(handle.mapped_ptr);
  /* On Windows, we must keep the handle alive for other processes to access the memory.
   * Only close the handle if keep_alive is false (when we're done with the memory). */
  if (!keep_alive && handle.handle != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle.handle));
    handle.handle = nullptr;
    CLOG_INFO(&LOG, "Closed handle for shared memory '%s'", handle.memory_id.c_str());
  }
#else
  munmap(handle.mapped_ptr, handle.size);
  /* On POSIX, closing the fd doesn't destroy the memory (need shm_unlink for that).
   * We can close it immediately. */
  if (handle.fd != -1) {
    close(handle.fd);
    handle.fd = -1;
  }
#endif

  CLOG_INFO(&LOG,
            "Unmapped shared memory '%s' (handle %s)",
            handle.memory_id.c_str(),
            keep_alive ? "kept alive" : "closed");
  handle.mapped_ptr = nullptr;
}

void destroy_shared_memory(const std::string &name)
{
#ifdef _WIN32
  // Windows file mappings are automatically destroyed when all handles are closed
  CLOG_INFO(&LOG, "Shared memory '%s' will be destroyed when all handles close", name.c_str());
#else
  std::string shm_name = "/" + name;
  if (shm_unlink(shm_name.c_str()) == -1) {
    CLOG_WARN(&LOG, "Failed to unlink shared memory '%s': %s", shm_name.c_str(), strerror(errno));
  }
  else {
    CLOG_INFO(&LOG, "Destroyed shared memory '%s'", name.c_str());
  }
#endif
}

// ============================================================================
// Geometry Serialization (Blender → Protobuf)
// ============================================================================

std::vector<uint8_t> curves_to_protobuf(const Curves *curves)
{
  if (curves == nullptr) {
    CLOG_ERROR(&LOG, "curves_to_protobuf: curves is nullptr");
    return {};
  }

  const bke::CurvesGeometry &curves_geom = curves->geometry.wrap();

  // Create protobuf message
  gemell::geometry::v1::GeometryData geom_data;
  geom_data.set_version(1);

  auto *curve_geom = geom_data.mutable_curves();
  curve_geom->set_num_curves(curves_geom.curve_num);
  curve_geom->set_num_points(curves_geom.point_num);

  // Get position attribute
  const Span<float3> positions = curves_geom.positions();

  // Serialize positions
  for (int i = 0; i < curves_geom.point_num; i++) {
    auto *pos = curve_geom->add_positions();
    pos->set_x(positions[i].x);
    pos->set_y(positions[i].y);
    pos->set_z(positions[i].z);
  }

  // Serialize curve offsets
  const OffsetIndices<int> curve_offsets = curves_geom.points_by_curve();
  for (int i = 0; i < curves_geom.curve_num; i++) {
    curve_geom->add_curve_offsets(curve_offsets[i].start());
  }

  // Serialize radii (if present)
  const VArray<float> radii_varray = *curves_geom.attributes().lookup_or_default<float>(
      "radius", bke::AttrDomain::Point, 0.01f);
  if (!radii_varray.is_single()) {
    const VArraySpan<float> radii(radii_varray);
    for (int i = 0; i < curves_geom.point_num; i++) {
      curve_geom->add_radii(radii[i]);
    }
  }

  // Serialize tangents (if present as attribute)
  if (curves_geom.attributes().contains("tangent")) {
    const VArray<float3> tangents_varray = *curves_geom.attributes().lookup<float3>(
        "tangent", bke::AttrDomain::Point);
    const VArraySpan<float3> tangents(tangents_varray);
    for (int i = 0; i < curves_geom.point_num; i++) {
      auto *tangent = curve_geom->add_tangents();
      tangent->set_x(tangents[i].x);
      tangent->set_y(tangents[i].y);
      tangent->set_z(tangents[i].z);
    }
  }

  // Serialize parameters (if present as attribute)
  if (curves_geom.attributes().contains("parameter")) {
    const VArray<float> params_varray = *curves_geom.attributes().lookup<float>(
        "parameter", bke::AttrDomain::Point);
    const VArraySpan<float> params(params_varray);
    for (int i = 0; i < curves_geom.point_num; i++) {
      curve_geom->add_parameters(params[i]);
    }
  }

  // Serialize to byte array
  size_t size = geom_data.ByteSizeLong();
  std::vector<uint8_t> buffer(size);

  if (!geom_data.SerializeToArray(buffer.data(), size)) {
    CLOG_ERROR(&LOG, "Failed to serialize geometry data to protobuf");
    return {};
  }

  CLOG_INFO(&LOG,
            "Serialized %d curves with %d points to %zu bytes",
            curves_geom.curve_num,
            curves_geom.point_num,
            size);

  return buffer;
}

Curves *protobuf_to_curves(const std::vector<uint8_t> &protobuf_data)
{
  if (protobuf_data.empty()) {
    CLOG_ERROR(&LOG, "protobuf_to_curves: protobuf_data is empty");
    return nullptr;
  }

  // Parse protobuf message
  gemell::geometry::v1::GeometryData geom_data;
  if (!geom_data.ParseFromArray(protobuf_data.data(), protobuf_data.size())) {
    CLOG_ERROR(&LOG, "Failed to parse protobuf geometry data");
    return nullptr;
  }

  // Validate that we have curve data
  if (!geom_data.has_curves()) {
    CLOG_ERROR(&LOG, "GeometryData does not contain curves");
    return nullptr;
  }

  const auto &curve_geom = geom_data.curves();

  // Validate data
  if (curve_geom.num_points() != curve_geom.positions_size()) {
    CLOG_ERROR(&LOG,
               "Curve geometry num_points (%d) does not match positions array size (%d)",
               curve_geom.num_points(),
               curve_geom.positions_size());
    return nullptr;
  }

  // Create new Curves data-block
  Curves *curves = bke::curves_new_nomain(curve_geom.num_points(), curve_geom.num_curves());
  bke::CurvesGeometry &curves_geom = curves->geometry.wrap();

  // Copy positions
  MutableSpan<float3> positions = curves_geom.positions_for_write();
  for (int i = 0; i < curve_geom.num_points(); i++) {
    const auto &pos = curve_geom.positions(i);
    positions[i] = float3(pos.x(), pos.y(), pos.z());
  }

  // Set curve offsets
  MutableSpan<int> offsets = curves_geom.offsets_for_write();
  for (int i = 0; i < curve_geom.num_curves(); i++) {
    offsets[i] = curve_geom.curve_offsets(i);
  }
  // Set final offset (total point count)
  offsets[curve_geom.num_curves()] = curve_geom.num_points();

  // Copy radii if present
  if (curve_geom.radii_size() == curve_geom.num_points()) {
    bke::MutableAttributeAccessor attributes = curves_geom.attributes_for_write();
    bke::SpanAttributeWriter<float> radii = attributes.lookup_or_add_for_write_span<float>(
        "radius", bke::AttrDomain::Point);
    for (int i = 0; i < curve_geom.num_points(); i++) {
      radii.span[i] = curve_geom.radii(i);
    }
    radii.finish();
  }

  // Copy tangents if present
  if (curve_geom.tangents_size() == curve_geom.num_points()) {
    bke::MutableAttributeAccessor attributes = curves_geom.attributes_for_write();
    bke::SpanAttributeWriter<float3> tangents = attributes.lookup_or_add_for_write_span<float3>(
        "tangent", bke::AttrDomain::Point);
    for (int i = 0; i < curve_geom.num_points(); i++) {
      const auto &tan = curve_geom.tangents(i);
      tangents.span[i] = float3(tan.x(), tan.y(), tan.z());
    }
    tangents.finish();
  }

  // Copy parameters if present
  if (curve_geom.parameters_size() == curve_geom.num_points()) {
    bke::MutableAttributeAccessor attributes = curves_geom.attributes_for_write();
    bke::SpanAttributeWriter<float> params = attributes.lookup_or_add_for_write_span<float>(
        "parameter", bke::AttrDomain::Point);
    for (int i = 0; i < curve_geom.num_points(); i++) {
      params.span[i] = curve_geom.parameters(i);
    }
    params.finish();
  }

  // Copy material indices if present (curve-domain attribute)
  if (curve_geom.material_indices_size() == curve_geom.num_curves()) {
    bke::MutableAttributeAccessor attributes = curves_geom.attributes_for_write();
    bke::SpanAttributeWriter<int> material_indices =
        attributes.lookup_or_add_for_write_span<int>("material_index", bke::AttrDomain::Curve);
    for (int i = 0; i < curve_geom.num_curves(); i++) {
      material_indices.span[i] = curve_geom.material_indices(i);
    }
    material_indices.finish();

    CLOG_INFO(&LOG,
              "Loaded material indices for %d curves (domain: Curve)",
              curve_geom.num_curves());
  }

  // Copy material profile indices if present (curve-domain attribute)
  if (curve_geom.material_profile_indices_size() == curve_geom.num_curves()) {
    bke::MutableAttributeAccessor attributes = curves_geom.attributes_for_write();
    bke::SpanAttributeWriter<int> material_profile_indices =
        attributes.lookup_or_add_for_write_span<int>("material_profile_index",
                                                      bke::AttrDomain::Curve);
    for (int i = 0; i < curve_geom.num_curves(); i++) {
      material_profile_indices.span[i] = curve_geom.material_profile_indices(i);
    }
    material_profile_indices.finish();

    CLOG_INFO(&LOG,
              "Loaded material profile indices for %d curves (domain: Curve)",
              curve_geom.num_curves());
  }

  // Copy per-curve colors if present (curve-domain attribute)
  // Format: 4 floats per curve [r, g, b, a] in Scene Linear color space (premultiplied alpha)
  // Matches Blender's ColorGeometry4f = ColorSceneLinear4f<eAlpha::Premultiplied>
  if (curve_geom.curve_colors_linear_rgba_size() == curve_geom.num_curves() * 4) {
    bke::MutableAttributeAccessor attributes = curves_geom.attributes_for_write();

    // Create curve-domain color attribute (like material_index)
    bke::SpanAttributeWriter<ColorGeometry4f> curve_colors =
        attributes.lookup_or_add_for_write_span<ColorGeometry4f>("curve_color",
                                                                  bke::AttrDomain::Curve);

    for (int i = 0; i < curve_geom.num_curves(); i++) {
      // Extract 4 floats for this curve (r, g, b, a)
      int base_idx = i * 4;
      float r = curve_geom.curve_colors_linear_rgba(base_idx + 0);
      float g = curve_geom.curve_colors_linear_rgba(base_idx + 1);
      float b = curve_geom.curve_colors_linear_rgba(base_idx + 2);
      float a = curve_geom.curve_colors_linear_rgba(base_idx + 3);

      // ColorGeometry4f is Scene Linear with premultiplied alpha - direct assignment
      curve_colors.span[i] = ColorGeometry4f(r, g, b, a);
    }

    curve_colors.finish();

    CLOG_INFO(&LOG,
              "Loaded per-curve colors for %d curves (domain: Curve, format: Scene Linear RGBA)",
              curve_geom.num_curves());
  }

  CLOG_INFO(&LOG,
            "Deserialized %d curves with %d points from protobuf",
            curve_geom.num_curves(),
            curve_geom.num_points());

  return curves;
}

// ============================================================================
// High-Level Shared Memory + Protobuf API
// ============================================================================

std::string create_shared_memory_from_curves(const Curves *curves, const std::string &session_id)
{
  if (curves == nullptr) {
    CLOG_ERROR(&LOG, "create_shared_memory_from_curves: curves is nullptr");
    return "";
  }

  // Serialize curves to protobuf
  std::vector<uint8_t> protobuf_data = curves_to_protobuf(curves);
  if (protobuf_data.empty()) {
    CLOG_ERROR(&LOG, "Failed to serialize curves to protobuf");
    return "";
  }

  // Generate unique memory ID
  std::string memory_id = generate_memory_id("geonodes_input", session_id);

  // Create shared memory with 8-byte header + protobuf data
  // Header format: uint64_t data_size (little-endian)
  const size_t header_size = sizeof(uint64_t);
  const size_t total_size = header_size + protobuf_data.size();

  SharedMemoryHandle handle = create_shared_memory(memory_id, total_size);
  if (handle.mapped_ptr == nullptr) {
    CLOG_ERROR(&LOG, "Failed to create shared memory");
    return "";
  }

  // Write 8-byte size header (little-endian uint64_t)
  uint64_t data_size = static_cast<uint64_t>(protobuf_data.size());
  memcpy(handle.mapped_ptr, &data_size, sizeof(uint64_t));

  // Write protobuf data after header
  memcpy(static_cast<uint8_t *>(handle.mapped_ptr) + header_size,
         protobuf_data.data(),
         protobuf_data.size());

  // Unmap but keep handle alive for external service to read
  // On Windows, closing the handle would destroy the shared memory
  unmap_shared_memory(handle, /* keep_alive = */ true);

  CLOG_INFO(&LOG,
            "Created shared memory '%s' with 8-byte header + %zu bytes of protobuf data (total: "
            "%zu bytes)",
            memory_id.c_str(),
            protobuf_data.size(),
            total_size);

  return memory_id;
}

Curves *read_curves_from_shared_memory(const std::string &memory_id)
{
  if (memory_id.empty()) {
    CLOG_ERROR(&LOG, "read_curves_from_shared_memory: memory_id is empty");
    return nullptr;
  }

  // Open shared memory
  SharedMemoryHandle handle = open_shared_memory(memory_id);
  if (handle.mapped_ptr == nullptr) {
    CLOG_ERROR(&LOG, "Failed to open shared memory '%s'", memory_id.c_str());
    return nullptr;
  }

  // Read 8-byte size header (little-endian uint64_t)
  const size_t header_size = sizeof(uint64_t);
  if (handle.size < header_size) {
    CLOG_ERROR(&LOG, "Shared memory '%s' too small (%zu bytes)", memory_id.c_str(), handle.size);
    unmap_shared_memory(handle, /* keep_alive = */ false);
    return nullptr;
  }

  uint64_t data_size = 0;
  memcpy(&data_size, handle.mapped_ptr, sizeof(uint64_t));

  // Validate size (allow Windows page-aligned sizes to be larger)
  if (data_size + header_size > handle.size) {
    CLOG_ERROR(&LOG,
               "Invalid size in shared memory '%s': header says %llu bytes, but total size is %zu "
               "bytes",
               memory_id.c_str(),
               static_cast<unsigned long long>(data_size),
               handle.size);
    unmap_shared_memory(handle, /* keep_alive = */ false);
    return nullptr;
  }

  if (data_size + header_size != handle.size) {
    CLOG_INFO(&LOG,
              "Shared memory '%s' is page-aligned: header says %llu bytes protobuf data, total "
              "size is %zu bytes (padding: %zu bytes)",
              memory_id.c_str(),
              static_cast<unsigned long long>(data_size),
              handle.size,
              handle.size - (data_size + header_size));
  }

  // Copy protobuf data using size from header (NOT total memory size)
  // This is important because Windows rounds shared memory to page boundaries
  const size_t protobuf_size = static_cast<size_t>(data_size);
  std::vector<uint8_t> protobuf_data(protobuf_size);
  memcpy(protobuf_data.data(),
         static_cast<uint8_t *>(handle.mapped_ptr) + header_size,
         protobuf_size);

  // Unmap and close handle (we're done reading)
  unmap_shared_memory(handle, /* keep_alive = */ false);

  // Deserialize protobuf to curves
  Curves *curves = protobuf_to_curves(protobuf_data);
  if (curves == nullptr) {
    CLOG_ERROR(&LOG, "Failed to deserialize curves from protobuf");
    return nullptr;
  }

  CLOG_INFO(&LOG, "Read curves from shared memory '%s'", memory_id.c_str());

  return curves;
}

void release_shared_memory(const std::string &memory_id, const std::string &service_url)
{
  if (memory_id.empty() || service_url.empty()) {
    CLOG_WARN(&LOG, "release_shared_memory: memory_id or service_url is empty");
    return;
  }

  // Build JSON request body
  std::string request_body = "{\"memory_id\":\"" + memory_id + "\"}";

  // Send HTTP POST request
  std::string endpoint = service_url + "/api/v1/geonodes/memory/release";

  try {
    http::HttpResponse response = http::http_post(endpoint, request_body, 5000);

    if (!response.success || response.status_code != 200) {
      CLOG_WARN(&LOG,
                "Failed to release shared memory '%s' via REST API: HTTP %d",
                memory_id.c_str(),
                response.status_code);
    }
    else {
      CLOG_INFO(&LOG, "Released shared memory '%s' via REST API", memory_id.c_str());
    }
  }
  catch (const std::exception &e) {
    CLOG_WARN(&LOG, "Exception releasing shared memory '%s': %s", memory_id.c_str(), e.what());
  }
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string generate_uuid()
{
  // Simple UUID v4 generation (random)
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 15);
  std::uniform_int_distribution<> dis2(8, 11);

  std::stringstream ss;
  ss << std::hex;

  for (int i = 0; i < 8; i++) {
    ss << dis(gen);
  }
  ss << "-";

  for (int i = 0; i < 4; i++) {
    ss << dis(gen);
  }
  ss << "-4";  // UUID version 4

  for (int i = 0; i < 3; i++) {
    ss << dis(gen);
  }
  ss << "-";

  ss << dis2(gen);  // UUID variant

  for (int i = 0; i < 3; i++) {
    ss << dis(gen);
  }
  ss << "-";

  for (int i = 0; i < 12; i++) {
    ss << dis(gen);
  }

  return ss.str();
}

std::string generate_memory_id(const std::string &prefix, const std::string &session_id)
{
  std::string uuid = generate_uuid();
  return prefix + "_" + session_id + "_" + uuid;
}

std::string create_shared_memory_descriptor_json(const std::string &memory_id, size_t size)
{
  std::stringstream ss;
  ss << "{"
     << "\"type\":\"shared_memory\","
     << "\"memory_id\":\"" << memory_id << "\","
     << "\"size\":" << size << "}";
  return ss.str();
}

}  // namespace blender::bke::protobuf
