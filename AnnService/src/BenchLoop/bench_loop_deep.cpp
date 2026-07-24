// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// DEEP-200M SPFresh bench harness: two-phase protocol matching fnct-db's
// hermes_sift.rs / pipeann's bench_loop.cpp, adapted to SPFresh's
// SPANN::Index<T> API (KV/RocksDB backend, since no SPDK/NVMe hardware is
// assumed). Config is hardcoded; only file paths come from argv.
//
// Phase 1 (SEEDN -> BASE): plain sequential insert via AddIndexSPFresh, no
// search. SPFresh's AddIndexSPFresh requires a nonempty index (it fails with
// ErrorCode::EmptyIndex otherwise), so the first SEEDN points are ingested up
// front via the classic offline BuildIndex path (SelectHead+BuildHead+
// BuildSSDIndex) to bootstrap a valid head+posting store; every point after
// that goes through the timed per-op AddIndexSPFresh loop.
//
// Phase 2 (BASE -> N): each BATCH-point step runs one concurrent insert+
// search step (undersampled, rate=MIXRATE, the only undersampled rows) for
// PRIMARY_CFG against a fixed held-out query set, then a fully-logged
// extra_search_cfgs() sweep at the same just-reached corpus size. Recall is
// computed post-hoc against fnct-db's gt_sift.rs log by joining on id/dist.
//
// Two output files: <out_prefix>records.csv (search rows, column-compatible
// with pipeann's bench_loop.cpp QueryRow schema) and <out_prefix>inserts.csv
// (insert rows: SPFresh's AddIndexSPFresh only enqueues to an in-memory
// persistent buffer + version map -- actual disk IO happens later on
// background merge/reassign threads decoupled from the caller, so there is
// no synchronous per-insert page/IO count to log here, unlike search).

#include "inc/Core/Common.h"
#include "inc/Core/SPANN/Index.h"
#include "inc/Core/VectorIndex.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace SPTAG;
using clk = std::chrono::steady_clock;

namespace {

using ValueT = float;
constexpr int DIM = 96;
constexpr VectorValueType VTYPE = VectorValueType::Float;

constexpr std::size_t N = 200'000'000;
constexpr std::size_t BASE = 100'000'000;
constexpr std::size_t BATCH = 1'000'000;
constexpr std::size_t QLEN = 10'000;
// Bootstrap size: ingested via the classic BuildIndex path before the timed
// AddIndexSPFresh loop takes over (see file header).
constexpr std::size_t SEEDN = BATCH;
constexpr int TOPK = 10;
// Undersample rate for the concurrent mix step only; every other row below
// is logged at rate 1 (unsampled).
constexpr int MIXRATE = 50;

struct SearchCfg { int k; int internalResultNum; };
constexpr SearchCfg PRIMARY_CFG{TOPK, 64};
std::vector<SearchCfg> extra_search_cfgs() {
    return {{TOPK, 32}, {TOPK, 96}, {TOPK, 128}, {TOPK, 192}};
}

int threads() {
    unsigned hw = std::thread::hardware_concurrency();
    return hw > 0 ? static_cast<int>(hw) : 1;
}

template <typename T>
void load_raw(const std::string &path, std::size_t off, std::size_t count, std::size_t dim, std::vector<T> &out) {
    out.assign(count * dim, T{});
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "open " << path << "\n"; std::exit(1); }
    f.seekg(static_cast<std::streamoff>(off * dim * sizeof(T)), std::ios::beg);
    f.read(reinterpret_cast<char *>(out.data()), count * dim * sizeof(T));
    if (!f) { std::cerr << "short read " << path << "\n"; std::exit(1); }
}

void write_search_header(std::ofstream &of, int max_k) {
    of << "batch,visitor,qi,start_ns,lat_ns";
    for (int i = 1; i <= max_k; i++) of << ",id@" << i << ",dist@" << i;
    of << ",n_ios,n_hops,n_cmps,total_us\n";
}

void write_insert_header(std::ofstream &of) {
    of << "batch,idx,vid,start_ns,lat_ns,total_us\n";
}

std::string cfgtag(const SearchCfg &c) {
    std::ostringstream ss;
    ss << "spfresh(k=" << c.k << ",irn=" << c.internalResultNum << ")_run0";
    return ss.str();
}

