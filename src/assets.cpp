
#include "assets.hh"

#include "psyqo/kernel.hh"

namespace {

psyqo::CDRomDevice *g_cdrom;
psyqo::ISO9660Parser *g_iso;
psyqo::GPU *g_gpu;
uint32_t g_fileLBA;

ArchiveHeader *g_dir;
alignas(4) uint8_t g_dirBuffer[2048];

alignas(4) uint8_t g_pageBuffer[32768];

bool readSectorsBlocking(uint32_t lba, uint32_t count, void *dest) {
    return g_cdrom->readSectorsBlocking(lba, count, dest, *g_gpu);
}

}

AssetMountResult assetsMount(psyqo::CDRomDevice &cdrom, psyqo::ISO9660Parser &iso,
                             psyqo::GPU &gpu) {
    g_cdrom = &cdrom;
    g_iso = &iso;
    g_gpu = &gpu;

    bool done = false, ok = false;
    iso.initialize([&done, &ok](bool success) {
        ok = success;
        done = true;
    });
    while (!done) gpu.pumpCallbacks();
    if (!ok) return MOUNT_NO_ISO;

    static const char *const kNames[] = {"POOM.DAT;1", "POOM.DAT"};
    psyqo::ISO9660Parser::DirEntry entry;
    entry.LBA = 0;
    for (auto name : kNames) {
        done = false;
        ok = false;
        iso.getDirentry(name, &entry, [&done, &ok](bool success) {
            ok = success;
            done = true;
        });
        while (!done) gpu.pumpCallbacks();
        if (ok && entry.LBA != 0) break;
        entry.LBA = 0;
    }
    if (entry.LBA == 0) return MOUNT_NO_FILE;

    g_fileLBA = entry.LBA;
    if (!readSectorsBlocking(g_fileLBA, 1, g_dirBuffer)) return MOUNT_READ_FAILED;
    g_dir = (ArchiveHeader *)g_dirBuffer;
    if (g_dir->magic[0] != 'P' || g_dir->magic[1] != 'O' ||
        g_dir->magic[2] != 'O' || g_dir->magic[3] != 'M') {
        return MOUNT_BAD_MAGIC;
    }
    return MOUNT_OK;
}

const LumpEntry *assetsFind(const char *name) {
    if (!g_dir) return nullptr;
    for (uint32_t i = 0; i < g_dir->numLumps; i++) {
        const LumpEntry *l = &g_dir->lumps[i];
        int j = 0;
        for (; j < 12 && name[j]; j++) {
            if (l->name[j] != name[j]) break;
        }
        if (name[j] == 0 && (j == 12 || l->name[j] == 0)) return l;
    }
    return nullptr;
}

bool assetsRead(const LumpEntry *lump, void *dest, uint32_t capacity) {
    if (!lump) return false;
    uint32_t sectors = (lump->size + 2047) / 2048;
    if (sectors * 2048 > capacity) return false;
    return readSectorsBlocking(g_fileLBA + lump->offset / 2048, sectors, dest);
}

uint32_t assetsSize(const char *name) {
    const LumpEntry *l = assetsFind(name);
    return l ? l->size : 0;
}

bool assetsLoad(const char *name, void *dest, uint32_t capacity) {
    return assetsRead(assetsFind(name), dest, capacity);
}


bool assetsUploadPage(const char *name, psyqo::GPU &gpu, int pageX, int pageY) {
    const LumpEntry *l = assetsFind(name);
    if (!l) return false;
    if (!assetsRead(l, g_pageBuffer, sizeof(g_pageBuffer))) return false;
    psyqo::Rect region = {.pos = {{.x = (int16_t)(pageX * 64), .y = (int16_t)(pageY * 256)}},
                          .size = {{.w = 64, .h = 256}}};
    gpu.uploadToVRAM((const uint16_t *)g_pageBuffer, region);
    return true;
}

bool assetsUploadCluts(psyqo::GPU &gpu, int x, int y, int count) {
    const LumpEntry *l = assetsFind("CLUT");
    if (!l) return false;
    if (!assetsRead(l, g_pageBuffer, sizeof(g_pageBuffer))) return false;
    psyqo::Rect region = {.pos = {{.x = (int16_t)x, .y = (int16_t)y}},
                          .size = {{.w = 16, .h = (int16_t)count}}};
    gpu.uploadToVRAM((const uint16_t *)g_pageBuffer, region);
    return true;
}
