// PS5 Share Hub installer payload.
// Sends this title (embedded below by tools/ps5share-installer/gen.py)
// to /data/homebrew/PPSA99992, then tells the user with a toast.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEST_ROOT "/data/homebrew/PPSA99992"
#define TITLE_ID "PPSA99992"

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

int sceNotificationSend(int userId, bool isLogged, const char *payload);

struct Blob
{
    const char *path; // relative to DEST_ROOT, forward slashes
    const unsigned char *data;
    const unsigned char *end;
};

extern const struct Blob kFiles[];
extern const unsigned long kFileCount;

static void toast(const char *body, const char *sub)
{
    char payload[2048];
    snprintf(payload, sizeof(payload),
             "{\n"
             "  \"rawData\": {\n"
             "    \"viewTemplateType\": \"InteractiveToastTemplateB\",\n"
             "    \"channelType\": \"Downloads\",\n"
             "    \"useCaseId\": \"IDC\",\n"
             "    \"toastOverwriteType\": \"No\",\n"
             "    \"isImmediate\": true,\n"
             "    \"priority\": 100,\n"
             "    \"viewData\": {\n"
             "      \"message\": {\n"
             "        \"body\": \"%s\"\n"
             "      },\n"
             "      \"subMessage\": {\n"
             "        \"body\": \"%s\"\n"
             "      }\n"
             "    }\n"
             "  }\n"
             "}\n",
             body, sub);
    sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, false, payload);
}

// mkdir -p for the directory part of DEST_ROOT + rel.
static int make_dirs(const char *rel)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", DEST_ROOT, rel) >= (int)sizeof(path))
        return -1;
    for (char *p = path + 1; *p; ++p)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return 0;
}

static int write_file(const char *rel, const unsigned char *data, unsigned long size)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", DEST_ROOT, rel) >= (int)sizeof(path))
        return -1;
    if (make_dirs(rel) != 0)
        return -1;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    unsigned long left = size;
    const unsigned char *p = data;
    while (left > 0)
    {
        const ssize_t wrote = write(fd, p, left > 65536 ? 65536 : (size_t)left);
        if (wrote <= 0)
        {
            close(fd);
            return -1;
        }
        p += wrote;
        left -= (unsigned long)wrote;
    }
    close(fd);
    return 0;
}

int main(void)
{
    char sub[256];
    snprintf(sub, sizeof(sub), "Created by Vintendo \xC2\xB7 %lu files to %s", kFileCount,
             DEST_ROOT);
    toast("Installing PS5 Share Hub", sub);

    for (unsigned long i = 0; i < kFileCount; ++i)
    {
        const unsigned long size =
            (unsigned long)(kFiles[i].end - kFiles[i].data);
        if (write_file(kFiles[i].path, kFiles[i].data, size) != 0)
        {
            snprintf(sub, sizeof(sub), "failed at %s (errno %d)", kFiles[i].path,
                     errno);
            toast("PS5 Share install failed", sub);
            return 1;
        }
    }

    toast("PS5 Share installed",
          "Created by Vintendo \xC2\xB7 Open it from the Media section");
    return 0;
}
