#include "audio_mixer.h"

#include <algorithm>
#include <cmath>

namespace recmeet {

std::vector<int16_t> mix_audio(const std::vector<int16_t>& a,
                                const std::vector<int16_t>& b) {
    size_t len = std::max(a.size(), b.size());
    std::vector<int16_t> out(len);

    for (size_t i = 0; i < len; ++i) {
        int32_t sa = (i < a.size()) ? a[i] : 0;
        int32_t sb = (i < b.size()) ? b[i] : 0;
        // Average the two streams, clamp to int16 range
        int32_t mixed = (sa + sb) / 2;
        out[i] = static_cast<int16_t>(std::clamp(mixed, (int32_t)-32768, (int32_t)32767));
    }
    return out;
}

} // namespace recmeet
