OodleForge Technical Reference
Version: 33.4 Scanner Robustness, Advanced Block Detection, and Critical Buffer Fixes.
Date: June 28, 2026
Status: Production-Ready | Native Linux Support | Dynamic Discovery Engine

This document contains complete low-level technical details of the OodleForge 
archive format, data structures, algorithms, and implementation notes.

================================================================================
1. ARCHIVE FILE FORMAT (.oodle)
================================================================================

1.1 PreHeader (64 bytes at offset 0)

struct PreHeader {
    uint32_t magic;           // 0x50524546 ("PREF")
    uint32_t version;         // 33
    uint64_t original_size;   // Original file size
    uint32_t block_count;     // Number of blocks
    uint8_t  use_aes;         // 0 = disabled, 1 = enabled
    uint8_t  aes_key[32];     // AES-256 key (if use_aes == 1)
    uint8_t  reserved[23];    // Padding to 64 bytes
} __attribute__((packed));

1.2 File Layout Overview

[ PreHeader (64 bytes) ]
[ Block Data Region (variable) ]
    Gap data (raw bytes from original file)
    Block 0 data (BlockHeader + decompressed data OR raw compressed data)
    Gap data
    Block 1 data
    ...
[ PreBlock Metadata Array (at END of file) ]
    PreBlock[0]
    PreBlock[1]
    ...
    PreBlock[block_count-1]

The metadata is stored at the end to allow streaming writes during encoding.

================================================================================
2. PREBLOCK STRUCTURE (40 bytes per entry)
================================================================================

struct PreBlock {
    uint64_t original_offset;          // Offset in original file
    uint32_t stored_size;              // Size of data stored in .oodle
    uint32_t decompressed_size;        // Uncompressed size (usize)
    uint32_t original_compressed_size; // Original compressed size
    uint32_t compressor;               // method | (level << 8)
    uint8_t  exact_match;              // 1 = exact match
    uint8_t  was_encrypted;            // 1 = was AES encrypted
    uint8_t  reserved;                 // 1 byte padding
    uint32_t crc32;                    // CRC32 of data as written
} __attribute__((packed));

================================================================================
3. BLOCK TYPES
================================================================================

exact_match = 1: Exact Oodle match
- Stored as: BlockHeader (4 bytes) + decompressed data
- Reconstruction: Re-compress using saved method + level

exact_match = 0: Raw / Failed match
- Stored as: Original compressed Oodle data (verbatim)
- Reconstruction: Copied as-is

BlockHeader (only for exact_match blocks):
struct BlockHeader {
    uint32_t stored_size;   // == decompressed_size
} __attribute__((packed));

================================================================================
4. EXAMPLE HEX DUMPS
================================================================================

4.1 PreHeader Example (AES enabled)

