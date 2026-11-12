// Link and load check for the board libraries: RGA, MPP, RKNN runtime.
// Exit code 0 only if RGA reports a version, MPP creates and initialises an H.265 encoder,
// and rknn_init rejects a junk model (proves librknnrt loads and the symbol resolves).
#include <cstdio>
#include <cstring>

#include "im2d.hpp"
#include "rk_mpi.h"
#include "rknn_api.h"

int main() {
    int failures = 0;

    const char* rgaVersion = querystring(RGA_VERSION);
    std::printf("RGA  : %s\n", rgaVersion != nullptr ? rgaVersion : "(null)");
    if (rgaVersion == nullptr) {
        ++failures;
    }

    MppCtx ctx = nullptr;
    MppApi* api = nullptr;
    MPP_RET mppRet = mpp_create(&ctx, &api);
    std::printf("MPP  : mpp_create ret=%d\n", static_cast<int>(mppRet));
    if (mppRet == MPP_OK) {
        mppRet = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
        std::printf("MPP  : mpp_init(ENC,HEVC) ret=%d\n", static_cast<int>(mppRet));
        mpp_destroy(ctx);
    }
    if (mppRet != MPP_OK) {
        ++failures;
    }

    rknn_context rknnCtx = 0;
    char junk[64];
    std::memset(junk, 0, sizeof(junk));
    const int rknnRet = rknn_init(&rknnCtx, junk, sizeof(junk), 0, nullptr);
    std::printf("RKNN : rknn_init(junk) ret=%d (nonzero expected)\n", rknnRet);
    if (rknnRet == 0) {
        ++failures;
    }

    std::printf(failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? 0 : 1;
}
