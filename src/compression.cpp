#include "compression.hpp"

#include <zlib.h>

#include <limits>
#include <stdexcept>

namespace dpdfcomp
{
std::string deflate(std::string_view bytes)
{
    if (bytes.size() > std::numeric_limits<uLong>::max())
        throw std::runtime_error("stream is too large for zlib");
    uLongf size = compressBound(static_cast<uLong>(bytes.size()));
    std::string result(size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(result.data()), &size,
                  reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uLong>(bytes.size()),
                  Z_BEST_COMPRESSION) != Z_OK)
        throw std::runtime_error("zlib compression failed");
    result.resize(size);
    return result;
}
} // namespace dpdfcomp
