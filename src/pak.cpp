// Controller Pak emulation for Rush 2.
//
// The runtime's osPfs* functions always report that no Controller Pak is inserted, so the game's own copies
// are routed here instead (renamed rush2_osPfs* in tools/gen_syms.py and ignored in us.toml). This implements
// the libultra Controller Pak file system on top of a standard 32KB .mpk image (the same layout emulators use),
// stored next to the runtime's save file as saves/<game id>.mpk.
//
// Image layout (256-byte pages, 32-byte blocks):
//   page 0     ID area (copies in blocks 1, 3, 4 and 6) and label (block 7)
//   page 1     inode table: one big-endian u16 per page, next page in the file / 1 = last page / 3 = free;
//              entry 0's low byte holds the checksum of entries 5-127
//   page 2     inode table backup
//   pages 3-4  directory: 16 entries of 32 bytes
//   pages 5+   file data
//
// Port 1 holds the Controller Pak. Other ports report a Rumble Pak, which the game detects by osPfsInitPak
// failing with PFS_ERR_DEVICE and osMotorInit succeeding. Port 1 rumbles too (see rush2_enable_pak_rumble).

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <vector>

#include "recomp.h"
#include "rush2_hooks.h"
#include "car2049.h"
#include "collectibles.h"
#include "track2049.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/files.hpp"

namespace {
    constexpr int32_t pak_port = 0;

    constexpr size_t pak_size = 0x8000;
    constexpr size_t page_size = 0x100;
    constexpr size_t block_size = 0x20;
    constexpr int num_pages = 128;
    constexpr int inode_page = 1;
    constexpr int inode_backup_page = 2;
    constexpr int dir_page = 3;
    constexpr int dir_entries = 16;
    constexpr int first_data_page = 5;
    constexpr uint16_t inode_last = 1;
    constexpr uint16_t inode_free = 3;
    constexpr int id_blocks[] = { 1, 3, 4, 6 };
    constexpr int label_block = 7;
    constexpr size_t id_size = 0x20;

    constexpr int32_t PFS_ERR_NOPACK = 1;
    constexpr int32_t PFS_ERR_INCONSISTENT = 3;
    constexpr int32_t PFS_ERR_INVALID = 5;
    constexpr int32_t PFS_DATA_FULL = 7;
    constexpr int32_t PFS_DIR_FULL = 8;
    constexpr int32_t PFS_ERR_EXIST = 9;
    constexpr int32_t PFS_ERR_DEVICE = 11;
    constexpr int32_t PFS_INITIALIZED = 1;
    constexpr int32_t PFS_READ = 0;
    constexpr uint8_t DIR_STATUS_OCCUPIED = 2;

    // OSPfs field offsets.
    constexpr int pfs_status = 0x00;
    constexpr int pfs_queue = 0x04;
    constexpr int pfs_channel = 0x08;
    constexpr int pfs_id = 0x0C;
    constexpr int pfs_label = 0x2C;
    constexpr int pfs_version = 0x4C;
    constexpr int pfs_dir_size = 0x50;
    constexpr int pfs_inode_table = 0x54;
    constexpr int pfs_minode_table = 0x58;
    constexpr int pfs_dir_table = 0x5C;
    constexpr int pfs_inode_start_page = 0x60;
    constexpr int pfs_banks = 0x64;
    constexpr int pfs_activebank = 0x65;

    // __OSDir field offsets.
    constexpr int dir_game_code = 0x00;
    constexpr int dir_company_code = 0x04;
    constexpr int dir_start_page = 0x06;
    constexpr int dir_status = 0x08;
    constexpr int dir_ext_name = 0x0C;
    constexpr int dir_game_name = 0x10;
    constexpr size_t ext_name_len = 4;
    constexpr size_t game_name_len = 16;

    std::mutex pak_mutex;
    std::array<uint8_t, pak_size> pak{};
    std::filesystem::path pak_path;

    uint16_t read16(size_t offset) {
        return (uint16_t)((pak[offset] << 8) | pak[offset + 1]);
    }

    uint32_t read32(size_t offset) {
        return ((uint32_t)read16(offset) << 16) | read16(offset + 2);
    }