00000000  46 52 45 50 21 00 00 00 00 00 00 00 00 00 00 00  PREF!...........
00000010  00 00 00 00 00 00 00 00 01 2b 7e 15 16 28 ae d2 a6  .........+~..(..
00000020  ab f7 15 88 09 cf 4f 3c 00 00 00 00 00 00 00 00 00  ......O<........
00000030  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................

- magic = 50 52 45 46 ("PREF")
- version = 33
- use_aes = 01
- AES key starts at byte 0x11

4.2 PreBlock Example (Exact Match, Kraken Level 5)

00000000  00 00 00 00 00 00 00 00 00 10 00 00 00 00 10 00  ................
00000010  00 00 10 00 05 08 00 00 01 01 00 00 ab cd ef 12  ................

- original_offset = 0
- stored_size = 4096
- decompressed_size = 4096
- compressor = 0x00000805 (Method 8 / Level 5)
- exact_match = 1
- was_encrypted = 1

================================================================================
5. CORE ALGORITHMS (v33.4 Architecture)
================================================================================

5.1 Dynamic Discovery Block Scanning

Unlike legacy tools that guess block sizes, Oodleforge uses a 3-tier strategy:

1. Dynamic Discovery (Primary):
   - Allocates a persistent 32MB chain_probe_buf_
   - Passes raw header to OodleLZ_Decompress with massive output buffer
   - Oodle self-terminates and returns exact usize
   - Catches 100% of blocks, including non-standard sizes

2. Brute Force Fallback (Secondary):
   - Iterates through Config::TEST_USIZES
   - Finds valid decompression match

3. WalkOodleChain (Tertiary):
   - Parses multi-segment chain headers
   - Uses Config::MIN_VALID_FIRST_SEGMENT = 8
   - Prevents false rejections

5.2 Relaxed Validation Logic

To prevent rejecting valid uncompressed or slightly-expanded blocks:

OLD: if (found_csize > 0 && found_csize < usize && found_csize >= 16)

NEW: if (found_csize > 0 && found_csize <= found_usize + 1024 && found_csize >= 16)

This allows csize to be up to **1024 bytes larger** than usize, catching:
- Uncompressed blocks (csize > usize due to header overhead)
- Blocks where compression slightly expanded the data

5.3 Exact Match Detection

- Tries multiple method + level combinations
- Uses OodleLZ_Compress + memcmp for perfect byte-for-byte match
- Successful pairs are cached across threads using std::shared_mutex
- Prevents "Thundering Herd" problem

5.4 I/O Optimizations (The Memory Beast)

ThreadSafeReader:
- Uses pread() on Linux for thread-safe, lock-free random reads
- Platform-specific fast random reads

FastStreamWriter (Async Double-Buffered):
- 64 MB in-memory ring buffer
- Dedicated background physical disk worker thread
- True parallelization: Main encoder threads write to RAM at ~50 GB/s while 
  the drive flushes 64MB chunks in the background
- Zero-allocation buffer swapping using std::swap
- Reduces HDD write operations from 4000+ to only ~16-17 per GB
- pending_disk_bytes tracker synchronizes UI with background thread

5.5 AES-256-CBC Encryption

- Uses tiny-AES-C implementation
- Per-block encryption/decryption
- Standard CBC mode (IV = previous ciphertext)
- Padded to 16-byte boundaries
- IV zero-initialized with calloc (fixed in v33.2)

================================================================================
6. THREADING & PERFORMANCE
================================================================================

- ThreadPool with bounded queue
- Default max 8 threads
- Pipeline pattern with backpressure
- Thread-local buffers to reduce memory pressure
- ObjectPool for gap writing (prevents heap fragmentation)

================================================================================
7. CHANGELOG
================================================================================

OodleForge v33.4 Changelog (June 28, 2026)
Focus: Dynamic Discovery, Native Linux Dominance

MAJOR FEATURES:
- Dynamic Discovery Engine:
  * 32MB probe buffer lets Oodle dictate block sizes

- Relaxed Block Validation:
  * Updated to csize <= usize + 1024
  * Successfully captures uncompressed blocks
  * Captures blocks where compression expanded data

- Native Linux I/O:
  * Full dlopen and pread implementation
  * First native Linux Oodle precompressor

CRITICAL FIXES:
- Chain Walker Fix:
  * Replaced hardcoded 16 with Config::MIN_VALID_FIRST_SEGMENT = 8
  * Recovers valid micro-segments

- Memory Thrashing Fix:
  * Moved 32MB probe buffer allocation outside while loop
  * Prevents OS memory manager overload on large files


OodleForge v33.3 Changelog (June 2026)
- Engine-Specific Compatibility
- Expanded Method Support
- Scanner Robustness

OodleForge v33.2 Changelog (June 18, 2026)
Focus: Threading Stability, Cryptographic Integrity, and Resource Efficiency

CRITICAL FIXES:
- FastStreamWriter Thread-Safety:
  * Implemented file_io_mtx to serialize physical disk I/O
  * Prevents file handle contention during multi-threaded writes
  * Added pending_disk_bytes tracker for UI synchronization

- AES Cryptographic Correction:
  * IV Initialization: Swapped malloc for calloc in AES_Context_Create
  * Ensures IV is always zero-initialized
  * Key Length: Updated aes.h to force AES256 mode

- Scanner Cache (Performance Optimization):
  * Added "double-check" lock pattern in TryMatchBlock
  * Prevents "Thundering Herd" problem
  * Multiple threads no longer brute-force same block simultaneously

PORTABILITY & BUILD:
- POSIX Compatibility: Sanitized LoadOodle() for Linux/Unix
- Memory Management: Replaced GCC-specific packing with #pragma pack

UI & MONITORING:
- Fixed seekp implementation for background thread synchronization
- Real-time throughput keyed to pending_disk_bytes count

OodleForge v33.1 Changelog (June 2026)
- The Memory Beast: Async Double-Buffered I/O
- Major performance improvements
- Pacing removed

OodleForge v33.0 Changelog
- Multi-Method Edition
- Full support for Kraken + Leviathan + Mermaid + Selkie + Hydra
- -auto / -force modes

================================================================================
8. KNOWN LIMITATIONS
================================================================================

- Max recommended threads: 8
- AES assumes standard per-block CBC
- Scanning can have false positives on random data
- Requires matching oo2core DLL/SO version for perfect reconstruction
- No support for Oodle dictionaries
- Best performance on SSD

================================================================================
9. WINE COMPATIBILITY
================================================================================

This tool runs very well under Wine without problems. The "it actually works" 
factor is high. Tested on Linux via Wine.

================================================================================
10. BUILD INSTRUCTIONS
================================================================================

Linux (GCC/Clang):
g++ -O3 -std=c++17 -pthread -lz scan.cpp encode.cpp reconstruct.cpp common.cpp main.cpp -o oodleforge -ldl

Windows (MinGW):
make

Required flags:
- O3: Maximum optimization
- std=c++17: C++17 standard
- pthread: Threading support
- lz: zlib compression
- ldl: Dynamic library loading (Linux)

================================================================================
11. CONFIGURATION CONSTANTS
================================================================================

From common.h:

TEST_USIZES[] = {
    1048576, 655360, 524288, 393216, 327680, 262144, 196608, 131072,
    98304, 65536, 49152, 32768, 24576, 16384, 12288, 8192,
    4096, 2048
}

ALL_METHODS[] = {8, 9, 11, 12, 13}
ALL_LEVELS[] = {1, 2, 3, 4, 5, 6, 7, 8, 9}

MIN_VALID_FIRST_SEGMENT = 8
MAX_SAFE_COMPRESSED_SIZE = 100 MB
GAP_POOL_CHUNK = 16 MB

Magic bytes:
MAGIC_COMPRESSED_FIRST   = 0x8C
MAGIC_UNCOMPRESSED_FIRST = 0xCC
MAGIC_COMPRESSED_CHAIN   = 0x0C
MAGIC_UNCOMPRESSED_CHAIN = 0x4C

================================================================================
END OF TECHNICAL REFERENCE
================================================================================

For more information, visit:
https://github.com/johna124/Oodleforge

Developed in 45 days on legacy hardware.
The Kraken is free.

