#ifndef MPP_SOURCE_H
#define MPP_SOURCE_H

#include <optional>

#include "dmabuf.h"
#include "logger.h"

/**
 * @brief A DmaBuf imported into the MPP encoders (as a DRM buffer, the only type that MPP reads
 *        correctly, measured on the board). Move-only, the destructor releases the import.
 *
 * Import once per buffer and keep the object.
 */
class MppSource {
public:
    /**
     * @brief Imports the DMA-BUF descriptor of a buffer into MPP.
     * @param buffer The buffer. It must outlive the returned object.
     * @param log Logger that reports a failure to release the import.
     * @return The source, or empty when MPP rejects the descriptor, which is logged.
     */
    static std::optional<MppSource> import(DmaBuf& buffer, const Logger& log);

    MppSource(const MppSource&) = delete;
    MppSource& operator=(const MppSource&) = delete;
    MppSource(MppSource&& other) noexcept;
    MppSource& operator=(MppSource&& other) noexcept;
    ~MppSource();

    /** @brief The MPP buffer handle (an MppBuffer). Null only in a moved-from source. */
    void* handle() const noexcept {
        return imported;
    }

private:
    MppSource(void* handleValue, Logger logger) noexcept : imported{handleValue}, log{logger} {}

    void release() noexcept;

    void* imported{nullptr};
    Logger log;
};

#endif // MPP_SOURCE_H