    void write16(size_t offset, uint16_t value) {
        pak[offset + 0] = (uint8_t)(value >> 8);
        pak[offset + 1] = (uint8_t)value;
    }

    void write32(size_t offset, uint32_t value) {
        write16(offset + 0, (uint16_t)(value >> 16));
        write16(offset + 2, (uint16_t)value);
    }

    // __osIdCheckSum: sums of the first 14 halfwords and of their complements.
    void id_checksum(size_t id_offset, uint16_t& sum, uint16_t& inverted_sum) {
        sum = 0;
        inverted_sum = 0;
        for (size_t i = 0; i < id_size - 4; i += 2) {
            uint16_t data = read16(id_offset + i);
            sum += data;
            inverted_sum += (uint16_t)~data;
        }
    }

    bool id_valid(size_t id_offset) {
        uint16_t sum, inverted_sum;
        id_checksum(id_offset, sum, inverted_sum);
        return read16(id_offset + 0x1C) == sum && read16(id_offset + 0x1E) == inverted_sum;
    }

    // Writes a new pack ID (random serial, 1 bank) to every ID block.
    void write_new_id() {
        std::random_device rd;
        size_t id = id_blocks[0] * block_size;
        std::fill_n(pak.begin() + id, id_size, 0);
        write32(id + 0x00, 0xFFFFFFFF); // repaired
        write32(id + 0x04, rd());       // random
        write32(id + 0x08, rd());       // serial_mid
        write32(id + 0x0C, rd());
        write32(id + 0x10, rd());       // serial_low
        write32(id + 0x14, rd());
        write16(id + 0x18, 0x0001);     // deviceid (bit 0 set: ID is valid)
        pak[id + 0x1A] = 1;             // banks
        pak[id + 0x1B] = 0;             // version
        uint16_t sum, inverted_sum;
        id_checksum(id, sum, inverted_sum);
        write16(id + 0x1C, sum);
        write16(id + 0x1E, inverted_sum);
        for (int block : id_blocks) {
            std::copy_n(pak.begin() + id, id_size, pak.begin() + block * block_size);
        }
    }

    uint8_t inode_checksum(int page) {
        uint8_t sum = 0;
        for (size_t i = first_data_page * 2; i < page_size; i++) {
            sum += pak[page * page_size + i];
        }
        return sum;
    }

    bool inode_valid(int page) {
        return pak[page * page_size + 1] == inode_checksum(page);
    }

    uint16_t inode_get(int page) {
        return read16(inode_page * page_size + page * 2);
    }

    void inode_set(int page, uint16_t value) {
        write16(inode_page * page_size + page * 2, value);
    }

    // Updates the inode checksum and copies the table to its backup.
    void inode_commit() {
        pak[inode_page * page_size + 1] = inode_checksum(inode_page);
        std::copy_n(pak.begin() + inode_page * page_size, page_size, pak.begin() + inode_backup_page * page_size);
    }

    size_t dir_offset(int file_no) {
        return dir_page * page_size + file_no * block_size;
    }

    bool dir_used(int file_no) {
        size_t dir = dir_offset(file_no);
        return read32(dir + dir_game_code) != 0 && read16(dir + dir_company_code) != 0;
    }

    // Collects the pages of a file in order. Returns false if the chain is broken or crosses pages marked in `used`.
    bool file_pages(int file_no, std::vector<int>& pages, std::array<bool, num_pages>& used) {
        pages.clear();
        uint16_t cur = read16(dir_offset(file_no) + dir_start_page);
        while (true) {
            if (cur < first_data_page || cur >= num_pages || used[cur]) {
                return false;
            }
            used[cur] = true;
            pages.push_back(cur);
            uint16_t next = inode_get(cur);
            if (next == inode_last) {
                return true;
            }
            cur = next;
        }
    }

    bool file_pages(int file_no, std::vector<int>& pages) {
        std::array<bool, num_pages> used{};
        return file_pages(file_no, pages, used);
    }

