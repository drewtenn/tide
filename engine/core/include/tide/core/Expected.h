#pragma once

#include <expected>
#include <utility>

namespace tide {

template <class T, class E> using expected = std::expected<T, E>;

template <class E> using unexpected = std::unexpected<E>;

} // namespace tide
