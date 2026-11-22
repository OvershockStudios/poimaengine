// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

namespace poima::world_schema::detail {
using Json = nlohmann::json;

inline Json object_schema(Json properties, Json required = Json::array()) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}

inline Json vector(Json item, int size) {
    return {{"type", "array"}, {"items", item}, {"minItems", size}, {"maxItems", size}};
}

void add_asset_method_schemas(Json& discovery, const Json& entity_id,
                              const Json& revision, const Json& asset_id,
                              const Json& model_node, const Json& number);
}
