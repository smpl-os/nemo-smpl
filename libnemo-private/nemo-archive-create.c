#define _GNU_SOURCE
#include <config.h>
#include "nemo-archive-create.h"

#ifdef NEMO_SMPL
#include "nemo-transfer-safety.h"
#include <archive.h>
#include <archive_entry.h>
#include <gio/gunixmounts.h>
#include <glib/gi18n.h>
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NEMO_ARCHIVE_CREATE_WORKER
#define NEMO_ARCHIVE_CREATE_WORKER LIBEXECDIR "/nemo-archive-create-worker"
#endif

#define SOURCE_ATTRIBUTES \
    "standard::name,standard::type,standard::size,standard::symlink-target," \
    "time::modified,time::modified-usec,time::changed,time::changed-usec," \
    "id::file,etag::value"

static sqlite3_vfs manifest_vfs;
static sqlite3_vfs *manifest_base_vfs;
static gsize manifest_vfs_ready;

static int
manifest_full_path (sqlite3_vfs *vfs, const char *name, int size, char *output)
{
    (void) vfs;
    /* SQLite's Unix xFullPathname otherwise resolves /proc/self/fd to its
     * mutable display pathname, silently discarding the directory pin. */
    if (!g_str_has_prefix (name, "/proc/self/fd/") || strlen (name) >= (gsize) size)
        return SQLITE_CANTOPEN;
    g_strlcpy (output, name, size);
    return SQLITE_OK;
}

static int
manifest_open (sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *output_flags)
{
    (void) vfs;
    /* There are no sorts or temporary tables in this manifest. Refuse an
     * unexpected anonymous spill instead of falling back to host temp storage. */
    if (!name || !g_str_has_prefix (name, "/proc/self/fd/"))
        return SQLITE_CANTOPEN;
    return manifest_base_vfs->xOpen (manifest_base_vfs, name, file, flags, output_flags);
}

static gboolean
register_manifest_vfs (void)
{
    if (g_once_init_enter (&manifest_vfs_ready)) {
        manifest_base_vfs = sqlite3_vfs_find ("unix");
        gboolean ok = manifest_base_vfs != NULL;
        if (ok) {
            manifest_vfs = *manifest_base_vfs;
            manifest_vfs.zName = "nemo-archive-create-pinned";
            manifest_vfs.pNext = NULL;
            manifest_vfs.xFullPathname = manifest_full_path;
            manifest_vfs.xOpen = manifest_open;
            ok = sqlite3_vfs_register (&manifest_vfs, 0) == SQLITE_OK;
        }
        g_once_init_leave (&manifest_vfs_ready, ok ? 1 : 2);
    }
    return manifest_vfs_ready == 1;
}

typedef struct {
    GList *sources;
    GFile *destination;
    NemoProgressInfo *progress;
    GCancellable *cancel;
    GUnixMountMonitor *monitor;
    gboolean move, overwrite;
    NemoArchiveCreateResult result;
    GError *error;
    NemoTransferGuard *guard;
    NemoTransferTransaction *transaction;
    sqlite3 *db;
    int scratch_fd, scratch_parent_fd, stage_fd;
    char *scratch_name, *scratch_path;
    struct stat scratch_identity, manifest_identity, destination_identity;
    gboolean destination_exists;
    gboolean keep_manifest;
    guint64 bytes, total_bytes;
    GString *details;
} ArchiveJob;

typedef struct {
    gint64 id;
    char *uri, *name, *signature, *link, *sha256;
    struct stat identity;
    gboolean native;
    int type;
    gint64 size, mtime;
    long mtime_nsec;
} Member;

static gboolean
failure (GError **error, const char *message)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
    return FALSE;
}

static gboolean
system_error (GError **error, const char *message)
{
    int saved = errno;
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved),
                 "%s: %s", message, g_strerror (saved));
    return FALSE;
}

static gboolean
same_identity (const struct stat *a, const struct stat *b, gboolean directory)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_mode == b->st_mode && a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
           (directory || (a->st_size == b->st_size &&
            a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
            a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec));
}

static void
member_clear (Member *member)
{
    g_free (member->uri);
    g_free (member->name);
    g_free (member->signature);
    g_free (member->link);
    g_free (member->sha256);
    memset (member, 0, sizeof *member);
}

static gboolean
sql_error (ArchiveJob *job, GError **error)
{
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Archive manifest: %s"),
                 sqlite3_errmsg (job->db));
    return FALSE;
}

static gboolean
sql_exec (ArchiveJob *job, const char *sql, GError **error)
{
    return sqlite3_exec (job->db, sql, NULL, NULL, NULL) == SQLITE_OK || sql_error (job, error);
}

static gboolean
sql_prepare (ArchiveJob *job, const char *sql, sqlite3_stmt **statement, GError **error)
{
    return sqlite3_prepare_v2 (job->db, sql, -1, statement, NULL) == SQLITE_OK ||
           sql_error (job, error);
}

static char *
info_signature (GFileInfo *info)
{
    /* Opaque backend version identifiers supplement times and size. Not all
     * read-only virtual files implement etags; absence is not an empty match
     * against a previously present version identifier. */
    g_autofree char *id = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_ID_FILE);
    g_autofree char *etag = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE);
    return g_strdup_printf ("%d:%" G_GOFFSET_FORMAT ":%" G_GUINT64_FORMAT ":%u:%" G_GUINT64_FORMAT ":%u:%s:%s",
        g_file_info_get_file_type (info),
        g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ?
            g_file_info_get_size (info) : (goffset) -1,
        g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
        g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC),
        g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED),
        g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC),
        id ? id : "-", etag ? etag : "-");
}

static gboolean
check_member (ArchiveJob *job, const Member *member, GFile *bound, GError **error)
{
    if (g_cancellable_set_error_if_cancelled (job->cancel, error))
        return FALSE;
    if (member->native) {
        g_autofree char *path = g_file_get_path (bound);
        struct stat current;
        if (lstat (path, &current) < 0)
            return system_error (error, _("Could not inspect archive input"));
        if (!same_identity (&member->identity, &current, FALSE))
            return failure (error, _("An archive input changed; remaining sources were retained."));
    } else {
        g_autoptr (GFileInfo) info = g_file_query_info (bound, SOURCE_ATTRIBUTES,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancel, error);
        if (!info)
            return FALSE;
        g_autofree char *signature = info_signature (info);
        if (g_strcmp0 (signature, member->signature) != 0 ||
            g_strcmp0 (g_file_info_get_attribute_byte_string (
                info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET), member->link) != 0)
            return failure (error, _("A virtual archive input changed; no sources were removed."));
    }
    return TRUE;
}

