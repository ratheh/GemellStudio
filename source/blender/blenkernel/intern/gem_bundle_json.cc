/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Bundle JSON Conversion Implementation
 *
 * Converts between Blender Bundle data structures and JSON (via BLI_serialize).
 * Used by external geometry nodes for bundle input/output serialization.
 */

#include "BKE_gem_bundle_json.hh"

#include "BKE_node.hh"
#include "BKE_node_socket_value.hh"

#include "BLI_serialize.hh"
#include "BLI_string_ref.hh"

#include "NOD_geometry_nodes_bundle.hh"

#include "DNA_node_types.h"

#include <iostream>
#include <sstream>

namespace blender::bke::bundle {

using namespace blender::io::serialize;
using namespace blender::nodes;

// ============================================================================
// JSON → Bundle Conversion
// ============================================================================

/**
 * Convert a JSON value to BundleItemValue.
 * Handles all JSON types: null, boolean, integer, float, string, array, object.
 */
std::optional<BundleItemValue> json_value_to_bundle_item(const Value &value,
                                                          const std::string &path)
{
  switch (value.type()) {
    case eValueType::Null: {
      /* Bundle doesn't have a null type, use int 0 as convention */
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_INT);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find INT socket type for null value at: " << path
                  << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(int(0));
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    case eValueType::Boolean: {
      const BooleanValue *bool_val = value.as_boolean_value();
      if (!bool_val) {
        return std::nullopt;
      }
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_BOOLEAN);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find BOOLEAN socket type at: " << path << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(bool_val->value());
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    case eValueType::Int: {
      const IntValue *int_val = value.as_int_value();
      if (!int_val) {
        return std::nullopt;
      }
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_INT);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find INT socket type at: " << path << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(int(int_val->value()));
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    case eValueType::Double: {
      const DoubleValue *double_val = value.as_double_value();
      if (!double_val) {
        return std::nullopt;
      }
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_FLOAT);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find FLOAT socket type at: " << path << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(float(double_val->value()));
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    case eValueType::String: {
      const StringValue *str_val = value.as_string_value();
      if (!str_val) {
        return std::nullopt;
      }
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_STRING);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find STRING socket type at: " << path << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(std::string(str_val->value()));
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    case eValueType::Array: {
      /* Convert array to nested bundle with indexed keys "0", "1", "2", ... */
      const ArrayValue *array_val = value.as_array_value();
      if (!array_val) {
        return std::nullopt;
      }

      BundlePtr array_bundle = Bundle::create();
      if (!array_bundle) {
        std::cerr << "[ERROR] Failed to create bundle for array at: " << path << std::endl;
        return std::nullopt;
      }

      /* Get mutable reference to bundle */
      Bundle &mutable_array_bundle = const_cast<Bundle &>(*array_bundle);

      int index = 0;
      for (const auto &element : array_val->elements()) {
        const std::string element_path = path + "[" + std::to_string(index) + "]";
        std::optional<BundleItemValue> item = json_value_to_bundle_item(*element, element_path);
        if (item.has_value()) {
          mutable_array_bundle.add_override(std::to_string(index), item.value());
        }
        index++;
      }

      /* Wrap nested bundle as BundleItemValue */
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_BUNDLE);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find BUNDLE socket type for array at: " << path
                  << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(array_bundle);
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    case eValueType::Dictionary: {
      /* Convert object to nested bundle (recursive) */
      const DictionaryValue *dict_val = value.as_dictionary_value();
      if (!dict_val) {
        return std::nullopt;
      }

      BundlePtr nested_bundle = json_dict_to_bundle(*dict_val);
      if (!nested_bundle) {
        std::cerr << "[ERROR] Failed to convert nested dictionary at: " << path << std::endl;
        return std::nullopt;
      }

      /* Wrap nested bundle as BundleItemValue */
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_BUNDLE);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find BUNDLE socket type for dictionary at: " << path
                  << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(nested_bundle);
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    case eValueType::Enum: {
      /* Treat enum as integer */
      const EnumValue *enum_val = value.as_enum_value();
      if (!enum_val) {
        return std::nullopt;
      }
      const bNodeSocketType *socket_type = node_socket_type_find_static(SOCK_INT);
      if (!socket_type) {
        std::cerr << "[WARN] Failed to find INT socket type for enum at: " << path << std::endl;
        return std::nullopt;
      }
      SocketValueVariant value_variant = SocketValueVariant::From(int(enum_val->value()));
      return BundleItemValue{BundleItemSocketValue{socket_type, value_variant}};
    }

    default:
      std::cerr << "[WARN] Unsupported JSON value type at: " << path << std::endl;
      return std::nullopt;
  }
}

/**
 * Convert JSON dictionary to Blender Bundle.
 * Main entry point for bundle output conversion.
 */
BundlePtr json_dict_to_bundle(const DictionaryValue &json_dict)
{
  BundlePtr bundle = Bundle::create();
  if (!bundle) {
    std::cerr << "[ERROR] Failed to create bundle" << std::endl;
    return nullptr;
  }

  /* Get mutable reference to bundle */
  Bundle &mutable_bundle = const_cast<Bundle &>(*bundle);

  /* Iterate all key-value pairs in JSON dictionary */
  for (const auto &item : json_dict.elements()) {
    const std::string &key = item.first;
    const std::shared_ptr<Value> &value = item.second;

    if (!value) {
      std::cerr << "[WARN] Null value for key: " << key << std::endl;
      continue;
    }

    /* Convert JSON value to BundleItemValue */
    std::optional<BundleItemValue> bundle_item = json_value_to_bundle_item(*value, key);
    if (bundle_item.has_value()) {
      mutable_bundle.add_override(key, bundle_item.value());
    }
    else {
      std::cerr << "[WARN] Failed to convert value for key: " << key << std::endl;
    }
  }

  return bundle;
}

// ============================================================================
// Bundle → JSON Conversion
// ============================================================================

/**
 * Check if bundle represents a JSON array.
 * Arrays have sequential integer keys: "0", "1", "2", ...
 */
bool bundle_is_array(const BundlePtr &bundle)
{
  if (!bundle || bundle->is_empty()) {
    return false;
  }

  const auto items = bundle->items();
  for (int i = 0; i < items.size(); i++) {
    const std::string expected_key = std::to_string(i);
    if (items[i].key != expected_key) {
      return false;
    }
  }

  return true;
}

/**
 * Convert BundleItemValue to JSON Value.
 * Handles all bundle item types including nested bundles.
 */
static std::shared_ptr<Value> bundle_item_to_json_value(const BundleItemValue &item,
                                                         const std::string &path)
{
  /* Try to get as socket value */
  const BundleItemSocketValue *socket_value = std::get_if<BundleItemSocketValue>(&item.value);
  if (!socket_value) {
    /* Internal value type - not supported for JSON conversion */
    std::cerr << "[WARN] Cannot convert internal bundle value to JSON at: " << path << std::endl;
    return std::make_shared<NullValue>();
  }

  /* Check socket type and convert appropriately */
  const int socket_type = socket_value->type->type;

  switch (socket_type) {
    case SOCK_BOOLEAN: {
      std::optional<bool> bool_val = item.as<bool>();
      if (bool_val.has_value()) {
        return std::make_shared<BooleanValue>(bool_val.value());
      }
      break;
    }

    case SOCK_INT: {
      std::optional<int> int_val = item.as<int>();
      if (int_val.has_value()) {
        return std::make_shared<IntValue>(int_val.value());
      }
      break;
    }

    case SOCK_FLOAT: {
      std::optional<float> float_val = item.as<float>();
      if (float_val.has_value()) {
        return std::make_shared<DoubleValue>(double(float_val.value()));
      }
      break;
    }

    case SOCK_STRING: {
      std::optional<std::string> str_val = item.as<std::string>();
      if (str_val.has_value()) {
        return std::make_shared<StringValue>(str_val.value());
      }
      break;
    }

    case SOCK_BUNDLE: {
      /* Nested bundle - recursively convert */
      std::optional<BundlePtr> nested_bundle = item.as<BundlePtr>();
      if (nested_bundle.has_value() && nested_bundle.value()) {
        /* Check if this bundle represents an array */
        if (bundle_is_array(nested_bundle.value())) {
          /* Convert to JSON array */
          auto array = std::make_shared<ArrayValue>();
          for (const auto &bundle_item : nested_bundle.value()->items()) {
            const std::string item_path = path + "[" + bundle_item.key + "]";
            std::shared_ptr<Value> json_val = bundle_item_to_json_value(bundle_item.value,
                                                                         item_path);
            if (json_val) {
              array->append(json_val);
            }
          }
          return array;
        }
        else {
          /* Convert to JSON object */
          return bundle_to_json_dict(nested_bundle.value());
        }
      }
      break;
    }

    default:
      std::cerr << "[WARN] Unsupported socket type " << socket_type << " at: " << path
                << std::endl;
      break;
  }

  /* Fallback to null */
  return std::make_shared<NullValue>();
}

/**
 * Convert Blender Bundle to JSON dictionary.
 * Main entry point for bundle input conversion.
 */
std::shared_ptr<DictionaryValue> bundle_to_json_dict(const BundlePtr &bundle)
{
  auto json_dict = std::make_shared<DictionaryValue>();

  if (!bundle || bundle->is_empty()) {
    return json_dict;
  }

  /* Iterate all items in bundle */
  for (const auto &bundle_item : bundle->items()) {
    const std::string &key = bundle_item.key;
    const BundleItemValue &value = bundle_item.value;

    /* Convert bundle item to JSON value */
    std::shared_ptr<Value> json_val = bundle_item_to_json_value(value, key);
    if (json_val) {
      json_dict->append(key, json_val);
    }
    else {
      std::cerr << "[WARN] Failed to convert bundle item for key: " << key << std::endl;
    }
  }

  return json_dict;
}

}  // namespace blender::bke::bundle
