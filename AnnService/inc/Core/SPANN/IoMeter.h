// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _SPTAG_SPANN_IOMETER_H_
#define _SPTAG_SPANN_IOMETER_H_

#include <atomic>
#include <cstdint>

namespace SPTAG
{
    namespace SPANN
    {
        /// Which pass a device page is charged to. A thread carries one role
        /// at a time; the split pool sets its own for a whole job, so an
        /// append that merely enqueues a split is never charged its pages.
        enum class IoRole { Front, Split, Reasg };

        /// Device pages one thread submitted to the block layer.
        struct IoCount {
            std::uint64_t reads = 0;
            std::uint64_t write = 0;
        };

        /// Roles a page can be charged to; bounds IO_READS and IO_WRITE.
        inline constexpr int NROLE = 3;

        /// This thread's running total, read as a delta around one job so a
        /// foreground op is charged exactly the pages it submitted.
        inline thread_local IoCount IO_LOCAL;

        /// The role this thread charges to, Front until a scope says else.
        inline thread_local IoRole IO_ROLE = IoRole::Front;

        /// Whole-run pages per role over every thread, so the split pool's
        /// traffic still reaches a report even though no foreground op can be
        /// charged for it.
        inline std::atomic<std::uint64_t> IO_READS[NROLE];
        inline std::atomic<std::uint64_t> IO_WRITE[NROLE];

        /// Charges one submitted page to the calling thread and its role.
        /// The block controller is the only caller, at each point a request
        /// reaches the SPDK reactor, so a timed-out or partial batch is
        /// charged exactly what it put on the device.
        inline void charge(bool write)
        {
            int slot = static_cast<int>(IO_ROLE);
            if (write) {
                IO_LOCAL.write++;
                IO_WRITE[slot].fetch_add(1, std::memory_order_relaxed);
            } else {
                IO_LOCAL.reads++;
                IO_READS[slot].fetch_add(1, std::memory_order_relaxed);
            }
        }

        /// This thread's pages so far. A caller keeps one and subtracts.
        inline IoCount taken() { return IO_LOCAL; }

        /// Pages one role has submitted across every thread since the run
        /// began. A caller keeps one and subtracts to get a stage's share.
        inline IoCount rolesum(IoRole role)
        {
            int slot = static_cast<int>(role);
            IoCount total;
            total.reads = IO_READS[slot].load(std::memory_order_relaxed);
            total.write = IO_WRITE[slot].load(std::memory_order_relaxed);
            return total;
        }

        /// Sets the calling thread's role for its own lifetime and restores
        /// the prior one, so a split running inline under a foreground insert
        /// still charges Split, and a reassign nested in a split charges
        /// Reasg without stranding the split's role.
        class RoleScope {
          public:
            explicit RoleScope(IoRole role) : prior(IO_ROLE) { IO_ROLE = role; }
            ~RoleScope() { IO_ROLE = prior; }
            RoleScope(const RoleScope&) = delete;
            RoleScope& operator=(const RoleScope&) = delete;

          private:
            IoRole prior;
        };
    }
}

#endif // _SPTAG_SPANN_IOMETER_H_
