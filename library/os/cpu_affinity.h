#ifndef CPU_AFFINITY_H
#define CPU_AFFINITY_H

#include <pthread.h>
#include <sched.h>

/**
 * @brief Pins the calling thread to one CPU.
 * @param cpu CPU number, 0 to CPU_SETSIZE - 1.
 * @return true on success, false when the CPU does not exist or the call is refused.
 */
inline bool pinThreadToCpu(int cpu) noexcept {
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

#endif // CPU_AFFINITY_H