    // Equivalent of osPfsChecker: restores the ID and inode table from their backups if needed, drops directory
    // entries with broken page chains and frees pages that don't belong to any file. Returns true if the image changed.
    bool repair() {
        auto before = pak;

        if (!id_valid(id_blocks[0] * block_size)) {
            bool restored = false;
            for (int block : id_blocks) {
                if (id_valid(block * block_size)) {
                    std::copy_n(pak.begin() + block * block_size, id_size, pak.begin() + id_blocks[0] * block_size);
                    restored = true;
                    break;
                }
            }
            if (!restored) {
                write_new_id();
            }
        }
        for (int block : id_blocks) {
            std::copy_n(pak.begin() + id_blocks[0] * block_size, id_size, pak.begin() + block * block_size);
        }

        if (!inode_valid(inode_page) && inode_valid(inode_backup_page)) {
            std::copy_n(pak.begin() + inode_backup_page * page_size, page_size, pak.begin() + inode_page * page_size);
        }

        std::array<bool, num_pages> used{};
        std::vector<int> pages;
        for (int file_no = 0; file_no < dir_entries; file_no++) {
            if (!dir_used(file_no)) {
                continue;
            }
            std::array<bool, num_pages> file_used = used;
            if (file_pages(file_no, pages, file_used)) {
                used = file_used;
            }
            else {
                fprintf(stderr, "[Controller Pak] Removing file %d with a broken page chain\n", file_no);
                std::fill_n(pak.begin() + dir_offset(file_no), block_size, 0);
            }
        }
        for (int page = first_data_page; page < num_pages; page++) {
            if (!used[page]) {
                inode_set(page, inode_free);
            }
        }
        inode_commit();

        return pak != before;
    }

    void save() {
        {
            std::ofstream file = recomp::open_output_file_with_backup(pak_path, std::ios_base::binary);
            if (file.good()) {
                file.write(reinterpret_cast<const char*>(pak.data()), pak.size());
            }
            if (!file.good()) {
                fprintf(stderr, "[Controller Pak] Failed to write %s\n", pak_path.string().c_str());
                return;
            }
        }
        if (!recomp::finalize_output_file_with_backup(pak_path)) {
            fprintf(stderr, "[Controller Pak] Failed to write %s\n", pak_path.string().c_str());
        }
    }

    // Loads the pak image from disk, creating a formatted one if it doesn't exist.
    bool load() {
        std::filesystem::path save_path = ultramodern::get_save_file_path();
        if (save_path.empty()) {
            return false;
        }
        pak_path = save_path.replace_extension(".mpk");

        pak.fill(0);
        bool found = false;
        {
            std::ifstream file = recomp::open_input_file_with_backup(pak_path, std::ios_base::binary);
            if (file.good()) {
                file.read(reinterpret_cast<char*>(pak.data()), pak.size());
                found = true;
            }
        }

        if (!found) {
            printf("[Controller Pak] Creating %s\n", pak_path.string().c_str());
            std::error_code ec;
            std::filesystem::create_directories(pak_path.parent_path(), ec);
            pak.fill(0);
        }
        if (repair() || !found) {
            save();
        }
        return true;
    }

    gpr stack_arg(recomp_context* ctx, int index) {
        return ctx->r29 + 0x10 + index * 4;
    }

    // Returns true if the OSPfs refers to the emulated Controller Pak.
    bool is_pak(uint8_t* rdram, gpr pfs) {
        return MEM_W(pfs_channel, pfs) == pak_port;
    }

    bool names_match(uint8_t* rdram, size_t dir, gpr game_name, gpr ext_name) {
        for (size_t i = 0; i < game_name_len; i++) {
            if (pak[dir + dir_game_name + i] != (uint8_t)MEM_B(i, game_name)) {
                return false;
            }
        }
        for (size_t i = 0; i < ext_name_len; i++) {
            if (pak[dir + dir_ext_name + i] != (uint8_t)MEM_B(i, ext_name)) {
                return false;
            }
        }
        return true;
    }

    int find_file(uint8_t* rdram, uint16_t company_code, uint32_t game_code, gpr game_name, gpr ext_name) {
        for (int file_no = 0; file_no < dir_entries; file_no++) {
            size_t dir = dir_offset(file_no);
            if (read16(dir + dir_company_code) == company_code && read32(dir + dir_game_code) == game_code &&
                names_match(rdram, dir, game_name, ext_name)) {
                return file_no;
            }
        }
        return -1;
    }
}

