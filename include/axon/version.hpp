#pragma once

#include <string_view>

namespace axon {

// Project version, e.g. "0.1.0". Set from the CMake project() version.
std::string_view version() noexcept;

}  // namespace axon
