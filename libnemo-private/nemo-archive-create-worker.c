/* A separate process is essential: the 7zip writer spools compressed data
 * using TMPDIR. Never change Nemo's process-global temporary directory. */
#define _GNU_SOURCE
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <signal.h>
#include <locale.h>
#include <unistd.h>

static int
fail (struct archive *reader, struct archive *writer, const char *message)
{
    fprintf (stderr, "Archive compression failed: %.500s\n", message ? message : "I/O error");
    archive_read_free (reader);
    archive_write_free (writer);
    return 1;
}

int
main (void)
{
    setlocale (LC_ALL, "");
    if (!setlocale (LC_CTYPE, "C.UTF-8")) {
        fprintf (stderr, "A UTF-8 locale is required for archive compression\n");
        return 1;
    }
    struct stat scratch, output;
    pid_t parent = getppid ();
    if (prctl (PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid () != parent ||
        fstat (5, &scratch) < 0 || !S_ISDIR (scratch.st_mode) ||
        fstat (4, &output) < 0 || !S_ISREG (output.st_mode) ||
        strcmp (getenv ("TMPDIR") ? getenv ("TMPDIR") : "", "/proc/self/fd/5") != 0) {
        fprintf (stderr, "Invalid archive worker descriptors or spool directory\n");
        return 1;
    }
    umask (077);
    struct archive *reader = archive_read_new ();
    struct archive *writer = archive_write_new ();
    if (!reader || !writer)
        return fail (reader, writer, "Cannot allocate libarchive");
    if (archive_read_support_format_tar (reader) != ARCHIVE_OK ||
        archive_read_open_fd (reader, 3, 128 * 1024) != ARCHIVE_OK)
        return fail (reader, writer, archive_error_string (reader));
    if (archive_write_set_format_7zip (writer) != ARCHIVE_OK ||
        archive_write_set_options (writer, "7zip:compression=lzma2,7zip:compression-level=9") != ARCHIVE_OK ||
        archive_write_open_fd (writer, 4) != ARCHIVE_OK)
        return fail (reader, writer, archive_error_string (writer));
    struct archive_entry *input;
    int result;
    char buffer[128 * 1024];
    while ((result = archive_read_next_header (reader, &input)) == ARCHIVE_OK) {
        mode_t type = archive_entry_filetype (input);
        if ((type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK) ||
            archive_entry_hardlink (input))
            return fail (reader, writer, "Unsupported input member");
        struct archive_entry *entry = archive_entry_new ();
        const char *name = archive_entry_pathname_utf8 (input);
        const char *link = archive_entry_symlink_utf8 (input);
        if (!name || (type == AE_IFLNK && !link)) {
            archive_entry_free (entry);
            return fail (reader, writer, "Input member name is not representable as UTF-8");
        }
        archive_entry_set_pathname_utf8 (entry, name);
        archive_entry_set_filetype (entry, type);
        archive_entry_set_perm (entry, type == AE_IFDIR ? 0755 : 0644);
        archive_entry_set_mtime (entry, archive_entry_mtime (input),
                                 archive_entry_mtime_nsec (input));
        archive_entry_set_size (entry, type == AE_IFREG ? archive_entry_size (input) : 0);
        if (type == AE_IFLNK)
            archive_entry_set_symlink_utf8 (entry, link);
        result = archive_write_header (writer, entry);
        archive_entry_free (entry);
        if (result != ARCHIVE_OK)
            return fail (reader, writer, archive_error_string (writer));
        la_ssize_t count;
        while ((count = archive_read_data (reader, buffer, sizeof buffer)) > 0) {
            if (archive_write_data (writer, buffer, count) != count)
                return fail (reader, writer, archive_error_string (writer));
        }
        if (count < 0)
            return fail (reader, writer, archive_error_string (reader));
        if (archive_write_finish_entry (writer) != ARCHIVE_OK)
            return fail (reader, writer, archive_error_string (writer));
    }
    if (result != ARCHIVE_EOF)
        return fail (reader, writer, archive_error_string (reader));
    if (archive_read_close (reader) != ARCHIVE_OK ||
        archive_write_close (writer) != ARCHIVE_OK)
        return fail (reader, writer, archive_error_string (writer));
    archive_read_free (reader);
    archive_write_free (writer);
    int synced;
    do {
        synced = fsync (4);
    } while (synced < 0 && errno == EINTR);
    if (synced < 0 || close (4) < 0) {
        fprintf (stderr, "Could not synchronize/close archive output: %s\n", strerror (errno));
        return 1;
    }
    return 0;
}