// Runs one (k, internalResultNum) sweep over the fixed QLEN query set,
// appending one CSV row per query (undersampled by `rate`; only the phase-2
// mix step passes rate>1).
void run_search_sweep(std::ofstream &of, std::mutex &of_mtx, SPANN::Index<ValueT> *index,
                      const std::vector<ValueT> &queries, const SearchCfg &sc, std::uint64_t batch,
                      clk::time_point t0, int nthreads, int rate) {
    std::string visitor = cfgtag(sc);
    std::atomic_size_t next(0);
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; t++) {
        pool.emplace_back([&]() {
            index->Initialize();
            std::ostringstream buf;
            std::size_t rows = 0, qi;
            while ((qi = next.fetch_add(1)) < QLEN) {
                if (rate > 1 && (qi % static_cast<std::size_t>(rate)) != 0) continue;
                QueryResult result(nullptr, sc.internalResultNum, false);
                result.SetTarget(reinterpret_cast<const void *>(queries.data() + qi * DIM));
                SPANN::SearchStats stats;
                auto ts = clk::now();
                std::uint64_t start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ts - t0).count();
                index->GetMemoryIndex()->SearchIndex(result);
                index->SearchDiskIndex(result, &stats);
                std::uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - ts).count();

                buf << batch << ',' << visitor << ',' << qi << ',' << start_ns << ',' << lat_ns;
                for (int j = 0; j < TOPK; j++) {
                    if (j < sc.k) {
                        auto *r = result.GetResult(j);
                        buf << ',' << r->VID << ',' << r->Dist;
                    } else buf << ",,";
                }
                buf << ',' << stats.m_diskIOCount << ',' << stats.m_totalListElementsCount
                    << ',' << stats.m_exCheck << ',' << (lat_ns / 1000) << '\n';
                if (++rows % 256 == 0) {
                    std::lock_guard<std::mutex> lk(of_mtx);
                    of << buf.str(); of.flush(); buf.str("");
                }
            }
            if (!buf.str().empty()) { std::lock_guard<std::mutex> lk(of_mtx); of << buf.str(); of.flush(); }
        });
    }
    for (auto &th : pool) th.join();
}

