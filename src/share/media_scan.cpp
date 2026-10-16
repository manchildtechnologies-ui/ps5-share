// PS5 Share media store implementation.
#include "share/media_store.hpp"

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

#ifdef HUI_PS5_BUILD
#include <cstdint>
// Directory enumeration for titles mirrors the proven SDL title's
// platform layer exactly: kernel descriptor from sceKernelOpen, entries
// from the getdents import read in 64 KiB chunks. Smaller chunks are
// refused with EINVAL by this kernel path, and raw syscall instructions
// trap in a title, so neither shortcut is available here.
extern "C" int sceKernelOpen(const char *path, int flags, int mode);
extern "C" int sceKernelClose(int fd);
#endif

namespace hui::share
{

namespace
{

constexpr unsigned kMaxDepth = 8;
constexpr std::size_t kMaxEntries = 20000;



std::string join_path(const std::string &root, const std::string &relative)
{
    return relative.empty() ? root : root + "/" + relative;
}

std::string lower_ext(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool is_photo_name(const std::string &name)
{
    const std::string ext = lower_ext(name);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
           ext == ".bmp";
}

bool is_video_name(const std::string &name)
{
    const std::string ext = lower_ext(name);
    return ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".mkv" ||
           ext == ".webm" || ext == ".avi";
}

} // namespace

std::string MediaStore::stem_lower(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return stem;
}

void MediaStore::scan_dir(const std::string &root, const std::string &parent, unsigned depth,
                          Listing &listing)
{
    const std::string dir_path = parent.empty() ? root : join_path(root, parent);
    // Enumerate this level first; the per-entry processing below is shared.
    // libc readdir inside console mounts returns nothing on this runtime
    // (the framework works around the same gap with build-time index files),
    // so the PS5 build enumerates with getdents() instead.
    std::vector<std::pair<std::string, unsigned char>> names;
#ifdef HUI_PS5_BUILD
    {
        // Kernel-native open + list (see the note at the top of this file).
        // Errors are recorded as raw return codes: kernel-style imports do
        // not set errno, so strerror text would be stale garbage.
        constexpr int kKernelReadOnly = 0x00000000;
        constexpr int kKernelDirectory = 0x00020000;
        const int fd =
            sceKernelOpen(dir_path.c_str(), kKernelReadOnly | kKernelDirectory, 0);
        if (fd < 0)
        {
            if (listing.error.empty())
                listing.error = "kopen ret=" + std::to_string(fd);
            listing.warnings = true;
            return;
        }
        struct KernelDirent
        {
            std::uint32_t fileno;
            std::uint16_t reclen;
            std::uint8_t type;
            std::uint8_t namlen;
            char name[256];
        };
        // One-call whole-directory read, matching the proven title scan
        // byte for byte: a 64 KiB chunk. Smaller requests are refused
        // with EINVAL by this kernel path (and raw syscall instructions
        // trap in a title), which is what the previous builds hit.
        static thread_local char buffer[65536 + 512];
        const auto list_once = [&]() -> int {
            return getdents(fd, buffer, 65536);
        };
        for (;;)
        {
            const int count = list_once();
            if (count < 0)
            {
                if (listing.error.empty())
                    listing.error = "getdents ret=" + std::to_string(count) +
                                    " errno=" + std::to_string(errno);
                listing.warnings = true;
                break;
            }
            if (count == 0)
                break;
            int offset = 0;
            while (offset + 8 <= count)
            {
                const auto *view =
                    reinterpret_cast<const KernelDirent *>(buffer + offset);
                if (view->reclen < 8)
                {
                    listing.warnings = true;
                    offset = count;
                    break;
                }
                if (view->namlen > 0)
                    names.emplace_back(std::string(view->name, view->namlen), view->type);
                offset += view->reclen;
            }
        }
        sceKernelClose(fd);
    }
#else
    {
        const int fd = open(dir_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
        {
            listing.warnings = true;
            return;
        }
        DIR *dir = fdopendir(fd);
        if (!dir)
        {
            if (listing.error.empty())
                listing.error = std::strerror(errno);
            close(fd);
            listing.warnings = true;
            return;
        }
        for (;;)
        {
            errno = 0;
            dirent *item = readdir(dir);
            if (!item)
            {
                if (errno != 0)
                    listing.warnings = true;
                break;
            }
            names.emplace_back(item->d_name, item->d_type);
        }
        closedir(dir);
    }
#endif
    for (const auto &entry_pair : names)
    {
        const char *name = entry_pair.first.c_str();
        if (!name || name[0] == '\0' || (name[0] == '.' && name[1] == '\0') ||
            (name[0] == '.' && name[1] == '.' && name[2] == '\0'))
            continue;
        const unsigned char type = entry_pair.second;
        if (type == DT_LNK)
            continue;
        if (type != DT_DIR && type != DT_REG && type != DT_UNKNOWN)
            continue;
        const std::string relative = parent.empty() ? std::string(name) : parent + "/" + name;
        struct stat info = {};
        const bool have_stat = stat(join_path(root, relative).c_str(), &info) == 0;
        if (have_stat && S_ISLNK(info.st_mode))
            continue;
        bool is_dir =
            type == DT_DIR || (have_stat && S_ISDIR(info.st_mode));
        if (!is_dir && type == DT_UNKNOWN && !have_stat)
        {
            // Sandbox dirent/stat cannot be trusted: probe with a directory
            // open so capture subfolders are never mistaken for files.
            const int probe = open(join_path(root, relative).c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (probe >= 0)
            {
                close(probe);
                is_dir = true;
            }
            else if (errno != ENOTDIR && errno != ENOENT)
            {
                listing.warnings = true;
            }
        }
        if (is_dir)
        {
            if (depth >= kMaxDepth)
                continue;
            scan_dir(root, relative, depth + 1, listing);
            continue;
        }
        if (have_stat && !S_ISREG(info.st_mode))
            continue;
        if (listing.entries.size() >= kMaxEntries)
            continue;
        Entry entry;
        entry.name = name;
        entry.relative = relative;
        entry.absolute = join_path(root, relative);
        entry.bytes = have_stat && info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
        entry.modified = have_stat ? static_cast<std::int64_t>(info.st_mtime) : 0;
        listing.entries.push_back(std::move(entry));
    }
}

MediaStore::Listing MediaStore::scan_root(const char *primary, const char *fallback)
{
    Listing best;
    best.root = primary;
    bool have_root = false;
    int last_error = ENOENT;
    const char *candidates[] = {primary, fallback};
    for (const char *candidate : candidates)
    {
        if (!candidate || !*candidate)
            continue;
        // Accessibility probe uses the same descriptor family as the scan
        // itself (kernel descriptors on PS5, libc elsewhere).
#ifdef HUI_PS5_BUILD
        const int root_fd = sceKernelOpen(candidate, 0x00020000, 0);
        if (root_fd < 0)
        {
            last_error = ENOENT;
            continue;
        }
        sceKernelClose(root_fd);
#else
        const int root_fd = open(candidate, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (root_fd < 0)
        {
            last_error = errno;
            continue;
        }
        close(root_fd);
#endif
        Listing current;
        current.root = candidate;
        current.accessible = true;
        scan_dir(current.root, "", 0, current);
        std::sort(current.entries.begin(), current.entries.end(),
                  [](const Entry &l, const Entry &r) {
                      if (l.modified != r.modified)
                          return l.modified > r.modified;
                      return l.relative < r.relative;
                  });
        const auto supported = [&](const Listing &l, bool photos) {
            std::size_t n = 0;
            for (const Entry &e : l.entries)
                if (photos ? is_photo_name(e.name) : is_video_name(e.name))
                    ++n;
            return n;
        };
        const bool better =
            !have_root || supported(current, true) + supported(current, false) >
                             supported(best, true) + supported(best, false);
        if (better)
            best = std::move(current);
        have_root = true;
    }
    if (!have_root)
        best.error = std::strerror(last_error);
    return best;
}

std::string format_duration(double seconds)
{
    if (seconds < 0.0)
        return {};
    const long long total = static_cast<long long>(seconds + 0.5);
    char buf[32];
    if (total >= 3600)
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", total / 3600,
                      (total / 60) % 60, total % 60);
    else
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", total / 60, total % 60);
    return buf;
}

namespace
{

std::uint32_t be32(const std::uint8_t *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t be64(const std::uint8_t *p)
{
    return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

bool read_at(int fd, std::uint64_t offset, void *buf, std::size_t len)
{
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
        return false;
    auto *p = static_cast<std::uint8_t *>(buf);
    while (len > 0)
    {
        const ssize_t got = ::read(fd, p, len);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return false;
        p += got;
        len -= static_cast<std::size_t>(got);
    }
    return true;
}

// One box header inside an in-memory box sequence: returns total box size
// and sets `type`/`content` (offset of the box payload). 0 = stop.
std::size_t box_next(const std::uint8_t *data, std::size_t size, std::size_t pos,
                     char type[5], std::size_t &content)
{
    if (pos + 8 > size)
        return 0;
    const std::uint32_t small = be32(data + pos);
    std::memcpy(type, data + pos + 4, 4);
    type[4] = '\0';
    content = pos + 8;
    if (small == 1)
    {
        if (pos + 16 > size)
            return 0;
        const std::uint64_t large = be64(data + pos + 8);
        content = pos + 16;
        return large > size ? 0 : static_cast<std::size_t>(large);
    }
    if (small == 0)
        return size - pos;
    return small;
}

// Duration (mvhd) and largest track dimensions (tkhd) from the moov box.
// No media data is decoded; this is pure container parsing, so it works
// for clips the app cannot play.
bool parse_mp4_info(const std::string &path, double &duration_s, int &out_w, int &out_h)
{
    duration_s = -1.0;
    out_w = 0;
    out_h = 0;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    struct stat info
    {
    };
    if (::fstat(fd, &info) != 0 || info.st_size < 8)
    {
        ::close(fd);
        return false;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(info.st_size);
    std::uint64_t moov_at = 0, moov_size = 0;
    std::uint64_t pos = 0;
    for (int i = 0; i < 128 && pos + 8 <= file_size; ++i)
    {
        std::uint8_t hdr[16];
        if (!read_at(fd, pos, hdr, 8))
            break;
        const std::uint32_t small = be32(hdr);
        std::uint64_t box_size = small;
        std::uint64_t header = 8;
        if (small == 1)
        {
            if (!read_at(fd, pos + 8, hdr + 8, 8))
                break;
            box_size = be64(hdr + 8);
            header = 16;
        }
        else if (small == 0)
        {
            box_size = file_size - pos;
        }
        if (box_size < header || pos + box_size > file_size)
            break;
        if (std::memcmp(hdr + 4, "moov", 4) == 0)
        {
            moov_at = pos + header;
            moov_size = box_size - header;
            break;
        }
        pos += box_size;
    }
    if (moov_size == 0)
    {
        ::close(fd);
        return false;
    }
    // moov is metadata only; cap the read so a pathological file cannot
    // make the scan allocate without bound.
    const std::size_t want =
        static_cast<std::size_t>(std::min<std::uint64_t>(moov_size, 8u << 20));
    std::vector<std::uint8_t> moov(want);
    const bool read_ok = read_at(fd, moov_at, moov.data(), want);
    ::close(fd);
    if (!read_ok)
        return false;
    const std::uint8_t *data = moov.data();
    std::size_t at = 0;
    char type[5];
    std::size_t content = 0;
    while (const std::size_t box = box_next(data, want, at, type, content))
    {
        const std::size_t payload = box - (content - at);
        if (std::strcmp(type, "mvhd") == 0 && payload >= 20)
        {
            const std::uint8_t *v = data + content;
            std::uint32_t timescale = 0;
            std::uint64_t units = 0;
            if (v[0] == 1 && payload >= 32)
            {
                timescale = be32(v + 20);
                units = be64(v + 24);
            }
            else if (v[0] == 0)
            {
                timescale = be32(v + 12);
                units = be32(v + 16);
            }
            if (timescale > 0)
                duration_s = static_cast<double>(units) / timescale;
        }
        else if (std::strcmp(type, "trak") == 0)
        {
            std::size_t tat = content;
            char ttype[5];
            std::size_t tcontent = 0;
            while (const std::size_t tbox =
                       box_next(data, content + payload, tat, ttype, tcontent))
            {
                if (std::strcmp(ttype, "tkhd") == 0)
                {
                    const std::size_t tpay = tbox - (tcontent - tat);
                    const std::uint8_t *v = data + tcontent;
                    const std::size_t woff = v[0] == 1 ? 88 : 76;
                    if (tpay >= woff + 8)
                    {
                        const int w = static_cast<int>(be32(v + woff) >> 16);
                        const int h = static_cast<int>(be32(v + woff + 4) >> 16);
                        if (w > out_w)
                        {
                            out_w = w;
                            out_h = h;
                        }
                    }
                }
                tat += tbox;
            }
        }
        at += box;
    }
    return duration_s >= 0.0 || out_w > 0;
}

} // namespace

std::string MediaStore::describe(std::int64_t modified, std::uint64_t bytes,
                                 const std::string &format)
{
    // Self-contained civil date (no libc localtime/strftime: the PS5 link
    // set only guarantees localtime_s). Displays in UTC.
    char date[32] = {};
    if (modified > 0)
    {
        std::int64_t days = modified / 86400;
        const std::int64_t z = days + 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned y = yoe + era * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned day = doy - (153 * mp + 2) / 5 + 1;
        const unsigned month = mp < 10 ? mp + 3 : mp - 9;
        const unsigned year = y + (month <= 2 ? 1 : 0);
        static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        if (month >= 1 && month <= 12)
            std::snprintf(date, sizeof(date), "%s %u, %u", kMonths[month - 1], day, year);
    }
    char size[32] = {};
    if (bytes >= 1024ULL * 1024ULL)
        std::snprintf(size, sizeof(size), "%.1f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        std::snprintf(size, sizeof(size), "%.0f KB", bytes / 1024.0);
    else
        std::snprintf(size, sizeof(size), "%llu B", static_cast<unsigned long long>(bytes));
    std::string out = date[0] ? date : "Unknown date";
    out += " \xC2\xB7 ";
    out += format;
    out += " \xC2\xB7 ";
    out += size;
    return out;
}

bool MediaStore::refresh()
{
    photos_.clear();
    videos_.clear();
    thumbnails_.clear();
    pending_ = 0;
    pending_video_ = 0;
    std::string base;
    if (const char *override = std::getenv("PS5SHARE_MEDIA_ROOT"))
        base = override;
    const std::string photo_primary = base.empty() ? "/av_contents/photo" : base + "/photo";
    const std::string photo_fallback =
        base.empty() ? "/user/av_contents/photo" : base + "/photo";
    const std::string video_primary = base.empty() ? "/av_contents/video" : base + "/video";
    const std::string video_fallback =
        base.empty() ? "/user/av_contents/video" : base + "/video";
    const std::string thumb_primary =
        base.empty() ? "/av_contents/thumbnails" : base + "/thumbnails";
    const std::string thumb_fallback =
        base.empty() ? "/user/av_contents/thumbnails" : base + "/thumbnails";
    const Listing photos = scan_root(photo_primary.c_str(), photo_fallback.c_str());
    const Listing videos = scan_root(video_primary.c_str(), video_fallback.c_str());
    const Listing thumbs = scan_root(thumb_primary.c_str(), thumb_fallback.c_str());
    thumbnails_ = thumbs.entries;
    // Sidecar thumbnails stored next to the videos themselves also count.
    for (const Entry &e : videos.entries)
    {
        if (is_photo_name(e.name))
            thumbnails_.push_back(e);
    }
    thumb_info_ = thumbs.root + ": " + std::to_string(thumbnails_.size()) + " files";
    if (!thumbs.entries.empty())
        thumb_info_ += ", first: " + thumbs.entries.front().name;
    char diag[256] = {};
    const auto status_of = [](const Listing &l) {
        return l.error.empty() ? "ok" : l.error.c_str();
    };
    std::snprintf(diag, sizeof(diag), "Scan: %s - %zu files, %s; %s - %zu files, %s.",
                  photos.root.c_str(), photos.entries.size(), status_of(photos),
                  videos.root.c_str(), videos.entries.size(), status_of(videos));
    diagnosis_ = diag;
    auto collect = [&](const Listing &listing, bool want_photos, std::vector<Capture> &out) {
        bool first = true;
        for (const Entry &e : listing.entries)
        {
            if (want_photos ? !is_photo_name(e.name) : !is_video_name(e.name))
                continue;
            Capture cap;
            cap.name = e.name;
            cap.absolute_path = e.absolute;
            cap.video = !want_photos;
            cap.latest = first;
            first = false;
            std::string ext = lower_ext(e.name);
            if (!ext.empty() && ext[0] == '.')
                ext.erase(0, 1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            cap.format = ext.empty() ? "FILE" : ext;
            if (!want_photos)
            {
                double duration = -1.0;
                int w = 0, h = 0;
                if (parse_mp4_info(e.absolute, duration, w, h))
                {
                    cap.duration_seconds = duration;
                    cap.width = w;
                    cap.height = h;
                }
            }
            cap.meta = describe(e.modified, e.bytes, cap.format);
            if (cap.duration_seconds >= 0.0)
                cap.meta += " \xC2\xB7 " + format_duration(cap.duration_seconds);
            out.push_back(std::move(cap));
        }
    };
    collect(photos, true, photos_);
    collect(videos, false, videos_);
    return photos.accessible || videos.accessible;
}

std::string MediaStore::diagnosis() const
{
    return diagnosis_;
}

std::size_t MediaStore::pending_tiles() const
{
    std::size_t pending = 0;
    for (const Capture &cap : photos_)
    {
        if (cap.texture == 0 && !cap.preview_failed)
            ++pending;
    }
    for (const Capture &cap : videos_)
    {
        if (cap.texture == 0 && !cap.preview_failed)
            ++pending;
    }
    return pending;
}

std::size_t MediaStore::failed_tiles() const
{
    std::size_t failed = 0;
    for (const Capture &cap : photos_)
    {
        if (cap.preview_failed)
            ++failed;
    }
    for (const Capture &cap : videos_)
    {
        if (cap.preview_failed)
            ++failed;
    }
    return failed;
}

} // namespace hui::share
