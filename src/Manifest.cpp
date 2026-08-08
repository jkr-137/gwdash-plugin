#include "Manifest.h"

#include <cctype>
#include <cstring>
#include <sstream>

#include <glaze/glaze.hpp>

extern "C" {
#include "ed25519.h"
}

#include "UpdatePublicKey.h"

template <> struct glz::meta<gwdash::ManifestFile> {
    using T = gwdash::ManifestFile;
    static constexpr auto value = object("name", &T::name, "sha256", &T::sha256);
};

template <> struct glz::meta<gwdash::UpdateManifest> {
    using T = gwdash::UpdateManifest;
    static constexpr auto value = object("v", &T::v, "version", &T::version, "files", &T::files);
};

namespace gwdash {
    std::vector<uint8_t> DecodeHex(const std::string_view hex)
    {
        if (hex.size() % 2 != 0) {
            return {};
        }
        std::vector<uint8_t> out;
        out.reserve(hex.size() / 2);
        const auto nibble = [](const char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            const int hi = nibble(hex[i]);
            const int lo = nibble(hex[i + 1]);
            if (hi < 0 || lo < 0) {
                return {};
            }
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return out;
    }

    bool ManifestSigningBytes(const UpdateManifest& manifest, std::string& out, std::string& error)
    {
        std::ostringstream oss;
        oss << "{\"v\":" << manifest.v << ",\"version\":\"" << manifest.version << "\",\"files\":[";
        for (std::size_t i = 0; i < manifest.files.size(); ++i) {
            if (i != 0) {
                oss << ',';
            }
            oss << "{\"name\":\"" << manifest.files[i].name << "\",\"sha256\":\""
                << manifest.files[i].sha256 << "\"}";
        }
        oss << "]}";
        out = oss.str();
        if (manifest.version.empty() || manifest.files.empty()) {
            error = "manifest missing version or files";
            return false;
        }
        error.clear();
        return true;
    }

    bool ParseManifest(const std::string_view json, UpdateManifest& out, std::string& error)
    {
        constexpr glz::opts lenient{.error_on_unknown_keys = false};
        UpdateManifest staged{};
        if (glz::read<lenient>(staged, json)) {
            error = "could not parse manifest.json";
            return false;
        }
        if (staged.v != 1 || staged.version.empty() || staged.files.empty()) {
            error = "manifest schema unsupported";
            return false;
        }
        out = std::move(staged);
        error.clear();
        return true;
    }

    bool VerifyManifestSignature(const std::string_view signing_bytes,
                                 const std::string_view sig_hex, std::string& error)
    {
        const std::vector<uint8_t> signature = DecodeHex(sig_hex);
        if (signature.size() != 64) {
            error = "manifest.sig has invalid length";
            return false;
        }

        if (!ed25519_verify(signature.data(),
                            reinterpret_cast<const unsigned char*>(signing_bytes.data()),
                            signing_bytes.size(), UPDATE_PUBLIC_KEY.data())) {
            error = "manifest signature verification failed";
            return false;
        }
        error.clear();
        return true;
    }
} // namespace gwdash
