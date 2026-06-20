#include "common.h"
#include <filesystem>
#include <cctype>

int main(int argc, char** argv) {
std::ios::sync_with_stdio(false);
std::cin.tie(nullptr);

if (argc >= 2) {
std::string first_arg = argv[1];
if (first_arg == "-V" || first_arg == "--version") {
std::cout << "OodleForge v33.3\n"
<< "Advanced Multi-Method Archive Extraction & Reconstruction\n"
<< "Build Date: " << __DATE__ << " " << __TIME__ << "\n"
<< "Architecture: "
#if defined(__x86_64__) || defined(_M_X64)
<< "x86_64 (64-bit)"
#else
<< "Unknown"
#endif
<< "\n";
return 0;
}
}

if (argc < 3) {
std::cout << "=================================================================================\n"
<< "  OODLEFORGE [v33.3] - Advanced Multi-Method Archive Extraction & Reconstruction\n"
<< "==================================================================================\n"
<< "USAGE: oodleforge <operation> <input_file> [output_file] [options]\n"
<< "OPERATIONS:\n"
<< "  e      Encode/Scan   - Scans input, identifies Oodle blocks, and extracts them.\n"
<< "  r      Reconstruct   - Rebuilds the original file from a processed archive.\n"
<< "  scan   Analysis      - Analyzes a percentage of the file to detect optimal settings.\n"
<< "COMPRESSION & DETECTION:\n"
<< "  -m <alg>   Set compressor(s). Supports multiple: kraken+leviathan+hydra\n"
<< "             Options: kraken/8, leviathan/9, mermaid/11, selkie/12, hydra/13.\n"
<< "  -level <n> Set compression level(s). Supports multiple: 4+8 (Range: 0-9).\n"
<< "  -auto      (OFF by default) Scans initial blocks to auto-detect the exact level.\n"
<< "  -force     Ignores cache/defaults and brute-forces levels 3 through 8.\n"
<< "ENGINE SPECIFIC:\n"
<< "  -tradeoff <n> Set spaceSpeedTradeoffBytes (Crucial for Frostbite engine exact matches).\n"
<< "  -quantumcrc   Enable sendQuantumCRCs (Required for The Crew 2 internal CRC checks).\n"
<< "PERFORMANCE & ENCRYPTION:\n"
<< "  -j <num>   Set worker threads. Default: Auto (CPU_Cores - 1).\n"
<< "  -k <hex>   AES-256 key for encrypted archives (must be 64 hex characters).\n"
<< "  -v         Verbose mode. Prints real-time match details to the console.\n"
<< "SCAN OPTIONS:\n"
<< "  -scan <pct> Scan X% of the file to test methods and levels.\n"
<< "GENERAL:\n"
<< "  -h, --help     Show this help message.\n"
<< "  -V, --version  Print version and build information.\n"
<< "  --debug        Enable forensic debug logging to oodleforge_debug.log.\n"
<< "EXAMPLES:\n"
<< "  oodleforge e input.bin output.oodle -m kraken -level 4\n"
<< "  oodleforge e frostbite.bin out.oodle -tradeoff 1024\n"
<< "  oodleforge e crew2.bin out.oodle -quantumcrc\n"
<< "  oodleforge scan input.bin -scan 1.0\n"
<< "  oodleforge r archive.oodle restored.bin\n"
<< "================================================================================\n";
return 0;
}

if (!LoadOodle()) {
std::cerr << "[FATAL] Oodle DLLs could not be loaded. Ensure oo2core_9_win64.dll or liboo2core.so.9 is present." << std::endl;
return static_cast<int>(ErrorCode::ERR_FILE_NOT_FOUND);
}

bool verbose = false, debug_mode = false, opt_auto = false, opt_force = false, quantum_crc = false;
std::vector<int32_t> m_ids = {8}, opt_levels;
int32_t num_threads = std::max(1, (int32_t)std::thread::hardware_concurrency() - 1);
if (num_threads > 8) num_threads = 8;
std::string keyHex;
double scan_percent = 0.0;
uint32_t tradeoff_bytes = 0;

int start_idx = 4;
if (std::string(argv[1]) == "scan") start_idx = 3;

for (int i = start_idx; i < argc; ++i) {
std::string arg = argv[i];
try {
if (arg == "-m" && i + 1 < argc) m_ids = ParseMethods(argv[++i]);
else if (arg == "-v" || arg == "--verbose") verbose = true;
else if (arg == "--debug") debug_mode = true;
else if (arg == "-auto") opt_auto = true;
else if (arg == "-force") opt_force = true;
else if (arg == "-quantumcrc") quantum_crc = true;
else if (arg == "-level" && i + 1 < argc) opt_levels = ParseLevels(argv[++i]);
else if (arg == "-tradeoff" && i + 1 < argc) tradeoff_bytes = std::stoul(argv[++i]);
else if (arg == "-j" && i + 1 < argc) {
num_threads = std::stoi(argv[++i]);
num_threads = std::clamp(num_threads, 1, std::max(1, (int)std::thread::hardware_concurrency()));
}
else if ((arg == "-k" || arg == "--key") && i + 1 < argc) keyHex = argv[++i];
else if (arg == "-scan" && i + 1 < argc) scan_percent = std::clamp(std::stod(argv[++i]), 0.0, 100.0);
} catch (...) {
std::cerr << "[FATAL] Error parsing argument: " << arg << std::endl;
return static_cast<int>(ErrorCode::ERR_INVALID_ARGUMENT);
}
}

Logger::Init(debug_mode);
std::vector<uint8_t> aesKey = ParseKey(keyHex);
bool useAES = !aesKey.empty() && std::any_of(aesKey.begin(), aesKey.end(), [](uint8_t x) { return x != 0; });

std::string op = argv[1];
std::string input = argv[2];
std::string output = (argc > 3) ? argv[3] : "";

if (op == "e") {
try {
auto space = std::filesystem::space(input);
if (space.available < 1024 * 1024 * 1024) {
std::cerr << "[FATAL] Insufficient disk space. At least 1GB free space is required." << std::endl;
return static_cast<int>(ErrorCode::ERR_FILE_NOT_FOUND);
}
} catch (const std::exception& e) {
Logger::Log(Logger::Level::Warn, "Could not check disk space: " + std::string(e.what()));
}
}

auto handle_result = [](Result<int> res) -> int {
if (res.is_err()) {
std::cerr << "[FATAL] " << res.get_error() << std::endl;
return static_cast<int>(res.get_code());
}
return res.get_value();
};

if (op == "scan") return handle_result(RunScan(input, verbose, num_threads, m_ids, aesKey, useAES, scan_percent, debug_mode, tradeoff_bytes, quantum_crc));
if (op == "e") return handle_result(RunEncode(input, output, verbose, num_threads, m_ids, opt_levels, opt_auto, opt_force, aesKey, useAES, tradeoff_bytes, quantum_crc));
if (op == "r") return handle_result(RunReconstruct(input, output, verbose, num_threads, aesKey, useAES, tradeoff_bytes, quantum_crc));

std::cerr << "[FATAL] Unknown operation: " << op << std::endl;
return static_cast<int>(ErrorCode::ERR_INVALID_ARGUMENT);
}
