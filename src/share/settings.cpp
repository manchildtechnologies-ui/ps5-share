// PS5 Share Hub settings: persistent Discord webhook URL.
// The JSON shape and URL validation are ported from the legacy SDL/ImGui
// PS5 Share app. The file access deliberately goes through the app's own
// save:: helpers instead of raw open/openat: the ported raw-fd version
// crashed the console load on this runtime (bisected on hardware), while
// save::read_file/write_atomic are what the shell itself boots with.
#include "settings.hpp"

#include "core/save_file.hpp"

#include <cstring>
#include <string>

namespace hui {
namespace share {
namespace {

constexpr const char* kSettingsDir = "/download0/PS5Share";
constexpr const char* kSettingsFile = "/download0/PS5Share/webhooks.json";
constexpr const char* kPackagedSettings = "/app0/assets/webhooks.json";
constexpr const char* kPayloadSettingsFile = "/data/PS5Share/webhooks.json";
constexpr std::size_t kMaximumSettingsBytes = 8192;

void skipSpace(const std::string& text, std::size_t& i) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' ||
           text[i] == '\r' || text[i] == '\t')) {
        ++i;
    }
}

bool parseString(const std::string& text, std::size_t& i, std::string& out) {
    if (i >= text.size() || text[i++] != '"') return false;
    out.clear();
    while (i < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[i++]);
        if (ch == '"') return true;
        if (ch < 0x20) return false;
        if (ch != '\\') {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        if (i >= text.size()) return false;
        const char escaped = text[i++];
        switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: return false;
        }
    }
    return false;
}

std::string quoteJson(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        default:
            if (ch < 0x20) return {};
            result.push_back(static_cast<char>(ch));
        }
    }
    result.push_back('"');
    return result;
}

bool parseSettingsText(std::string text, ShareSettings& settings) {
    if (text.size() > kMaximumSettingsBytes) return false;
    // Windows editors may save UTF-8 with a BOM; accept and skip it.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb &&
        static_cast<unsigned char>(text[2]) == 0xbf) {
        text.erase(0, 3);
    }
    std::size_t i = 0;
    std::string key;
    std::string value;
    skipSpace(text, i);
    if (i >= text.size() || text[i++] != '{') return false;
    skipSpace(text, i);
    if (!parseString(text, i, key) || key != "webhook_url") return false;
    skipSpace(text, i);
    if (i >= text.size() || text[i++] != ':') return false;
    skipSpace(text, i);
    if (!parseString(text, i, value)) return false;
    skipSpace(text, i);
    if (i >= text.size() || text[i++] != '}') return false;
    skipSpace(text, i);
    if (i != text.size() || (!value.empty() && !is_allowed_webhook_url(value))) return false;
    settings.webhook_url = value;
    return true;
}

bool readSettingsFile(const char* path, ShareSettings& settings) {
    std::string text;
    return save::read_file(path, &text, kMaximumSettingsBytes) &&
           parseSettingsText(text, settings);
}

}  // namespace

bool is_allowed_webhook_url(const std::string& url) {
    static constexpr const char* kPrefixes[] = {
        "https://discord.com/api/webhooks/",
        "https://canary.discord.com/api/webhooks/",
        "https://ptb.discord.com/api/webhooks/",
        "https://discordapp.com/api/webhooks/",
    };
    bool prefixOk = false;
    for (const char* prefix : kPrefixes) {
        const size_t length = std::strlen(prefix);
        if (url.size() > length && url.compare(0, length, prefix) == 0) {
            prefixOk = true;
            break;
        }
    }
    if (!prefixOk || url.size() > 2048) {
        return false;
    }
    for (unsigned char ch : url) {
        if (ch <= 0x20 || ch == 0x7f || ch == '\\' || ch == '"' || ch == '\'') {
            return false;
        }
    }
    return true;
}

bool load_settings(ShareSettings& settings) {
    // Saved file first: it is what the Settings webhook editor writes, so
    // user edits win over the packaged default (the legacy app's order).
    // The packaged copy remains the fallback when no save exists.
    return readSettingsFile(kSettingsFile, settings) ||
           readSettingsFile(kPackagedSettings, settings) ||
           readSettingsFile(kPayloadSettingsFile, settings);
}

bool save_settings(const ShareSettings& settings) {
    if (!settings.webhook_url.empty() && !is_allowed_webhook_url(settings.webhook_url)) return false;
    const std::string quoted = quoteJson(settings.webhook_url);
    if (quoted.empty() && !settings.webhook_url.empty()) return false;
    const std::string text = "{\n  \"webhook_url\": " + quoted + "\n}\n";
    if (!save::ensure_directory(kSettingsDir)) return false;
    return save::write_atomic(kSettingsFile, text).empty();
}

}  // namespace share
}  // namespace hui
