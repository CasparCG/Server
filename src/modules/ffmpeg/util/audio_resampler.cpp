#include "audio_resampler.h"
#include "av_assert.h"

extern "C" {
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace caspar::ffmpeg {

AudioResampler::AudioResampler(int sample_rate, AVSampleFormat in_sample_fmt)
{
    AVChannelLayout channel_layout     = AV_CHANNEL_LAYOUT_7POINT1;
    AVChannelLayout channel_layout_out = AV_CHANNEL_LAYOUT_HEXADECAGONAL;

    SwrContext* raw_ctx = nullptr;
    FF(swr_alloc_set_opts2(&raw_ctx,
                           &channel_layout_out,
                           AV_SAMPLE_FMT_S32,
                           sample_rate,
                           &channel_layout,
                           in_sample_fmt,
                           sample_rate,
                           0,
                           nullptr));

    ctx = std::shared_ptr<SwrContext>(raw_ctx, [](SwrContext* ptr) { swr_free(&ptr); });

    FF_RET(swr_init(ctx.get()), "swr_init");
}

caspar::array<int32_t> AudioResampler::convert(int frames, const void** src)
{
    // Built from a std::vector so size() reports the element count, not a byte count (as array<T>'s
    // raw-size constructor would) - matching every other array<int32_t> audio buffer in the codebase.
    std::vector<int32_t> result(static_cast<std::size_t>(frames) * 16, 0);
    auto                 ptr = reinterpret_cast<uint8_t*>(result.data());
    const auto samples_out   = swr_convert(ctx.get(), &ptr, frames, reinterpret_cast<const uint8_t**>(src), frames);
    FF_RET(samples_out, "swr_convert");
    result.resize(static_cast<std::size_t>(samples_out) * 16);

    return result;
}

}; // namespace caspar::ffmpeg
