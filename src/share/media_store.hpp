// PS5 Share media store: read-only capture scan plus tile textures.
// Copyright (C) 2026 PS5 Share contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scans the console capture roots (the same path-based scan the SDL app
// proved), decodes each photo to a small tile texture (thumbnail store
// first, the capture itself as fallback), and hands the hub real items.
// Uploads are budgeted: refresh() only scans, update() decodes and uploads
// at most one tile per call so frames never hitch.

#pragma once

#include "gfx/gl_batch.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hui::share
{

struct Capture
{
    std::string name;         // file name
    std::string meta;         // "Oct 15, 2026 · JPG · 1.1 MB"
    std::string format;       // "JPG", "MP4", ...
    std::string absolute_path;
    bool video = false;
    bool latest = false; // newest of its tab
    std::uint32_t texture = 0;
    int texture_width = 0;
    int texture_height = 0;
    bool preview_failed = false;  // both thumbnail and capture decode failed
    std::string preview_error;    // short reason, shown in the detail panel
    double duration_seconds = -1.0; // MP4 header parse; -1: unknown
    int width = 0;                  // MP4 track dimensions; 0: unknown
    int height = 0;
};

// "1:23" / "0:47" / "1:02:05" for a duration in seconds.
std::string format_duration(double seconds);

class MediaStore
{
  public:
    MediaStore() = default;
    MediaStore(const MediaStore &) = delete;
    MediaStore &operator=(const MediaStore &) = delete;

    // Scan the roots (PS5SHARE_MEDIA_ROOT overrides /av_contents on hosts).
    // Never decodes: safe to call up front.
    bool refresh();
    // Decode and upload at most one pending tile texture.
    void update(gfx::GlBatch &batch);

    const std::vector<Capture> &photos() const
    {
        return photos_;
    }
    const std::vector<Capture> &videos() const
    {
        return videos_;
    }
    bool empty() const
    {
        return photos_.empty() && videos_.empty();
    }
    // Captures still awaiting their tile decode/upload pass.
    std::size_t pending_tiles() const;
    // Captures with no usable preview (decode failed / no video thumbnail).
    std::size_t failed_tiles() const;
    // Diagnostics for the empty-gallery state.
    std::string diagnosis() const;
    // What the thumbnail store scan found ("<root>: N files, first: <name>").
    std::string thumbnail_info() const
    {
        return thumb_info_;
    }

  private:
    static constexpr int kTileWidth = 480; // tile textures stay small; tiles draw 250 wide
    struct Entry
    {
        std::string name;
        std::string relative;
        std::string absolute;
        std::uint64_t bytes = 0;
        std::int64_t modified = 0;
    };
    struct Listing
    {
        std::string root;
        bool accessible = false;
        bool warnings = false;
        std::string error;
        std::vector<Entry> entries;
    };

    static Listing scan_root(const char *primary, const char *fallback);
    static void scan_dir(const std::string &root, const std::string &parent, unsigned depth,
                         Listing &listing);
    static std::string stem_lower(const std::string &name);
    static bool thumb_matches(const std::string &thumb_name, const std::string &wanted);
    static std::uint32_t upload_tile(gfx::GlBatch &batch, const std::string &path, int &w,
                                     int &h, std::string *error);
    static std::string describe(std::int64_t modified, std::uint64_t bytes,
                                const std::string &format);

    std::vector<Capture> photos_;
    std::vector<Capture> videos_;
    std::vector<Entry> thumbnails_;
    std::string thumb_info_;
    std::string diagnosis_;
    std::size_t pending_ = 0;       // index into photos_ awaiting upload
    std::size_t pending_video_ = 0; // index into videos_ awaiting a thumbnail
};

} // namespace hui::share
