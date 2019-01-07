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

#include "CIICommandsImpl.h"
#include "CIIProtocolStrategy.h"
#include <algorithm>
#include <common/env.h>
#include <core/diagnostics/call_context.h>
#include <core/mixer/mixer.h>
#include <core/producer/transition/transition_producer.h>
#include <sstream>
#include <string>

#include <boost/algorithm/string/replace.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 1) // TODO: Legacy code, just disable warnings
#endif

namespace caspar { namespace protocol { namespace cii {

using namespace core;

const std::wstring CIIProtocolStrategy::MessageDelimiter = L"\r\n";
const wchar_t      CIIProtocolStrategy::TokenDelimiter   = L'\\';

CIIProtocolStrategy::CIIProtocolStrategy(const std::vector<spl::shared_ptr<core::video_channel>>&    channels,
                                         const spl::shared_ptr<core::cg_producer_registry>&          cg_registry,
                                         const spl::shared_ptr<const core::frame_producer_registry>& producer_registry)
    : executor_(L"CIIProtocolStrategy")
    , pChannel_(channels.at(0))
    , channels_(channels)
    , cg_registry_(cg_registry)
    , producer_registry_(producer_registry)
{
}

// The paser method expects message to be complete messages with the delimiter stripped away.
// Thesefore the AMCPProtocolStrategy should be decorated with a delimiter_based_chunking_strategy
void CIIProtocolStrategy::Parse(const std::wstring& message, IO::ClientInfoPtr client)
{
    if (message.length() > 0) {
        ProcessMessage(message, client);
        client->send(std::wstring(L"*\r\n"));
    }
}

void CIIProtocolStrategy::ProcessMessage(const std::wstring& message, IO::ClientInfoPtr pClientInfo)
{
    CASPAR_LOG(info) << L"Received message from " << pClientInfo->address() << ": " << message << L"\\r\\n";

    std::vector<std::wstring> tokens;
    int                       tokenCount = TokenizeMessage(message, &tokens);

    CIICommandPtr pCommand = Create(tokens[0]);
    if ((pCommand != 0) && (tokenCount - 1) >= pCommand->GetMinimumParameters()) {
        pCommand->Setup(tokens);
        executor_.begin_invoke([=] { pCommand->Execute(); });
    } else {
    } // report error
}

int CIIProtocolStrategy::TokenizeMessage(const std::wstring& message, std::vector<std::wstring>* pTokenVector)
{
    std::wstringstream currentToken;

    for (unsigned int charIndex = 0; charIndex < message.size(); ++charIndex) {
        if (message[charIndex] == TokenDelimiter) {
            pTokenVector->push_back(currentToken.str());
            currentToken.str(L"");
            continue;
        }

        if (message[charIndex] == L'\"')
            currentToken << L"&quot;";
        else if (message[charIndex] == L'<')
            currentToken << L"&lt;";
        else if (message[charIndex] == L'>')
            currentToken << L"&gt;";
        else
            currentToken << message[charIndex];
    }

    if (currentToken.str().size() > 0)
        pTokenVector->push_back(currentToken.str());

    return (int)pTokenVector->size();
}

/************
// Examples (<X> = ASCIICHAR X)

I\25\3\VII\\									s�tter outputtype till 'vii'
I\25\4\1\\										enablar framebuffer (ignore
this)

M\C/SVTNEWS\\									pekar ut vilken grafisk profil som
skall anv�ndas

W\4009\4067\Jonas Bj�rkman\\					Skriver "Jonas Bj�rkman" till f�rsta textf�ltet i
template 4067 och sparar den f�rdiga skylten som 4009

T\7\4009.VII\A\\								l�gger ut skylt 4009

Y\<205><247><202><196><192><192><200><248>\\	l�gger ut skylten 4008 (<205><247><202><196><192><192><200><248> =
"=g:4008h" om man drar bort 144 fr�n varje asciiv�rde)

V\5\3\1\1\namn.tga\1\\							l�gger ut bilden namn.tga
V\0\1\D\C\10\0\0\0\\							g�r n�gon inst�llning som har med f�reg�ende
kommando att g�ra.

*************/

/**********************
New Commands to support the Netupe automation system
V\5\13\1\1\Template\0\TabField1\TabField2...\\		Build. Ettan f�re Template indikerar vilket lager den nya
templaten skall laddas in i. OBS. Skall inte
visas efter det h�r steget Y\<27>\\
Stop. H�r kommer ett lagerID ocks�
att skickas med (<27> = ESC) Y\<254>\\ Clear Canvas. H�r kommer ett lagerID ocks� att skickas med, utan det skall allt
t�mmas Y\<213><243>\\ Play. H�r kommer ett lagerID ocks� att skickas med

**********************/
CIICommandPtr CIIProtocolStrategy::Create(const std::wstring& name)
{
    switch (name[0]) {
        case L'M':
            return std::make_shared<MediaCommand>(this);
        case L'W':
            return std::make_shared<WriteCommand>(this);
        case L'T':
            return std::make_shared<ImagestoreCommand>(this);
        case L'V':
            return std::make_shared<MiscellaneousCommand>(this);
        case L'Y':
            return std::make_shared<KeydataCommand>(this);
        default:
            return nullptr;
    }
}

void CIIProtocolStrategy::WriteTemplateData(const std::wstring& templateName,
                                            const std::wstring& titleName,
                                            const std::wstring& xmlData)
{
    std::wstring fullTemplateFilename = templateName;

    if (!currentProfile_.empty())
        fullTemplateFilename = currentProfile_ + L"/" + templateName;

    core::diagnostics::scoped_call_context save;
    core::diagnostics::call_context::for_thread().video_channel = 1;
    core::diagnostics::call_context::for_thread().layer         = 0;
    auto producer = cg_registry_->create_producer(get_dependencies(), fullTemplateFilename);

    if (producer == core::frame_producer::empty()) {
        CASPAR_LOG(error) << "Failed to save instance of " << templateName << L" as " << titleName << L", template "
                          << fullTemplateFilename << L"not found";
        return;
    }

    cg_registry_->get_proxy(producer)->add(1, fullTemplateFilename, true, L"", xmlData);

    CASPAR_LOG(info) << "Saved an instance of " << templateName << L" as " << titleName;

    PutPreparedTemplate(titleName, spl::shared_ptr<core::frame_producer>(std::move(producer)));
}

void CIIProtocolStrategy::DisplayTemplate(const std::wstring& titleName)
{
    try {
        pChannel_->stage()->load(0, GetPreparedTemplate(titleName));
        pChannel_->stage()->play(0);

        CASPAR_LOG(info) << L"Displayed title " << titleName;
    } catch (caspar_exception&) {
        CASPAR_LOG(error) << L"Failed to display title " << titleName;
    }
}

void CIIProtocolStrategy::DisplayMediaFile(const std::wstring& filename)
{
    transition_info transition;
    transition.type     = transition_type::mix;
    transition.duration = 12;

    core::diagnostics::scoped_call_context save;
    core::diagnostics::call_context::for_thread().video_channel = 1;
    core::diagnostics::call_context::for_thread().layer         = 0;

    auto pFP         = get_producer_registry()->create_producer(get_dependencies(), filename);
    auto pTransition = create_transition_producer(pFP, transition);

    try {
        pChannel_->stage()->load(0, pTransition);
    } catch (...) {
        CASPAR_LOG_CURRENT_EXCEPTION();
        CASPAR_LOG(error) << L"Failed to display " << filename;
        return;
    }

    pChannel_->stage()->play(0);

    CASPAR_LOG(info) << L"Displayed " << filename;
}

spl::shared_ptr<core::frame_producer> CIIProtocolStrategy::GetPreparedTemplate(const std::wstring& titleName)
{
    spl::shared_ptr<core::frame_producer> result(frame_producer::empty());

    TitleList::iterator it = std::find(titles_.begin(), titles_.end(), titleName);
    if (it != titles_.end()) {
        CASPAR_LOG(info) << L"Found title with name " << it->titleName;
        result = (*it).pframe_producer;
    } else
        CASPAR_LOG(error) << L"Could not find title with name " << titleName;

    return result;
}

void CIIProtocolStrategy::PutPreparedTemplate(const std::wstring&                          titleName,
                                              const spl::shared_ptr<core::frame_producer>& pFP)
{
    CASPAR_LOG(info) << L"Saved title with name " << titleName;

    TitleList::iterator it = std::find(titles_.begin(), titles_.end(), titleName);
    if (it != titles_.end()) {
        titles_.remove((*it));
    }

    titles_.push_front(TitleHolder(titleName, pFP));

    if (titles_.size() >= 6)
        titles_.resize(5);
}

bool operator==(const CIIProtocolStrategy::TitleHolder& lhs, const std::wstring& rhs) { return lhs.titleName == rhs; }

bool operator==(const std::wstring& lhs, const CIIProtocolStrategy::TitleHolder& rhs) { return lhs == rhs.titleName; }

}}} // namespace caspar::protocol::cii