// Inserts [lo, hi) one point at a time (across nthreads workers, via
// AddIndexSPFresh), logging one row per insert undersampled by `rate`
// (rate=1 for the plain phase-1 loop, rate=MIXRATE for the phase-2 mix
// step). Blocks until SPFresh's background merge/reassign threads have
// caught up before returning.
void run_insert(std::ofstream &of, std::mutex &of_mtx, SPANN::Index<ValueT> *index,
                const std::vector<ValueT> &pts, std::size_t lo, std::size_t hi,
                std::uint64_t batch, clk::time_point t0, int nthreads, int rate) {
    std::atomic_size_t next(lo);
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; t++) {
        pool.emplace_back([&]() {
            index->Initialize();
            std::ostringstream buf;
            std::size_t rows = 0, idx;
            while ((idx = next.fetch_add(1)) < hi) {
                SizeType vid = 0;
                auto ts = clk::now();
                std::uint64_t start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ts - t0).count();
                index->AddIndexSPFresh(pts.data() + (idx - lo) * DIM, 1, DIM, &vid);
                std::uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - ts).count();
                if (rate == 1 || (idx % static_cast<std::size_t>(rate)) == 0) {
                    buf << batch << ',' << idx << ',' << vid << ',' << start_ns << ',' << lat_ns
                        << ',' << (lat_ns / 1000) << '\n';
                    rows++;
                }
                if (rows >= 256) {
                    std::lock_guard<std::mutex> lk(of_mtx);
                    of << buf.str(); of.flush(); buf.str(""); rows = 0;
                }
            }
            if (!buf.str().empty()) { std::lock_guard<std::mutex> lk(of_mtx); of << buf.str(); of.flush(); }
        });
    }
    for (auto &th : pool) th.join();
    while (!index->AllFinished()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::cerr << "usage: " << argv[0] << " <data_raw> <queries_raw> <index_dir> <out_prefix>\n";
        return 1;
    }
    std::string data = argv[1], queries_path = argv[2], index_dir = argv[3], out_prefix = argv[4];
    int nthreads = threads();

    auto vi = VectorIndex::CreateInstance(IndexAlgoType::SPANN, VTYPE);
    auto *index = static_cast<SPANN::Index<ValueT> *>(vi.get());

    index->SetParameter("Dim", std::to_string(DIM).c_str(), "Base");
    index->SetParameter("DistCalcMethod", "L2", "Base");
    index->SetParameter("IndexAlgoType", "BKT", "Base");
    index->SetParameter("IndexDirectory", index_dir.c_str(), "Base");

    index->SetParameter("isExecute", "true", "SelectHead");
    index->SetParameter("NumberOfThreads", std::to_string(nthreads).c_str(), "SelectHead");
    index->SetParameter("BKTKmeansK", "32", "SelectHead");
    index->SetParameter("BKTLeafSize", "8", "SelectHead");
    index->SetParameter("SamplesNumber", "1000", "SelectHead");
    index->SetParameter("SelectThreshold", "12", "SelectHead");
    index->SetParameter("SplitFactor", "9", "SelectHead");
    index->SetParameter("SplitThreshold", "18", "SelectHead");
    index->SetParameter("Ratio", "0.1", "SelectHead");

    index->SetParameter("isExecute", "true", "BuildHead");
    index->SetParameter("NumberOfThreads", std::to_string(nthreads).c_str(), "BuildHead");

    index->SetParameter("isExecute", "true", "BuildSSDIndex");
    index->SetParameter("BuildSsdIndex", "true", "BuildSSDIndex");
    index->SetParameter("NumberOfThreads", std::to_string(nthreads).c_str(), "BuildSSDIndex");
    index->SetParameter("InternalResultNum", "64", "BuildSSDIndex");
    index->SetParameter("ReplicaCount", "8", "BuildSSDIndex");
    index->SetParameter("PostingPageLimit", "4", "BuildSSDIndex");
    index->SetParameter("TmpDir", (index_dir + "/tmp").c_str(), "BuildSSDIndex");
    index->SetParameter("UseKV", "true", "BuildSSDIndex");
    index->SetParameter("KVPath", (index_dir + "/rocksdb").c_str(), "BuildSSDIndex");
    index->SetParameter("MergeThreshold", "10", "BuildSSDIndex");
    index->SetParameter("LatencyLimit", "10.0", "BuildSSDIndex");
    index->SetParameter("BufferLength", "6", "BuildSSDIndex");
    index->SetParameter("InsertThreadNum", std::to_string(nthreads).c_str(), "BuildSSDIndex");
    index->SetParameter("AppendThreadNum", std::to_string(nthreads).c_str(), "BuildSSDIndex");
    index->SetParameter("ReassignThreadNum", std::to_string(nthreads).c_str(), "BuildSSDIndex");
    index->SetParameter("ReassignK", "64", "BuildSSDIndex");
    index->SetParameter("SearchInternalResultNum", std::to_string(PRIMARY_CFG.internalResultNum).c_str(), "BuildSSDIndex");
    index->SetParameter("ResultNum", std::to_string(TOPK).c_str(), "BuildSSDIndex");

    std::vector<ValueT> seed;
    load_raw<ValueT>(data, 0, SEEDN, DIM, seed);
    if (index->BuildIndex(seed.data(), static_cast<SizeType>(SEEDN), DIM, false, false) != ErrorCode::Success) {
        std::cerr << "bootstrap BuildIndex failed\n";
        return 1;
    }

    std::vector<ValueT> queries;
    load_raw<ValueT>(queries_path, 0, QLEN, DIM, queries);

    std::ofstream search_of(out_prefix + "records.csv");
    std::ofstream insert_of(out_prefix + "inserts.csv");
    if (!search_of || !insert_of) { std::cerr << "open output\n"; return 1; }
    write_search_header(search_of, TOPK);
    write_insert_header(insert_of);
    std::mutex search_mtx, insert_mtx;

    auto t0 = clk::now();
    std::vector<ValueT> buf;
    std::uint64_t batch_id = 0;

    // Phase 1: plain sequential insert, SEEDN -> BASE, no search.
    for (std::size_t off = SEEDN; off < BASE; off += BATCH) {
        std::size_t bsz = std::min(BATCH, BASE - off);
        load_raw<ValueT>(data, off, bsz, DIM, buf);
        run_insert(insert_of, insert_mtx, index, buf, off, off + bsz, batch_id, t0, nthreads, 1);
        batch_id++;
        std::cerr << "[phase1 batch " << batch_id << "] inserted up to " << (off + bsz) << "\n";
    }

    // Phase 2: windowed run, BASE -> N.
    for (std::size_t off = BASE; off < N; off += BATCH) {
        std::size_t bsz = std::min(BATCH, N - off);
        load_raw<ValueT>(data, off, bsz, DIM, buf);

        std::thread ins_th([&]() {
            run_insert(insert_of, insert_mtx, index, buf, off, off + bsz, batch_id, t0, nthreads, MIXRATE);
        });
        run_search_sweep(search_of, search_mtx, index, queries, PRIMARY_CFG, batch_id, t0, nthreads, MIXRATE);
        ins_th.join();

        for (auto &sc : extra_search_cfgs()) {
            run_search_sweep(search_of, search_mtx, index, queries, sc, batch_id, t0, nthreads, 1);
        }
        batch_id++;
        std::cerr << "[phase2 batch " << batch_id << "] reached " << (off + bsz) << "\n";
    }

    return 0;
}
