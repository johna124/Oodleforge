#include "common.h"
#include <filesystem>

Result<int> RunEncode(const std::string& input_path, const std::string& output_path, bool verbose, int num_threads, const std::vector<int32_t>& m_ids, const std::vector<int32_t>& opt_levels, bool opt_auto, bool opt_force, const std::vector<uint8_t>& aesKey, bool useAES) {
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
    fast_out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    ThreadPool pool(num_threads);
    UI ui(fSize, 0, verbose);
    BlockScanner scanner(reader, fSize, useAES, aesKey, Config::WIN_SIZE_LARGE);
    uint64_t pos = 0;
    uint64_t last_out = 0;
    std::vector<PreBlock> blocks;
    std::deque<std::future<std::shared_ptr<BlockTask>>> pipeline;
    uint32_t good_matches_count = 0;
    uint32_t fail_count = 0;
    std::atomic<int32_t> detected_auto_level{-1};
    std::vector<uint32_t> usizes_vec(std::begin(Config::TEST_USIZES), std::end(Config::TEST_USIZES));
    ObjectPool<char> gap_pool(num_threads * 2, Config::GAP_POOL_CHUNK);

    auto process_single_task = [&](std::shared_ptr<BlockTask> task) {
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
                   << " [Lvl " << task->matched_level << " Mth:" << task->matched_method << "]"
                   << (task->is_encrypted ? " [AES]" : "");
                ui.log(ss.str());
            }
        } else {
            fail_count++;
            fast_out.write(reinterpret_cast<const char*>(task->raw_win_buf.data()), task->csize);
            uint32_t crc = CalculateCRC32(task->raw_win_buf.data(), task->csize);
            blocks.push_back({
                task->pos, task->csize, task->usize, task->csize,
                static_cast<uint32_t>(m_ids[0]), 0,
                static_cast<uint8_t>(task->is_encrypted ? 1 : 0), 0, crc
            });
            if (verbose) {
                std::ostringstream ss;
                ss << "Match Failed at 0x" << std::hex << task->pos << std::dec << " | Tracked and stored verbatim.";
                ui.log(ss.str());
            }
        }
        last_out = task->pos + task->csize;
        task->raw_win_buf.clear(); task->raw_win_buf.shrink_to_fit();
        task->dec_data.clear(); task->dec_data.shrink_to_fit();
    };

    auto last_ui_time = std::chrono::steady_clock::now();

    while (auto task = scanner.extract_next_block(pos, fSize, usizes_vec)) {
        pipeline.push_back(pool.enqueue([task, m_ids, opt_levels, opt_auto, opt_force, &detected_auto_level]() {
            thread_local std::vector<uint8_t> local_temp;
            size_t required_size = static_cast<size_t>(task->usize) * 2 + 4096;
            if (local_temp.size() < required_size) local_temp.resize(required_size);

            int32_t levels_to_check[9];
            int num_levels = 0;
            if (!opt_levels.empty()) {
                for (int32_t l : opt_levels) levels_to_check[num_levels++] = l;
            } else if (opt_auto) {
                int32_t cached = detected_auto_level.load();
                if (cached != -1) levels_to_check[num_levels++] = cached;
                else { levels_to_check[num_levels++] = 4; levels_to_check[num_levels++] = 6; levels_to_check[num_levels++] = 7; }
            } else if (opt_force) {
                int32_t force_lvls[] = {3, 4, 5, 6, 7, 8};
                for (int32_t l : force_lvls) levels_to_check[num_levels++] = l;
            } else {
                int32_t def_lvls[] = {4, 5, 6, 7};
                for (int32_t l : def_lvls) levels_to_check[num_levels++] = l;
            }

            for (int32_t method : m_ids) {
                for (int i = 0; i < num_levels; ++i) {
                    int32_t test_level = levels_to_check[i];
                    int64_t res = OodleLZ_Compress(method, task->dec_data.data(), task->usize, local_temp.data(), test_level, nullptr, nullptr, nullptr, 0);
                    if (res > 0 && res == static_cast<int64_t>(task->csize)) {
                        if (std::memcmp(task->raw_win_buf.data(), local_temp.data(), task->csize) == 0) {
                            task->exact_match_found = true;
                            task->matched_level = test_level;
                            task->matched_method = method;
                            if (opt_auto && detected_auto_level.load() == -1) detected_auto_level.store(test_level);
                            goto done_search;
                        }
                    }
                }
            }
            done_search:
            return task;
        }));

        while (pipeline.size() >= static_cast<size_t>(num_threads * 3)) {
            process_single_task(pipeline.front().get());
            pipeline.pop_front();
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ui_time).count() >= 1000) {
            ui.set_stats(good_matches_count, fail_count);
            ui.update(last_out, static_cast<uint32_t>(blocks.size()), "ENC", fast_out.tellp());
            last_ui_time = now;
        }
    }

    while (!pipeline.empty()) {
        process_single_task(pipeline.front().get());
        pipeline.pop_front();
    }

    if (fSize > last_out) {
        write_gap(reader, fast_out, gap_pool, last_out, fSize - last_out);
        fast_out.flush();
    }

    hdr.block_count = static_cast<uint32_t>(blocks.size());
    uint64_t out_end = fast_out.tellp();
    fast_out.write(reinterpret_cast<const char*>(blocks.data()), blocks.size() * sizeof(PreBlock));
    fast_out.seekp(0);
    fast_out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

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

    std::cout << std::endl << "--- Encoder Processing Metrics ---" << std::endl
              << "Input File Size:        " << fSize / 1024.0 / 1024.0 << " MB" << std::endl
              << "Processed Output Size:  " << out_end / 1048576.0 << " MB" << std::endl
              << "Exact Matches: " << good_matches_count << " (" << std::fixed << std::setprecision(2) << match_pct << "%)" << std::endl
              << "Full Fails:    " << fail_count << std::endl
              << "Methods Used: " << (method_str.empty() ? "None" : method_str) << std::endl
              << "Levels Used:  " << (level_str.empty() ? "None" : level_str) << std::endl
              << "Total Scan Duration:    " << ui.format_time(ui.get_elapsed()) << std::endl
              << "Mode: Async Buffered I/O (64MB Ring)" << std::endl;

    return Result<int>(0);
}