static gboolean
valid_component (const char *name)
{
    return name && *name && strcmp (name, ".") && strcmp (name, "..") &&
           !strchr (name, '/') && !strchr (name, '\\') && g_utf8_validate (name, -1, NULL);
}

static gboolean
insert_member (ArchiveJob *job, GFile *source, const char *name, GError **error)
{
    g_autoptr (GFile) bound = nemo_transfer_guard_file (job->guard, source, FALSE, error);
    if (!bound)
        return FALSE;
    Member member = { 0 };
    member.uri = g_file_get_uri (source);
    member.name = g_strdup (name);
    g_autofree char *path = g_file_get_path (bound);
    member.native = path != NULL && g_file_is_native (bound);
    gboolean ok = FALSE;
    sqlite3_stmt *statement = NULL;
    if (member.native) {
        if (lstat (path, &member.identity) < 0) {
            system_error (error, _("Could not inspect archive input"));
            goto out;
        }
        member.type = member.identity.st_mode & S_IFMT;
        member.size = member.type == S_IFREG ? member.identity.st_size : 0;
        member.mtime = member.identity.st_mtim.tv_sec;
        member.mtime_nsec = member.identity.st_mtim.tv_nsec;
        if (member.type == S_IFDIR) {
            struct stat destination_parent;
            if (fstat (job->scratch_parent_fd, &destination_parent) < 0) {
                system_error (error, _("Could not inspect the archive destination folder"));
                goto out;
            }
            if (member.identity.st_dev == destination_parent.st_dev &&
                member.identity.st_ino == destination_parent.st_ino) {
                failure (error, _("The archive destination folder aliases a selected source directory."));
                goto out;
            }
        }
        if (job->destination_exists &&
            member.identity.st_dev == job->destination_identity.st_dev &&
            member.identity.st_ino == job->destination_identity.st_ino) {
            failure (error, _("The destination is also a selected source or a hard-link alias."));
            goto out;
        }
        if (member.type == S_IFLNK) {
            gsize length = MAX ((gsize) member.identity.st_size + 1, (gsize) 4096);
            member.link = g_malloc (length + 1);
            ssize_t got = readlink (path, member.link, length);
            if (got < 0 || (gsize) got == length) {
                system_error (error, _("Could not read an archive input symbolic link"));
                goto out;
            }
            member.link[got] = 0;
        }
    } else {
        g_autoptr (GFileInfo) info = g_file_query_info (bound, SOURCE_ATTRIBUTES,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancel, error);
        if (!info)
            goto out;
        GFileType type = g_file_info_get_file_type (info);
        member.type = type == G_FILE_TYPE_REGULAR ? S_IFREG :
                      type == G_FILE_TYPE_DIRECTORY ? S_IFDIR :
                      type == G_FILE_TYPE_SYMBOLIC_LINK ? S_IFLNK : 0;
        if (member.type == S_IFREG &&
            !g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
            failure (error, _("An archive input does not provide its file size."));
            goto out;
        }
        member.size = member.type == S_IFREG ? g_file_info_get_size (info) : 0;
        member.mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        member.mtime_nsec = 1000L * g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
        member.signature = info_signature (info);
        member.link = g_strdup (g_file_info_get_attribute_byte_string (
            info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET));
    }
    if ((member.type != S_IFREG && member.type != S_IFDIR && member.type != S_IFLNK) ||
        member.size < 0 || (member.type == S_IFLNK && (!member.link || !g_utf8_validate (member.link, -1, NULL)))) {
        failure (error, _("Unsupported archive input: only regular files, directories and UTF-8 symbolic links are supported."));
        goto out;
    }
    if (strlen (name) > 65535) {
        failure (error, _("An archive member path is too long."));
        goto out;
    }
    if (!check_member (job, &member, bound, error) ||
        !sql_prepare (job, "INSERT INTO members(uri,name,type,size,mtime,nsec,native,identity,signature,link,device,inode)"
                           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", &statement, error))
        goto out;
    sqlite3_bind_text (statement, 1, member.uri, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (statement, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (statement, 3, member.type);
    sqlite3_bind_int64 (statement, 4, member.size);
    sqlite3_bind_int64 (statement, 5, member.mtime);
    sqlite3_bind_int64 (statement, 6, member.mtime_nsec);
    sqlite3_bind_int (statement, 7, member.native);
    sqlite3_bind_blob (statement, 8, &member.identity, sizeof member.identity, SQLITE_TRANSIENT);
    sqlite3_bind_text (statement, 9, member.signature, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (statement, 10, member.link, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (statement, 11, member.identity.st_dev);
    sqlite3_bind_int64 (statement, 12, member.identity.st_ino);
    if (sqlite3_step (statement) != SQLITE_DONE) {
        if ((sqlite3_errcode (job->db) & 0xff) == SQLITE_CONSTRAINT)
            failure (error, _("Duplicate archive input names or selected hard-link aliases are not supported."));
        else
            sql_error (job, error);
        goto out;
    }
    job->total_bytes += member.size;
    ok = TRUE;
out:
    sqlite3_finalize (statement);
    member_clear (&member);
    return ok;
}

/* Keyset iteration never holds a tree or an active SQLite read cursor in
 * memory while adding descendants. Reverse IDs are bottom-up (parents are
 * inserted first), avoiding a disk/RAM sort for source retirement. */
static gboolean
read_member (ArchiveJob *job, gint64 after, gboolean reverse, Member *member,
             gboolean *found, GError **error)
{
    sqlite3_stmt *statement = NULL;
    *found = FALSE;
    if (!sql_prepare (job, reverse ?
        "SELECT id,uri,name,type,size,mtime,nsec,native,identity,signature,link,sha256 FROM members WHERE id<? ORDER BY id DESC LIMIT 1" :
        "SELECT id,uri,name,type,size,mtime,nsec,native,identity,signature,link,sha256 FROM members WHERE id>? ORDER BY id LIMIT 1",
        &statement, error))
        return FALSE;
    sqlite3_bind_int64 (statement, 1, after);
    int result = sqlite3_step (statement);
    gboolean ok = result == SQLITE_ROW || result == SQLITE_DONE;
    if (result == SQLITE_ROW) {
        *found = TRUE;
        member->id = sqlite3_column_int64 (statement, 0);
        member->uri = g_strdup ((const char *) sqlite3_column_text (statement, 1));
        member->name = g_strdup ((const char *) sqlite3_column_text (statement, 2));
        member->type = sqlite3_column_int (statement, 3);
        member->size = sqlite3_column_int64 (statement, 4);
        member->mtime = sqlite3_column_int64 (statement, 5);
        member->mtime_nsec = sqlite3_column_int64 (statement, 6);
        member->native = sqlite3_column_int (statement, 7);
        if (sqlite3_column_bytes (statement, 8) != sizeof member->identity)
            ok = failure (error, _("Invalid archive manifest identity."));
        else
            memcpy (&member->identity, sqlite3_column_blob (statement, 8), sizeof member->identity);
        member->signature = g_strdup ((const char *) sqlite3_column_text (statement, 9));
        member->link = g_strdup ((const char *) sqlite3_column_text (statement, 10));
        member->sha256 = g_strdup ((const char *) sqlite3_column_text (statement, 11));
    } else if (!ok) {
        sql_error (job, error);
    }
    sqlite3_finalize (statement);
    return ok;
}

static gboolean
scan_sources (ArchiveJob *job, GError **error)
{
    g_autoptr (GFile) common_parent = NULL;
    if (!job->sources)
        return failure (error, _("No archive inputs were selected."));
    for (GList *item = job->sources; item; item = item->next) {
        GFile *source = item->data;
        g_autoptr (GFile) parent = g_file_get_parent (source);
        g_autofree char *name = g_file_get_basename (source);
        if (!parent || !valid_component (name))
            return failure (error, _("Archive inputs require safe UTF-8 names, without backslashes."));
        if (!common_parent)
            common_parent = g_object_ref (parent);
        if (!g_file_equal (parent, common_parent))
            return failure (error, _("Select archive inputs from one folder; mixed parents and overlapping selections are not supported."));
        if (g_file_equal (source, job->destination) || g_file_has_prefix (job->destination, source))
            return failure (error, _("The archive destination cannot be inside a selected source."));
        if (!insert_member (job, source, name, error))
            return FALSE;
    }
    gint64 id = 0;
    for (;;) {
        Member member = { 0 };
        gboolean found, ok = read_member (job, id, FALSE, &member, &found, error);
        if (!ok || !found) {
            member_clear (&member);
            return ok;
        }
        id = member.id;
        if (member.type == S_IFDIR) {
            g_autoptr (GFile) source = g_file_new_for_uri (member.uri);
            g_autoptr (GFile) metadata = nemo_transfer_guard_file (job->guard, source, FALSE, error);
            g_autoptr (GFile) bound = metadata ?
                nemo_transfer_guard_directory (job->guard, source, FALSE, error) : NULL;
            g_autoptr (GFileEnumerator) enumerator = NULL;
            if (bound && check_member (job, &member, metadata, error))
                enumerator = g_file_enumerate_children (bound, G_FILE_ATTRIBUTE_STANDARD_NAME,
                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancel, error);
            ok = enumerator != NULL;
            while (ok) {
                g_autoptr (GFileInfo) info = g_file_enumerator_next_file (enumerator, job->cancel, error);
                if (!info) {
                    ok = !*error;
                    break;
                }
                const char *name = g_file_info_get_name (info);
                if (!valid_component (name)) {
                    ok = failure (error, _("An input contains an unsupported filename."));
                    break;
                }
                g_autoptr (GFile) child = g_file_get_child (source, name);
                g_autofree char *relative = g_strconcat (member.name, "/", name, NULL);
                ok = insert_member (job, child, relative, error);
            }
            if (enumerator && !g_file_enumerator_close (enumerator, job->cancel, ok ? error : NULL))
                ok = FALSE;
            if (ok)
                ok = check_member (job, &member, metadata, error);
        }
        member_clear (&member);
        if (!ok)
            return FALSE;
        nemo_progress_info_pulse_progress (job->progress);
    }
}

typedef struct {
    int socket;
    GSubprocess *child;
} WorkerPipe;

static void
cancel_worker (GCancellable *cancel, gpointer data)
{
    WorkerPipe *pipe = data;
    (void) cancel;
    shutdown (pipe->socket, SHUT_RDWR);
    g_subprocess_force_exit (pipe->child);
}

static la_ssize_t
tar_output (struct archive *archive, void *data, const void *buffer, size_t length)
{
    WorkerPipe *pipe = data;
    size_t done = 0;
    while (done < length) {
        ssize_t count = send (pipe->socket, (const char *) buffer + done,
                              length - done, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            archive_set_error (archive, errno, "Archive worker input failed");
            return -1;
        }
        done += count;
    }
    return done;
}

static gboolean
archive_error (struct archive *archive, GError **error)
{
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Archive error: %s"),
                 archive_error_string (archive) ? archive_error_string (archive) : _("invalid archive"));
    return FALSE;
}

static gboolean
stream_member (ArchiveJob *job, struct archive *tar, Member *member, GError **error)
{
    g_autoptr (GFile) source = g_file_new_for_uri (member->uri);
    g_autoptr (GFile) bound = nemo_transfer_guard_file (job->guard, source, FALSE, error);
    if (!bound || !check_member (job, member, bound, error))
        return FALSE;
    struct archive_entry *entry = archive_entry_new ();
    archive_entry_set_pathname_utf8 (entry, member->name);
    archive_entry_set_filetype (entry, member->type);
    archive_entry_set_perm (entry, member->type == S_IFDIR ? 0755 : 0644);
    archive_entry_set_size (entry, member->size);
    archive_entry_set_mtime (entry, member->mtime, member->mtime_nsec);
    if (member->link)
        archive_entry_set_symlink_utf8 (entry, member->link);
    int status = archive_write_header (tar, entry);
    archive_entry_free (entry);
    if (status != ARCHIVE_OK)
        return archive_error (tar, error);
    g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
    gboolean ok = TRUE;
    int fd = -1;
    g_autoptr (GFileInputStream) stream = NULL;
    struct stat opened;
    if (member->type == S_IFREG) {
        if (member->native) {
            g_autofree char *path = g_file_get_path (bound);
            fd = open (path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                return system_error (error, _("Could not open archive input without following links"));
            if (fstat (fd, &opened) < 0 || !same_identity (&member->identity, &opened, FALSE))
                ok = failure (error, _("An archive input was replaced before reading."));
        } else {
            stream = g_file_read (bound, job->cancel, error);
            ok = stream != NULL;
        }
        char buffer[128 * 1024];
        gint64 total = 0;
        while (ok) {
            if (g_cancellable_set_error_if_cancelled (job->cancel, error)) {
                ok = FALSE;
                break;
            }
            gssize count = fd >= 0 ? read (fd, buffer, sizeof buffer) :
                g_input_stream_read (G_INPUT_STREAM (stream), buffer, sizeof buffer, job->cancel, error);
            if (count < 0 && fd >= 0 && errno == EINTR)
                continue;
            if (count < 0) {
                if (fd >= 0)
                    system_error (error, _("Could not read archive input"));
                ok = FALSE;
                break;
            }
            if (!count)
                break;
            total += count;
            if (total > member->size) {
                ok = failure (error, _("An archive input grew during compression."));
                break;
            }
            g_checksum_update (checksum, (const guchar *) buffer, count);
            if (archive_write_data (tar, buffer, count) != count) {
                ok = archive_error (tar, error);
                break;
            }
            job->bytes += count;
            nemo_progress_info_set_progress (job->progress, job->bytes, MAX (job->total_bytes, 1));
        }
        if (ok && total != member->size)
            ok = failure (error, _("An archive input was truncated during compression."));
        if (fd >= 0) {
            if (ok && (fstat (fd, &opened) < 0 || !same_identity (&member->identity, &opened, FALSE)))
                ok = failure (error, _("An archive input changed while being read."));
            if (close (fd) < 0 && ok)
                ok = system_error (error, _("Could not close archive input"));
        }
        if (stream && !g_input_stream_close (G_INPUT_STREAM (stream), job->cancel, ok ? error : NULL))
            ok = FALSE;
    } else if (member->link) {
        g_checksum_update (checksum, (const guchar *) member->link, strlen (member->link));
    }
    if (ok)
        ok = check_member (job, member, bound, error);
    if (ok && archive_write_finish_entry (tar) != ARCHIVE_OK)
        ok = archive_error (tar, error);
    sqlite3_stmt *statement = NULL;
    if (ok)
        ok = sql_prepare (job, "UPDATE members SET sha256=? WHERE id=?", &statement, error);
    if (ok) {
        sqlite3_bind_text (statement, 1, g_checksum_get_string (checksum), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (statement, 2, member->id);
        if (sqlite3_step (statement) != SQLITE_DONE)
            ok = sql_error (job, error);
    }
    sqlite3_finalize (statement);
    return ok;
}

static gboolean
compress_sources (ArchiveJob *job, GError **error)
{
    int sockets[2];
    if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0)
        return system_error (error, _("Could not create archive worker channel"));
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    int output = fcntl (job->stage_fd, F_DUPFD_CLOEXEC, 6);
    int scratch = fcntl (job->scratch_fd, F_DUPFD_CLOEXEC, 6);
    if (output < 0 || scratch < 0) {
        if (output >= 0) close (output);
        if (scratch >= 0) close (scratch);
        close (sockets[0]);
        close (sockets[1]);
        return system_error (error, _("Could not retain archive worker descriptors"));
    }
    g_subprocess_launcher_take_fd (launcher, sockets[1], 3);
    g_subprocess_launcher_take_fd (launcher, output, 4);
    g_subprocess_launcher_take_fd (launcher, scratch, 5);
    g_subprocess_launcher_setenv (launcher, "TMPDIR", "/proc/self/fd/5", TRUE);
    g_subprocess_launcher_setenv (launcher, "TMP", "/proc/self/fd/5", TRUE);
    g_subprocess_launcher_setenv (launcher, "TEMP", "/proc/self/fd/5", TRUE);
    g_autoptr (GSubprocess) child = g_subprocess_launcher_spawn (launcher, error,
                                                              NEMO_ARCHIVE_CREATE_WORKER, NULL);
    /* The launcher retains its taken descriptors after spawn. Release the
     * parent's receiving endpoint before sending, so worker failure produces
     * EPIPE rather than an indefinitely full socket with a live local reader. */
    g_clear_object (&launcher);
    if (!child) {
        close (sockets[0]);
        return FALSE;
    }
    WorkerPipe pipe = { .socket = sockets[0], .child = child };
    gulong cancellation = g_cancellable_connect (job->cancel, G_CALLBACK (cancel_worker), &pipe, NULL);
    struct archive *tar = archive_write_new ();
    gboolean ok = archive_write_set_format_pax (tar) == ARCHIVE_OK &&
                  archive_write_open (tar, &pipe, NULL, tar_output, NULL) == ARCHIVE_OK;
    if (!ok)
        archive_error (tar, error);
    gint64 id = 0;
    while (ok) {
        Member member = { 0 };
        gboolean found;
        ok = read_member (job, id, FALSE, &member, &found, error);
        if (ok && found) {
            id = member.id;
            nemo_progress_info_take_details (job->progress, g_strdup_printf (_("Compressing %s"), member.name));
            ok = stream_member (job, tar, &member, error);
        }
        member_clear (&member);
        if (!found)
            break;
    }
    if (ok && archive_write_close (tar) != ARCHIVE_OK)
        ok = archive_error (tar, error);
    /* On a producer failure terminate first; archive_write_free may otherwise
     * try to flush a large buffered tail into a worker we no longer need. */
    if (!ok)
        cancel_worker (job->cancel, &pipe);
    archive_write_free (tar);
    shutdown (pipe.socket, SHUT_WR);
    g_autofree char *stderr_text = NULL;
    GError *worker_error = NULL;
    gboolean communicated = g_subprocess_communicate_utf8 (child, NULL, NULL, NULL,
                                                          &stderr_text, &worker_error);
    g_cancellable_disconnect (job->cancel, cancellation);
    close (pipe.socket);
    if (!communicated) {
        g_subprocess_force_exit (child);
        g_subprocess_wait (child, NULL, NULL);
    }
    if (!communicated || !g_subprocess_get_successful (child)) {
        if (ok) {
            if (worker_error)
                g_propagate_error (error, g_steal_pointer (&worker_error));
            else
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                             stderr_text && *stderr_text ? stderr_text : _("Archive worker failed."));
        } else if (stderr_text && *stderr_text) {
            g_string_append_printf (job->details, "\n%s", stderr_text);
        }
        ok = FALSE;
    }
    g_clear_error (&worker_error);
    if (g_cancellable_is_cancelled (job->cancel)) {
        g_clear_error (error);
        g_cancellable_set_error_if_cancelled (job->cancel, error);
        ok = FALSE;
    }
    return ok;
}

static gboolean
verify_archive (ArchiveJob *job, char **digest, GError **error)
{
    struct stat before, after;
    if (fchmod (job->stage_fd, 0644) < 0 || fsync (job->stage_fd) < 0 ||
        fstat (job->stage_fd, &before) < 0 || lseek (job->stage_fd, 0, SEEK_SET) < 0)
        return system_error (error, _("Could not synchronize archive staging data"));
    struct archive *reader = archive_read_new ();
    gboolean ok = archive_read_support_format_7zip (reader) == ARCHIVE_OK &&
                  archive_read_open_fd (reader, job->stage_fd, 128 * 1024) == ARCHIVE_OK;
    if (!ok)
        archive_error (reader, error);
    struct archive_entry *entry;
    int status = ARCHIVE_FATAL;
    while (ok && (status = archive_read_next_header (reader, &entry)) == ARCHIVE_OK) {
        if (g_cancellable_set_error_if_cancelled (job->cancel, error)) {
            ok = FALSE;
            break;
        }
        const char *pathname = archive_entry_pathname_utf8 (entry);
        if (!pathname) {
            ok = failure (error, _("The archive contains an unnamed member."));
            break;
        }
        g_autofree char *name = g_strdup (pathname);
        gsize length = strlen (name);
        if (archive_entry_filetype (entry) == AE_IFDIR && length && name[length - 1] == '/')
            name[length - 1] = 0;
        sqlite3_stmt *statement = NULL;
        if (!sql_prepare (job, "SELECT id,type,size,sha256,link,mtime,nsec,verified FROM members WHERE name=?",
                          &statement, error)) {
            ok = FALSE;
            break;
        }
        sqlite3_bind_text (statement, 1, name, -1, SQLITE_TRANSIENT);
        int row = sqlite3_step (statement);
        if (row != SQLITE_ROW) {
            ok = row == SQLITE_DONE ? failure (error, _("The archive contains an unexpected member.")) :
                                      sql_error (job, error);
            sqlite3_finalize (statement);
            break;
        }
        gint64 id = sqlite3_column_int64 (statement, 0);
        int type = sqlite3_column_int (statement, 1);
        gint64 size = sqlite3_column_int64 (statement, 2);
        g_autofree char *expected = g_strdup ((const char *) sqlite3_column_text (statement, 3));
        g_autofree char *link = g_strdup ((const char *) sqlite3_column_text (statement, 4));
        gint64 mtime = sqlite3_column_int64 (statement, 5);
        long nsec = sqlite3_column_int64 (statement, 6);
        gboolean verified = sqlite3_column_int (statement, 7);
        sqlite3_finalize (statement);
        if (verified || (mode_t) type != archive_entry_filetype (entry) ||
            (type == S_IFREG && size != archive_entry_size (entry)) ||
            g_strcmp0 (link, archive_entry_symlink_utf8 (entry)) ||
            archive_entry_hardlink (entry) ||
            archive_entry_perm (entry) != (type == S_IFDIR ? 0755 : 0644) ||
            archive_entry_uid (entry) != 0 || archive_entry_gid (entry) != 0 ||
            (archive_entry_uname (entry) && *archive_entry_uname (entry)) ||
            (archive_entry_gname (entry) && *archive_entry_gname (entry)) ||
            archive_entry_xattr_count (entry) != 0 ||
            archive_entry_acl_count (entry, ARCHIVE_ENTRY_ACL_TYPE_ACCESS |
                ARCHIVE_ENTRY_ACL_TYPE_DEFAULT | ARCHIVE_ENTRY_ACL_TYPE_NFS4) != 0 ||
            archive_entry_mtime (entry) != mtime ||
            archive_entry_mtime_nsec (entry) / 100 != nsec / 100) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("Archive member structure or normalized metadata did not verify: %s "
                           "(type %o/%o, permissions %o/%o, mtime %" G_GINT64_FORMAT ".%09ld/%"
                           G_GINT64_FORMAT ".%09ld)."),
                         name, type, archive_entry_filetype (entry),
                         type == S_IFDIR ? 0755 : 0644, archive_entry_perm (entry),
                         mtime, nsec, (gint64) archive_entry_mtime (entry), archive_entry_mtime_nsec (entry));
            ok = FALSE;
            break;
        }
        g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
        if (link)
            g_checksum_update (checksum, (const guchar *) link, strlen (link));
        gint64 total = 0;
        gint64 data_size = link ? (gint64) strlen (link) : size;
        const void *buffer;
        size_t count;
        la_int64_t offset;
        int data_status;
        /* read_data synthesizes zero padding for already-consumed 7zip link
         * payloads. The block API exposes the actual decoder output instead. */
        while ((data_status = archive_read_data_block (reader, &buffer, &count, &offset)) == ARCHIVE_OK) {
            if (g_cancellable_set_error_if_cancelled (job->cancel, error)) {
                ok = FALSE;
                break;
            }
            if ((type != S_IFREG && type != S_IFLNK) || offset != total ||
                count > (guint64) (data_size - total)) {
                ok = failure (error, _("Unexpected archive member content."));
                break;
            }
            if (link) {
                if (memcmp (buffer, link + total, count) != 0) {
                    ok = failure (error, _("Archive symbolic-link target verification failed."));
                    break;
                }
            } else {
                g_checksum_update (checksum, buffer, count);
            }
            total += count;
        }
        if (ok && data_status != ARCHIVE_EOF)
            ok = archive_error (reader, error);
        /* Some libarchive readers expose the symlink bytes both in the header
         * and as member data; others consume them while reading the header. */
        if (link && total == 0) {
            total = data_size;
        }
        if (ok && (total != data_size || !expected ||
                   strcmp (expected, g_checksum_get_string (checksum)))) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("Archive member SHA-256 verification failed: %s (%" G_GINT64_FORMAT
                           "/%" G_GINT64_FORMAT " bytes, %s/%s)."),
                         name, total, data_size, g_checksum_get_string (checksum),
                         expected ? expected : "missing");
            ok = FALSE;
        }
        if (ok)
            ok = sql_prepare (job, "UPDATE members SET verified=1 WHERE id=?", &statement, error);
        else
            statement = NULL;
        if (ok) {
            sqlite3_bind_int64 (statement, 1, id);
            if (sqlite3_step (statement) != SQLITE_DONE)
                ok = sql_error (job, error);
        }
        sqlite3_finalize (statement);
        if (ok)
            job->result.archived_items++;
        nemo_progress_info_pulse_progress (job->progress);
    }
    if (ok && status != ARCHIVE_EOF)
        ok = archive_error (reader, error);
    if (archive_read_close (reader) != ARCHIVE_OK && ok)
        ok = archive_error (reader, error);
    archive_read_free (reader);
    sqlite3_stmt *statement = NULL;
    if (ok)
        ok = sql_prepare (job, "SELECT 1 FROM members WHERE verified=0 LIMIT 1", &statement, error);
    if (ok) {
        int row = sqlite3_step (statement);
        if (row != SQLITE_DONE)
            ok = row == SQLITE_ROW ? failure (error, _("The archive is missing selected input data.")) :
                                      sql_error (job, error);
    }
    sqlite3_finalize (statement);
    g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
    if (ok && lseek (job->stage_fd, 0, SEEK_SET) < 0)
        ok = system_error (error, _("Could not rewind the verified archive"));
    char buffer[128 * 1024];
    while (ok) {
        if (g_cancellable_set_error_if_cancelled (job->cancel, error)) {
            ok = FALSE;
            break;
        }
        ssize_t count = read (job->stage_fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ok = system_error (error, _("Could not hash the verified archive"));
            break;
        }
        if (!count)
            break;
        g_checksum_update (checksum, (const guchar *) buffer, count);
    }
    if (ok && (fstat (job->stage_fd, &after) < 0 || !same_identity (&before, &after, FALSE)))
        ok = failure (error, _("The archive changed during independent verification."));
    if (ok)
        *digest = g_strdup (g_checksum_get_string (checksum));
    return ok;
}

static gboolean
recheck_sources (ArchiveJob *job, GError **error)
{
    gint64 id = 0;
    for (;;) {
        Member member = { 0 };
        gboolean found, ok = read_member (job, id, FALSE, &member, &found, error);
        if (ok && found) {
            id = member.id;
            g_autoptr (GFile) source = g_file_new_for_uri (member.uri);
            g_autoptr (GFile) bound = nemo_transfer_guard_file (job->guard, source, FALSE, error);
            ok = bound && check_member (job, &member, bound, error);
        }
        member_clear (&member);
        if (!ok || !found)
            return ok;
    }
}

static gboolean
retire_sources (ArchiveJob *job, GError **error)
{
    gint64 id = G_MAXINT64;
    for (;;) {
        Member member = { 0 };
        gboolean found, ok = read_member (job, id, TRUE, &member, &found, error);
        if (ok && found) {
            id = member.id;
            g_autoptr (GFile) source = g_file_new_for_uri (member.uri);
            nemo_progress_info_take_details (job->progress,
                g_strdup_printf (_("Removing verified original %s"), member.name));
            ok = member.native && nemo_transfer_transaction_retire_archived (
                job->transaction, source, &member.identity, member.sha256, member.link, job->cancel, error);
            if (!ok && !*error)
                failure (error, _("Destructive archive moves require supported local sources."));
            if (!ok)
                g_string_append_printf (job->details, _("\nSource retirement stopped at: %s"), member.uri);
            sqlite3_stmt *statement = NULL;
            if (ok) {
                job->result.removed_items++;
                ok = sql_prepare (job, "UPDATE members SET removed=1 WHERE id=?", &statement, error);
            }
            if (ok) {
                sqlite3_bind_int64 (statement, 1, id);
                if (sqlite3_step (statement) != SQLITE_DONE)
                    ok = sql_error (job, error);
            }
            sqlite3_finalize (statement);
        }
        member_clear (&member);
        if (!ok || !found)
            return ok;
    }
}

static gboolean
create_workspace (ArchiveJob *job, GFile *parent, GError **error)
{
    g_autoptr (GFile) bound = nemo_transfer_guard_directory (job->guard, parent, TRUE, error);
    if (!bound)
        return FALSE;
    g_autofree char *path = g_file_get_path (bound);
    job->scratch_parent_fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (job->scratch_parent_fd < 0)
        return system_error (error, _("Could not pin the archive workspace parent"));
    g_autofree char *uuid = g_uuid_string_random ();
    job->scratch_name = g_strconcat (".nemo-archive-", uuid, NULL);
    g_autoptr (GFile) scratch = g_file_get_child (parent, job->scratch_name);
    job->scratch_path = g_file_get_path (scratch);
    if (mkdirat (job->scratch_parent_fd, job->scratch_name, 0700) < 0)
        return system_error (error, _("Could not create the private on-disk archive workspace"));
    job->scratch_fd = openat (job->scratch_parent_fd, job->scratch_name,
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (job->scratch_fd < 0 || fstat (job->scratch_fd, &job->scratch_identity) < 0)
        return system_error (error, _("Could not identify the archive workspace"));
    int fd = openat (job->scratch_fd, "manifest.sqlite", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return system_error (error, _("Could not create the archive manifest"));
    gboolean ok = fstat (fd, &job->manifest_identity) == 0;
    if (close (fd) < 0)
        ok = FALSE;
    if (!ok)
        return system_error (error, _("Could not identify the archive manifest"));
    g_autofree char *database_path = g_strdup_printf ("/proc/self/fd/%d/manifest.sqlite", job->scratch_fd);
    if (!register_manifest_vfs ())
        return failure (error, _("Could not initialize the descriptor-bound archive manifest."));
    if (sqlite3_open_v2 (database_path, &job->db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                         manifest_vfs.zName) != SQLITE_OK)
        return sql_error (job, error);
    sqlite3_extended_result_codes (job->db, 1);
    return sql_exec (job,
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA cache_size=-2048;"
        "CREATE TABLE members(id INTEGER PRIMARY KEY,uri TEXT NOT NULL UNIQUE,name TEXT NOT NULL UNIQUE,"
        "type INTEGER NOT NULL,size INTEGER NOT NULL,mtime INTEGER,nsec INTEGER,native INTEGER,"
        "identity BLOB,signature TEXT,link TEXT,sha256 TEXT,device INTEGER,inode INTEGER,"
        "verified INTEGER NOT NULL DEFAULT 0,removed INTEGER NOT NULL DEFAULT 0);"
        "CREATE UNIQUE INDEX native_files ON members(device,inode) WHERE native=1 AND type=32768; BEGIN", error);
}

static gboolean
close_workspace (ArchiveJob *job, GError **error)
{
    gboolean ok = TRUE;
    if (job->db) {
        /* Commit the partial manifest as well: it identifies the exact archived
         * input set and the entries whose removal was confirmed. */
        if (!sqlite3_get_autocommit (job->db))
            ok = sql_exec (job, "COMMIT", error);
        if (sqlite3_close (job->db) != SQLITE_OK && ok)
            ok = failure (error, _("Could not close the archive manifest."));
        job->db = NULL;
    }
    if (job->scratch_fd >= 0) {
        struct stat current;
        if (job->keep_manifest || !ok) {
            g_string_append_printf (job->details,
                _("\nRetained archive manifest: %s/manifest.sqlite\n"
                  "The members table records original URIs, archive names, SHA-256 values and confirmed removals. "
                  "Entries with removed=0 must be inspected at their original or reported recovery paths."),
                job->scratch_path);
            if ((fsync (job->scratch_fd) < 0 || fsync (job->scratch_parent_fd) < 0) && ok)
                ok = system_error (error, _("Could not synchronize the retained manifest location"));
        } else if (fstatat (job->scratch_fd, "manifest.sqlite", &current, AT_SYMLINK_NOFOLLOW) == 0) {
            if (current.st_dev != job->manifest_identity.st_dev || current.st_ino != job->manifest_identity.st_ino)
                ok = failure (error, _("The archive manifest changed; its workspace was retained."));
            else if (unlinkat (job->scratch_fd, "manifest.sqlite", 0) < 0)
                ok = system_error (error, _("Could not remove the private archive manifest"));
        } else if (errno != ENOENT) {
            ok = system_error (error, _("Could not inspect the private archive manifest"));
        }
        if (!job->keep_manifest && ok) {
            if (fstatat (job->scratch_parent_fd, job->scratch_name, &current, AT_SYMLINK_NOFOLLOW) < 0 ||
                !same_identity (&job->scratch_identity, &current, TRUE)) {
                ok = failure (error, _("The archive workspace changed; it was retained."));
            } else if (unlinkat (job->scratch_parent_fd, job->scratch_name, AT_REMOVEDIR) < 0 ||
                       fsync (job->scratch_parent_fd) < 0) {
                ok = system_error (error, _("Could not remove the private archive workspace"));
            }
        }
        if (close (job->scratch_fd) < 0 && ok)
            ok = system_error (error, _("Could not close the archive workspace"));
        job->scratch_fd = -1;
    }
    if (!ok && job->scratch_path)
        g_string_append_printf (job->details, _("\nInspect archive workspace: %s"), job->scratch_path);
    if (job->scratch_parent_fd >= 0) {
        if (close (job->scratch_parent_fd) < 0 && ok)
            ok = system_error (error, _("Could not close the archive workspace parent"));
        job->scratch_parent_fd = -1;
    }
    return ok;
}

static gboolean
run_archive (ArchiveJob *job, GError **error)
{
    if (!job->sources)
        return failure (error, _("No archive inputs were selected."));
    g_autofree char *basename = g_file_get_basename (job->destination);
    if (!basename || strlen (basename) < 3 ||
        g_ascii_strcasecmp (basename + strlen (basename) - 3, ".7z") != 0)
        return failure (error, _("The archive destination must have a .7z extension."));
    g_autoptr (GFile) parent = g_file_get_parent (job->destination);
    if (!parent)
        return failure (error, _("The archive needs a supported local destination folder."));
    job->guard = nemo_transfer_guard_new (job->sources, parent, job->move);
    if (!nemo_transfer_guard_check (job->guard, error))
        return FALSE;
    g_autoptr (GFile) bound_destination = nemo_transfer_guard_file (job->guard, job->destination, TRUE, error);
    if (!bound_destination)
        return FALSE;
    g_autofree char *destination_path = g_file_get_path (bound_destination);
    if (lstat (destination_path, &job->destination_identity) == 0) {
        job->destination_exists = TRUE;
        if (!job->overwrite) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, _("The archive destination already exists."));
            return FALSE;
        }
        if (!S_ISREG (job->destination_identity.st_mode))
            return failure (error, _("Only an existing regular archive file can be replaced."));
        if (!nemo_transfer_guard_expect (job->guard, job->destination, error))
            return FALSE;
        struct stat current;
        if (lstat (destination_path, &current) < 0 ||
            !same_identity (&job->destination_identity, &current, FALSE))
            return failure (error, _("The archive destination changed before replacement was prepared."));
    } else if (errno != ENOENT) {
        return system_error (error, _("Could not inspect the archive destination"));
    }
    if (!create_workspace (job, parent, error))
        return FALSE;
    nemo_progress_info_set_status (job->progress, _("Scanning archive inputs"));
    if (!scan_sources (job, error))
        return FALSE;
    job->transaction = nemo_transfer_transaction_new (job->guard, job->sources->data,
                                                      job->destination, job->cancel, error);
    if (!job->transaction)
        return FALSE;
    g_autofree char *stage_path = g_file_get_path (nemo_transfer_transaction_stage (job->transaction));
    job->stage_fd = open (stage_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (job->stage_fd < 0)
        return system_error (error, _("Could not create the exclusive archive staging file"));
    if (!nemo_transfer_transaction_stage_created (job->transaction, error))
        return FALSE;
    nemo_progress_info_set_status (job->progress, _("Creating 7z archive — maximum compression"));
    if (!compress_sources (job, error))
        return FALSE;
    nemo_progress_info_set_status (job->progress, _("Independently verifying every archive member"));
    nemo_progress_info_set_details (job->progress, _("Checking names, types, normalized metadata and SHA-256 content"));
    g_autofree char *digest = NULL;
    if (!verify_archive (job, &digest, error) || !recheck_sources (job, error) ||
        !sql_exec (job, "COMMIT", error) ||
        !nemo_transfer_transaction_expect_archive (job->transaction, digest, error))
        return FALSE;
    int stage_fd = job->stage_fd;
    job->stage_fd = -1;
    if (close (stage_fd) < 0)
        return system_error (error, _("Could not close the verified archive staging file"));
    nemo_progress_info_set_status (job->progress, _("Publishing verified archive"));
    if (!nemo_transfer_transaction_publish (job->transaction, job->overwrite,
            &job->result.published, job->cancel, error) ||
        !nemo_transfer_transaction_finish (job->transaction, error) ||
        !nemo_transfer_transaction_confirm_archive (job->transaction, digest, error))
        return FALSE;
    job->result.archive_ok = TRUE;
    if (job->move) {
        job->keep_manifest = TRUE;
        nemo_progress_info_set_status (job->progress, _("Removing verified archived originals"));
        if (!retire_sources (job, error)) {
            GError *archive_error = NULL;
            if (!nemo_transfer_transaction_confirm_archive (job->transaction, digest, &archive_error)) {
                job->result.archive_ok = FALSE;
                g_string_append_printf (job->details,
                    _("\nThe published archive can no longer be confirmed: %s"), archive_error->message);
                g_clear_error (&archive_error);
            }
            return FALSE;
        }
        job->result.sources_removed = TRUE;
        if (!nemo_transfer_transaction_confirm_archive (job->transaction, digest, error)) {
            job->result.archive_ok = FALSE;
            return FALSE;
        }
        job->keep_manifest = FALSE;
    }
    return TRUE;
}

static void
archive_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancel)
{
    ArchiveJob *job = task_data;
    (void) source_object;
    (void) cancel;
    nemo_progress_info_start (job->progress);
    /* libarchive occasionally converts even explicitly UTF-8 entry names via
     * the calling locale (notably directory names). Use a thread-local CTYPE,
     * never setlocale(), which would change the GUI and other transfer jobs. */
    locale_t locale = duplocale (uselocale ((locale_t) 0));
    if (locale) {
        locale_t utf8 = newlocale (LC_CTYPE_MASK, "C.UTF-8", locale);
        if (!utf8)
            freelocale (locale);
        locale = utf8;
    }
    locale_t previous_locale = locale ? uselocale (locale) : (locale_t) 0;
    gboolean ok = previous_locale ? run_archive (job, &job->error) :
        failure (&job->error, _("A UTF-8 locale is required to create archives safely."));
    if (job->stage_fd >= 0) {
        if (close (job->stage_fd) < 0 && ok)
            ok = system_error (&job->error, _("Could not close archive staging data"));
        job->stage_fd = -1;
    }
    GError *cleanup_error = NULL;
    if (!close_workspace (job, &cleanup_error)) {
        if (ok) {
            job->error = g_steal_pointer (&cleanup_error);
            ok = FALSE;
        } else if (cleanup_error) {
            g_string_append_printf (job->details, "\n%s", cleanup_error->message);
        }
        g_clear_error (&cleanup_error);
    }
    if (job->transaction) {
        nemo_transfer_transaction_free (job->transaction);
        job->transaction = NULL;
    }
    if (job->guard) {
        if (!nemo_transfer_guard_release (job->guard, &cleanup_error)) {
            if (ok) {
                job->error = g_steal_pointer (&cleanup_error);
                ok = FALSE;
            } else if (cleanup_error) {
                g_string_append_printf (job->details, "\n%s", cleanup_error->message);
            }
            g_clear_error (&cleanup_error);
        }
        g_autofree char *guard_details = nemo_transfer_guard_take_details (job->guard);
        g_string_append (job->details, guard_details);
    }
    if (!ok && !job->error)
        failure (&job->error, _("Archive creation did not complete."));
    g_autofree char *destination = g_file_get_parse_name (job->destination);
    g_string_prepend (job->details, "\n");
    if (job->error)
        g_string_prepend (job->details, job->error->message);
    g_string_append_printf (job->details,
        _("\nArchive destination: %s\nVerified archived entries: %" G_GUINT64_FORMAT
          "\nConfirmed removed originals: %" G_GUINT64_FORMAT), destination,
        job->result.archived_items, job->result.removed_items);
    if (!job->result.published)
        g_string_append (job->details, _("\nNo archive was published. All original sources were retained."));
    else if (!job->result.archive_ok)
        g_string_append (job->details, _("\nThe published archive is not confirmed. Inspect the destination, original-source and recovery locations. Do not remove any remaining originals."));
    else if (job->move && !job->result.sources_removed)
        g_string_append (job->details, _("\nThe verified archive was published, but original-source removal is incomplete. Do not retry removal blindly."));
    NemoProgressResult result = {
        .operation = job->move ? NEMO_PROGRESS_OPERATION_MOVE : NEMO_PROGRESS_OPERATION_COPY,
        .outcome = ok ? NEMO_PROGRESS_OUTCOME_SUCCESS :
            job->result.published ? NEMO_PROGRESS_OUTCOME_PARTIAL :
            g_error_matches (job->error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ?
                NEMO_PROGRESS_OUTCOME_CANCELLED : NEMO_PROGRESS_OUTCOME_FAILED,
        .verification_requested = TRUE,
        .completed_items = job->result.archive_ok ? 1 : 0,
        .completed_regular_files = job->result.archive_ok ? 1 : 0,
        .checksum_verified_files = job->result.archive_ok ? 1 : 0,
        .failed_items = ok ? 0 : 1
    };
    nemo_progress_info_set_result (job->progress, &result);
    nemo_progress_info_take_completion_context (job->progress, g_strdup (
        job->move ? _("Create verified 7z archive, then remove originals") : _("Create verified 7z archive")));
    nemo_progress_info_take_completion_details (job->progress, g_strdup (job->details->str));
    nemo_progress_info_set_status (job->progress, ok ? _("Archive operation completed") : _("Archive operation incomplete"));
    nemo_progress_info_finish (job->progress);
    if (previous_locale)
        uselocale (previous_locale);
    if (locale)
        freelocale (locale);
    g_task_return_boolean (task, ok);
}

static void
archive_job_free (gpointer data)
{
    ArchiveJob *job = data;
    g_list_free_full (job->sources, g_object_unref);
    g_object_unref (job->destination);
    g_object_unref (job->progress);
    g_object_unref (job->cancel);
    g_object_unref (job->monitor);
    if (job->guard)
        nemo_transfer_guard_unref (job->guard);
    g_clear_error (&job->error);
    g_free (job->scratch_name);
    g_free (job->scratch_path);
    g_string_free (job->details, TRUE);
    g_free (job);
}

NemoProgressInfo *
nemo_archive_create_async (GList *sources, GFile *destination, gboolean move,
                            gboolean overwrite, GtkWindow *parent,
                            GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_val_if_fail (G_IS_FILE (destination), NULL);
    ArchiveJob *job = g_new0 (ArchiveJob, 1);
    job->sources = g_list_copy (sources);
    for (GList *item = job->sources; item; item = item->next)
        g_object_ref (item->data);
    job->destination = g_object_ref (destination);
    job->move = move;
    job->overwrite = overwrite;
    job->scratch_fd = job->scratch_parent_fd = job->stage_fd = -1;
    job->details = g_string_new (NULL);
    job->progress = nemo_progress_info_new ();
    job->cancel = nemo_progress_info_get_cancellable (job->progress);
    /* Initialize the shared mount monitor on the dispatched main context,
     * before the safety guard is constructed in the worker. */
    job->monitor = g_unix_mount_monitor_get ();
    nemo_progress_info_set_parent_window (job->progress, parent);
    nemo_progress_info_set_status (job->progress, _("Preparing archive creation"));
    nemo_progress_info_queue (job->progress);
    g_autoptr (GTask) task = g_task_new (job->progress, NULL, callback, user_data);
    g_task_set_source_tag (task, nemo_archive_create_async);
    g_task_set_check_cancellable (task, FALSE);
    g_task_set_task_data (task, job, archive_job_free);
    NemoProgressInfo *progress = g_object_ref (job->progress);
    g_task_run_in_thread (task, archive_thread);
    return progress;
}

gboolean
nemo_archive_create_finish (GAsyncResult *result, NemoArchiveCreateResult *outcome, GError **error)
{
    g_return_val_if_fail (G_IS_TASK (result), FALSE);
    g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == nemo_archive_create_async, FALSE);
    ArchiveJob *job = g_task_get_task_data (G_TASK (result));
    if (outcome)
        *outcome = job->result;
    gboolean ok = g_task_propagate_boolean (G_TASK (result), NULL);
    if (!ok && job->error)
        g_propagate_error (error, g_error_copy (job->error));
    return ok;
}
#endif
