#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gwdash {
    struct ManifestFile {
        std::string name;
        std::string sha256;
    };

    struct UpdateManifest {
        int v = 1;
        std::string version;
        std::vector<ManifestFile> files;
    };

    /** Canonical UTF-8 JSON bytes that must be signed (stable key order). */
    bool ManifestSigningBytes(const UpdateManifest& manifest, std::string& out, std::string& error);

    bool ParseManifest(std::string_view json, UpdateManifest& out, std::string& error);

    /** Hex-encoded 64-byte ed25519 signature over the signing bytes. */
    bool VerifyManifestSignature(std::string_view signing_bytes, std::string_view sig_hex,
                                 std::string& error);

    /** Decode lowercase/uppercase hex; empty on failure. */
    std::vector<uint8_t> DecodeHex(std::string_view hex);
} // namespace gwdash
