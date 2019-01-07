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

#include "../StdAfx.h"

#pragma warning(disable : 4244)

#include "CIICommandsImpl.h"
#include "CIIProtocolStrategy.h"

#include <core/producer/cg_proxy.h>

#include <algorithm>
#include <sstream>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/locale.hpp>

namespace caspar { namespace protocol { namespace cii {

////////
// MediaCommand
void MediaCommand::Setup(const std::vector<std::wstring>& parameters) { graphicProfile_ = parameters[1].substr(2); }

void MediaCommand::Execute() { pCIIStrategy_->SetProfile(graphicProfile_); }

////////
// WriteCommand
void WriteCommand::Setup(const std::vector<std::wstring>& parameters)
{
    try {
        if (parameters.size() > 2) {
            targetName_   = parameters[1];
            templateName_ = parameters[2];

            std::wstringstream dataStream;

            dataStream << L"<templateData>";

            std::vector<std::wstring>::size_type end = parameters.size();
            for (std::vector<std::wstring>::size_type i = 3; i < end; ++i)
                dataStream << L"<componentData id=\"field" << i - 2 << L"\"><data id=\"text\" value=\"" << parameters[i]
                           << L"\" /></componentData>";

            dataStream << L"</templateData>";
            xmlData_ = dataStream.str();
        }
    } catch (std::exception&) {
    }
}

void WriteCommand::Execute() { pCIIStrategy_->WriteTemplateData(templateName_, targetName_, xmlData_); }

//////////
// ImagestoreCommand
void ImagestoreCommand::Setup(const std::vector<std::wstring>& parameters)
{
    if (parameters[1] == L"7" && parameters.size() > 2)
        titleName_ = parameters[2].substr(0, 4);
}

void ImagestoreCommand::Execute() { pCIIStrategy_->DisplayTemplate(titleName_); }

//////////
// MiscellaneousCommand
void MiscellaneousCommand::Setup(const std::vector<std::wstring>& parameters)
{
    // HAWRYS:	V\5\3\1\1\namn.tga\1
    //			Display still
    if ((parameters.size() > 5) && parameters[1] == L"5" && parameters[2] == L"3") {
        filename_ = parameters[5];
        filename_ = filename_.substr(0, filename_.find_last_of(L'.'));
        filename_.append(L".ft");
        state_ = 0;
        return;
    }

    // NEPTUNE:	V\5\13\1\X\Template\0\TabField1\TabField2...
    //			Add Template to layer X in the active templatehost
    if ((parameters.size() > 5) && parameters[1] == L"5" && parameters[2] == L"13") {
        layer_    = boost::lexical_cast<int>(parameters[4]);
        filename_ = parameters[5];
        if (filename_.find(L"PK/") == std::wstring::npos && filename_.find(L"PK\\") == std::wstring::npos)
            filename_ = L"PK/" + filename_;

        state_ = 1;
        if (parameters.size() > 7) {
            std::wstringstream dataStream;

            dataStream << L"<templateData>";
            std::vector<std::wstring>::size_type end = parameters.size();
            for (std::vector<std::wstring>::size_type i = 7; i < end; ++i) {
                dataStream << L"<componentData id=\"f" << i - 7 << L"\"><data id=\"text\" value=\"" << parameters[i]
                           << L"\" /></componentData>";
            }
            dataStream << L"</templateData>";

            xmlData_ = dataStream.str();
        }
    }

    // VIDEO MODE V\5\14\MODE
    if ((parameters.size() > 3) && parameters[1] == L"5" && parameters[2] == L"14") {
        std::wstring value = parameters[3];
        boost::to_upper(value);

        this->pCIIStrategy_->GetChannel()->video_format_desc(core::video_format_desc(value));
    }
}

void MiscellaneousCommand::Execute()
{
    if (state_ == 0)
        pCIIStrategy_->DisplayMediaFile(filename_);

    // TODO: Need to be checked for validity
    else if (state_ == 1) {
        // HACK fix. The data sent is UTF8, however the protocol is implemented for ISO-8859-1. Instead of doing risky
        // changes we simply convert into proper encoding when leaving protocol code.
        auto xmlData2 = boost::locale::conv::utf_to_utf<wchar_t, char>(std::string(xmlData_.begin(), xmlData_.end()));
        auto proxy    = pCIIStrategy_->get_cg_registry()->get_or_create_proxy(
            pCIIStrategy_->GetChannel(), pCIIStrategy_->get_dependencies(), core::cg_proxy::DEFAULT_LAYER, filename_);
        proxy->add(layer_, filename_, false, L"", xmlData2);
    }
}

/////////
// KeydataCommand
void KeydataCommand::Execute()
{
    auto proxy = pCIIStrategy_->get_cg_registry()->get_proxy(pCIIStrategy_->GetChannel(), casparLayer_);

    if (state_ == 0)
        pCIIStrategy_->DisplayTemplate(titleName_);

    // TODO: Need to be checked for validity
    else if (state_ == 1)
        proxy->stop(layer_, 0);
    else if (state_ == 2)
        pCIIStrategy_->GetChannel()->stage()->clear();
    else if (state_ == 3)
        proxy->play(layer_);
}

void KeydataCommand::Setup(const std::vector<std::wstring>& parameters)
{
    // HAWRYS:	Y\<205><247><202><196><192><192><200><248>
    // parameter[1] looks like this: "=g:XXXXh" where XXXX is the name that we want
    if (parameters[1].size() > 6) {
        titleName_.resize(4);
        for (int i = 0; i < 4; ++i) {
            if (parameters[1][i + 3] < 176) {
                titleName_.clear();
                break;
            }
            titleName_[i] = parameters[1][i + 3] - 144;
        }
        state_ = 0;
    }

    casparLayer_ = core::cg_proxy::DEFAULT_LAYER;
    if (parameters.size() > 2) {
        // The layer parameter now supports casparlayers.
        // the format is [CasparLayer]-[FlashLayer]
        std::wstring              str = boost::trim_copy(parameters[2]);
        std::vector<std::wstring> split;
        boost::split(split, str, boost::is_any_of("-"));

        try {
            casparLayer_ = boost::lexical_cast<int>(split[0]);

            if (split.size() > 1)
                layer_ = boost::lexical_cast<int>(split[1]);
        } catch (...) {
            casparLayer_ = core::cg_proxy::DEFAULT_LAYER;
            layer_       = 0;
        }
    }

    if (parameters[1].at(0) == 27) // NEPTUNE:	Y\<27>\X			Stop layer X.
        state_ = 1;
    else if (static_cast<unsigned char>(parameters[1].at(1)) == 190) // NEPTUNE:	Y\<254>
                                                                     // Clear Canvas.
        state_ = 2;
    else if (static_cast<unsigned char>(parameters[1].at(1)) == 149) // NEPTUNE:	Y\<213><243>\X	Play layer X.
        state_ = 3; // UPDATE 2011-05-09: These char-codes are aparently not valid after converting to wide-chars
                    // the correct sequence is <195><149><195><179>
}

}}} // namespace caspar::protocol::cii
