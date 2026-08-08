#include "TradePresetText.h"

#include <algorithm>
#include <cctype>

namespace {
    std::string ToLowerAscii(std::string_view raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (const unsigned char ch : raw) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    }

    bool StartsWithKind(std::string_view message)
    {
        if (message.size() < 3) {
            return false;
        }
        const auto head = ToLowerAscii(message.substr(0, 3));
        if (head != "wtb" && head != "wts" && head != "wtt") {
            return false;
        }
        return message.size() == 3 ||
               std::isspace(static_cast<unsigned char>(message[3])) ||
               message[3] == ':';
    }
}

namespace gwdash {
    std::string SanitizePresetName(const std::string_view raw)
    {
        std::string out;
        out.reserve(std::min(raw.size(), MAX_PRESET_NAME_LEN));
        for (const unsigned char ch : raw) {
            if (out.size() >= MAX_PRESET_NAME_LEN) {
                break;
            }
            if (std::isalnum(ch) || ch == '_' || ch == '-') {
                out.push_back(static_cast<char>(std::tolower(ch)));
            }
        }
        return out;
    }

    std::string SanitizePresetKind(const std::string_view raw)
    {
        const std::string lower = ToLowerAscii(raw);
        if (lower == "wtb" || lower == "wts" || lower == "wtt") {
            return lower;
        }
        return "wts";
    }

    std::string SanitizePresetMessage(const std::string_view raw)
    {
        std::string out;
        out.reserve(std::min(raw.size(), MAX_PRESET_MESSAGE_LEN));

        std::size_t start = 0;
        while (start < raw.size() &&
               std::isspace(static_cast<unsigned char>(raw[start]))) {
            ++start;
        }

        for (std::size_t i = start; i < raw.size() && out.size() < MAX_PRESET_MESSAGE_LEN; ++i) {
            const unsigned char ch = static_cast<unsigned char>(raw[i]);
            if (ch == '\r' || ch == '\n') {
                continue;
            }
            out.push_back(static_cast<char>(ch));
        }

        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
            out.pop_back();
        }
        return out;
    }

    std::string ComposeTradeLine(const std::string_view kind, const std::string_view message)
    {
        const std::string clean_kind = SanitizePresetKind(kind);
        const std::string clean_message = SanitizePresetMessage(message);
        if (clean_message.empty()) {
            return {};
        }
        if (StartsWithKind(clean_message)) {
            return clean_message.size() > MAX_PRESET_MESSAGE_LEN
                       ? clean_message.substr(0, MAX_PRESET_MESSAGE_LEN)
                       : clean_message;
        }

        std::string line = clean_kind;
        line.push_back(' ');
        line += clean_message;
        if (line.size() > MAX_PRESET_MESSAGE_LEN) {
            line.resize(MAX_PRESET_MESSAGE_LEN);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
                line.pop_back();
            }
        }
        return line;
    }
}
