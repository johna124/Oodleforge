#include "common.h"
#include <filesystem>
#include <set>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <array>

Result<int> RunEncode(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads,
                      const std::vector<int32_t>& m_ids, const std::vector<int32_t>& opt_levels, bool opt_auto, bool opt_force,
                      const std::vector<uint8_t>& aesKey, bool useAES, uint32_t tradeoff_bytes, bool quantum_crc) {
    
    std::vector<int32_t> effective_m_ids = m_ids;
    if (effective_m_ids.empty()) {
        if (opt_auto) {
            for (int32_t m : Config::ALL_METHODS) effective_m_ids.push_back(m);
        } else {
            effective_m_ids.push_back(8);
        }
    }

    ThreadSafeReader reader(input_path);
    if (!reader.is_open()) {
        return Result<int>(ErrorCode::ERR_FILE_NOT_FOUND, "Failed to open input payload file: " + input_path);
    }

    uint64_t fSize = reader.get_size();
    std::cout << "[ENC] Encoding " << fSize / 1024 / 1024 << " MB using " << num_threads << " threads." << std::endl;

    FastStreamWriter fast_out;
    if (!fast_out.open(output_path)) {
        return Result<int>(ErrorCode::ERR_FILE_NOT_FOUND, "Failed to open output.");
    }

    PreHeader hdr{};
    hdr.magic = 0x50524546; 
    hdr.version = 33; 
    hdr.original_size = fSize; 
    hdr.block_count = 0; 
    hdr.use_aes = 0;
    
    if (useAES) {
        hdr.use_aes = 1;
        std::copy(aesKey.begin(), aesKey.end(), hdr.aes_key);
    }
    
    try {
        fast_out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    } catch(const std::exception& e) {
        return Result<int>(ErrorCode::ERR_UNKNOWN, "Initial file structure setup failed.");
    }

    ThreadPool pool(num_threads);
    UI ui(fSize, 0, verbose);
    BlockScanner scanner(reader, fSize, useAES, aesKey, Config::WIN_SIZE_LARGE);
    
    uint64_t pos = 0; 
    uint64_t last_out = 0;
    std::vector<PreBlock> blocks;
    std::deque<std::future<std::shared_ptr<BlockTask>>> pipeline;
    uint32_t good_matches_count = 0; 
    uint32_t fail_count = 0;
    
    std::array<std::atomic<int32_t>, 14> detected_auto_level_by_method;
    for (auto& v : detected_auto_level_by_method) v.store(-1);

    std::vector<uint32_t> usizes_vec(std::begin(Config::TEST_USIZES), std::end(Config::TEST_USIZES));
    ObjectPool<char> gap_pool(num_threads * 2, Config::GAP_POOL_CHUNK);
    
    OodleLZ_CompressOptions opts = {};
    opts.version = 232;
    opts.spaceSpeedTradeoffBytes = tradeoff_bytes;
    opts.sendQuantumCRCs = quantum_crc ? 1 : 0;

    auto process_single_task = [&](std::shared_ptr<BlockTask> task) {
        try {
            if (task->pos > last_out) {
                write_gap(reader, fast_out, gap_pool, last_out, task->pos - last_out);
            }
            
            if (task->exact_match_found) {
                good_matches_count++;
                BlockHeader bh{task->usize};
                fast_out.write(reinterpret_cast<const char*>(&bh), sizeof(bh));
                fast_out.write(reinterpret_cast<const char*>(task->dec_data.data()), task->usize);
                
                std::vector<uint8_t> written(sizeof(BlockHeader) + task->usize);
                std::memcpy(written.data(), &bh, sizeof(BlockHeader));
                std::memcpy(written.data() + sizeof(BlockHeader), task->dec_data.data(), task->usize);
                uint32_t crc = CalculateCRC32(written.data(), written.size());
                
                blocks.push_back({
                    task->pos, static_cast<uint32_t>(sizeof(bh) + bh.stored_size), task->usize, task->csize,
                    static_cast<uint32_t>(task->matched_method | (task->matched_level << 8)), 1,
                    static_cast<uint8_t>(task->is_encrypted ? 1 : 0), 0, crc
                });
                
                if (verbose) {
                    std::ostringstream ss;
                    ss << "Match Found at 0x" << std::hex << task->pos << std::dec
                       << " | " << task->usize << " -> " << task->csize
                       << " [Lvl " << task->matched_level << " Mth:" << task->matched_method << "]";
                    ui.log(ss.str());
                }
            } else {
                fail_count++;
                fast_out.write(reinterpret_cast<const char*>(task->raw_win_buf.data()), task->csize);
                uint32_t crc = CalculateCRC32(task->raw_win_buf.data(), task->csize);
                
                blocks.push_back({
                    task->pos, task->csize, task->usize, task->csize,
                    static_cast<uint32_t>(effective_m_ids[0]), 0, 
                    static_cast<uint8_t>(task->is_encrypted ? 1 : 0), 0, crc
                });
                
                if (verbose) {
                    std::ostringstream ss;
                    ss << "Match Failed at 0x" << std::hex << task->pos << std::dec << " | Tracked and stored verbatim.";
                    ui.log(ss.str());
                }
            }
        } catch(const std::exception& e) {
            std::cerr << "\n[FATAL] I/O error writing output: " << e.what() << std::endl;
            exit(1); // Fatal exit on I/O fail to prevent metadata corruption
        }
        
        last_out = task->pos + task->csize;
        task->raw_win_buf.clear();
        task->dec_data.clear();
    };

    auto last_ui_time = std::chrono::steady_clock::now();
    
    while (true) {
        auto task = scanner.extract_next_block(pos, fSize, usizes_vec);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ui_time).count() >= 500) {
            ui.set_stats(good_matches_count, fail_count);
            ui.update(pos, static_cast<uint32_t>(blocks.size()), "ENC", fast_out.tellp());
            last_ui_time = now;
        }
        if (!task) break;

        pipeline.push_back(pool.enqueue([&effective_m_ids, opt_levels, opt_auto, opt_force, &detected_auto_level_by_method, &opts, task]() {
            size_t required_size = static_cast<size_t>(task->usize) * 2 + 65536;
            std::vector<uint8_t> local_buf_vec(required_size);
            uint8_t* local_buf = local_buf_vec.data();
            
            int32_t static_levels[10];
            int static_num_levels = 0;
            bool use_static_levels = !opt_auto;
            
            if (!opt_levels.empty()) {
                for (int32_t l : opt_levels) static_levels[static_num_levels++] = l;
                use_static_levels = true;
            } else if (opt_auto) {
                // handled per-method
            } else if (opt_force) {
                int32_t def_lvls[] = {3, 4, 5, 6, 7, 8, 9};
                for (int32_t l : def_lvls) static_levels[static_num_levels++] = l;
            } else {
                int32_t def_lvls[] = {4, 5, 6, 7};
                for (int32_t l : def_lvls) static_levels[static_num_levels++] = l;
            }
            
            auto try_method = [&](int32_t method) -> bool {
                if (static_num_levels > 0) {
                    for (int i = 0; i < static_num_levels; ++i) {
                        int32_t test_level = static_levels[i];
                        int64_t res = OodleLZ_Compress(method, task->dec_data.data(), task->usize, 
                                                       local_buf, test_level, &opts, nullptr, nullptr, nullptr, 0);
                        if (res > 0 && res == static_cast<int64_t>(task->csize) && 
                            std::memcmp(task->raw_win_buf.data(), local_buf, task->csize) == 0) {
                            task->exact_match_found = true;
                            task->matched_level = test_level;
                            task->matched_method = method;
                            return true;
                        }
                    }
                    return false;
                }

                if (opt_auto) {
                    int32_t cached = (method >= 0 && method < (int32_t)detected_auto_level_by_method.size())
                        ? detected_auto_level_by_method[method].load() : -1;
                    std::vector<int32_t> levels_to_try = (cached != -1) ? std::vector<int32_t>{cached} : std::vector<int32_t>{4, 6, 7};
                    for (int32_t test_level : levels_to_try) {
                        int64_t res = OodleLZ_Compress(method, task->dec_data.data(), task->usize, local_buf, test_level, &opts, nullptr, nullptr, nullptr, 0);
                        if (res > 0 && res == static_cast<int64_t>(task->csize) && std::memcmp(task->raw_win_buf.data(), local_buf, task->csize) == 0) {
                            task->exact_match_found = true;
                            task->matched_level = test_level;
                            task->matched_method = method;
                            if (cached == -1 && method >= 0 && method < (int32_t)detected_auto_level_by_method.size()) {
                                detected_auto_level_by_method[method].store(test_level);
                            }
                            return true;
                        }
                    }
                }
                return false;
            };            
            
            int32_t known_method = task->matched_method;
            bool is_allowed = false;
            for (int32_t m : effective_m_ids) {
                if (m == known_method) { is_allowed = true; break; }
            }
            if (is_allowed && try_method(known_method)) {
                return task;
            }

            for (int32_t method : effective_m_ids) {
                if (is_allowed && method == known_method) continue;
                if (try_method(method)) return task;
            }
            return task;
        }));
        
        // [FIX] Decreased allowable inflight tasks to 2 per core to save memory
        while (pipeline.size() >= static_cast<size_t>(num_threads * 2)) {
            process_single_task(pipeline.front().get());
            pipeline.pop_front();
        }
    }
    
    while (!pipeline.empty()) {
        process_single_task(pipeline.front().get());
        pipeline.pop_front();
    }
    
    try {
        if (fSize > last_out) {
            write_gap(reader, fast_out, gap_pool, last_out, fSize - last_out);
            fast_out.flush();
        }
        
        hdr.block_count = static_cast<uint32_t>(blocks.size());
        uint64_t out_end = fast_out.tellp();
        fast_out.write(reinterpret_cast<const char*>(blocks.data()), blocks.size() * sizeof(PreBlock));
        fast_out.seekp(0);
        fast_out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    } catch (const std::exception& e) {
        return Result<int>(ErrorCode::ERR_UNKNOWN, std::string("Finalization I/O write failed: ") + e.what());
    }
    
    uint32_t total = good_matches_count + fail_count;
    double match_pct = total > 0 ? static_cast<double>(good_matches_count) / total * 100.0 : 0.0;
    
    std::set<int32_t> u_methods, u_levels;
    for (const auto& b : blocks) {
        if (b.exact_match) {
            u_methods.insert(b.compressor & 0xFF);
            u_levels.insert(b.compressor >> 8);
        }
    }
    
    std::string method_str, level_str;
    for (int m : u_methods) method_str += std::to_string(m) + "+";
    for (int l : u_levels) level_str += std::to_string(l) + "+";
    if (!method_str.empty()) method_str.pop_back();
    if (!level_str.empty()) level_str.pop_back();
    
    // Use fast_out.tellp() safely, no longer blindly trusting
    uint64_t final_size = 0;
    try { final_size = fast_out.tellp(); } catch(...) {}
    
    std::cout << std::endl << "--- Encoder Processing Metrics ---" << std::endl
              << "Input File Size:        " << fSize / 1024.0 / 1024.0 << " MB" << std::endl
              << "Processed Output Size:  " << final_size / 1048576.0 << " MB" << std::endl
              << "Exact Matches: " << good_matches_count << " (" << std::fixed << std::setprecision(2) << match_pct << "%)" << std::endl
              << "Full Fails:    " << fail_count << std::endl
              << "Methods Used: " << (method_str.empty() ? "None" : method_str) << std::endl
              << "Levels Used:  " << (level_str.empty() ? "None" : level_str) << std::endl
              << "Total Duration:    " << ui.format_time(ui.get_elapsed()) << std::endl
              << "Mode: Async Buffered I/O (32MB Ring - Safe)" << std::endl;
              
    return Result<int>(0);
}
