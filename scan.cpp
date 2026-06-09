#include "common.h"

static uint32_t FindCompressedSize(const uint8_t* src, size_t max_len, uint32_t usize, uint8_t* temp_dec_buf) {
    uint32_t low = 8, high = static_cast<uint32_t>(max_len), csize = 0;
    while (low <= high) {
        uint32_t mid = low + (high - low) / 2;
        if (OodleLZ_Decompress(src, mid, temp_dec_buf, usize, 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0, 0) == static_cast<int64_t>(usize)) {
            csize = mid; high = mid - 1;
        } else { low = mid + 1; }
    }
    return csize;
}

static bool TryMatchBlock(const std::shared_ptr<BlockTask>& task,
                          const std::vector<int32_t>& all_methods,
                          const std::vector<int32_t>& all_levels,
                          std::set<std::pair<int32_t, int32_t>>& cache,
                          std::shared_mutex& cache_mutex,
                          int32_t& out_method, int32_t& out_level) {
    thread_local std::vector<uint8_t> local_comp_buf;
    size_t required_size = std::max(static_cast<size_t>(task->usize) * 2, static_cast<size_t>(256 * 1024));
    if (local_comp_buf.size() < required_size) local_comp_buf.resize(required_size);

    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        for (const auto& pair : cache) {
            if (CompressAndVerify(pair.first, pair.second, task->dec_data.data(), task->usize, task->raw_win_buf.data(), task->csize, local_comp_buf) > 0) {
                out_method = pair.first;
                out_level = pair.second;
                return true;
            }
        }
    }

    for (int32_t method : all_methods) {
        for (int32_t level : all_levels) {
            {
                std::shared_lock<std::shared_mutex> lock(cache_mutex);
                if (cache.find({method, level}) != cache.end()) continue;
            }
            if (CompressAndVerify(method, level, task->dec_data.data(), task->usize, task->raw_win_buf.data(), task->csize, local_comp_buf) > 0) {
                out_method = method;
                out_level = level;
                std::unique_lock<std::shared_mutex> lock(cache_mutex);
                cache.insert({method, level});
                return true;
            }
        }
    }
    return false;
}

Result<int> RunScan(const std::string& input_path, bool verbose, int num_threads, const std::vector<int32_t>& m_ids,
                    const std::vector<uint8_t>& aesKey, bool useAES, double scan_percent, bool debug_mode) {
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
    if (scan_limit < 1024) scan_limit = fSize;

    std::cout << "[SCAN] Scanning " << scan_percent << "% of file (" << scan_limit / 1024 / 1024 << " MB) using " << num_threads << " threads." << std::endl;

    ThreadPool pool(num_threads);
    UI ui(scan_limit, 0, verbose);
    BlockScanner scanner(reader, fSize, useAES, aesKey, Config::WIN_SIZE_SMALL);
    ScanStats stats;
    std::set<std::pair<int32_t, int32_t>> identified_cache;
    std::shared_mutex scan_mutex;
    std::queue<std::future<std::shared_ptr<BlockTask>>> scan_queue;
    uint64_t pos = 0;
    auto last_ui_time = std::chrono::steady_clock::now();
    std::vector<uint32_t> usizes_vec(std::begin(Config::TEST_USIZES), std::end(Config::TEST_USIZES));
    std::vector<int32_t> methods_vec(std::begin(Config::ALL_METHODS), std::end(Config::ALL_METHODS));
    std::vector<int32_t> levels_vec(std::begin(Config::ALL_LEVELS), std::end(Config::ALL_LEVELS));

    while (pos < scan_limit && pos < fSize - 16) {
        auto task = scanner.extract_next_block(pos, scan_limit, usizes_vec);
        if (!task) { pos++; continue; }

        {
            std::unique_lock<std::shared_mutex> lock(scan_mutex);
            stats.blocks_found++;
        }

        Logger::Log(Logger::Level::Debug, "Found potential block at 0x" + std::to_string(task->pos) + " (usize: " + std::to_string(task->usize) + ", csize: " + std::to_string(task->csize) + ")");

        scan_queue.push(pool.enqueue([task, methods_vec, levels_vec, &scan_mutex, &identified_cache, &stats]() {
            int32_t m = -1, l = -1;
            bool match = TryMatchBlock(task, methods_vec, levels_vec, identified_cache, scan_mutex, m, l);
            {
                std::unique_lock<std::shared_mutex> lock(scan_mutex);
                if (match) {
                    stats.add_match(m, l);
                    Logger::Log(Logger::Level::Debug, "Matched block to Method " + std::to_string(m) + " Level " + std::to_string(l));
                } else {
                    stats.fails_unidentified++;
                }
            }
            return task;
        }));

        while (scan_queue.size() >= static_cast<size_t>(num_threads) * 4) {
            auto completed = scan_queue.front().get();
            scan_queue.pop();
            completed->raw_win_buf.clear(); completed->raw_win_buf.shrink_to_fit();
            completed->dec_data.clear(); completed->dec_data.shrink_to_fit();
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ui_time).count() >= 500) {
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
    }

    while (!scan_queue.empty()) {
        auto completed = scan_queue.front().get();
        scan_queue.pop();
        completed->raw_win_buf.clear(); completed->raw_win_buf.shrink_to_fit();
        completed->dec_data.clear(); completed->dec_data.shrink_to_fit();
    }

    std::cout << std::endl << "--- Scan Report ---" << std::endl
              << "Blocks Found: " << stats.blocks_found << " | Matches: " << stats.matches_identified << " | Fails: " << stats.fails_unidentified << std::endl;
    if (stats.blocks_found > 0) {
        std::cout << "Success Rate: " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(stats.matches_identified) / stats.blocks_found * 100.0) << "%" << std::endl;
    }
    if (!stats.method_counts.empty()) {
        std::cout << "Detected Methods Distribution:" << std::endl;
        for (const auto& [id, count] : stats.method_counts) {
            std::string name = (id == 8 ? "Kraken" : id == 9 ? "Leviathan" : id == 11 ? "Mermaid" : id == 12 ? "Selkie" : "Unknown(" + std::to_string(id) + ")");
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
    return Result<int>(0);
}
