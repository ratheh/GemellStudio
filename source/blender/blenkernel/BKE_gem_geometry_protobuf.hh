// SPDX-FileCopyrightText: 2025 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/** \file
 * \ingroup bke
 * \brief Protocol Buffers serialization for geometry data transfer to external services
 *
 * This module provides functionality to serialize/deserialize Blender geometry data
 * to/from Protocol Buffer format for use with external geometry node services.
 * Uses shared memory for efficient zero-copy data transfer.
 *
 * **Architecture**:
 * 1. Blender converts Curves → Protobuf bytes → Shared memory
 * 2. External service maps shared memory → Parses Protobuf → Processes geometry
 * 3. External service generates output → Serializes to new shared memory
 * 4. Blender maps output shared memory → Parses Protobuf → Reconstructs Curves
 *
 * **Protobuf Schema**: gemell/geometry/v1/*.proto (MIT-licensed, dual-licensing)
 */

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Curves;

namespace blender::bke::protobuf {

// Forward declare Windows HANDLE type (avoid including windows.h in header)
#ifdef _WIN32
using HANDLE_TYPE = void *;
#else
using HANDLE_TYPE = int;
#endif

// ============================================================================
// Shared Memory Management
// ============================================================================

/**
 * Shared memory handle for cross-process communication
 *
 * Platform-specific shared memory implementation:
 * - Windows: CreateFileMapping() / MapViewOfFile()
 * - Linux/macOS: shm_open() / mmap()
 */
struct SharedMemoryHandle {
  /** Unique memory region identifier (e.g., "geonodes_input_uuid_12345") */
  std::string memory_id;

  /** Mapped memory pointer (read/write access) */
  void *mapped_ptr = nullptr;

  /** Size of the memory region in bytes */
  size_t size = 0;

#ifdef _WIN32
  /** Windows file mapping handle */
  HANDLE_TYPE handle = nullptr;
#else
  /** Unix file descriptor for shared memory */
  HANDLE_TYPE fd = -1;
#endif

  /** Whether this handle owns the shared memory (for cleanup) */
  bool is_owner = false;
};

/**
 * Create a new shared memory region with the given name and size.
 *
 * \param name: Unique identifier for the shared memory region
 * \param size: Size in bytes of the memory region to allocate
 * \return Handle to the created shared memory, or handle with nullptr on failure
 *
 * **Windows**: Creates a file mapping in the page file
 * **Unix**: Creates a POSIX shared memory object in /dev/shm/
 */
SharedMemoryHandle create_shared_memory(const std::string &name, size_t size);

/**
 * Open an existing shared memory region by name (read-only or read-write).
 *
 * \param name: Identifier of the shared memory region to open
 * \return Handle to the opened shared memory, or handle with nullptr on failure
 */
SharedMemoryHandle open_shared_memory(const std::string &name);

/**
 * Unmap and release a shared memory region.
 *
 * \param handle: Handle to the shared memory to release
 * \param keep_alive: If true (default false), keeps the handle alive for other processes (Windows).
 *
 * **Windows**: Closing the handle destroys the shared memory. Use keep_alive=true for IPC.
 * **POSIX**: The fd is always closed; memory persists until shm_unlink() is called.
 */
void unmap_shared_memory(SharedMemoryHandle &handle, bool keep_alive = false);

/**
 * Destroy a shared memory region (removes from system).
 *
 * \param name: Identifier of the shared memory region to destroy
 *
 * **Warning**: Only call this after all processes have unmapped the memory.
 */
void destroy_shared_memory(const std::string &name);

// ============================================================================
// Geometry Serialization (Blender → Protobuf)
// ============================================================================

/**
 * Convert Blender Curves geometry to Protocol Buffer format.
 *
 * Serializes curve data (positions, radii, tangents, parameters, offsets) into
 * a gemell::geometry::v1::GeometryData protobuf message.
 *
 * \param curves: Blender curves data structure
 * \return Serialized protobuf bytes, empty vector on failure
 *
 * **Protobuf Structure**:
 * - GeometryData.curves.positions (Vec3f array)
 * - GeometryData.curves.radii (float array, optional)
 * - GeometryData.curves.tangents (Vec3f array, optional)
 * - GeometryData.curves.parameters (float array, optional)
 * - GeometryData.curves.curve_offsets (int32 array)
 * - GeometryData.curves.num_curves (int32)
 * - GeometryData.curves.num_points (int32)
 */
std::vector<uint8_t> curves_to_protobuf(const Curves *curves);

/**
 * Convert Protocol Buffer format to Blender Curves geometry.
 *
 * Deserializes a gemell::geometry::v1::GeometryData protobuf message and reconstructs
 * Blender Curves data structure.
 *
 * \param protobuf_data: Serialized protobuf bytes
 * \return Newly allocated Blender Curves object, or nullptr on failure
 *
 * **Caller is responsible for freeing the returned Curves object**
 */
Curves *protobuf_to_curves(const std::vector<uint8_t> &protobuf_data);

// ============================================================================
// High-Level Shared Memory + Protobuf API
// ============================================================================

/**
 * Create shared memory with serialized curve data.
 *
 * Convenience function that:
 * 1. Serializes curves to protobuf bytes
 * 2. Creates shared memory region
 * 3. Writes protobuf bytes to shared memory
 *
 * \param curves: Blender curves to serialize
 * \param session_id: Service session ID for memory ID generation
 * \return Unique memory ID string, or empty string on failure
 *
 * **Memory ID Format**: "geonodes_input_{session_id}_{uuid}"
 */
std::string create_shared_memory_from_curves(const Curves *curves,
                                              const std::string &session_id);

/**
 * Read curve data from shared memory.
 *
 * Convenience function that:
 * 1. Opens shared memory by ID
 * 2. Reads protobuf bytes from memory
 * 3. Deserializes to Blender Curves
 * 4. Unmaps shared memory
 *
 * \param memory_id: Shared memory identifier
 * \return Newly allocated Blender Curves object, or nullptr on failure
 *
 * **Caller is responsible for freeing the returned Curves object**
 */
Curves *read_curves_from_shared_memory(const std::string &memory_id);

/**
 * Release shared memory region via external service REST API.
 *
 * Sends a POST request to the external service to signal that the shared memory
 * can be released. The service will call destroy_shared_memory() on its side.
 *
 * \param memory_id: Shared memory identifier to release
 * \param service_url: Base URL of the external service API
 *
 * **REST Endpoint**: POST {service_url}/api/v1/geonodes/memory/release
 * **Request Body**: {"memory_id": "..."}
 */
void release_shared_memory(const std::string &memory_id, const std::string &service_url);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Generate a unique memory ID with session prefix.
 *
 * \param prefix: Prefix for the memory ID (e.g., "geonodes_input")
 * \param session_id: Service session ID
 * \return Unique memory ID string
 *
 * **Format**: "{prefix}_{session_id}_{uuid}"
 */
std::string generate_memory_id(const std::string &prefix, const std::string &session_id);

/**
 * Generate a UUID v4 string.
 *
 * \return UUID string in format "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
 */
std::string generate_uuid();

/**
 * Create a JSON descriptor for shared memory reference.
 *
 * Helper to create JSON object for REST API requests:
 * ```json
 * {
 *   "type": "shared_memory",
 *   "memory_id": "...",
 *   "size": 12345
 * }
 * ```
 *
 * \param memory_id: Shared memory identifier
 * \param size: Size of the shared memory region
 * \return JSON object as string
 */
std::string create_shared_memory_descriptor_json(const std::string &memory_id, size_t size);

}  // namespace blender::bke::protobuf
