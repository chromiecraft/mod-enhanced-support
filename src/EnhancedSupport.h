/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOD_ENHANCED_SUPPORT_H
#define MOD_ENHANCED_SUPPORT_H

#include "Define.h"

#include <string>
#include <string_view>
#include <vector>

// Shared module API. State lives in mod_enhanced_support.cpp; the command
// script (cs_enhanced_support.cpp) reaches it through these functions.
namespace EnhancedSupport
{
    void LoadConfig();
    void LoadKeywords();

    // Canonical (trimmed + lowercased) form used for storage and matching.
    std::string NormalizeKeyword(std::string_view input);

    std::vector<std::string> const& GetKeywords();
    bool HasKeyword(std::string const& normalized);
    void AddKeyword(std::string const& normalized);
    void RemoveKeyword(std::string const& normalized);

    // Current active settings (for the .support info command).
    bool IsEnabled();
    uint8 GetMailFilterAction();
    std::string_view GetMailFilterActionName();
    std::string const& GetMailFilterMessage();
    std::string const& GetBanAuthor();

    // Money threshold (in copper) at/above which mail is logged; 0 disables the check.
    uint32 GetGoldFilterThreshold();

    // Formats a copper amount as a "Xg Ys Zc" string.
    std::string FormatMoney(uint32 copper);

    // Highest valid mail filter action value.
    uint8 GetMaxMailFilterAction();

    // Runtime-only override of the mail filter action; reverts to config on reload.
    void SetMailFilterAction(uint8 action);
}

#endif // MOD_ENHANCED_SUPPORT_H
