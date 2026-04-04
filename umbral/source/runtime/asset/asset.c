// asset.c — runtime asset registry backed by .umpack v2 files.
// compressed entries are LZ4-decompressed into rt_alloc'd buffers on init.

#include "asset.h"
#include <sys/log.h>

#include <lz4.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef UM_DEBUG
#define UM_DEBUG 1
#endif

typedef uint64_t um_alloc_handle_t;
um_alloc_handle_t rt_alloc(uint64_t size, uint64_t align, uint32_t tag,
                            uint32_t site);
void              rt_free(uint64_t h, uint32_t site);
um_slice_u8_t     rt_slice_from_alloc(uint64_t h, uint64_t elem_size,
                                      uint64_t elem_align, uint64_t elem_len,
                                      uint32_t site, uint32_t mut_flag);

#define ASSET_ALLOC_TAG 0x41535400u
#define RT_MAX_ASSETS   1024u
#define RT_MAX_DECOMPRESSED 1024u

// .umpack v2 constants
#define UMPACK_MAGIC       0x554D504Bu
#define UMPACK_ENDIAN_LE   0x1234u
#define UMPACK_META_RAW    0u
#define UMPACK_META_IMAGE  1u
#define UMPACK_META_AUDIO  2u

static uint64_t asset_fnv1a64(const char *s, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
    return h;
}

typedef struct {
    uint64_t       id;
    const uint8_t *ptr;        // decompressed data (view or alloc'd buffer)
    uint64_t       len;
    uint32_t       meta_type;
    uint32_t       meta[4];
} asset_entry_t;

typedef struct {
    um_alloc_handle_t pack_data_handle;
    uint8_t          *pack_data;
    uint64_t          pack_len;

    asset_entry_t     entries[RT_MAX_ASSETS];
    uint32_t          entry_count;

    um_alloc_handle_t decompressed[RT_MAX_DECOMPRESSED];
    uint32_t          decompressed_count;
} asset_pack_t;

static asset_pack_t *pack_ptr(asset_pack_handle_t h) {
    um_slice_u8_t s = rt_slice_from_alloc(h, 1, 1, sizeof(asset_pack_t), 0, 1);
    return (asset_pack_t *)(uintptr_t)s.ptr;
}

static void track_decompressed(asset_pack_t *p, um_alloc_handle_t h) {
    if (p->decompressed_count < RT_MAX_DECOMPRESSED)
        p->decompressed[p->decompressed_count++] = h;
}

asset_pack_handle_t rt_assets_init(const uint8_t *path_ptr, uint64_t path_len) {
    char path_buf[512];
    if (path_len >= sizeof(path_buf)) {
        RT_LOG_ERROR("asset", "pack path too long");
        return ASSET_NULL_HANDLE;
    }
    memcpy(path_buf, path_ptr, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "rb");

    // fallback: try the executable's directory if not found in cwd
    char exe_path[512];
    if (!f) {
        ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (n > 0) {
            exe_path[n] = '\0';
            // find last slash and replace filename
            char *slash = strrchr(exe_path, '/');
            if (slash && (size_t)(slash - exe_path + 1 + path_len) < sizeof(exe_path)) {
                memcpy(slash + 1, path_ptr, path_len);
                slash[1 + path_len] = '\0';
                f = fopen(exe_path, "rb");
            }
        }
    }

    if (!f) {
        RT_LOG_ERROR("asset", "cannot open '%s'", path_buf);
        return ASSET_NULL_HANDLE;
    }
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    rewind(f);
    if (file_sz <= 0) { fclose(f); return ASSET_NULL_HANDLE; }

    um_alloc_handle_t ph = rt_alloc(sizeof(asset_pack_t),
                                     _Alignof(asset_pack_t),
                                     ASSET_ALLOC_TAG, 0);
    if (!ph) { fclose(f); return ASSET_NULL_HANDLE; }

    asset_pack_t *p = pack_ptr(ph);
    memset(p, 0, sizeof(*p));

    p->pack_data_handle = rt_alloc((uint64_t)file_sz, 8, ASSET_ALLOC_TAG, 0);
    if (!p->pack_data_handle) {
        fclose(f);
        rt_free(ph, 0);
        return ASSET_NULL_HANDLE;
    }
    um_slice_u8_t ds = rt_slice_from_alloc(
        p->pack_data_handle, 1, 1, (uint64_t)file_sz, 0, 1);
    p->pack_data = (uint8_t *)(uintptr_t)ds.ptr;
    p->pack_len  = (uint64_t)file_sz;

    fread(p->pack_data, 1, (size_t)file_sz, f);
    fclose(f);

    if (p->pack_len < 16) goto bad;
    uint32_t magic;
    uint16_t version, endian;
    uint32_t entry_count;
    memcpy(&magic, p->pack_data + 0, 4);
    memcpy(&version, p->pack_data + 4, 2);
    memcpy(&endian, p->pack_data + 6, 2);
    memcpy(&entry_count, p->pack_data + 12, 4);

    if (magic != UMPACK_MAGIC || endian != UMPACK_ENDIAN_LE) goto bad;

    int has_meta = (version >= 2);
    RT_LOG_DEBUG("asset", "init: version=%u entries=%u has_meta=%d pack_len=%lu",
                 version, entry_count, has_meta, (unsigned long)p->pack_len);

    uint64_t cur = 16;
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (cur + 4 > p->pack_len) goto bad;
        uint32_t name_len;
        memcpy(&name_len, p->pack_data + cur, 4);
        cur += 4;

        uint32_t entry_tail = has_meta ? 36u : 16u;
        if (cur + name_len + entry_tail > p->pack_len) goto bad;

        const char *name = (const char *)(p->pack_data + cur);
        cur += name_len;

        uint64_t data_offset;
        uint32_t compressed_len, original_len;
        memcpy(&data_offset, p->pack_data + cur, 8);   cur += 8;
        memcpy(&compressed_len, p->pack_data + cur, 4); cur += 4;
        memcpy(&original_len, p->pack_data + cur, 4);   cur += 4;

        uint32_t meta_type = 0;
        uint32_t meta[4] = {0};
        if (has_meta) {
            memcpy(&meta_type, p->pack_data + cur, 4); cur += 4;
            memcpy(meta, p->pack_data + cur, 16);       cur += 16;
        }

        if (p->entry_count >= RT_MAX_ASSETS) break;
        if (data_offset + compressed_len > p->pack_len) goto bad;

        asset_entry_t *e = &p->entries[p->entry_count++];
        e->id        = asset_fnv1a64(name, name_len);
        e->meta_type = meta_type;
        RT_LOG_TRACE("asset", "  [%u] '%.*s' id=0x%lx off=%lu comp=%u orig=%u meta=%u",
                     p->entry_count - 1, (int)name_len, name,
                     (unsigned long)e->id, (unsigned long)data_offset,
                     compressed_len, original_len, meta_type);
        memcpy(e->meta, meta, sizeof(e->meta));

        if (compressed_len == original_len) {
            e->ptr = p->pack_data + data_offset;
            e->len = original_len;
        } else {
            um_alloc_handle_t dh = rt_alloc(original_len, 8, ASSET_ALLOC_TAG, 0);
            if (!dh) {
                RT_LOG_ERROR("asset", "decompress alloc failed");
                continue;
            }
            um_slice_u8_t dslice = rt_slice_from_alloc(dh, 1, 1, original_len, 0, 1);
            uint8_t *dst = (uint8_t *)(uintptr_t)dslice.ptr;

            int dec_sz = LZ4_decompress_safe(
                (const char *)(p->pack_data + data_offset),
                (char *)dst,
                (int)compressed_len,
                (int)original_len);
            if (dec_sz < 0 || (uint32_t)dec_sz != original_len) {
                RT_LOG_ERROR("asset", "LZ4 decompress failed for '%.*s'",
                             (int)name_len, name);
                rt_free(dh, 0);
                p->entry_count--;
                continue;
            }
            e->ptr = dst;
            e->len = original_len;
            track_decompressed(p, dh);
        }
    }
    return (asset_pack_handle_t)ph;

