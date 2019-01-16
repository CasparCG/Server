#pragma once

#include "../util/lock_container.h"
#include <common/memory.h>
#include <core/producer/stage.h>
#include <core/video_channel.h>
#include <utility>

namespace caspar { namespace protocol { namespace amcp {

class channel_context
{
  public:
    explicit channel_context() {}
    explicit channel_context(std::shared_ptr<core::video_channel> c,
                             std::shared_ptr<core::stage_base>    s,
                             const std::wstring&                  lifecycle_key)
        : raw_channel(std::move(c))
        , stage(std::move(s))
        , lock(std::make_shared<caspar::IO::lock_container>(lifecycle_key))
        , lifecycle_key_(lifecycle_key)
    {
    }
    const std::shared_ptr<core::video_channel>        raw_channel;
    const std::shared_ptr<core::stage_base>           stage;
    const std::shared_ptr<caspar::IO::lock_container> lock;
    const std::wstring                                lifecycle_key_;
};

struct command_context_simple
{
    const IO::ClientInfoPtr         client;
    const int                       channel_index;
    const int                       layer_id;
    const std::vector<std::wstring> parameters;

    int layer_index(int default_ = 0) const { return layer_id == -1 ? default_ : layer_id; }

    command_context_simple(IO::ClientInfoPtr                client,
                           int                              channel_index,
                           int                              layer_id,
                           const std::vector<std::wstring>& parameters)
        : client(std::move(client))
        , channel_index(channel_index)
        , layer_id(layer_id)
        , parameters(parameters)
    {
    }
};

typedef std::function<std::future<std::wstring>(const command_context_simple&       args,
                                                const std::vector<channel_context>& channels)>
    amcp_command_func;

}}} // namespace caspar::protocol::amcp