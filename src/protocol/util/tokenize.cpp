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
 */

#include "tokenize.h"

namespace caspar { namespace IO {

std::size_t tokenize(const std::wstring& message, std::list<std::wstring>& pTokenVector)
{
    // split on whitespace but keep strings within quotationmarks
    // treat \ as the start of an escape-sequence: the following char will indicate what to actually put in the
    // string

    std::wstring currentToken;

    bool inQuote        = false;
    bool getSpecialCode = false;

    for (unsigned int charIndex = 0; charIndex < message.size(); ++charIndex) {
        if (getSpecialCode) {
            // insert code-handling here
            switch (message[charIndex]) {
                case L'\\':
                    currentToken += L"\\";
                    break;
                case L'\"':
                    currentToken += L"\"";
                    break;
                case L'n':
                    currentToken += L"\n";
                    break;
                default:
                    break;
            };
            getSpecialCode = false;
            continue;
        }

        if (message[charIndex] == L'\\') {
            getSpecialCode = true;
            continue;
        }

        if (message[charIndex] == L' ' && inQuote == false) {
            if (!currentToken.empty()) {
                pTokenVector.push_back(currentToken);
                currentToken.clear();
            }
            continue;
        }

        if (message[charIndex] == L'\"') {
            inQuote = !inQuote;

            if (!currentToken.empty() || !inQuote) {
                pTokenVector.push_back(currentToken);
                currentToken.clear();
            }
            continue;
        }

        currentToken += message[charIndex];
    }

    if (!currentToken.empty()) {
        pTokenVector.push_back(currentToken);
        currentToken.clear();
    }

    return pTokenVector.size();
}

}} // namespace caspar::IO