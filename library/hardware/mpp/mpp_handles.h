#ifndef MPP_HANDLES_H
#define MPP_HANDLES_H

#include <memory>

#include "rk_mpi.h"

/** @brief Deleter of an MppFrame (a void pointer in the SDK). */
struct MppFrameDeleter {
    void operator()(void* frame) const noexcept {
        MppFrame owned{frame};
        (void)mpp_frame_deinit(&owned);
    }
};

/** @brief Deleter of an MppPacket. */
struct MppPacketDeleter {
    void operator()(void* packet) const noexcept {
        MppPacket owned{packet};
        (void)mpp_packet_deinit(&owned);
    }
};

/** @brief Deleter of an MppEncCfg. */
struct MppConfigDeleter {
    void operator()(void* config) const noexcept {
        (void)mpp_enc_cfg_deinit(static_cast<MppEncCfg>(config));
    }
};

/** @brief Owners that release the MPP object when they go out of scope. */
using MppFramePtr = std::unique_ptr<void, MppFrameDeleter>;
using MppPacketPtr = std::unique_ptr<void, MppPacketDeleter>;
using MppConfigPtr = std::unique_ptr<void, MppConfigDeleter>;

#endif // MPP_HANDLES_H
