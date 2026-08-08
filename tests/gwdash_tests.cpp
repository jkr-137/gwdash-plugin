#include <cstdlib>
#include <iostream>
#include <string>

#include "Manifest.h"
#include "PriceSnapshot.h"
#include "TradePresetText.h"

extern "C" {
#include "ed25519.h"
}

namespace {
    int failures = 0;

    void Expect(const bool cond, const char* msg)
    {
        if (!cond) {
            std::cerr << "FAIL: " << msg << '\n';
            ++failures;
        }
    }
} // namespace

int main()
{
    using namespace gwdash;

    Expect(SanitizePresetName("Arms!") == "arms", "sanitize name");
    Expect(SanitizePresetKind("WTB") == "wtb", "sanitize kind");
    Expect(ComposeTradeLine("wts", "27e arms") == "wts 27e arms", "compose prefix");
    Expect(ComposeTradeLine("wts", "wtb 5e eg") == "wtb 5e eg", "compose keeps existing kind");
    Expect(FormatEcto(27.0) == "27e", "format ecto whole");
    Expect(FormatEcto(27.4).find("27.4") == 0, "format ecto decimal");
    Expect(FormatGold(5600) == "5.6k", "format gold");

    PriceSnapshot snap{};
    Expect(!ParseSnapshot("{}", snap), "reject empty snapshot");
    Expect(ParseSnapshot(R"({"v":1,"at":1,"ecto":{},"armbrace":{},"blackdye":{}})", snap),
           "accept v1 snapshot");
    Expect(!ParseSnapshot(R"({"v":2,"at":1,"ecto":{},"armbrace":{},"blackdye":{}})", snap),
           "reject v2 snapshot");

    UpdateManifest manifest{.v = 1, .version = "1.2.3", .files = {{"GWDash.core.dll", "abc"}}};
    std::string bytes;
    std::string error;
    Expect(ManifestSigningBytes(manifest, bytes, error), "manifest bytes");
    Expect(bytes.find("\"version\":\"1.2.3\"") != std::string::npos, "manifest contains version");

    // Round-trip sign/verify with the embedded public key requires the matching
    // private seed; here we only check decode + reject bad signatures.
    Expect(!VerifyManifestSignature(bytes, "00", error), "reject short sig");

    unsigned char seed[32]{};
    for (int i = 0; i < 32; ++i) {
        seed[i] = static_cast<unsigned char>(i + 1);
    }
    unsigned char public_key[32]{};
    unsigned char private_key[64]{};
    ed25519_create_keypair(public_key, private_key, seed);
    unsigned char signature[64]{};
    ed25519_sign(signature, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                 public_key, private_key);
    Expect(ed25519_verify(signature, reinterpret_cast<const unsigned char*>(bytes.data()),
                          bytes.size(), public_key) == 1,
           "ed25519 self verify");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All gwdash_tests passed\n";
    return 0;
}