bad:
    RT_LOG_ERROR("asset", "malformed pack file '%s'", path_buf);
    rt_free(p->pack_data_handle, 0);
    rt_free(ph, 0);
    return ASSET_NULL_HANDLE;
}

void rt_assets_cleanup(asset_pack_handle_t pack) {
    if (!pack) return;
    asset_pack_t *p = pack_ptr(pack);
    if (!p) return;
    for (uint32_t i = 0; i < p->decompressed_count; ++i)
        if (p->decompressed[i]) rt_free(p->decompressed[i], 0);
    if (p->pack_data_handle) rt_free(p->pack_data_handle, 0);
    rt_free(pack, 0);
}

um_slice_u8_t rt_asset_load(asset_pack_handle_t pack, uint64_t id) {
    asset_pack_t *p = pack_ptr(pack);
    if (!p) return (um_slice_u8_t){NULL, 0};
    for (uint32_t i = 0; i < p->entry_count; ++i)
        if (p->entries[i].id == id) {
            RT_LOG_TRACE("asset", "load: id=0x%lx → %lu bytes @ %p (meta=%u)",
                         (unsigned long)id, (unsigned long)p->entries[i].len,
                         (void *)p->entries[i].ptr, p->entries[i].meta_type);
            return (um_slice_u8_t){p->entries[i].ptr, p->entries[i].len};
        }
    RT_LOG_WARN("asset", "load: id=0x%lx NOT FOUND (%u entries)",
                (unsigned long)id, p->entry_count);
    return (um_slice_u8_t){NULL, 0};
}

uint64_t rt_asset_id_from_name(const uint8_t *name, uint64_t len) {
    return asset_fnv1a64((const char *)name, (size_t)len);
}

static asset_entry_t *find_entry(asset_pack_t *p, uint64_t id) {
    for (uint32_t i = 0; i < p->entry_count; ++i)
        if (p->entries[i].id == id) return &p->entries[i];
    return NULL;
}

void rt_asset_image_meta(asset_pack_handle_t pack, uint64_t id,
                         uint32_t *out_w, uint32_t *out_h) {
    if (!pack) return;
    asset_pack_t *p = pack_ptr(pack);
    if (!p) return;
    asset_entry_t *e = find_entry(p, id);
    if (!e || e->meta_type != UMPACK_META_IMAGE) return;
    *out_w = e->meta[0];
    *out_h = e->meta[1];
}

void rt_asset_audio_meta(asset_pack_handle_t pack, uint64_t id,
                         uint64_t *out_frame_count, uint32_t *out_channels,
                         uint32_t *out_sample_rate) {
    if (!pack) return;
    asset_pack_t *p = pack_ptr(pack);
    if (!p) return;
    asset_entry_t *e = find_entry(p, id);
    if (!e || e->meta_type != UMPACK_META_AUDIO) return;
    *out_frame_count = (uint64_t)e->meta[0] | ((uint64_t)e->meta[1] << 32);
    *out_channels    = e->meta[2];
    *out_sample_rate = e->meta[3];
}
