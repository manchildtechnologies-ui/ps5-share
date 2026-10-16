// PS5 Share media scan tests: the read-only capture catalogue.
// Copyright (C) 2026 PS5 Share contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "share/media_store.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

namespace
{

void write_bytes(const std::string &path, const char *data, std::size_t size,
                 std::time_t modified)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    ASSERT_NE(file, nullptr) << path;
    ASSERT_EQ(std::fwrite(data, 1, size, file), size);
    ASSERT_EQ(std::fclose(file), 0);
    struct utimbuf times = {};
    times.actime = modified;
    times.modtime = modified;
    ASSERT_EQ(utime(path.c_str(), &times), 0);
}

void make_dirs(const std::string &root)
{
    for (const char *sub : {"photo", "photo/album", "video", "thumbnails"})
    {
        const std::string path = root + "/" + sub;
        ASSERT_EQ(mkdir(path.c_str(), 0755), 0) << path;
    }
}

TEST(MediaStore, ScansRecursivelyNewestFirstAndIgnoresJunk)
{
    char directory[] = "/tmp/hui-media-XXXXXX";
    ASSERT_NE(mkdtemp(directory), nullptr);
    const std::string root(directory);
    make_dirs(root);
    write_bytes(root + "/photo/album/b_new.jpg", "fakejpeg", 8, 1000001000);
    write_bytes(root + "/photo/album/a_old.png", "fakepng", 7, 1000000000);
    write_bytes(root + "/photo/notes.txt", "junk", 4, 1000002000);
    write_bytes(root + "/video/clip.mp4", "fakemp4", 7, 1000000500);

    ASSERT_EQ(setenv("PS5SHARE_MEDIA_ROOT", root.c_str(), 1), 0);
    hui::share::MediaStore store;
    EXPECT_TRUE(store.refresh());
    ASSERT_EQ(unsetenv("PS5SHARE_MEDIA_ROOT"), 0);

    ASSERT_EQ(store.photos().size(), 2u);
    EXPECT_EQ(store.photos()[0].name, "b_new.jpg");
    EXPECT_EQ(store.photos()[1].name, "a_old.png");
    EXPECT_TRUE(store.photos()[0].latest);
    EXPECT_FALSE(store.photos()[1].latest);
    EXPECT_EQ(store.photos()[0].format, "JPG");
    EXPECT_NE(store.photos()[0].meta.find("JPG"), std::string::npos);
    ASSERT_EQ(store.videos().size(), 1u);
    EXPECT_EQ(store.videos()[0].name, "clip.mp4");
    EXPECT_TRUE(store.videos()[0].video);
    EXPECT_FALSE(store.diagnosis().empty());
}

TEST(MediaStore, MissingRootsAreReportedNotFatal)
{
    ASSERT_EQ(setenv("PS5SHARE_MEDIA_ROOT", "/tmp/hui-media-missing-xyz", 1), 0);
    hui::share::MediaStore store;
    EXPECT_FALSE(store.refresh());
    EXPECT_TRUE(store.photos().empty());
    EXPECT_TRUE(store.videos().empty());
    EXPECT_FALSE(store.diagnosis().empty());
    ASSERT_EQ(unsetenv("PS5SHARE_MEDIA_ROOT"), 0);
}

} // namespace
