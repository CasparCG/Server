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
 * Author: Nicklas P Andersson
 */

#pragma once

#include <iostream>
#include <memory>
#include <string>

#include "protocol_strategy.h"
#include <common/log.h>

namespace caspar { namespace IO {

typedef spl::shared_ptr<client_connection<wchar_t>> ClientInfoPtr;
typedef std::shared_ptr<client_connection<wchar_t>> ClientInfoPtrStd;

struct ConsoleClientInfo : public client_connection<wchar_t>
{
    void send(std::wstring&& data, bool skip_log) override
    {
        std::wcout << (L"#" + caspar::log::replace_nonprintable_copy(data, L'?')) << std::flush;
    }
    void         disconnect() override {}
    std::wstring address() const override { return L"Console"; }
    void add_lifecycle_bound_object(const std::wstring& key, const std::shared_ptr<void>& lifecycle_bound) override {}
    std::shared_ptr<void> remove_lifecycle_bound_object(const std::wstring& key) override
    {
        return std::shared_ptr<void>();
    }
};

}} // namespace caspar::IO
