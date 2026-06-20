================================================================================
OodleForge: The Kraken Slayer
Advanced OodleLZ Archive Tool | Multi-Method | AES Support
================================================================================

Prologue: The Legend of the Kraken Slayer
-----------------------------------------
In the ancient age of the First Compression, when the world was still young
and the great Archives lay hidden beneath mountains of raw data, a terrible
beast roamed the digital realms - the mighty Kraken of Oodle.

Its tentacles of compressed blocks reached into every dungeon, every dragon
hoard, and every forgotten realm. Heroes fell before its unyielding compression.
Only the bravest archivists dared challenge it.

Legends tell of a lone wanderer who forged a powerful artifact - the Oodle Engine.
Armed with the runes of Kraken, Leviathan, Mermaid, and Selkie, and protected
by the ancient shield of AES-256, this hero became known as...

**THE KRAKEN SLAYER**

This tool is your inheritance. Wield it wisely, adventurer. May your blades
cut through even the densest dungeons, and may your dragons be forever
unpacked.

What is Oddleforger?
--------------
Oddleforger is a high-performance, specialized tool designed to detect, extract,
re-compress, and reconstruct files containing OodleLZ compressed blocks
(Kraken, Leviathan, Mermaid, Selkie).

It was built for reverse engineers, game modders, and archivists who need
to work with modern game data that uses Oodle compression + optional AES-256
encryption.

Whether you're unpacking a massive game archive, analyzing unknown formats,
or rebuilding modified files - Oddleforger has your back.

Features
--------
* Lightning-fast Oodle block scanning & detection
* Support for 4 Oodle compressors: Kraken(8), Leviathan(9), Mermaid(11), Selkie(12)
* Automatic level detection (-auto)
* Brute-force mode (-force)
* Full AES-256-CBC encryption/decryption support
* Async Double-Buffered I/O (64MB Ring) for maximum disk throughput
* Multi-threaded pipeline architecture
* Detailed progress UI + verbose logging
* Scan mode for quick format analysis
* Perfect round-trip reconstruction

=== Technical Architecture ===
1. **FastStreamWriter (Async Double-Buffered)**
- 64 MB in-memory ring buffer
- Dedicated background physical disk worker thread
- True parallelization: Main threads write to RAM while disk flushes in background
- Zero-allocation buffer swapping to prevent heap thrashing
- Reduces HDD write operations from 4K+ to only ~16-17 per GB

2. **Sliding Window + Paced Read**
- 64-128 MiB window for fast random access during scanning
- AES-CBC decryption happens transparently inside the window when needed
- Alignment to 16-byte boundaries for AES

3. **Block Processing Pipeline**
- ThreadPool (default: cores-1, capped at 8)
- Tasks are enqueued for compression verification
- Pipeline draining prevents memory explosion

--------------------------------------------------------------------------------
HOW TO USE
--------------------------------------------------------------------------------

Basic Syntax:
Oddleforger <operation> <input_file> [output_file] [options]

Operations:
e      Encode (Scan + Compress into .oodle archive)
r      Reconstruct (Rebuild original file from .oodle archive)
scan   Analysis mode (Quick scan to detect methods/levels)

--------------------------------------------------------------------------------
FULL OPTIONS REFERENCE
--------------------------------------------------------------------------------

Performance:
-j <num>          Number of worker threads (default: CPU cores - 1, max 8)

Compression Settings:
-m <methods>      Compressor(s) to use. Supports multiple: kraken+leviathan
                  Options: kraken/8, leviathan/9, mermaid/11, selkie/12
-level <n>        Compression level(s). Supports multiple: 4+6+8
-auto             Auto-detect optimal level from first few blocks (recommended)
-force            Bruteforce levels 3-8 (slow but thorough)

Encryption:
-k <hex>          AES-256 key (64 hexadecimal characters)
                  Example: -k 00112233445566778899aabbccddeeff...
--key <hex>       Same as -k

Misc:
-v                Verbose mode - shows every match in real time
-scan <percent>   Only used with "scan" operation (0.1 - 100.0)

--------------------------------------------------------------------------------
EXAMPLES
--------------------------------------------------------------------------------

1. Basic encoding (recommended):
Oddleforger e game_data.bin game_data.oodle -m kraken -auto

2. Multi-method + specific levels:
Oddleforger e assets.pack assets.oodle -m kraken+leviathan -level 5+7

3. With AES encryption:
Oddleforger e secure.bin secure.oodle -k 2b7e151628aed2a6abf7158809cf4f3c...

4. Quick analysis first:
Oddleforger scan bigfile.dat -scan 2.5

5. Reconstruct:
Oddleforger r archive.oodle restored.bin

6. Maximum performance:
Oddleforger e input.bin output.oodle -j 12 -auto

--------------------------------------------------------------------------------
PRO TIPS
--------------------------------------------------------------------------------
* Always run a "-scan 1.0" or "-scan 5.0" first on unknown files.
* Use -auto whenever possible - it usually finds the perfect level.
* Keep your Oodle DLL (oo2core_*.dll or liboo2core.so) in the same folder.
* AES key must be exactly 64 hex characters (256-bit).

--------------------------------------------------------------------------------
TECHNICAL DETAILS
--------------------------------------------------------------------------------
* Header magic: 0x50524546 ("PREF")
* Block magic: 0x424C4B58 ("BLKX")
* Uses large buffered I/O (64MB+ windows)
* Async Double-Buffered Disk Writer (64MB Ring)
* Thread pool with task pipelining
* Exact binary matching verification (no false positives)

================================================================================
Happy Kraken Hunting!
================================================================================

Made with blood, sweat, and too much coffee.
Version 33.3 - Engine-Specific Compatibility, Expanded Method Support & Scanner Robustness.

NOTE:
This project is designed for GCC/Clang (Linux/MinGW). Visual Studio
(MSVC) compatibility is not supported. The project relies on specific
POSIX-compliant headers for threading and dynamic loading
(<dlfcn.h>, <unistd.h>) that are not natively available in the MSVC toolchain.
To build on Windows, use MinGW-w64 (MSYS2).
To build on Linux, ensure you have g++ or clang++ installed with pthread support.

NOTE 2:
Requires the dll oo2core_9_win64.dll to encode/recover/scan
