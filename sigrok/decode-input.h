#ifndef DSVIEW_DECODE_INPUT_H
#define DSVIEW_DECODE_INPUT_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Convert DSView's channel bit planes to the official libsigrokdecode sample ABI.
// Null planes represent constant channels; negative mappings are absent channels.
inline std::vector<uint8_t> dsview_interleave(const uint8_t *const *planes,
    const uint8_t *constants, const unsigned *offsets, const int *mapping,
    size_t channels, size_t samples, size_t unitsize)
{
    std::vector<uint8_t> result(samples * unitsize, 0);
    for (size_t sample = 0; sample < samples; sample++) {
        for (size_t j = 0; j < channels; j++) {
            const int channel = mapping[j];
            if (channel < 0) continue;
            const size_t bit = sample + offsets[j];
            const bool high = planes[j] ? ((planes[j][bit / 8] >> (bit % 8)) & 1) : constants[j];
            if (high) result[sample * unitsize + channel / 8] |= 1u << (channel % 8);
        }
    }
    return result;
}
#endif
