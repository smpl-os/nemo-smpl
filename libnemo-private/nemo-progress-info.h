/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   nemo-progress-info.h: file operation progress info.
 
   Copyright (C) 2007 Red Hat, Inc.
  
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
  
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
  
   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 51 Franklin Street - Suite 500,
   Boston, MA 02110-1335, USA.
  
   Author: Alexander Larsson <alexl@redhat.com>
*/

#ifndef NEMO_PROGRESS_INFO_H
#define NEMO_PROGRESS_INFO_H

#include <glib-object.h>
#include <gio/gio.h>

#define NEMO_TYPE_PROGRESS_INFO         (nemo_progress_info_get_type ())
#define NEMO_PROGRESS_INFO(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), NEMO_TYPE_PROGRESS_INFO, NemoProgressInfo))
#define NEMO_PROGRESS_INFO_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), NEMO_TYPE_PROGRESS_INFO, NemoProgressInfoClass))
#define NEMO_IS_PROGRESS_INFO(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), NEMO_TYPE_PROGRESS_INFO))
#define NEMO_IS_PROGRESS_INFO_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), NEMO_TYPE_PROGRESS_INFO))
#define NEMO_PROGRESS_INFO_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), NEMO_TYPE_PROGRESS_INFO, NemoProgressInfoClass))

typedef struct _NemoProgressInfo      NemoProgressInfo;
typedef struct _NemoProgressInfoClass NemoProgressInfoClass;

#ifdef NEMO_SMPL
typedef enum {
	NEMO_PROGRESS_OUTCOME_UNKNOWN,
	NEMO_PROGRESS_OUTCOME_SUCCESS,
	NEMO_PROGRESS_OUTCOME_PARTIAL,
	NEMO_PROGRESS_OUTCOME_FAILED,
	NEMO_PROGRESS_OUTCOME_CANCELLED,
	/* A neutral COPY result, not content satisfaction or callback success. */
	NEMO_PROGRESS_OUTCOME_RETAINED
} NemoProgressOutcome;

typedef enum {
	NEMO_PROGRESS_OPERATION_UNKNOWN,
	NEMO_PROGRESS_OPERATION_COPY,
	NEMO_PROGRESS_OPERATION_MOVE
} NemoProgressOperation;

typedef struct {
	NemoProgressOutcome outcome;
	NemoProgressOperation operation;
	/* Completed entry operations, including recursive directories. An atomic
	 * directory move counts once; its contents were not individually visited. */
	guint64 completed_items;
	guint64 completed_regular_files;
	guint64 completed_symlinks;
	/* Distinct incomplete paths; skipped and failed are disjoint. */
	guint64 skipped_items;
	guint64 failed_items;
	/* These are subsets of completed_items, never checks of failed copies. */
	guint64 checksum_verified_files;
	guint64 verified_symlinks;
	guint64 atomic_moves;
	guint64 completed_directories;
	/* Disjoint from completed_items: no destination was created or replaced. */
	guint64 existing_verified_regular_files;
	guint64 existing_verified_symlinks;
	/* Disjoint from skipped_items/failed_items; equality is unknown. */
	guint64 unverified_retained_files;
	gboolean verification_requested;
} NemoProgressResult;

/* Copies a snapshot. Updates after finish are ignored. */
void          nemo_progress_info_set_result (NemoProgressInfo *info,
                                            const NemoProgressResult *result);
gboolean      nemo_progress_info_get_result (NemoProgressInfo *info,
                                            NemoProgressResult *result);
char *        nemo_progress_info_get_completion_text (NemoProgressInfo *info);
void          nemo_progress_info_take_completion_details (NemoProgressInfo *info,
                                                         char *details);
#endif

GType nemo_progress_info_get_type (void) G_GNUC_CONST;

/* Signals:
   "changed" - status or details changed
   "progress-changed" - the percentage progress changed (or we pulsed if in activity_mode
   "started" - emited on job start
   "finished" - emitted when job is done
   
   All signals are emitted from idles in main loop.
   All methods are threadsafe.
 */

NemoProgressInfo *nemo_progress_info_new (void);

GList *       nemo_get_all_progress_info (void);

char *        nemo_progress_info_get_status      (NemoProgressInfo *info);
char *        nemo_progress_info_get_details     (NemoProgressInfo *info);
char *        nemo_progress_info_get_initial_details (NemoProgressInfo *info);
double        nemo_progress_info_get_progress    (NemoProgressInfo *info);
GCancellable *nemo_progress_info_get_cancellable (NemoProgressInfo *info);
void          nemo_progress_info_cancel          (NemoProgressInfo *info);
gboolean      nemo_progress_info_get_is_started  (NemoProgressInfo *info);
gboolean      nemo_progress_info_get_is_finished (NemoProgressInfo *info);
gboolean      nemo_progress_info_get_is_paused   (NemoProgressInfo *info);

void          nemo_progress_info_queue           (NemoProgressInfo *info);
void          nemo_progress_info_start           (NemoProgressInfo *info);
void          nemo_progress_info_finish          (NemoProgressInfo *info);
void          nemo_progress_info_pause           (NemoProgressInfo *info);
void          nemo_progress_info_resume          (NemoProgressInfo *info);
void          nemo_progress_info_set_status      (NemoProgressInfo *info,
						      const char           *status);
void          nemo_progress_info_take_status     (NemoProgressInfo *info,
						      char                 *status);
void          nemo_progress_info_set_details     (NemoProgressInfo *info,
						      const char           *details);
void          nemo_progress_info_take_initial_details (NemoProgressInfo *info,
                              char                 *initial_details);
void          nemo_progress_info_take_details    (NemoProgressInfo *info,
						      char                 *details);
void          nemo_progress_info_set_progress    (NemoProgressInfo *info,
						      double                current,
						      double                total);
void          nemo_progress_info_pulse_progress  (NemoProgressInfo *info);

gdouble       nemo_progress_info_get_elapsed_time (NemoProgressInfo *info);


#endif /* NEMO_PROGRESS_INFO_H */
