#pragma once

#include "data.hh"
#include "psyqo/cdrom-device.hh"
#include "psyqo/gpu.hh"
#include "psyqo/iso9660-parser.hh"

enum AssetMountResult {
    MOUNT_OK = 0,
    MOUNT_NO_ISO,
    MOUNT_NO_FILE,
    MOUNT_READ_FAILED,
    MOUNT_BAD_MAGIC,
};

AssetMountResult assetsMount(psyqo::CDRomDevice &cdrom, psyqo::ISO9660Parser &iso,
                             psyqo::GPU &gpu);
const LumpEntry *assetsFind(const char *name);
bool assetsRead(const LumpEntry *lump, void *dest, uint32_t capacity);
bool assetsLoad(const char *name, void *dest, uint32_t capacity);
uint32_t assetsSize(const char *name);
bool assetsUploadPage(const char *name, psyqo::GPU &gpu, int pageX, int pageY);
bool assetsUploadCluts(psyqo::GPU &gpu, int x, int y, int count);
