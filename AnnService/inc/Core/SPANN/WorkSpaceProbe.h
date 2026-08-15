// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _SPTAG_SPANN_WORKSPACEPROBE_H_
#define _SPTAG_SPANN_WORKSPACEPROBE_H_

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace SPTAG
{
    namespace SPANN
    {
        /// Monotonic source of workspace identities, so a panic can name the
        /// object two jobs shared rather than just its address.
        inline std::atomic<std::uint64_t> g_workSpaceSeq{ 1 };

        /// Identity of the job this thread currently runs, stamped by the
        /// benchmark before it enters the index. Zero means unstamped, which
        /// exempts the caller from the check: SPFresh's own background pools
        /// hold a workspace for their whole life and never go through a job.
        inline thread_local std::uint64_t t_currentJobId = 0;

        inline void SetCurrentJob(std::uint64_t p_jobId)
        {
            t_currentJobId = p_jobId;
        }

        /// Aborts when a job enters a workspace a prior job never closed.
        /// Sequential reuse after a clean close is legitimate pooling and
        /// never reaches here.
        [[noreturn]] inline void ProbeFail(std::uint64_t p_wsId, std::uint64_t p_openJobId,
                                           std::uint64_t p_jobId)
        {
            std::fprintf(stderr,
                         "PROBE PANIC: workspace %llu entered by job %llu while job %llu still open\n",
                         (unsigned long long)p_wsId, (unsigned long long)p_jobId,
                         (unsigned long long)p_openJobId);
            std::fflush(stderr);
            std::abort();
        }
    }
}

#endif // _SPTAG_SPANN_WORKSPACEPROBE_H_
