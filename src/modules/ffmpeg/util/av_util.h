#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <memory>

enum AVPixelFormat;
struct AVFrame;
struct AVPacket;

namespace caspar { namespace ffmpeg {

std::shared_ptr<AVFrame>  alloc_frame();
std::shared_ptr<AVPacket> alloc_packet();

core::pixel_format      get_pixel_format(AVPixelFormat pix_fmt);
core::pixel_format_desc pixel_format_desc(AVPixelFormat pix_fmt, int width, int height);
core::mutable_frame     make_frame(void*                    tag,
                                   core::frame_factory&     frame_factory,
                                   std::shared_ptr<AVFrame> video,
                                   std::shared_ptr<AVFrame> audio);

std::shared_ptr<AVFrame> make_av_video_frame(const core::const_frame& frame, const core::video_format_desc& format_des);
std::shared_ptr<AVFrame> make_av_audio_frame(const core::const_frame& frame, const core::video_format_desc& format_des);

int graph_execute(struct AVFilterContext *ctx, avfilter_action_func *func, void *arg, int *ret, int count);
int codec_execute(struct AVCodecContext *c, int(*func)(struct AVCodecContext *c2, void *arg), void *arg2, int *ret, int count, int size);
int codec_execute2(struct AVCodecContext *c, int(*func)(struct AVCodecContext *c2, void *arg, int jobnr, int threadnr), void *arg2, int *ret, int coun);
}} // namespace caspar::ffmpeg