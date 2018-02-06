#include "accelerator.h"

#include "cpu/image/image_mixer.h"
#include "ogl/image/image_mixer.h"
#include "ogl/util/device.h"

#include <boost/property_tree/ptree.hpp>

#include <common/env.h>

#include <core/mixer/image/image_mixer.h>

#include <mutex>

namespace caspar { namespace accelerator {

struct accelerator::impl
{
    const std::wstring           path_;
    std::mutex                   mutex_;
    std::shared_ptr<ogl::device> ogl_device_;

    impl(const std::wstring& path)
        : path_(path)
    {
    }

    std::unique_ptr<core::image_mixer> create_image_mixer(int channel_id)
    {
        try {
            if (path_ == L"gpu" || path_ == L"ogl" || path_ == L"auto" || path_ == L"default") {
                std::lock_guard<std::mutex> lock(mutex_);

                if (!ogl_device_) {
                    ogl_device_.reset(new ogl::device());
                }

                return std::make_unique<ogl::image_mixer>(spl::make_shared_ptr(ogl_device_), channel_id);
            }
        } catch (...) {
            if (path_ == L"gpu" || path_ == L"ogl") {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }
        return std::make_unique<cpu::image_mixer>(channel_id);
    }
};

accelerator::accelerator(const std::wstring& path)
    : impl_(std::make_unique<impl>(path))
{
}

accelerator::~accelerator() {}

std::unique_ptr<core::image_mixer> accelerator::create_image_mixer(int channel_id)
{
    return impl_->create_image_mixer(channel_id);
}

}} // namespace caspar::accelerator
