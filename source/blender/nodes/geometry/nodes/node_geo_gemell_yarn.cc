/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"

#include "BLI_math_vector_types.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_gemell_yarn_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Vector>("Offset").default_value({0, 0, 1});
  b.add_output<decl::Geometry>("Curves").propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  /* Get inputs from sockets. */
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  const float3 offset = params.extract_input<float3>("Offset");

  /* Process curves if available. */
  if (Curves *curves = geometry_set.get_curves_for_write()) {
    bke::CurvesGeometry &curves_geom = curves->geometry.wrap();
    MutableSpan<float3> positions = curves_geom.positions_for_write();
    for (const int i : positions.index_range()) {
      positions[i] += offset;
    }
    curves_geom.tag_positions_changed();
  }

  /* Set output socket. */
  params.set_output("Curves", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGemellYarn");
  ntype.ui_name = "Curve to Yarn";
  ntype.ui_description = "Gemell curve to yarn generator";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_gemell_yarn_cc