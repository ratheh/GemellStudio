# Gemell Geometry Protocol (MIT License)

## Overview

This directory contains **MIT-licensed** Protocol Buffer definitions for geometry data interchange between Blender (GemellStudio) and external geometry node services.

## Licensing

**These .proto files are licensed under the MIT License**, separate from the GPL v3 license of GemellStudio. This dual-licensing approach enables:

1. **GemellStudio (GPL v3)** - Compiles these .proto files, uses generated C++ code in GPL-licensed binaries
2. **External Services (Proprietary)** - Can compile the same .proto files without GPL contamination

## Legal Basis

- **GPL projects CAN contain files with different licenses** (precedent: Linux kernel dual-licensed files)
- **.proto files are interface definitions**, not implementation code
- **Generated code from `protoc` inherits the license of the .proto file** (MIT in this case)
- Both GPL (Blender) and proprietary (Warp, FabricDataLoaderAPI) code can compile and use the same schema

## Architecture

### Shared Memory + Protobuf

The Gemell geometry node system uses **shared memory** for geometry data transfer:

1. **Blender** serializes geometry to protobuf bytes, writes to shared memory
2. **REST API** passes shared memory ID to external service
3. **External Service** (Warp) maps shared memory, parses protobuf using `ParseFromArray()`
4. **External Service** generates output, serializes to new shared memory
5. **Blender** maps output shared memory, parses protobuf, reconstructs geometry

**Key Point**: While not truly "zero-copy" (protobuf must parse/serialize), this architecture eliminates intermediate buffers and network copies. Same overhead as Apache Arrow would have.

## Schema Files

### gemell/geometry/v1/

- **geometry.proto** - Top-level `GeometryData` wrapper (extensible oneof for curve/mesh/point types)
- **curves.proto** - `CurveGeometry` message (positions, offsets, radii, tangents, parameters)
- **attributes.proto** - `AttributeData` for generic custom fields (float/int/vec3/string arrays)
- **materials.proto** - `MaterialProfile` message for rendering material data

## Usage in GemellStudio (Blender)

### CMake Configuration

```cmake
# In source/blender/blenkernel/CMakeLists.txt
find_package(Protobuf REQUIRED)

set(PROTO_FILES
  proto/gemell/geometry/v1/geometry.proto
  proto/gemell/geometry/v1/curves.proto
  proto/gemell/geometry/v1/attributes.proto
  proto/gemell/geometry/v1/materials.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

set(SRC
  # Existing sources...
  intern/gem_geometry_protobuf.cc
  ${PROTO_SRCS}  # Generated protobuf C++ code
)

set(INC
  ${Protobuf_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}  # For generated .pb.h files
)

set(LIB
  ${Protobuf_LIBRARIES}
)
```

### C++ Code Example

```cpp
#include "gemell/geometry/v1/geometry.pb.h"

// Serialize Blender curves to protobuf
gemell::geometry::v1::GeometryData geom_data;
auto* curves = geom_data.mutable_curves();

for (const auto& pos : blender_positions) {
  auto* vec3 = curves->add_positions();
  vec3->set_x(pos.x);
  vec3->set_y(pos.y);
  vec3->set_z(pos.z);
}

// Write to shared memory
size_t size = geom_data.ByteSizeLong();
void* shmem_ptr = create_shared_memory("geonodes_input_uuid", size);
geom_data.SerializeToArray(shmem_ptr, size);

// ... later, read from shared memory
gemell::geometry::v1::GeometryData output_geom;
output_geom.ParseFromArray(output_shmem_ptr, output_size);
```

## Usage in Warp (Proprietary)

Warp can reference the same .proto files from GemellStudio or copy them to its own project.

### Option A: Reference GemellStudio Proto Files

```cmake
# In Fabric/libs/warp/CMakeLists.txt
set(GEMELL_PROTO_DIR "D:/bdev/GemellStudio/source/blender/blenkernel/proto")

set(PROTO_FILES
  ${GEMELL_PROTO_DIR}/gemell/geometry/v1/geometry.proto
  ${GEMELL_PROTO_DIR}/gemell/geometry/v1/curves.proto
  ${GEMELL_PROTO_DIR}/gemell/geometry/v1/attributes.proto
  ${GEMELL_PROTO_DIR}/gemell/geometry/v1/materials.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
```

### Option B: Copy Proto Files to Warp Project

```bash
# Copy .proto files to Warp
cp -r D:/bdev/GemellStudio/source/blender/blenkernel/proto/gemell \
      D:/dev4/addons/Fabric/libs/warp/proto/

# Update CMakeLists.txt to use local copy
```

### C++ Code Example (Warp)

```cpp
#include "gemell/geometry/v1/geometry.pb.h"

// Warp C API function
wpStatus warp_thread_generate_filaments_from_shmem(
    CWarpSession *session,
    const char *input_memory_id,
    char **output_curves_memory_id,
    char **error_message)
{
  // 1. Map input shared memory
  void* input_ptr = map_shared_memory(input_memory_id);
  size_t input_size = get_shared_memory_size(input_memory_id);

  // 2. Parse protobuf from shared memory
  gemell::geometry::v1::GeometryData input_geom;
  if (!input_geom.ParseFromArray(input_ptr, input_size)) {
    *error_message = strdup("Failed to parse input geometry");
    return wpStatus::Fail;
  }

  // 3. Extract curve data
  const auto& input_curves = input_geom.curves();
  // ... process input ...

  // 4. Generate output
  gemell::geometry::v1::GeometryData output_geom;
  auto* output_curves = output_geom.mutable_curves();
  // ... populate output ...

  // 5. Serialize to output shared memory
  std::string out_shmem_id = generate_memory_id("warp_output_curves");
  size_t output_size = output_geom.ByteSizeLong();
  void* out_ptr = create_shared_memory(out_shmem_id, output_size);
  output_geom.SerializeToArray(out_ptr, output_size);

  *output_curves_memory_id = strdup(out_shmem_id.c_str());
  return wpStatus::Ok;
}
```

## Schema Evolution

### Versioning Strategy

- Package name includes version: `gemell.geometry.v1`
- `GeometryData.version` field for runtime version checks
- Field numbers reserved for future additions

### Best Practices

1. **Never remove fields** - Mark as deprecated instead
2. **Use field numbers 1-19 for frequent fields** (1-byte encoding)
3. **Reserve field numbers** for planned features
4. **Increment version on breaking changes**

Example:
```protobuf
message CurveGeometry {
  reserved 100 to 199;  // Reserved for future use

  repeated Vec3f positions = 1;  // Frequent field (1-byte encoding)
  repeated float radii = 2;      // Frequent field

  // NEW in v1.1 (optional, backward compatible)
  repeated Vec3f normals = 20;

  // DEPRECATED in v1.2 (keep for backward compat)
  int32 legacy_field = 50 [deprecated = true];
}
```

## Related Documentation

- [External Geometry Node System Design](../../../../Design-External-Geometry-Node-System.md)
- [GemellStudio Implementation Guide](../../../../Design-GemellStudio-Geometry-Nodes.md)
- [FabricDataLoaderAPI Implementation Guide](../../../../Design-FabricDataLoaderAPI-Geometry-Nodes.md)

## License

See [LICENSE_MIT.txt](LICENSE_MIT.txt) for the full MIT License text.
