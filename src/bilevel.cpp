#include "bilevel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dpdfcomp
{
namespace
{
struct Pixel {
    std::array<unsigned, 3> values{}; // After any reversed Decode mapping.
    bool hidden = false;
};

Pixel read_pixel(const Raster& source, const BilevelInput& input, int x, int y)
{
    const unsigned maximum = (1U << source.bits) - 1;
    Pixel pixel;
    pixel.hidden = !input.color_key.empty();
    for (int c = 0; c < source.colors; ++c) {
        auto value = sample(source, x, y, c);
        if (!input.color_key.empty())
            pixel.hidden &= value >= input.color_key[2 * c] && value <= input.color_key[2 * c + 1];
        pixel.values[c] = input.reversed[c] ? maximum - value : value;
    }
    if (input.alpha) {
        const auto alpha = sample(*input.alpha, x, y, 0);
        pixel.hidden = input.alpha_reversed ? alpha == (1U << input.alpha->bits) - 1 : alpha == 0;
    }
    return pixel;
}

// Rec. 601 luma on decoded device samples, where white is 1000 times the maximum sample.
std::int64_t luma(const Pixel& pixel, int colors)
{
    return colors == 1 ? 1000 * std::int64_t(pixel.values[0])
                       : 299 * std::int64_t(pixel.values[0]) + 587 * std::int64_t(pixel.values[1]) +
                             114 * std::int64_t(pixel.values[2]);
}

int scaled(int length, int scale)
{
    const auto result = std::max<std::int64_t>(1, (std::int64_t(length) * scale + 50) / 100);
    if (result > std::numeric_limits<int>::max())
        throw std::runtime_error("scaled bilevel image dimensions overflow");
    return static_cast<int>(result);
}

struct Tap {
    int index;
    double weight; // An exact integer, relative to the other taps of the same sample.
};

// Source taps for each of m output samples along an axis of n source samples:
// linear interpolation between pixel centers when enlarging, exact area
// coverage when reducing. Either way the output spans the same extent.
std::vector<std::vector<Tap>> axis_taps(std::int64_t n, std::int64_t m)
{
    std::vector<std::vector<Tap>> result(m);
    for (std::int64_t o = 0; o < m; ++o) {
        auto& taps = result[o];
        if (m >= n) {
            // The output center lies (numerator / denominator) source pixels past
            // the center of source pixel 0; beyond the outer centers, edges extend.
            const auto numerator = (2 * o + 1) * n - m, denominator = 2 * m;
            const auto first = numerator < 0 ? -1 : numerator / denominator;
            const auto fraction = numerator - first * denominator;
            taps.push_back({int(std::max<std::int64_t>(first, 0)), double(denominator - fraction)});
            if (fraction)
                taps.push_back({int(std::min(first + 1, n - 1)), double(fraction)});
        } else {
            // Output sample o covers [o n, (o + 1) n) in units of 1/m source pixel.
            for (auto i = o * n / m; i * m < (o + 1) * n; ++i)
                taps.push_back(
                    {int(i), double(std::min((o + 1) * n, (i + 1) * m) - std::max(o * n, i * m))});
        }
    }
    return result;
}

// The source pixel containing the center of output sample o.
int nearest(std::int64_t o, std::int64_t n, std::int64_t m)
{
    return static_cast<int>((2 * o + 1) * n / (2 * m));
}

// Calls emit(x, y, hidden, sum, weight) for each output pixel. Hidden comes from
// the source pixel under the output center, as the original mask renders; sum
// and weight accumulate weighted values over visible source pixels only. That
// center pixel is always a tap, so a visible output pixel has visible weight.
template <class Value, class Emit>
void resample(const Raster& source, const BilevelInput& input, int width, int height,
              Value value, Emit emit)
{
    const auto columns = axis_taps(source.width, width), rows = axis_taps(source.height, height);
    std::vector<int> centers(width);
    for (int x = 0; x < width; ++x)
        centers[x] = nearest(x, source.width, width);
    std::vector<double> sums(source.width), weights(source.width);
    std::vector<char> hidden(source.width);
    for (int y = 0; y < height; ++y) {
        std::fill(sums.begin(), sums.end(), 0.0);
        std::fill(weights.begin(), weights.end(), 0.0);
        const auto center = nearest(y, source.height, height);
        for (const auto& tap : rows[y])
            for (int x = 0; x < source.width; ++x) {
                const auto pixel = read_pixel(source, input, x, tap.index);
                if (tap.index == center)
                    hidden[x] = pixel.hidden;
                if (!pixel.hidden) {
                    sums[x] += tap.weight * value(pixel);
                    weights[x] += tap.weight;
                }
            }
        for (int x = 0; x < width; ++x) {
            double sum = 0, weight = 0;
            for (const auto& tap : columns[x]) {
                sum += tap.weight * sums[tap.index];
                weight += tap.weight * weights[tap.index];
            }
            emit(x, y, hidden[centers[x]] != 0, sum, weight);
        }
    }
}

// Savitzky-Golay weights for a window of `size` samples fitted by least squares
// with a polynomial of `degree`, evaluated at each window position. Off-center
// positions serve the edges, where the window cannot extend past the image.
std::vector<std::vector<double>> savitzky_golay(int size, int degree)
{
    const int terms = degree + 1;
    const double half = (size - 1) / 2.0;
    auto basis = [&](int position) {
        // Coordinates normalized to [-1, 1] keep the normal equations well conditioned.
        const double x = half ? (position - half) / half : 0;
        std::vector<double> powers(terms);
        double power = 1;
        for (auto& value : powers) {
            value = power;
            power *= x;
        }
        return powers;
    };
    std::vector<double> gram(std::size_t(terms) * terms);
    for (int j = 0; j < size; ++j) {
        const auto powers = basis(j);
        for (int a = 0; a < terms; ++a)
            for (int b = 0; b < terms; ++b)
                gram[a * terms + b] += powers[a] * powers[b];
    }
    std::vector<std::vector<double>> result(size, std::vector<double>(size));
    for (int position = 0; position < size; ++position) {
        // The weight of sample j is basis(j) . z, where gram z = basis(position).
        auto matrix = gram;
        auto z = basis(position);
        for (int column = 0; column < terms; ++column) {
            int pivot = column;
            for (int row = column + 1; row < terms; ++row)
                if (std::abs(matrix[row * terms + column]) > std::abs(matrix[pivot * terms + column]))
                    pivot = row;
            for (int k = 0; k < terms; ++k)
                std::swap(matrix[column * terms + k], matrix[pivot * terms + k]);
            std::swap(z[column], z[pivot]);
            for (int row = column + 1; row < terms; ++row) {
                const auto factor = matrix[row * terms + column] / matrix[column * terms + column];
                for (int k = column; k < terms; ++k)
                    matrix[row * terms + k] -= factor * matrix[column * terms + k];
                z[row] -= factor * z[column];
            }
        }
        for (int row = terms - 1; row >= 0; --row) {
            for (int k = row + 1; k < terms; ++k)
                z[row] -= matrix[row * terms + k] * z[k];
            z[row] /= matrix[row * terms + row];
        }
        for (int j = 0; j < size; ++j) {
            const auto powers = basis(j);
            for (int a = 0; a < terms; ++a)
                result[position][j] += powers[a] * z[a];
        }
    }
    return result;
}

// Two-dimensional Savitzky-Golay smoothing with a square window and a
// tensor-product polynomial basis, which separates into row and column passes.
// Rows are smoothed into a ring buffer just ahead of the columns, in place.
void smooth(std::vector<std::uint8_t>& gray, int width, int height, int window, int degree)
{
    auto fit = [&](int length) { return std::min(window, length % 2 ? length : length - 1); };
    const int across = fit(width), down = fit(height);
    const auto horizontal = savitzky_golay(across, std::min(degree, across - 1));
    const auto vertical = savitzky_golay(down, std::min(degree, down - 1));
    std::vector<std::vector<double>> ring(down, std::vector<double>(width));
    int filtered = 0;
    for (int y = 0; y < height; ++y) {
        const int top = std::clamp(y - down / 2, 0, height - down);
        for (; filtered < top + down; ++filtered) {
            const auto* row = gray.data() + std::size_t(filtered) * width;
            auto& output = ring[filtered % down];
            for (int x = 0; x < width; ++x) {
                const int left = std::clamp(x - across / 2, 0, width - across);
                const auto& weights = horizontal[x - left];
                double value = 0;
                for (int j = 0; j < across; ++j)
                    value += weights[j] * row[left + j];
                output[x] = value;
            }
        }
        const auto& weights = vertical[y - top];
        auto* row = gray.data() + std::size_t(y) * width;
        for (int x = 0; x < width; ++x) {
            double value = 0;
            for (int j = 0; j < down; ++j)
                value += weights[j] * ring[(top + j) % down][x];
            row[x] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
        }
    }
}

// Window and degree by output resolution, in the bands ScanTailor uses. PDF
// placement is not consulted: sources are assumed to be ordinary 300 ppi scans.
std::pair<int, int> smoothing(int scale)
{
    const int resolution = 3 * scale;
    if (resolution <= 200)
        return {5, 3};
    if (resolution <= 400)
        return {7, 4};
    if (resolution <= 800)
        return {11, 4};
    return {11, 2};
}

// Otsu's method: the largest gray level still black that maximizes between-class
// variance, or nothing when every sample falls into a single class.
std::optional<int> otsu(const std::array<std::uint64_t, 256>& histogram)
{
    double count = 0, total = 0;
    for (int level = 0; level < 256; ++level) {
        count += double(histogram[level]);
        total += double(level) * double(histogram[level]);
    }
    double black = 0, black_total = 0, best = 0;
    std::optional<int> result;
    for (int level = 0; level < 255; ++level) {
        black += double(histogram[level]);
        black_total += double(level) * double(histogram[level]);
        const auto white = count - black;
        if (!black || !white)
            continue;
        const auto difference = black_total / black - (total - black_total) / white;
        const auto variance = black * white * difference * difference;
        if (!result || variance > best) {
            best = variance;
            result = level;
        }
    }
    return result;
}
} // namespace

std::optional<BilevelResult> convert_bilevel(const Raster& source, const BilevelInput& input,
                                             std::optional<ColorEncoding> conversion)
{
    if (!input.colors)
        return std::nullopt;
    const int scale = conversion ? conversion->scale : 100;
    BilevelResult result{
        Raster{scaled(source.width, scale), scaled(source.height, scale), 1, 1, {}, {}}, {}, {}};
    auto& raster = result.raster;
    if (raster.row_bytes() > std::numeric_limits<std::size_t>::max() / raster.height)
        throw std::runtime_error("scaled bilevel image dimensions overflow");
    raster.bytes.resize(raster.row_bytes() * raster.height);
    const unsigned maximum = (1U << source.bits) - 1;
    std::string mask(input.color_key.empty() ? 0 : raster.bytes.size(), '\0');
    std::array<bool, 2> transparent{}, opaque{};
    auto put = [&](int x, int y, bool hidden, bool white) {
        const auto pos = std::size_t(y) * raster.row_bytes() + x / 8;
        const auto bit = static_cast<char>(128 >> (x % 8));
        if (white)
            raster.bytes[pos] |= bit;
        if (!input.color_key.empty()) {
            (hidden ? transparent : opaque)[white] = true;
            if (hidden)
                mask[pos] |= bit;
        }
    };
    if (!conversion) {
        for (int y = 0; y < source.height; ++y)
            for (int x = 0; x < source.width; ++x) {
                const auto pixel = read_pixel(source, input, x, y);
                bool white = false;
                if (!pixel.hidden) {
                    white = pixel.values[0] == maximum;
                    for (int c = 0; c < source.colors; ++c)
                        if (pixel.values[c] != (white ? maximum : 0))
                            return std::nullopt;
                }
                put(x, y, pixel.hidden, white);
            }
    } else if (!conversion->smooth) {
        // White at or above the chosen percentage of maximum luma. Summing signed
        // differences keeps uniform areas exact: no rounding can flip their sign.
        const auto level = 10 * std::int64_t(maximum) * conversion->parameter;
        resample(
            source, input, raster.width, raster.height,
            [&](const Pixel& pixel) { return double(luma(pixel, source.colors) - level); },
            [&](int x, int y, bool hidden, double sum, double) {
                put(x, y, hidden, !hidden && sum >= 0);
            });
    } else {
        // Scale, then smooth and choose the threshold at the output resolution.
        std::vector<std::uint8_t> gray(std::size_t(raster.width) * raster.height);
        std::vector<bool> hidden(input.color_key.empty() && !input.alpha ? 0 : gray.size());
        const double white = 1000.0 * maximum;
        resample(
            source, input, raster.width, raster.height,
            [&](const Pixel& pixel) { return double(luma(pixel, source.colors)); },
            [&](int x, int y, bool masked, double sum, double weight) {
                const auto i = std::size_t(y) * raster.width + x;
                if (!hidden.empty())
                    hidden[i] = masked;
                // Fully hidden neighborhoods have no color; white keeps them from darkening edges.
                const auto value = std::lround(255 * (weight ? sum / weight : white) / white);
                gray[i] = static_cast<std::uint8_t>(std::clamp(value, 0L, 255L));
            });
        const auto [window, degree] = smoothing(scale);
        smooth(gray, raster.width, raster.height, window, degree);
        std::array<std::uint64_t, 256> histogram{};
        for (std::size_t i = 0; i < gray.size(); ++i)
            if (hidden.empty() || !hidden[i])
                ++histogram[gray[i]];
        const auto black = otsu(histogram).value_or(127);
        for (int y = 0; y < raster.height; ++y)
            for (int x = 0; x < raster.width; ++x) {
                const auto i = std::size_t(y) * raster.width + x;
                const bool masked = !hidden.empty() && hidden[i];
                put(x, y, masked, !masked && gray[i] > black);
            }
    }
    if (!input.color_key.empty()) {
        if ((transparent[0] && opaque[0]) || (transparent[1] && opaque[1]))
            result.mask = std::move(mask);
        else if (transparent[0] || transparent[1])
            result.color_key = std::array<int, 2>{transparent[0] ? 0 : 1, transparent[1] ? 1 : 0};
        // No matching source samples: remove the color key rather than
        // accidentally making a newly introduced black or white value invisible.
    }
    return result;
}
} // namespace dpdfcomp
