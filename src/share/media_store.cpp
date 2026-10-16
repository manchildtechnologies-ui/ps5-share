// PS5 Share media store implementation.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#include "share/media_store.hpp"

#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hui::share
{

// Nearest-box downscale to a tile-sized RGBA image. A short human-readable
// reason is stored in *error when the tile cannot be produced.
std::uint32_t MediaStore::upload_tile(gfx::GlBatch &batch, const std::string &path, int &w,
                                       int &h, std::string *error)
{
    w = 0;
    h = 0;
    int src_w = 0, src_h = 0, comp = 0;
    if (!stbi_info(path.c_str(), &src_w, &src_h, &comp) || src_w <= 0 || src_h <= 0)
    {
        if (error != nullptr)
            *error = std::string("unsupported file (") + stbi_failure_reason() + ")";
        return 0;
    }
    if (src_w > 7680 || src_h > 4320)
    {
        if (error != nullptr)
        {
            char dims[48];
            std::snprintf(dims, sizeof(dims), "too large %dx%d", src_w, src_h);
            *error = dims;
        }
        return 0;
    }
    unsigned char *pixels = stbi_load(path.c_str(), &src_w, &src_h, &comp, 4);
    if (!pixels)
    {
        if (error != nullptr)
            *error = std::string("decode failed (") + stbi_failure_reason() + ")";
        return 0;
    }
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > kTileWidth)
    {
        dst_h = src_h * kTileWidth / src_w;
        dst_w = kTileWidth;
    }
    std::vector<unsigned char> small(static_cast<std::size_t>(dst_w) * dst_h * 4);
    for (int y = 0; y < dst_h; ++y)
    {
        const int sy0 = y * src_h / dst_h;
        const int sy1 = (y + 1) * src_h / dst_h;
        for (int x = 0; x < dst_w; ++x)
        {
            const int sx0 = x * src_w / dst_w;
            const int sx1 = (x + 1) * src_w / dst_w;
            unsigned sum[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx)
                {
                    const unsigned char *p = pixels + (sy * src_w + sx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                    ++n;
                }
            unsigned char *d = small.data() + (y * dst_w + x) * 4;
            d[0] = static_cast<unsigned char>(sum[0] / (n ? n : 1));
            d[1] = static_cast<unsigned char>(sum[1] / (n ? n : 1));
            d[2] = static_cast<unsigned char>(sum[2] / (n ? n : 1));
            d[3] = static_cast<unsigned char>(sum[3] / (n ? n : 1));
        }
    }
    stbi_image_free(pixels);
    const std::uint32_t texture = batch.create_texture(dst_w, dst_h, small.data());
    if (texture != 0)
    {
        w = dst_w;
        h = dst_h;
    }
    else if (error != nullptr)
    {
        char dims[48];
        std::snprintf(dims, sizeof(dims), "preview texture failed %dx%d", dst_w, dst_h);
        *error = dims;
    }
    return texture;
}

// Does this thumbnail file belong to the capture with stem `wanted`?
// Accepts an exact stem match, a double extension ("<capture>.mp4.jpg"),
// and a prefixed name ("<capture>_thumb.jpg").
bool MediaStore::thumb_matches(const std::string &thumb_name, const std::string &wanted)
{
    std::string stem = stem_lower(thumb_name);
    for (int strip = 0; strip < 3; ++strip)
    {
        if (stem == wanted)
            return true;
        if (stem.size() > wanted.size() && stem.compare(0, wanted.size(), wanted) == 0)
            return true;
        const std::size_t dot = stem.find_last_of('.');
        if (dot == std::string::npos)
            break;
        stem.erase(dot);
    }
    return stem == wanted;
}

void MediaStore::update(gfx::GlBatch &batch)
{
    if (pending_ >= photos_.size())
    {
        if (pending_video_ >= videos_.size())
            return;
        // Videos: only a stored thumbnail can provide a preview (the
        // .mp4 itself does not decode as an image). Without one the hub
        // draws its play-triangle placeholder tile.
        Capture &cap = videos_[pending_video_++];
        const std::string wanted = stem_lower(cap.name);
        for (const Entry &thumb : thumbnails_)
        {
            if (!thumb_matches(thumb.name, wanted))
                continue;
            int w = 0, h = 0;
            cap.texture = upload_tile(batch, thumb.absolute, w, h, nullptr);
            if (cap.texture != 0)
            {
                cap.texture_width = w;
                cap.texture_height = h;
                return;
            }
            break;
        }
        cap.preview_failed = true;
        cap.preview_error = "No thumbnail stored for this video";
        return;
    }
    Capture &cap = photos_[pending_++];
    // Thumbnail store first (small files), the capture itself as fallback.
    const std::string wanted = stem_lower(cap.name);
    for (const Entry &thumb : thumbnails_)
    {
        if (stem_lower(thumb.name) != wanted)
            continue;
        int w = 0, h = 0;
        cap.texture = upload_tile(batch, thumb.absolute, w, h, nullptr);
        if (cap.texture != 0)
        {
            cap.texture_width = w;
            cap.texture_height = h;
            return;
        }
        break;
    }
    int w = 0, h = 0;
    std::string error;
    cap.texture = upload_tile(batch, cap.absolute_path, w, h, &error);
    cap.texture_width = w;
    cap.texture_height = h;
    if (cap.texture == 0)
    {
        cap.preview_failed = true;
        cap.preview_error = error;
        sys::log("[HUI] tile failed %s: %s", cap.name.c_str(), error.c_str());
    }
}

} // namespace hui::share
