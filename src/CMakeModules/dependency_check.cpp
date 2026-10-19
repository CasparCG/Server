#include <boost/version.hpp>
#include <span>

extern "C" {
#include <libavcodec/avcodec.h>
}

static_assert(BOOST_VERSION >= 108300);

int main()
{
    int values[] = {1, 2};
    return std::span(values).size() == 2 && avcodec_version() != 0 ? 0 : 1;
}