// s32 osPfsInitPak(OSMesgQueue* queue, OSPfs* pfs, s32 channel)
extern "C" void rush2_osPfsInitPak(uint8_t* rdram, recomp_context* ctx) {
    gpr queue = ctx->r4;
    gpr pfs = ctx->r5;
    int32_t channel = (int32_t)ctx->r6;

    MEM_W(pfs_status, pfs) = 0;
    MEM_W(pfs_queue, pfs) = (int32_t)queue;
    MEM_W(pfs_channel, pfs) = channel;

    if (channel != pak_port) {
        // Rumble Pak: the ID area isn't readable.
        ctx->r2 = PFS_ERR_DEVICE;
        return;
    }

    std::lock_guard lock{ pak_mutex };
    if (!load()) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }

    size_t id = id_blocks[0] * block_size;
    for (size_t i = 0; i < id_size; i++) {
        MEM_B(pfs_id + i, pfs) = (int8_t)pak[id + i];
        MEM_B(pfs_label + i, pfs) = (int8_t)pak[label_block * block_size + i];
    }
    MEM_W(pfs_version, pfs) = pak[id + 0x1B];
    MEM_W(pfs_dir_size, pfs) = dir_entries;
    MEM_W(pfs_inode_table, pfs) = inode_page * 8;
    MEM_W(pfs_minode_table, pfs) = inode_backup_page * 8;
    MEM_W(pfs_dir_table, pfs) = dir_page * 8;
    MEM_W(pfs_inode_start_page, pfs) = first_data_page;
    MEM_B(pfs_banks, pfs) = 1;
    MEM_B(pfs_activebank, pfs) = 0;
    MEM_W(pfs_status, pfs) = PFS_INITIALIZED;

    ctx->r2 = 0;
}

