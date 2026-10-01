/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEMO_ARCHIVE_FILE_H
#define NEMO_ARCHIVE_FILE_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Register once in each process that consumes serialized archive URIs.
 * URI construction performs no archive I/O. Query/enumerate/read are synchronous
 * worker operations; GIO's default asynchronous wrappers also work.
 *
 * No member contents are staged on disk. Non-seekable backing streams are reopened
 * and skipped for random access. Member streams support seek by cancellably
 * replaying decompression with bounded buffers; seek beyond EOF is rejected,
 * and seeking relative to EOF requires a known member size.
 * Symbolic links are not followed. Unsupported hardlink encodings, encrypted
 * members and duplicate file paths fail explicitly, never outside the archive.
 * Completed SQLite metadata indexes are shared by live related files/streams
 * and invalidated against backing identity. Builds are cancellable and never
 * publish partial indexes; lookup/iteration materialize only requested members.
 * Each private index is immediately unlinked from the user-cache filesystem,
 * uses a 2 MiB SQLite page cache (no mmap), and has a 4 GiB metadata disk ceiling.
 * The completed-index LRU retains at most 16 indexes / 4 GiB across archives.
 * Active queries/cursors may keep an evicted snapshot until they finish.
 * Last-owner release or process exit reclaims it. There is no member-count cap.
 * Paths are limited to 64 KiB / 128 components; nesting is limited to 16 levels.
 * Requires SQLite >= 3.8.2. Initial indexing scans headers once; opening/seeking
 * a selected member can still replay headers/data as required by libarchive.
 * Seekable ZIP/TAR backing uses libarchive's header seeks, with bounded read
 * ahead, rather than streaming skipped payloads. Nonseekable backing and
 * compressed TAR may require reading/discarding data. The cache bounds do not
 * bound libarchive/libisofs internal allocations.
 * Raw ISO byte streams (including nested members) use libisofs >= 1.4 for
 * canonical Rock Ridge paths and selected-file reads/seeks, not image import
 * or extraction. Outer compression filters still use libarchive. Compressed
 * zisofs members are refused explicitly rather than returned as raw bytes.
 * Libarchive runs under a scoped thread-local UTF-8 locale (one must be
 * installed); the process locale is never changed by this provider.
 */
void   nemo_archive_file_register        (void);
GFile *nemo_archive_file_new_for_archive (GFile *archive);
/* Returns a new reference, or NULL for files outside this provider. */
GFile *nemo_archive_file_get_archive     (GFile *file);

G_END_DECLS

#endif
