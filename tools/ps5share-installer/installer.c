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

static unsigned toast_seq = 1;

// Full InteractiveToastTemplateB shape, mirroring the payload SDK's notify
// sample: trimmed payloads are dropped silently, and the call needs the
// logged flag set.
static void toast(const char *body, const char *sub)
{
    char payload[3072];
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
             "      \"icon\": {\n"
             "        \"type\": \"Predefined\",\n"
             "        \"parameters\": {\n"
             "          \"icon\": \"download\"\n"
             "        }\n"
             "      },\n"
             "      \"message\": {\n"
             "        \"body\": \"%s\"\n"
             "      },\n"
             "      \"subMessage\": {\n"
             "        \"body\": \"%s\"\n"
             "      }\n"
             "    },\n"
             "    \"platformViews\": {\n"
             "      \"previewDisabled\": {\n"
             "        \"viewData\": {\n"
             "          \"icon\": {\n"
             "            \"type\": \"Predefined\",\n"
             "            \"parameters\": {\n"
             "              \"icon\": \"download\"\n"
             "            }\n"
             "          },\n"
             "          \"message\": {\n"
             "            \"body\": \"%s\"\n"
             "          }\n"
             "        }\n"
             "      }\n"
             "    }\n"
             "  },\n"
             "  \"createdDateTime\": \"2026-10-16T00:00:00.000Z\",\n"
             "  \"localNotificationId\": \"%u\"\n"
             "}\n",
             body, sub, body, toast_seq++);
    sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true, payload);
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
    // The app icon goes down first: both toasts reference it.
    for (unsigned long i = 0; i < kFileCount; ++i)
    {
        if (strcmp(kFiles[i].path, "sce_sys/icon0.png") != 0)
            continue;
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
    toast("Installing PS5 Share Hub", "Created by Vintendo");

    for (unsigned long i = 0; i < kFileCount; ++i)
    {
        if (strcmp(kFiles[i].path, "sce_sys/icon0.png") == 0)
            continue; // already written above
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

    toast("PS5 Share installed", "Open it from the Media section");
    return 0;
}
