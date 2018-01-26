/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../../StdAfx.h"

#include "buffer.h"

#include "texture.h"
#include "device.h"

#include <common/except.h>
#include <common/gl/gl_check.h>

#include <GL/glew.h>

#include <boost/asio/deadline_timer.hpp>

namespace caspar { namespace accelerator { namespace ogl {

struct buffer::impl : boost::noncopyable
{
	GLuint     id_;
	GLsizei    size_;
    void*      data_;
    GLsync     fence_ = 0;
    bool       write_;
    GLenum     target_;
    GLbitfield flags_;

public:
	impl(int size, bool write)
		: size_(size)
        , write_(write)
        , target_(!write ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER)
		, flags_(GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_READ_BIT | (write ? GL_MAP_WRITE_BIT : 0))
	{
		GL(glCreateBuffers(1, &id_));
        GL(glNamedBufferStorage(id_, size_, nullptr, flags_));
        data_ = GL2(glMapNamedBufferRange(id_, 0, size_, flags_));
	}

	~impl()
	{
        GL(glUnmapNamedBuffer(id_));
		glDeleteBuffers(1, &id_);
        if (fence_) {
            glDeleteSync(fence_);
        }
	}

    void lock()
    {
        fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    bool try_wait()
    {
        if (!fence_) {
            return true;
        }

        auto wait = glClientWaitSync(fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 1);
        if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
            glDeleteSync(fence_);
            fence_ = 0;
            return true;
        }
        return false;
    }

    void wait(boost::asio::io_service& context)
    {
        for (auto n = 0; true; n = std::min(10, n + 1)) {
            context.dispatch([&]
            {
                try_wait();
            });

            if (fence_) {
                break;
            }

            boost::asio::deadline_timer timer(context, boost::posix_time::milliseconds(n));
            timer.wait();
        }
    }

    void bind()
    {
        GL(glBindBuffer(target_, id_));
    }

    void unbind()
    {
        GL(glBindBuffer(target_, 0));
    }
};

buffer::buffer(int size, bool write) : impl_(new impl(size, write)){}
buffer::buffer(buffer&& other) : impl_(std::move(other.impl_)){}
buffer::~buffer(){}
buffer& buffer::operator=(buffer&& other){impl_ = std::move(other.impl_); return *this;}
void* buffer::data(){return impl_->data_;}
bool buffer::write() const { return impl_->write_;  }
int buffer::size() const { return impl_->size_; }
bool buffer::try_wait() { return impl_->try_wait(); }
void buffer::wait(boost::asio::io_service& context) { return impl_->wait(context); }
void buffer::lock() { return impl_->lock(); }
void buffer::bind() { return impl_->bind(); }
void buffer::unbind() { return impl_->unbind(); }
int buffer::id() const {return impl_->id_;}

}}}