// s32 osPfsChecker(OSPfs* pfs)
extern "C" void rush2_osPfsChecker(uint8_t* rdram, recomp_context* ctx) {
    if (!is_pak(rdram, ctx->r4)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    if (repair()) {
        save();
    }
    ctx->r2 = 0;
}

// s32 osPfsFreeBlocks(OSPfs* pfs, s32* bytes_not_used)
extern "C" void rush2_osPfsFreeBlocks(uint8_t* rdram, recomp_context* ctx) {
    if (!is_pak(rdram, ctx->r4)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    int32_t free_pages = 0;
    for (int page = first_data_page; page < num_pages; page++) {
        if (inode_get(page) == inode_free) {
            free_pages++;
        }
    }
    MEM_W(0, ctx->r5) = free_pages * (int32_t)page_size;
    ctx->r2 = 0;
}

// s32 osPfsFindFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name, s32* file_no)
extern "C" void rush2_osPfsFindFile(uint8_t* rdram, recomp_context* ctx) {
    gpr file_no_ptr = (int32_t)MEM_W(0, stack_arg(ctx, 1));
    if (!is_pak(rdram, ctx->r4)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    int file_no = find_file(rdram, (uint16_t)ctx->r5, (uint32_t)ctx->r6, ctx->r7, (int32_t)MEM_W(0, stack_arg(ctx, 0)));
    MEM_W(0, file_no_ptr) = file_no;
    ctx->r2 = file_no < 0 ? PFS_ERR_INVALID : 0;
}

// s32 osPfsAllocateFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name, s32 length, s32* file_no)
extern "C" void rush2_osPfsAllocateFile(uint8_t* rdram, recomp_context* ctx) {
    gpr pfs = ctx->r4;
    uint16_t company_code = (uint16_t)ctx->r5;
    uint32_t game_code = (uint32_t)ctx->r6;
    gpr game_name = ctx->r7;
    gpr ext_name = (int32_t)MEM_W(0, stack_arg(ctx, 0));
    int32_t length = MEM_W(0, stack_arg(ctx, 1));
    gpr file_no_ptr = (int32_t)MEM_W(0, stack_arg(ctx, 2));

    if (!is_pak(rdram, pfs)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    if (company_code == 0 || game_code == 0 || length <= 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    std::lock_guard lock{ pak_mutex };
    int existing = find_file(rdram, company_code, game_code, game_name, ext_name);
    if (existing >= 0) {
        MEM_W(0, file_no_ptr) = existing;
        ctx->r2 = PFS_ERR_EXIST;
        return;
    }

    int file_no = 0;
    while (file_no < dir_entries && dir_used(file_no)) {
        file_no++;
    }
    if (file_no == dir_entries) {
        ctx->r2 = PFS_DIR_FULL;
        return;
    }

    std::vector<int> pages;
    int needed = (length + (int)page_size - 1) / (int)page_size;
    for (int page = first_data_page; page < num_pages && (int)pages.size() < needed; page++) {
        if (inode_get(page) == inode_free) {
            pages.push_back(page);
        }
    }
    if ((int)pages.size() < needed) {
        ctx->r2 = PFS_DATA_FULL;
        return;
    }

    for (size_t i = 0; i < pages.size(); i++) {
        inode_set(pages[i], i + 1 < pages.size() ? (uint16_t)pages[i + 1] : inode_last);
    }
    inode_commit();

    size_t dir = dir_offset(file_no);
    std::fill_n(pak.begin() + dir, block_size, 0);
    write32(dir + dir_game_code, game_code);
    write16(dir + dir_company_code, company_code);
    write16(dir + dir_start_page, (uint16_t)pages[0]);
    pak[dir + dir_status] = DIR_STATUS_OCCUPIED;
    for (size_t i = 0; i < ext_name_len; i++) {
        pak[dir + dir_ext_name + i] = (uint8_t)MEM_B(i, ext_name);
    }
    for (size_t i = 0; i < game_name_len; i++) {
        pak[dir + dir_game_name + i] = (uint8_t)MEM_B(i, game_name);
    }
    save();

    MEM_W(0, file_no_ptr) = file_no;
    ctx->r2 = 0;
}

// s32 osPfsDeleteFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name)
extern "C" void rush2_osPfsDeleteFile(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company_code = (uint16_t)ctx->r5;
    uint32_t game_code = (uint32_t)ctx->r6;
    if (!is_pak(rdram, ctx->r4)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    if (company_code == 0 || game_code == 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    std::lock_guard lock{ pak_mutex };
    int file_no = find_file(rdram, company_code, game_code, ctx->r7, (int32_t)MEM_W(0, stack_arg(ctx, 0)));
    if (file_no < 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    std::vector<int> pages;
    if (file_pages(file_no, pages)) {
        for (int page : pages) {
            inode_set(page, inode_free);
        }
        inode_commit();
    }
    std::fill_n(pak.begin() + dir_offset(file_no), block_size, 0);
    save();

    ctx->r2 = 0;
}

// s32 osPfsFileState(OSPfs* pfs, s32 file_no, OSPfsState* state)
extern "C" void rush2_osPfsFileState(uint8_t* rdram, recomp_context* ctx) {
    int32_t file_no = (int32_t)ctx->r5;
    gpr state = ctx->r6;
    if (!is_pak(rdram, ctx->r4)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    if (file_no < 0 || file_no >= dir_entries) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    std::lock_guard lock{ pak_mutex };
    if (!dir_used(file_no)) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    std::vector<int> pages;
    if (!file_pages(file_no, pages)) {
        ctx->r2 = PFS_ERR_INCONSISTENT;
        return;
    }

    // OSPfsState: u32 file_size, u32 game_code, u16 company_code, char ext_name[4], char game_name[16]
    size_t dir = dir_offset(file_no);
    MEM_W(0x00, state) = (int32_t)(pages.size() * page_size);
    MEM_W(0x04, state) = (int32_t)read32(dir + dir_game_code);
    MEM_H(0x08, state) = (int16_t)read16(dir + dir_company_code);
    for (size_t i = 0; i < ext_name_len; i++) {
        MEM_B(0x0A + i, state) = (int8_t)pak[dir + dir_ext_name + i];
    }
    for (size_t i = 0; i < game_name_len; i++) {
        MEM_B(0x0E + i, state) = (int8_t)pak[dir + dir_game_name + i];
    }
    ctx->r2 = 0;
}

// s32 osPfsReadWriteFile(OSPfs* pfs, s32 file_no, u8 flag, int offset, int size_in_bytes, u8* data_buffer)
extern "C" void rush2_osPfsReadWriteFile(uint8_t* rdram, recomp_context* ctx) {
    int32_t file_no = (int32_t)ctx->r5;
    int32_t flag = (uint8_t)ctx->r6;
    int32_t offset = (int32_t)ctx->r7;
    int32_t size = MEM_W(0, stack_arg(ctx, 0));
    gpr buffer = (int32_t)MEM_W(0, stack_arg(ctx, 1));

    if (!is_pak(rdram, ctx->r4)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    if (file_no < 0 || file_no >= dir_entries || size <= 0 || size % block_size != 0 || offset < 0 || offset % block_size != 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    std::lock_guard lock{ pak_mutex };
    if (!dir_used(file_no)) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    std::vector<int> pages;
    if (!file_pages(file_no, pages)) {
        ctx->r2 = PFS_ERR_INCONSISTENT;
        return;
    }
    if ((size_t)offset + size > pages.size() * page_size) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    for (int32_t i = 0; i < size; i++) {
        size_t pos = offset + i;
        size_t pak_offset = pages[pos / page_size] * page_size + pos % page_size;
        if (flag == PFS_READ) {
            MEM_B(i, buffer) = (int8_t)pak[pak_offset];
        }
        else {
            pak[pak_offset] = (uint8_t)MEM_B(i, buffer);
        }
    }
    if (flag != PFS_READ) {
        save();
    }

    ctx->r2 = 0;
}

// Hook in the game's pak thread (func_80098D14), right after a Controller Pak in port $s1 initialized
// successfully into the OSPfs at $sp+0xD0. Registers the port with the rumble code the same way the thread
// does for a Rumble Pak: the rumble update (func_80063E4C) drives osMotorStart/osMotorStop with the OSPfs
// copy at D_800D5298[port] for every port set in the D_800D4E75 mask. The Rumble Pak flag (D_800D35F4)
// stays clear, since the game uses it to treat the port as having no Controller Pak.
extern "C" void rush2_enable_pak_rumble(uint8_t* rdram, recomp_context* ctx) {
    constexpr gpr motor_pfs_array = (gpr)(int32_t)0x800D5298;
    constexpr gpr rumble_mask = (gpr)(int32_t)0x800D4E75;
    constexpr int pfs_size = 0x68;

    uint32_t port = (uint32_t)ctx->r17;
    if (port >= 4) {
        return;
    }
    gpr src = ctx->r29 + 0xD0;
    gpr dst = motor_pfs_array + port * pfs_size;
    for (int i = 0; i < pfs_size; i += 4) {
        MEM_W(i, dst) = MEM_W(i, src);
    }
    MEM_B(0, rumble_mask) = (int8_t)((uint8_t)MEM_B(0, rumble_mask) | (1 << port));
}

// Save menu without Controller Paks. There is only one pak (port 1), so the select player screen (input
// func_803B4D88, drawing func_803C1F44) skips everything that is about choosing or managing one:
// - CREATE PLAYER normally opens a controller list (state 2) where A on a controller decides, from its pak status,
//   between "no Rush 2 note, create one?" (3, whose YES is func_803B36A8: create the note, then name entry),
//   "no more entries, delete a player?" (4) and name entry on that pak (8). Port 1 is picked and that decision is
//   made right away, with the note created without asking.
// - Every way back that returns to the controller list returns to the player list (1) instead.
// - Player names and prompts drop the pak number and the Controller Pak wording.
namespace {
    constexpr gpr menu_state = (gpr)(int32_t)0x803D0598;    // s32 per player.
    constexpr gpr menu_pak = (gpr)(int32_t)0x803D05C8;      // s16 per player: the chosen controller.
    constexpr gpr pak_status = (gpr)(int32_t)0x800D9D98;    // 8 bytes per port, filled by func_800B22B8.
    constexpr int status_present = 0;
    constexpr int status_has_note = 2;
    constexpr int status_entries_left = 6;                  // s16.

    enum MenuState {
        state_player_list = 1,
        state_controller_list = 2,
        state_entries_full = 4,
        state_name_entry = 8,
    };

    void write_string(uint8_t* rdram, uint32_t addr, const char* text) {
        size_t i = 0;
        do {
            MEM_B(0, (gpr)(int32_t)(addr + i)) = (int8_t)text[i];
        } while (text[i++] != '\0');
    }
}

extern "C" void func_800B22B8(uint8_t* rdram, recomp_context* ctx); // Fills the pak status table from the pak flags.
extern "C" void func_803B36A8(uint8_t* rdram, recomp_context* ctx); // Creates the note for the player in $s0.

// func_803B2BB4 entry: sets the state of the player in $s0 to $a0 and runs its setup. Returns nonzero when the state
// change was replaced and func_803B2BB4 must return right away.
extern "C" int rush2_pak_menu_state(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r4 != state_controller_list) {
        return 0;
    }
    int player = (int)ctx->r16;
    int32_t current = MEM_W(player * 4, menu_state);
    // The status table is only refreshed while the controller list is up.
    recomp_context saved = *ctx;
    func_800B22B8(rdram, ctx);
    *ctx = saved;
    gpr status = pak_status + pak_port * 8;
    if (current != state_player_list || MEM_BU(status_present, status) == 0) {
        ctx->r4 = state_player_list;
        return 0;
    }
    MEM_H(player * 2, menu_pak) = (int16_t)pak_port;
    if (MEM_BU(status_has_note, status) == 0) {
        func_803B36A8(rdram, ctx);
        *ctx = saved;
        return 1;
    }
    ctx->r4 = MEM_H(status_entries_left, status) == 0 ? state_entries_full : state_name_entry;
    return 0;
}

// func_803B276C (the player list's setup) at 0x803B2A44, after the cursor ($t4) is placed: on the remembered player,
// or else on JUST PLAY (0). The list ($v1 entries) starts with JUST PLAY and CREATE PLAYER, then the saved players;
// when there are any, the first one is picked instead of JUST PLAY.
extern "C" void rush2_pak_menu_default_player(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_H(0, ctx->r12) == 0 && (int32_t)ctx->r3 > 2) {
        MEM_H(0, ctx->r12) = 2;
    }
}

// func_800A62C0 after the menu overlay is loaded.
extern "C" void rush2_pak_menu_overlay_loaded(uint8_t* rdram, recomp_context* ctx) {
    write_string(rdram, 0x803CA010, "%s");          // Player list: "%s (%d)", name and pak number.
    write_string(rdram, 0x803CA1E8, "%s");          // Records player choice: "%s (PAK %d)".
    write_string(rdram, 0x803CA294, "?");           // Clear records / delete player prompt: " %s %d%s", ON CTLR PAK n?
    write_string(rdram, 0x800CD170, "THERE ARE NO MORE ENTRIES AVAILABLE.");
}

// Deleting a player: the 2049 car options and selected car kept for its record (car2049.json, by record address), its
// 2049 and SF Rush records (track2049_records.json, by name) and its SF Rush keys and 2049 coins (collectibles.json,
// by name) go too, so a player created in its place or with its name starts fresh.
namespace {
    constexpr uint32_t pak_records = 0x8004B220;        // 4 paks of 0x2200 bytes: 5 player records of 0x6C0 each.
    constexpr uint32_t pak_stride = 0x2200;
    constexpr uint32_t record_size = 0x6C0;
    constexpr int records_per_pak = 5;

    void forget_player(uint8_t* rdram, int profile) {
        uint32_t record = pak_records + (profile / records_per_pak) * pak_stride + (profile % records_per_pak) * record_size;
        rush2::car2049::forget_record(rdram, record);
        rush2::track2049::clear_profile_records(rdram, profile);
        rush2::collectibles::clear_profile(rdram, profile);
    }
}

// Start of func_800B306C (Records > DELETE PLAYER): $a0 = the profile index (pak * 5 + record).
extern "C" void rush2_player_deleted(uint8_t* rdram, recomp_context* ctx) {
    int profile = (int32_t)ctx->r4;
    if (profile >= 0 && profile < 4 * records_per_pak) {
        forget_player(rdram, profile);
    }
}

// func_803B3CAC (the select player screen's delete list) at 0x803B3F74: $s2 = the record being deleted.
extern "C" void rush2_player_deleted_from_list(uint8_t* rdram, recomp_context* ctx) {
    uint32_t offset = (uint32_t)ctx->r18 - pak_records;
    if (offset < 4 * pak_stride && offset % pak_stride < records_per_pak * record_size &&
        offset % pak_stride % record_size == 0) {
        forget_player(rdram, (offset / pak_stride) * records_per_pak + offset % pak_stride / record_size);
    }
}
