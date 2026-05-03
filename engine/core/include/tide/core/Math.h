#pragma once

// Engine-wide math typedefs over GLM. Coordinate convention follows ADR-0002.
// Public headers should prefer these aliases over raw glm:: types.

#define GLM_FORCE_DEPTH_ZERO_TO_ONE // reversed-Z friendly, ADR-0010 forward-design
#define GLM_FORCE_RIGHT_HANDED      // ADR-0002

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace tide {

using vec2 = glm::vec2;
using vec3 = glm::vec3;
using vec4 = glm::vec4;

using ivec2 = glm::ivec2;
using ivec3 = glm::ivec3;
using ivec4 = glm::ivec4;

using uvec2 = glm::uvec2;
using uvec3 = glm::uvec3;
using uvec4 = glm::uvec4;

using mat3 = glm::mat3;
using mat4 = glm::mat4;
using quat = glm::quat;

} // namespace tide
