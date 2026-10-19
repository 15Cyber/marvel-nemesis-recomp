#include "app/app.h"
#include "kernel/crypto.h"

namespace app {

bool ValidateGameImage(const std::vector<uint8_t>& xbe, std::string& sha1, std::string& error,
                       const char* build_sha1) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, xbe.data(), xbe.size());
    crypto::ShaFinal(ctx, digest);
    sha1.clear();
    sha1.reserve(40);
    for (uint8_t b : digest) {
        sha1 += "0123456789abcdef"[b >> 4];
        sha1 += "0123456789abcdef"[b & 15];
    }
    error.clear();
    if (kGame.supported_xbe_sha1 && sha1 != kGame.supported_xbe_sha1) {
        error = "This XBE revision is not supported by this port's game-specific code.\n\nExpected SHA-1: ";
        error += kGame.supported_xbe_sha1;
    } else if (build_sha1 && sha1 != build_sha1) {
        error = "This XBE does not match the executable used to build this port.\n\nExpected SHA-1: ";
        error += build_sha1;
    } else {
        return true;
    }
    error += "\nFound SHA-1: " + sha1;
    error += "\n\nUse the matching game files. The game was not started.";
    return false;
}

}  // namespace app
