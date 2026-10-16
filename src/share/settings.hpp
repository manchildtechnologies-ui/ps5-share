// PS5 Share Hub settings: persistent Discord webhook URL.
// Ported from the legacy SDL/ImGui PS5 Share app (settings.cpp). Logic is
// verbatim: read from /download0/PS5Share (same PPSA99992 sandbox, so the
// user's saved webhook carries over), fall back to packaged/payload files.
#pragma once

#include <string>

namespace hui {
namespace share {

struct ShareSettings {
    std::string webhook_url;
};

bool is_allowed_webhook_url(const std::string& url);
bool load_settings(ShareSettings& settings);
bool save_settings(const ShareSettings& settings);

}  // namespace share
}  // namespace hui
