#include "common.h"

Result<int> RunScan(const std::string& input_path, bool verbose, int num_threads,
                    const std::vector<int32_t>& m_ids, const std::vector<int32_t>& opt_levels,
                    bool opt_auto, bool opt_force, const std::vector<uint8_t>& aesKey,
                    bool useAES, double scan_percent, bool debug_mode,
                    uint32_t tradeoff_bytes, bool quantum_crc) 
{
    if (scan_percent <= 0.0 || scan_percent > 100.0) {
        return Result<int>(ErrorCode::ERR_INVALID_ARGUMENT, "Scan percentage must be between 0.0 and 100.0");
    }

    Logger::Init(debug_mode);

    ThreadSafeReader reader(input_path);
    if (!reader.is_open()) {
        return Result<int>(ErrorCode::ERR_FILE_NOT_FOUND, "Failed to open input file: " + input_path);
    }

    uint64_t fSize = reader.get_size();
    uint64_t scan_limit = static_cast<uint64_t>(fSize * (scan_percent / 100.0));
    if (scan_percent >= 99.9) scan_limit = fSize;

    std::cout << "[BUILD MARKER: STABLE_V2_" << __DATE__ << "_" << __TIME__ << "]" << std::endl;
    std::cout << "[SCAN] Scanning " << scan_percent << "% of file ("
              << scan_limit / 1024 / 1024 << " / " << fSize / 1024 / 1024
              << " MB) using " << num_threads << " threads." << std::endl;

    OodleLZ_CompressOptions opts = {};
    opts.version = 232;
    opts.spaceSpeedTradeoffBytes = tradeoff_bytes;
    opts.sendQuantumCRCs = quantum_crc ? 1 : 0;

    ThreadPool pool(num_threads);
    UI ui(scan_limit, 0, verbose);

    size_t actual_win = Config::WIN_SIZE_LARGE; 
    BlockScanner scanner(reader, fSize, useAES, aesKey, actual_win);

    ScanStats stats;
    std::set<std::pair<int32_t, int32_t>> identified_cache;
    std::shared_mutex scan_mutex;
    std::queue<std::future<std::shared_ptr<BlockTask>>> scan_queue;

    uint64_t pos = 0;
    uint64_t last_block_end = 0;
    std::vector<std::pair<uint64_t, uint64_t>> gaps;
    auto last_ui_time = std::chrono::steady_clock::now();

    std::vector<uint32_t> usizes_vec(std::begin(Config::TEST_USIZES), std::end(Config::TEST_USIZES));
    std::vector<int32_t> methods_vec = m_ids.empty() 
        ? std::vector<int32_t>(std::begin(Config::ALL_METHODS), std::end(Config::ALL_METHODS)) 
        : m_ids;

    std::vector<int32_t> levels_vec;
    if (!opt_levels.empty()) {
        levels_vec = opt_levels;
    } else if (opt_auto) {
        levels_vec = {4, 6, 7}; 
    } else if (opt_force) {
        levels_vec = {3, 4, 5, 6, 7, 8};
    } else {
        levels_vec = {4, 5, 6, 7};
    }

    while (pos < scan_limit && pos < fSize) {
        auto task = scanner.extract_next_block(pos, scan_limit, usizes_vec);
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ui_time).count() >= 1000) {
            uint32_t m = 0, f = 0, bf = 0;
            {
                std::shared_lock<std::shared_mutex> lock(scan_mutex);
                m = stats.matches_identified; 
                f = stats.fails_unidentified; 
                bf = stats.blocks_found;
            }
            ui.set_stats(m, f);
            ui.update(pos, bf, "SCAN", pos);
            last_ui_time = now;
        }

        if (!task) {
            pos += 64; 
            continue;
        }

        if (task->pos > last_block_end) {
            gaps.push_back({last_block_end, task->pos - last_block_end});
        }
        last_block_end = task->pos + task->csize;

        {
            std::unique_lock<std::shared_mutex> lock(scan_mutex);
            stats.blocks_found++;
        }

        scan_queue.push(pool.enqueue([task, methods_vec, levels_vec, &scan_mutex, &identified_cache, &stats, &opts]() {
            int32_t m = -1, l = -1;
            bool match = TryMatchBlock(task, methods_vec, levels_vec, identified_cache, scan_mutex, m, l, &opts);
            {
                std::unique_lock<std::shared_mutex> lock(scan_mutex);
                if (match) {
                    stats.add_match(m, l);
                } else {
                    stats.fails_unidentified++;
                }
            }
            return task;
        }));

        // [FIX] Decreased allowable inflight tasks to 2 per core to save memory
        while (scan_queue.size() >= static_cast<size_t>(num_threads) * 2) {
            try {
                auto completed = scan_queue.front().get();
            } catch (const std::exception& e) {
                std::cerr << "[SCAN] Task exception: " << e.what() << "\n";
            }
            scan_queue.pop();          
        }
    }

    while (!scan_queue.empty()) {
        try {
            auto completed = scan_queue.front().get();
        } catch (const std::exception& e) {
            std::cerr << "[SCAN] Task exception: " << e.what() << "\n";
        }
        scan_queue.pop();          
    }

    std::cout << std::endl << "--- Scan Report ---" << std::endl
              << "Blocks Found: " << stats.blocks_found 
              << " | Matches: " << stats.matches_identified 
              << " | Fails: " << stats.fails_unidentified << std::endl;

    if (stats.blocks_found > 0) {
        std::cout << "Success Rate: " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(stats.matches_identified) / stats.blocks_found * 100.0) << "%" << std::endl;
    }

    if (!stats.method_counts.empty()) {
        std::cout << "Detected Methods Distribution:" << std::endl;
        for (const auto& [id, count] : stats.method_counts) {
            std::string name = (id == 8 ? "Kraken" : id == 9 ? "Leviathan" : id == 11 ? "Mermaid" : id == 12 ? "Hydra" : id == 13 ? "Leviathan" : "Unknown(" + std::to_string(id) + ")");
            std::cout << "  " << name << ": " << count << " blocks" << std::endl;
        }
    }

    if (!stats.level_counts.empty()) {
        std::cout << "Detected Levels Distribution:" << std::endl;
        for (const auto& [lvl, count] : stats.level_counts) {
            std::cout << "  Level " << lvl << ": " << count << " blocks" << std::endl;
        }
    }

    std::cout << "Time: " << ui.format_time(ui.get_elapsed()) << std::endl;

    {
        const auto& d = scanner.diagnostics_;
        std::cout << std::endl << "--- Scan Diagnostics ---" << std::endl
                  << "Magic byte candidates found: " << d.magic_candidates_found << std::endl
                  << "Passed fast-rejection filter: " << d.passed_fast_rejection << std::endl
                  << "Rejected by fast-rejection:   " << d.rejected_by_fast_rejection << std::endl
                  << "--- WalkOodleChain breakdown ---" << std::endl
                  << "Calls: " << d.walk_called << std::endl
                  << "  Succeeded: " << d.walk_succeeded << std::endl;
    }

    if (scan_limit > last_block_end) {
        gaps.push_back({last_block_end, scan_limit - last_block_end});
    }

    {
        uint64_t total_gap_bytes = 0;
        for (const auto& g : gaps) total_gap_bytes += g.second;
        double gap_percentage = (fSize > 0) ? (total_gap_bytes / static_cast<double>(fSize) * 100.0) : 0.0;
        
        std::cout << std::endl << "--- Gap Report ---" << std::endl
                  << "Gap count: " << gaps.size() << " | Total gap bytes: " << total_gap_bytes << std::endl
                  << "Gap percentage: " << gap_percentage << "%" << std::endl;
        
        if (gap_percentage > 15.0) {
            std::cout << "\n⚠️  HIGH GAP PERCENTAGE DETECTED!" << std::endl;
        } else {
            std::cout << "\n✓ Gap percentage is within normal range" << std::endl;
        }
    }

    return Result<int>(0);
}
