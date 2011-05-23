/*
 * Copyright (c) 2009, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Written by Adam Moody <moody20@llnl.gov>.
 * LLNL-CODE-411039.
 * All rights reserved.
 * This file is part of The Scalable Checkpoint / Restart (SCR) library.
 * For details, see https://sourceforge.net/projects/scalablecr/
 * Please also read this file: LICENSE.TXT.
*/

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* variable length args */
#include <stdarg.h>
#include <errno.h>

/* gethostbyname */
#include <netdb.h>

/* basename/dirname */
#include <unistd.h>
#include <libgen.h>

/* gettimeofday */
#include <sys/time.h>

/* localtime, asctime */
#include <time.h>

/* compute crc32 */
#include <zlib.h>

/* list data structures */
/* need at least version 8.5 of queue.h from Berkeley */
/*#include <sys/queue.h>*/
#include "queue.h"

#include "mpi.h"

#include "scr_conf.h"
#include "scr.h"
#include "scr_io.h"
#include "scr_util.h"
#include "scr_meta.h"
#include "scr_halt.h"
#include "scr_log.h"
#include "scr_hash.h"
#include "scr_filemap.h"
#include "scr_param.h"
#include "scr_index_api.h"

#ifdef HAVE_LIBYOGRT
#include "yogrt.h"
#endif /* HAVE_LIBYOGRT */

#ifdef HAVE_LIBGCS
#include "gcs.h"
#endif /* HAVE_LIBGCS */

/*
=========================================
Globals
=========================================
*/

#define SCR_SUMMARY_FILE_VERSION_5 (5)

#define SCR_TEST_AND_HALT (1)
#define SCR_TEST_BUT_DONT_HALT (2)

#define SCR_CURRENT_LINK "scr.current"

/* copy file operation flags: copy file vs. move file */
#define COPY_FILES 0
#define MOVE_FILES 1

/* There are three prefix directories where SCR manages files: control, cache, and pfs.
 * 
 * The control directory is a fixed location where a job records its state and reads files
 * to interpret commands from the user.  This directory is fixed (hard coded) so that
 * scr utility scripts know where to look to read and write these files.
 *
 * The cache directory is where the job will cache its checkpoint files.
 * This can be changed by the user (via SCR_CACHE_BASE) to target
 * different devices (e.g. RAM disc vs. SSD). By default, it uses the same prefix as the
 * control directory.
 *
 * The pfs prefix directory is where the job will create checkpoint directories and flush
 * checkpoint files to.  Typically, this is on a parallel file system and is set via SCR_PREFIX.
 * If SCR_PREFIX is not set, the current working directory of the running program is used.
*/

static char scr_cntl_base[SCR_MAX_FILENAME]  = SCR_CNTL_BASE;
static char scr_cache_base[SCR_MAX_FILENAME] = SCR_CACHE_BASE;

static char* scr_cntl_prefix = NULL;
static char scr_par_prefix[SCR_MAX_FILENAME]   = "";

static char scr_master_map_file[SCR_MAX_FILENAME];
static char scr_map_file[SCR_MAX_FILENAME];
static char scr_halt_file[SCR_MAX_FILENAME];
static char scr_flush_file[SCR_MAX_FILENAME];
static char scr_nodes_file[SCR_MAX_FILENAME];
static char scr_transfer_file[SCR_MAX_FILENAME];

static scr_filemap* scr_map = NULL;
static scr_hash* scr_halt_hash = NULL;

static char* scr_username    = NULL;       /* username of owner for running job */
static char* scr_jobid       = NULL;       /* unique job id string of current job */
static char* scr_jobname     = NULL;       /* jobname string, used to tie different runs together */
static int scr_checkpoint_id = 0;          /* keeps track of the checkpoint id */
static int scr_in_checkpoint = 0;          /* flag tracks whether we are between start and complete checkpoint calls */
static int scr_initialized   = 0;          /* indicates whether the library has been initialized */
static int scr_enabled       = SCR_ENABLE; /* indicates whether the library is enabled */
static int scr_debug         = SCR_DEBUG;  /* set debug verbosity */
static int scr_log_enable    = SCR_LOG_ENABLE; /* whether to log SCR events */

static int scr_page_size = 0; /* records block size for aligning MPI and file buffers */

static int scr_cache_size       = SCR_CACHE_SIZE;       /* set number of checkpoints to keep at one time */
static int scr_copy_type        = SCR_COPY_TYPE;        /* select which redundancy algorithm to use */
static int scr_hop_distance     = SCR_HOP_DISTANCE;     /* number of nodes away to choose parnter */
static int scr_set_size         = SCR_SET_SIZE;         /* specify number of tasks in xor set */
static size_t scr_mpi_buf_size  = SCR_MPI_BUF_SIZE;     /* set MPI buffer size to chunk file transfer */

static int scr_halt_seconds     = SCR_HALT_SECONDS; /* secs remaining in allocation before job should be halted */

static int scr_distribute       = SCR_DISTRIBUTE;       /* whether to call scr_distribute_files during SCR_Init */
static int scr_fetch            = SCR_FETCH;            /* whether to call scr_fetch_files during SCR_Init */
static int scr_fetch_width      = SCR_FETCH_WIDTH;      /* specify number of processes to read files simultaneously */
static int scr_flush            = SCR_FLUSH;            /* how many checkpoints between flushes */
static int scr_flush_width      = SCR_FLUSH_WIDTH;      /* specify number of processes to write files simultaneously */
static int scr_flush_on_restart = SCR_FLUSH_ON_RESTART; /* specify whether to flush cache on restart */
static int scr_global_restart   = SCR_GLOBAL_RESTART;   /* set if code must be restarted from parallel file system */
static int scr_flush_async      = SCR_FLUSH_ASYNC;      /* whether to use asynchronous flush */
static double scr_flush_async_bw = SCR_FLUSH_ASYNC_BW;  /* bandwidth limit imposed during async flush */
static double scr_flush_async_percent = SCR_FLUSH_ASYNC_PERCENT;  /* runtime limit imposed during async flush */
static size_t scr_file_buf_size = SCR_FILE_BUF_SIZE;    /* set buffer size to chunk file copies to/from parallel file system */

static int scr_crc_on_copy   = SCR_CRC_ON_COPY;   /* whether to enable crc32 checks during scr_swap_files() */
static int scr_crc_on_flush  = SCR_CRC_ON_FLUSH;  /* whether to enable crc32 checks during flush and fetch */
static int scr_crc_on_delete = SCR_CRC_ON_DELETE; /* whether to enable crc32 checks when deleting checkpoints */

static int    scr_checkpoint_interval = SCR_CHECKPOINT_INTERVAL; /* times to call Need_checkpoint between checkpoints */
static int    scr_checkpoint_seconds   = SCR_CHECKPOINT_SECONDS;   /* min number of seconds between checkpoints */
static double scr_checkpoint_overhead  = SCR_CHECKPOINT_OVERHEAD;  /* max allowed overhead for checkpointing */
static int    scr_need_checkpoint_id   = 0;    /* tracks the number of times Need_checkpoint has been called */
static double scr_time_checkpoint_total = 0.0; /* keeps a running total of the time spent to checkpoint */
static int    scr_time_checkpoint_count = 0;   /* keeps a running count of the number of checkpoints taken */

static time_t scr_timestamp_checkpoint_start;  /* record timestamp of start of checkpoint */
static double scr_time_checkpoint_start;       /* records the start time of the current checkpoint */
static double scr_time_checkpoint_end;         /* records the end time of the current checkpoint */

static time_t scr_timestamp_compute_start;     /* record timestamp of start of compute phase */
static double scr_time_compute_start;          /* records the start time of the current compute phase */
static double scr_time_compute_end;            /* records the end time of the current compute phase */

static MPI_Comm scr_comm_world = MPI_COMM_NULL; /* dup of MPI_COMM_WORLD */
static MPI_Comm scr_comm_local = MPI_COMM_NULL; /* contains all tasks local to the same node */
static MPI_Comm scr_comm_level = MPI_COMM_NULL; /* contains tasks across all nodes at the same local rank level */

static int scr_ranks_world = 0; /* number of ranks in the job */
static int scr_ranks_local = 0; /* number of ranks on my node */
static int scr_ranks_level = 0; /* number of ranks at my level (i.e., processes with same local rank across nodes) */

static int  scr_my_rank_world = MPI_PROC_NULL;  /* my rank in world */
static int  scr_my_rank_local = MPI_PROC_NULL;  /* my local rank on my node */
static int  scr_my_rank_level = MPI_PROC_NULL;  /* my rank in processes at my level */

static char scr_my_hostname[256] = "";

scr_hash* scr_cachedesc_hash = NULL;
scr_hash* scr_ckptdesc_hash  = NULL;

/*
=========================================
Define checkpoint descriptor structure
=========================================
*/

struct scr_ckptdesc {
  int      enabled;
  int      index;
  int      interval;
  char*    base;
  char*    directory;
  int      copy_type;
  int      hop_distance;
  int      set_size;
  MPI_Comm comm;
  int      groups;
  int      group_id;
  int      ranks;
  int      my_rank;
  int      lhs_rank;
  int      lhs_rank_world;
  char     lhs_hostname[256];
  int      rhs_rank;
  int      rhs_rank_world;
  char     rhs_hostname[256];
};

static int scr_nckptdescs = 0;
static struct scr_ckptdesc* scr_ckptdescs = NULL;

/*
=========================================
Error and Debug Messages
=========================================
*/

/* print message to stderr */
void scr_err(const char *fmt, ...)
{
  va_list argp;
  fprintf(stderr, "SCR ERROR: rank %d on %s: ", scr_my_rank_world, scr_my_hostname);
  va_start(argp, fmt);
  vfprintf(stderr, fmt, argp);
  va_end(argp);
  fprintf(stderr, "\n");
}

/* print message to stdout if scr_debug is set and it is >= level */
void scr_dbg(int level, const char *fmt, ...)
{
  va_list argp;
  if (level == 0 || (scr_debug > 0 && scr_debug >= level)) {
    fprintf(stdout, "SCR: rank %d on %s: ", scr_my_rank_world, scr_my_hostname);
    va_start(argp, fmt);
    vfprintf(stdout, fmt, argp);
    va_end(argp);
    fprintf(stdout, "\n");
  }
}

/* print abort message and call MPI_Abort to kill run */
void scr_abort(int rc, const char *fmt, ...)
{
  va_list argp;
  fprintf(stderr, "SCR ABORT: rank %d on %s: ", scr_my_rank_world, scr_my_hostname);
  va_start(argp, fmt);
  vfprintf(stderr, fmt, argp);
  va_end(argp);
  fprintf(stderr, "\n");

  MPI_Abort(MPI_COMM_WORLD, 0);
}

/*
=========================================
MPI utility functions
=========================================
*/

/* returns true (non-zero) if flag on each process in scr_comm_world is true */
static int scr_alltrue(int flag)
{
  int all_true = 0;
  MPI_Allreduce(&flag, &all_true, 1, MPI_INT, MPI_LAND, scr_comm_world);
  return all_true;
}

/* given a comm as input, find the left and right partner ranks and hostnames */
static int scr_set_partners(MPI_Comm comm, int dist,
                            int* lhs_rank, int* lhs_rank_world, char* lhs_hostname,
                            int* rhs_rank, int* rhs_rank_world, char* rhs_hostname
                           )
{
  /* find our position in the communicator */
  int my_rank, ranks;
  MPI_Comm_rank(comm, &my_rank);
  MPI_Comm_size(comm, &ranks);

  /* shift parter distance to a valid range */
  while (dist > ranks) {
    dist -= ranks;
  }
  while (dist < 0) {
    dist += ranks;
  }

  /* compute ranks to our left and right partners */
  int lhs = (my_rank + ranks - dist) % ranks;
  int rhs = (my_rank + ranks + dist) % ranks;
  (*lhs_rank) = lhs;
  (*rhs_rank) = rhs;

  /* fetch hostnames from my left and right partners */
  strcpy(lhs_hostname, "");
  strcpy(rhs_hostname, "");

  MPI_Request request[2];
  MPI_Status  status[2];

  /* shift hostnames to the right */
  MPI_Irecv(lhs_hostname,    sizeof(scr_my_hostname), MPI_CHAR, lhs, 0, comm, &request[0]);
  MPI_Isend(scr_my_hostname, sizeof(scr_my_hostname), MPI_CHAR, rhs, 0, comm, &request[1]);
  MPI_Waitall(2, request, status);

  /* shift hostnames to the left */
  MPI_Irecv(rhs_hostname,    sizeof(scr_my_hostname), MPI_CHAR, rhs, 0, comm, &request[0]);
  MPI_Isend(scr_my_hostname, sizeof(scr_my_hostname), MPI_CHAR, lhs, 0, comm, &request[1]);
  MPI_Waitall(2, request, status);

  /* shift rank in scr_comm_world to the right */
  MPI_Irecv(lhs_rank_world,     1, MPI_INT, lhs, 0, comm, &request[0]);
  MPI_Isend(&scr_my_rank_world, 1, MPI_INT, rhs, 0, comm, &request[1]);
  MPI_Waitall(2, request, status);

  /* shift rank in scr_comm_world to the left */
  MPI_Irecv(rhs_rank_world,     1, MPI_INT, rhs, 0, comm, &request[0]);
  MPI_Isend(&scr_my_rank_world, 1, MPI_INT, lhs, 0, comm, &request[1]);
  MPI_Waitall(2, request, status);

  return SCR_SUCCESS;
}

/*
=========================================
Configuration file
=========================================
*/

/* read parameters from config file and fill in hash (parallel) */
int scr_config_read(const char* file, scr_hash* hash)
{
  int rc = SCR_FAILURE;

  /* only rank 0 reads the file */
  if (scr_my_rank_world == 0) {
    rc = scr_config_read_serial(file, hash);
  }

  /* broadcast whether rank 0 read the file ok */
  MPI_Bcast(&rc, 1, MPI_INT, 0, scr_comm_world);

  /* if rank 0 read the file, broadcast the hash */
  if (rc == SCR_SUCCESS) {
    rc = scr_hash_bcast(hash, 0, scr_comm_world);
  }

  return rc;
}

/*
=========================================
Checkpoint descriptor functions
=========================================
*/

/* initialize the specified checkpoint descriptor struct */
static int scr_ckptdesc_init(struct scr_ckptdesc* c)
{
  /* check that we got a valid checkpoint descriptor */
  if (c == NULL) {
    scr_err("No checkpoint descriptor to fill from hash @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* initialize the descriptor */
  c->enabled        = 0;
  c->index          = -1;
  c->interval       = -1;
  c->base           = NULL;
  c->directory      = NULL;
  c->copy_type      = SCR_COPY_NULL;
  c->comm           = MPI_COMM_NULL;
  c->groups         = 0;
  c->group_id       = -1;
  c->ranks          = 0;
  c->my_rank        = MPI_PROC_NULL;
  c->lhs_rank       = MPI_PROC_NULL;
  c->lhs_rank_world = MPI_PROC_NULL;
  strcpy(c->lhs_hostname, "");
  c->rhs_rank       = MPI_PROC_NULL;
  c->rhs_rank_world = MPI_PROC_NULL;
  strcpy(c->rhs_hostname, "");

  return SCR_SUCCESS;
}

/* free any memory associated with the specified checkpoint descriptor struct */
static int scr_ckptdesc_free(struct scr_ckptdesc* c)
{
  /* free the strings we strdup'd */
  if (c->base != NULL) {
    free(c->base);
    c->base = NULL;
  }

  if (c->directory != NULL) {
    free(c->directory);
    c->directory = NULL;
  }

  /* free the communicator we created */
  if (c->comm != MPI_COMM_NULL) {
    MPI_Comm_free(&c->comm);
  }

  return SCR_SUCCESS;
}

/* given a checkpoint id and a list of checkpoint descriptor structs,
 * select and return a pointer to a descriptor for the specified checkpoint id */
static struct scr_ckptdesc* scr_ckptdesc_get(int checkpoint_id, int nckpts, struct scr_ckptdesc* ckpts)
{
  struct scr_ckptdesc* c = NULL;

  /* pick the checkpoint descriptor that is:
   *   enabled
   *   has the highest interval that divides checkpoint_id evenly */
  int i;
  int interval = 0;
  for (i=0; i < nckpts; i++) {
    if (ckpts[i].enabled &&
        interval < ckpts[i].interval &&
        checkpoint_id % ckpts[i].interval == 0)
    {
      c = &ckpts[i];
      interval = ckpts[i].interval;
    }
  }

  return c;
}

/* convert the specified checkpoint descritpor struct into a corresponding hash */
static int scr_ckptdesc_store_to_hash(const struct scr_ckptdesc* c, scr_hash* hash)
{
  /* check that we got a valid pointer to a checkpoint descriptor and a hash */
  if (c == NULL || hash == NULL) {
    return SCR_FAILURE;
  }

  /* clear the hash */
  scr_hash_unset_all(hash);

  /* set the ENABLED key */
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_ENABLED, c->enabled);

  /* set the INDEX key */
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_INDEX, c->index);

  /* set the INTERVAL key */
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_INTERVAL, c->interval);

  /* set the BASE key */
  if (c->base != NULL) {
    scr_hash_set_kv(hash, SCR_CONFIG_KEY_BASE, c->base);
  }

  /* set the DIRECTORY key */
  if (c->directory != NULL) {
    scr_hash_set_kv(hash, SCR_CONFIG_KEY_DIRECTORY, c->directory);
  }

  /* set the TYPE key */
  switch (c->copy_type) {
  case SCR_COPY_LOCAL:
    scr_hash_set_kv(hash, SCR_CONFIG_KEY_TYPE, "LOCAL");
    break;
  case SCR_COPY_PARTNER:
    scr_hash_set_kv(hash, SCR_CONFIG_KEY_TYPE, "PARTNER");
    break;
  case SCR_COPY_XOR:
    scr_hash_set_kv(hash, SCR_CONFIG_KEY_TYPE, "XOR");
    break;
  }

  /* set the GROUP_ID and GROUP_RANK keys */
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_GROUPS,     c->groups);
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_GROUP_ID,   c->group_id);
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_GROUP_SIZE, c->ranks);
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_GROUP_RANK, c->my_rank);

  /* set the DISTANCE and SIZE */
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_HOP_DISTANCE, c->hop_distance);
  scr_hash_set_kv_int(hash, SCR_CONFIG_KEY_SET_SIZE,     c->set_size);

  return SCR_SUCCESS;
}

/* build a checkpoint descriptor corresponding to the specified hash,
 * this function is collective, because it issues MPI calls */
static int scr_ckptdesc_create_from_hash(struct scr_ckptdesc* c, int index, const scr_hash* hash)
{
  int rc = SCR_SUCCESS;

  /* check that we got a valid checkpoint descriptor */
  if (c == NULL) {
    scr_err("No checkpoint descriptor to fill from hash @ %s:%d",
            __FILE__, __LINE__
    );
    rc = SCR_FAILURE;
  }

  /* check that we got a valid pointer to a hash */
  if (hash == NULL) {
    scr_err("No hash specified to build checkpoint descriptor from @ %s:%d",
            __FILE__, __LINE__
    );
    rc = SCR_FAILURE;
  }

  /* check that everyone made it this far */
  if (!scr_alltrue(rc == SCR_SUCCESS)) {
    if (c != NULL) {
      c->enabled = 0;
    }
    return SCR_FAILURE;
  }

  /* initialize the descriptor */
  scr_ckptdesc_init(c);

  char* value = NULL;

  /* enable / disable the checkpoint */
  c->enabled = 1;
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_ENABLED);
  if (value != NULL) {
    c->enabled = atoi(value);
  }

  /* index of the checkpoint */
  c->index = index;
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_INDEX);
  if (value != NULL) {
    c->index = atoi(value);
  }

  /* set the checkpoint interval, default to 1 unless specified otherwise */
  c->interval = 1;
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_INTERVAL);
  if (value != NULL) {
    c->interval = atoi(value);
  }

  /* set the base checkpoint directory */
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_BASE);
  if (value != NULL) {
    c->base = strdup(value);
  }

  /* build the checkpoint directory name */
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_DIRECTORY);
  if (value != NULL) {
    /* directory name already set, just copy it */
    c->directory = strdup(value);
  } else if (c->base != NULL) {
    /* directory name was not already set, so we need to build it */
    char str[100];
    sprintf(str, "%d", c->index);
    int dirname_size = strlen(c->base) + 1 + strlen(scr_username) + strlen("/scr.") + strlen(scr_jobid) +
                       strlen("/index.") + strlen(str) + 1;
    c->directory = (char*) malloc(dirname_size);
    sprintf(c->directory, "%s/%s/scr.%s/index.%s", c->base, scr_username, scr_jobid, str);
  }
    
  /* set the partner hop distance */
  c->hop_distance = scr_hop_distance;
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_HOP_DISTANCE);
  if (value != NULL) {
    c->hop_distance = atoi(value);
  }

  /* set the xor set size */
  c->set_size = scr_set_size;
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_SET_SIZE);
  if (value != NULL) {
    c->set_size = atoi(value);
  }

  /* read the checkpoint type from the hash, and build our checkpoint communicator */
  value = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_TYPE);
  if (value != NULL) {
    if (strcasecmp(value, "LOCAL") == 0) {
      c->copy_type = SCR_COPY_LOCAL;
    } else if (strcasecmp(value, "PARTNER") == 0) {
      c->copy_type = SCR_COPY_PARTNER;
    } else if (strcasecmp(value, "XOR") == 0) {
      c->copy_type = SCR_COPY_XOR;
    } else {
      c->enabled = 0;
      if (scr_my_rank_world == 0) {
        scr_err("Unknown copy type %s in checkpoint descriptor %d, disabling checkpoint @ %s:%d",
                value, c->index, __FILE__, __LINE__
        );
      }
    }

    /* CONVENIENCE: if all ranks are on the same node, change checkpoint type to LOCAL */
    if (scr_ranks_local == scr_ranks_world) {
      if (scr_my_rank_world == 0) {
        if (c->copy_type != SCR_COPY_LOCAL) {
          /* print a warning if we changed things on the user */
          scr_dbg(1, "Forcing copy type to LOCAL in checkpoint descriptor %d @ %s:%d",
                  c->index, __FILE__, __LINE__
          );
        }
      }
      c->copy_type = SCR_COPY_LOCAL;
    }

    /* build the checkpoint communicator */
    char* group_id_str   = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_GROUP_ID);
    char* group_rank_str = scr_hash_elem_get_first_val(hash, SCR_CONFIG_KEY_GROUP_RANK);
    if (group_id_str != NULL && group_rank_str != NULL) {
      /* we already have a group id and rank, use that to rebuild the communicator */
      int group_id   = atoi(group_id_str);
      int group_rank = atoi(group_rank_str);
      MPI_Comm_split(scr_comm_world, group_id, group_rank, &c->comm);
    } else {
      /* otherwise, build the communicator based on the copy type and other parameters */
      int rel_rank, mod_rank, split_id;
      switch (c->copy_type) {
      case SCR_COPY_LOCAL:
        /* not going to communicate with anyone, so just dup COMM_SELF */
        MPI_Comm_dup(MPI_COMM_SELF, &c->comm);
        break;
      case SCR_COPY_PARTNER:
        /* dup the global level communicator */
        MPI_Comm_dup(scr_comm_level, &c->comm);
        break;
      case SCR_COPY_XOR:
        /* split the scr_comm_level communicator based on xor set size to create our xor communicator */
        rel_rank = scr_my_rank_level / c->hop_distance;
        mod_rank = scr_my_rank_level % c->hop_distance;
        split_id = (rel_rank / c->set_size) * c->hop_distance + mod_rank;
        MPI_Comm_split(scr_comm_level, split_id, scr_my_rank_world, &c->comm);
        break;
      }
    }

    /* find our position in the checkpoint communicator */
    MPI_Comm_rank(c->comm, &c->my_rank);
    MPI_Comm_size(c->comm, &c->ranks);

    /* for our group id, use the global rank of the rank 0 task in our checkpoint comm */
    int rank0 = 0;
    MPI_Group group, group_world;
    MPI_Comm_group(c->comm, &group);
    MPI_Comm_group(scr_comm_world, &group_world);
    MPI_Group_translate_ranks(group, 1, &rank0, group_world, &c->group_id);
    MPI_Group_free(&group);
    MPI_Group_free(&group_world);

    /* count the number of groups */
    int group_master = (c->my_rank == 0) ? 1 : 0;
    MPI_Allreduce(&group_master, &c->groups, 1, MPI_INT, MPI_SUM, scr_comm_world);

    /* find left and right-hand-side partners (LOCAL needs no partner nodes) */
    if (c->copy_type == SCR_COPY_PARTNER) {
      scr_set_partners(c->comm, c->hop_distance,
          &c->lhs_rank, &c->lhs_rank_world, c->lhs_hostname,
          &c->rhs_rank, &c->rhs_rank_world, c->rhs_hostname
      );
    } else if (c->copy_type == SCR_COPY_XOR) {
      scr_set_partners(c->comm, 1,
          &c->lhs_rank, &c->lhs_rank_world, c->lhs_hostname,
          &c->rhs_rank, &c->rhs_rank_world, c->rhs_hostname
      );
    }

    /* check that we have a valid partner node (LOCAL needs no partner nodes) */
    if (c->copy_type == SCR_COPY_PARTNER || c->copy_type == SCR_COPY_XOR) {
      if (strcmp(c->lhs_hostname, "") == 0 ||
          strcmp(c->rhs_hostname, "") == 0 ||
          strcmp(c->lhs_hostname, scr_my_hostname) == 0 ||
          strcmp(c->rhs_hostname, scr_my_hostname) == 0)
      {
        c->enabled = 0;
        scr_err("Failed to find partner processes for checkpoint descriptor %d, disabling checkpoint, too few nodes? @ %s:%d",
                c->index, __FILE__, __LINE__
        );
      } else {
        scr_dbg(2, "LHS partner: %s (%d)  -->  My name: %s (%d)  -->  RHS partner: %s (%d)",
                c->lhs_hostname, c->lhs_rank_world, scr_my_hostname, scr_my_rank_world, c->rhs_hostname, c->rhs_rank_world
        );
      }
    }

    /* if anyone has disabled this checkpoint, everyone needs to */
    if (!scr_alltrue(c->enabled)) {
      c->enabled = 0;
    }
  }

  return SCR_SUCCESS;
}

/* many times we just need the directory for the checkpoint,
 * it's overkill to create the whole descriptor each time */
static char* scr_ckptdesc_val_from_filemap(scr_filemap* map, int ckpt, int rank, char* name)
{
  /* check that we have a pointer to a map and a character buffer */
  if (map == NULL || name == NULL) {
    return NULL;
  }

  /* create an empty hash to store the checkpoint descriptor hash from the filemap */
  scr_hash* desc = scr_hash_new();
  if (desc == NULL) {
    return NULL;
  }

  /* get the checkpoint descriptor hash from the filemap */
  if (scr_filemap_get_desc(map, ckpt, rank, desc) != SCR_SUCCESS) {
    scr_hash_delete(desc);
    return NULL;
  }

  /* copy the directory from the checkpoint descriptor hash, if it's set */
  char* dup = NULL;
  char* val = scr_hash_elem_get_first_val(desc, name);
  if (val != NULL) {
    dup = strdup(val);
  }

  /* delete the hash object */
  scr_hash_delete(desc);

  return dup;
}

/* many times we just need the directory for the checkpoint,
 * it's overkill to create the whole descripter each time */
static char* scr_ckptdesc_base_from_filemap(scr_filemap* map, int ckpt, int rank)
{
  char* value = scr_ckptdesc_val_from_filemap(map, ckpt, rank, SCR_CONFIG_KEY_BASE);
  return value;
}

/* many times we just need the directory for the checkpoint,
 * it's overkill to create the whole descripter each time */
static char* scr_ckptdesc_dir_from_filemap(scr_filemap* map, int ckpt, int rank)
{
  char* value = scr_ckptdesc_val_from_filemap(map, ckpt, rank, SCR_CONFIG_KEY_DIRECTORY);
  return value;
}

/* build a checkpoint descriptor struct from its corresponding hash stored in the filemap,
 * this function is collective */
static int scr_ckptdesc_create_from_filemap(scr_filemap* map, int ckpt, int rank, struct scr_ckptdesc* c)
{
  /* check that we have a pointer to a map and a checkpoint descriptor */
  if (map == NULL || c == NULL) {
    return SCR_FAILURE;
  }

  /* create an empty hash to store the checkpoint descriptor hash from the filemap */
  scr_hash* desc = scr_hash_new();
  if (desc == NULL) {
    return SCR_FAILURE;
  }

  /* get the checkpoint descriptor hash from the filemap */
  if (scr_filemap_get_desc(map, ckpt, rank, desc) != SCR_SUCCESS) {
    scr_hash_delete(desc);
    return SCR_FAILURE;
  }

  /* fill in our checkpoint descriptor */
  if (scr_ckptdesc_create_from_hash(c, -1, desc) != SCR_SUCCESS) {
    scr_hash_delete(desc);
    return SCR_FAILURE;
  }

  /* delete the hash object */
  scr_hash_delete(desc);

  return SCR_SUCCESS;
}

static int scr_ckptdesc_create_list()
{
  /* set the number of checkpoint descriptors */
  scr_nckptdescs = 0;
  scr_hash* tmp = scr_hash_get(scr_ckptdesc_hash, SCR_CONFIG_KEY_CKPTDESC);
  if (tmp != NULL) {
    scr_nckptdescs = scr_hash_size(tmp);
  }

  /* allocate our checkpoint descriptors */
  if (scr_nckptdescs > 0) {
    scr_ckptdescs = (struct scr_ckptdesc*) malloc(scr_nckptdescs * sizeof(struct scr_ckptdesc));
    /* TODO: check for errors */
  }

  int all_valid = 1;

  /* iterate over each of our checkpoints filling in each corresponding descriptor */
  int i;
  for (i=0; i < scr_nckptdescs; i++) {
    /* get the info hash for this checkpoint */
    scr_hash* ckpt_hash = scr_hash_get_kv_int(scr_ckptdesc_hash, SCR_CONFIG_KEY_CKPTDESC, i);
    if (scr_ckptdesc_create_from_hash(&scr_ckptdescs[i], i, ckpt_hash) != SCR_SUCCESS) {
      all_valid = 0;
    }
  }

  /* determine whether everyone found a valid checkpoint descriptor */
  if (!all_valid) {
    return SCR_FAILURE;
  }
  return SCR_SUCCESS;
}

static int scr_ckptdesc_free_list()
{
  /* iterate over and free each of our checkpoint descriptors */
  if (scr_nckptdescs > 0 && scr_ckptdescs != NULL) {
    int i;
    for (i=0; i < scr_nckptdescs; i++) {
      scr_ckptdesc_free(&scr_ckptdescs[i]);
    }
  }

  /* set the count back to zero */
  scr_nckptdescs = 0;

  /* and free off the memory allocated */
  if (scr_ckptdescs != NULL) {
    free(scr_ckptdescs);
    scr_ckptdescs = NULL;
  }

  return SCR_SUCCESS;
}

/*
=========================================
Metadata functions
=========================================
*/

/* marks file as incomplete by deleting corresponding .scr meta file */
static int scr_incomplete(const char* file)
{
  /* delete the meta data file */
  int rc = scr_meta_unlink(file);
  return rc;
}

/* creates corresponding .scr meta file for file to record completion info */
static int scr_complete(const char* file, const scr_meta* meta)
{
  /* write out the meta data */
  int rc = scr_meta_write(file, meta);
  return rc;
}

/*
=========================================
Checkpoint functions
=========================================
*/

/*
  READ:
  master process on each node reads filemap
  and distributes pieces to others

  WRITE:
  all processes send their file info to master
  and master writes it out

  master filemap file
    list of ranks this node has files for
      for each rank, list of checkpoint ids
        for each checkpoint id, list of locations (RAM,SSD,PFS,etc)
            for each location, list of files for this rank for this checkpoint

  GOALS: 
    - support different number of processes per node on
      a restart
    - support multiple files per rank per checkpoint
    - support multiple checkpoints at different cache levels
*/

/* searches through the cache descriptors and returns the size of the cache whose BASE
 * matches the specified base */
static int scr_cachedesc_size(const char* base)
{
  /* iterate over each of our cache descriptors */
  scr_hash* index = scr_hash_get(scr_cachedesc_hash, SCR_CONFIG_KEY_CACHEDESC);
  scr_hash_elem* elem;
  for (elem = scr_hash_elem_first(index);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get a reference to the hash for the current descriptor */
    scr_hash* h = scr_hash_elem_hash(elem);

    /* get the BASE value for this descriptor */
    char* b = scr_hash_elem_get_first_val(h, SCR_CONFIG_KEY_BASE);

    /* if the BASE is set, and if it matches the specified base, lookup and return the size */
    if (b != NULL && strcmp(b, base) == 0) {
      char* s = scr_hash_elem_get_first_val(h, SCR_CONFIG_KEY_SIZE);
      if (s != NULL) {
        int size = atoi(s);
        return size;
      }

      /* found the base, but couldn't find the size, so return a size of 0 */
      return 0;
    }   
  }

  /* couldn't find the specified base, so return a size of 0 */
  return 0;
}

/* returns the checkpoint directory for a given checkpoint id */
static int scr_checkpoint_dir(const struct scr_ckptdesc* c, int checkpoint_id, char* dir)
{
  /* fatal error if c or c->directory is not set */
  if (c == NULL || c->directory == NULL) {
    scr_abort(-1, "NULL checkpoint descriptor or NULL checkpoint directory @ %s:%d",
            __FILE__, __LINE__
    );
  }

  /* now build the checkpoint directory name */
  sprintf(dir, "%s/checkpoint.%d", c->directory, checkpoint_id);
  return SCR_SUCCESS;
}

/* create a checkpoint directory given a checkpoint descriptor and checkpoint id,
 * waits for all tasks on the same node before returning */
static int scr_checkpoint_dir_create(const struct scr_ckptdesc* c, int checkpoint_id)
{
  int rc = SCR_SUCCESS;

  /* have the master rank on each node create the directory */
  if (scr_my_rank_local == 0) {
    /* get the name of the checkpoint directory for the given id */
    char dir[SCR_MAX_FILENAME];
    scr_checkpoint_dir(c, checkpoint_id, dir);

    /* create the directory */
    scr_dbg(2, "Creating checkpoint directory: %s", dir);
    rc = scr_mkdir(dir, S_IRWXU);

    /* check that we created the directory successfully, fatal error if not */
    if (rc != SCR_SUCCESS) {
      scr_abort(-1, "Failed to create checkpoint directory, aborting @ %s:%d",
                __FILE__, __LINE__
      );
    }
  }

  /* force all tasks on the same node to wait to ensure the directory is ready before returning */
  MPI_Barrier(scr_comm_local);

  return SCR_SUCCESS;
}

/* remove a checkpoint directory given a checkpoint descriptor and checkpoint id,
 * waits for all tasks on the same node before removing */
static int scr_checkpoint_dir_delete(const char* prefix, int checkpoint_id)
{
  /* force all tasks on the same node to wait before we delete the directory */
  MPI_Barrier(scr_comm_local);

  /* have the master rank on each node remove the directory */
  if (scr_my_rank_local == 0) {
    char dir[SCR_MAX_FILENAME];
    sprintf(dir, "%s/checkpoint.%d", prefix, checkpoint_id);
    scr_dbg(2, "Removing checkpoint directory: %s", dir);
    rmdir(dir);
  }

  return SCR_SUCCESS;
}

/* removes entries in flush file for given checkpoint id */
static int scr_flush_checkpoint_remove(int checkpoint_id)
{
  /* all master tasks write this file to their node */
  if (scr_my_rank_local == 0) {
    /* read the flush file into hash */
    scr_hash* hash = scr_hash_new();
    scr_hash_read(scr_flush_file, hash);

    /* delete this checkpoint id from the flush file */
    scr_hash_unset_kv_int(hash, SCR_FLUSH_KEY_CKPT, checkpoint_id);

    /* write the hash back to the flush file */
    scr_hash_write(scr_flush_file, hash);

    /* delete the hash */
    scr_hash_delete(hash);
  }
  return SCR_SUCCESS;
}

/* remove all checkpoint files and data associated with specified checkpoint */
static int scr_checkpoint_delete(scr_filemap* map, int checkpoint_id)
{
  /* print a debug messages */
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "Deleting checkpoint %d from cache", checkpoint_id);
  }

  /* for each file of each rank we have for this checkpoint, delete the file */
  scr_hash_elem* rank_elem;
  for (rank_elem = scr_filemap_first_rank_by_checkpoint(map, checkpoint_id);
       rank_elem != NULL;
       rank_elem = scr_hash_elem_next(rank_elem))
  {
    int rank = scr_hash_elem_key_int(rank_elem);

    scr_hash_elem* file_elem;
    for (file_elem = scr_filemap_first_file(map, checkpoint_id, rank);
         file_elem != NULL;
         file_elem = scr_hash_elem_next(file_elem))
    {
      /* get the filename */
      char* file = scr_hash_elem_key(file_elem); 

      /* check file's crc value (monitor that cache hardware isn't corrupting files on us) */
      if (scr_crc_on_delete) {
        /* TODO: if corruption, need to log */
        if (scr_compute_crc(file) != SCR_SUCCESS) {
          scr_err("Failed to verify CRC32 before deleting file %s, bad drive? @ %s:%d",
                  file, __FILE__, __LINE__
          );
        }
      }

      /* delete the file */
      unlink(file);

      /* remove the corresponding meta file */
      scr_incomplete(file);
    }
  }

  /* remove the cache directory for this checkpoint */
  char* ckpt_dir = scr_ckptdesc_dir_from_filemap(map, checkpoint_id, scr_my_rank_world);
  if (ckpt_dir != NULL) {
    /* remove the checkpoint directory from cache */
    /* get the name of the checkpoint directory for the given id */
    scr_checkpoint_dir_delete(ckpt_dir, checkpoint_id);
    free(ckpt_dir);
  } else {
    /* TODO: abort! */
  }

  /* delete any entry in the flush file for this checkpoint */
  scr_flush_checkpoint_remove(checkpoint_id);

  /* TODO: remove data from transfer file for this checkpoint */

  /* remove this checkpoint from the filemap, and write it to disk */
  scr_filemap_remove_checkpoint(map, checkpoint_id);
  scr_filemap_write(scr_map_file, map);

  return SCR_SUCCESS;
}

/* remove all checkpoint files recorded in filemap, and the filemap itself */
static int scr_unlink_all(scr_filemap* map)
{
  /* get the maximum number of checkpoints belonging to any rank on our node */
  int max_num_ckpts = -1;
  int num_ckpts = scr_filemap_num_checkpoints(map);
  MPI_Allreduce(&num_ckpts, &max_num_ckpts, 1, MPI_INT, MPI_MAX, scr_comm_local);

  /* now run through and delete each checkpoint */
  while (max_num_ckpts > 0) {
    /* get the maximum latest checkpoint id */
    int max_ckpt = -1;
    int ckpt = scr_filemap_latest_checkpoint(map);
    MPI_Allreduce(&ckpt, &max_ckpt, 1, MPI_INT, MPI_MAX, scr_comm_local);

    /* remove this checkpoint from all tasks */
    scr_checkpoint_delete(map, max_ckpt);

    /* get the number of checkpoints left on the node */
    max_num_ckpts = -1;
    num_ckpts = scr_filemap_num_checkpoints(map);
    MPI_Allreduce(&num_ckpts, &max_num_ckpts, 1, MPI_INT, MPI_MAX, scr_comm_local);
  }

  /* now delete the filemap itself */
  unlink(scr_map_file);
  scr_dbg(2, "scr_unlink_all: unlink(%s)",
          scr_map_file
  );

  /* TODO: want to clear the map object here? */

  return 1;
}

/* checks whether specifed file exists, is readable, and is complete */
static int scr_bool_have_file(scr_filemap* map, int ckpt, int rank, const char* file, int ranks)
{
  /* if no filename is given return false */
  if (file == NULL || strcmp(file,"") == 0) {
    scr_dbg(2, "File name is null or the empty string @ %s:%d",
            __FILE__, __LINE__
    );
    return 0;
  }

  /* check that we can read the file */
  if (access(file, R_OK) < 0) {
    scr_dbg(2, "Do not have read access to file: %s @ %s:%d",
            file, __FILE__, __LINE__
    );
    return 0;
  }

  /* allocate object to read meta data into */
  scr_meta* meta = scr_meta_new();

  /* check that we can read meta file for the file */
  if (scr_meta_read(file, meta) != SCR_SUCCESS) {
    scr_dbg(2, "Failed to read meta data file for file: %s @ %s:%d",
            file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }

  /* check that the file is complete */
  if (scr_meta_is_complete(meta) != SCR_SUCCESS) {
    scr_dbg(2, "File is marked as incomplete: %s @ %s:%d",
            file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }

  /* check that the file really belongs to the checkpoint id we think it does */
  int meta_ckpt = -1;
  if (scr_meta_get_checkpoint(meta, &meta_ckpt) != SCR_SUCCESS) {
    scr_dbg(2, "Failed to read checkpoint field in meta data: %s @ %s:%d",
            file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }
  if (ckpt != meta_ckpt) {
    scr_dbg(2, "File's checkpoint ID (%d) does not match id in meta data file (%d) for %s @ %s:%d",
            ckpt, meta_ckpt, file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }

  /* check that the file really belongs to the rank we think it does */
  int meta_rank = -1;
  if (scr_meta_get_rank(meta, &meta_rank) != SCR_SUCCESS) {
    scr_dbg(2, "Failed to read rank field in meta data: %s @ %s:%d",
            file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }
  if (rank != meta_rank) {
    scr_dbg(2, "File's rank (%d) does not match rank in meta data file (%d) for %s @ %s:%d",
            rank, meta_rank, file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }

  /* check that the file really belongs to the rank we think it does */
  int meta_ranks = -1;
  if (scr_meta_get_ranks(meta, &meta_ranks) != SCR_SUCCESS) {
    scr_dbg(2, "Failed to read ranks field in meta data: %s @ %s:%d",
            file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }
  if (ranks != meta_ranks) {
    scr_dbg(2, "File's ranks (%d) does not match ranks in meta data file (%d) for %s @ %s:%d",
            ranks, meta_ranks, file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }

  /* check that the file size matches (use strtol while reading data) */
  unsigned long size = scr_filesize(file);
  unsigned long meta_size = 0;
  if (scr_meta_get_filesize(meta, &meta_size) != SCR_SUCCESS) {
    scr_dbg(2, "Failed to read filesize field in meta data: %s @ %s:%d",
            file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }
  if (size != meta_size) {
    scr_dbg(2, "Filesize is incorrect, currently %lu, expected %lu for %s @ %s:%d",
            size, meta_size, file, __FILE__, __LINE__
    );
    scr_meta_delete(meta);
    return 0;
  }

  /* TODO: check that crc32 match if set (this would be expensive) */

  /* free meta data object */
  scr_meta_delete(meta);

  /* if we made it here, assume the file is good */
  return 1;
}

/* check whether we have all files for a given rank of a given checkpoint */
static int scr_bool_have_files(scr_filemap* map, int ckpt, int rank)
{
  /* check whether we have any files for the specified rank */
  if (!scr_filemap_have_rank_by_checkpoint(map, ckpt, rank)) {
    return 0;
  }

  /* check whether we have all of the files we should */
  int expected_files = scr_filemap_num_expected_files(map, ckpt, rank);
  int num_files = scr_filemap_num_files(map, ckpt, rank);
  if (num_files != expected_files) {
    return 0;
  }

  /* check the integrity of each of the files */
  int missing_a_file = 0;
  scr_hash_elem* file_elem;
  for (file_elem = scr_filemap_first_file(map, ckpt, rank);
       file_elem != NULL;
       file_elem = scr_hash_elem_next(file_elem))
  {
    char* file = scr_hash_elem_key(file_elem);
    if (!scr_bool_have_file(map, ckpt, rank, file, scr_ranks_world)) {
      missing_a_file = 1;
    }
  }
  if (missing_a_file) {
    return 0;
  }

  /* if we make it here, we have all of our files */
  return 1;
}

/* opens the filemap, inspects that all listed files are readable and complete, unlinks any that are not */
static int scr_clean_files(scr_filemap* map)
{
  /* create a map to remember which files to keep */
  scr_filemap* keep_map = scr_filemap_new();

  /* scan each file for each rank of each checkpoint */
  scr_hash_elem* ckpt_elem;
  for (ckpt_elem = scr_filemap_first_checkpoint(map);
       ckpt_elem != NULL;
       ckpt_elem = scr_hash_elem_next(ckpt_elem))
  {
    int ckpt = scr_hash_elem_key_int(ckpt_elem);

    scr_hash_elem* rank_elem;
    for (rank_elem = scr_filemap_first_rank_by_checkpoint(map, ckpt);
         rank_elem != NULL;
         rank_elem = scr_hash_elem_next(rank_elem))
    {
      int rank = scr_hash_elem_key_int(rank_elem);

      /* if we're missing any file for this rank in this checkpoint,
       * we'll delete them all */
      int missing_file = 0;

      /* first time through the file list, check that we have each file */
      scr_hash_elem* file_elem = NULL;
      for (file_elem = scr_filemap_first_file(map, ckpt, rank);
           file_elem != NULL;
           file_elem = scr_hash_elem_next(file_elem))
      {
        /* get filename */
        char* file = scr_hash_elem_key(file_elem);

        /* check whether we have it */
        if (!scr_bool_have_file(map, ckpt, rank, file, scr_ranks_world)) {
            missing_file = 1;
            scr_dbg(1, "File is unreadable or incomplete: CheckpointID %d, Rank %d, File: %s",
                    ckpt, rank, file
            );
        }
      }

      /* add checkpoint descriptor to keep map, if one is set */
      scr_hash* desc = scr_hash_new();
      if (scr_filemap_get_desc(map, ckpt, rank, desc) == SCR_SUCCESS) {
        scr_filemap_set_desc(keep_map, ckpt, rank, desc);
      }
      scr_hash_delete(desc);

      /* check whether we have all the files we think we should */
      int expected_files = scr_filemap_num_expected_files(map, ckpt, rank);
      int num_files = scr_filemap_num_files(map, ckpt, rank);
      if (num_files != expected_files) {
        missing_file = 1;
      }

      /* if we have all the files, set the expected file number in the keep_map */
      if (!missing_file) {
        scr_filemap_set_expected_files(keep_map, ckpt, rank, expected_files);
      }

      /* second time through, either add all files to keep_map or delete them all */
      for (file_elem = scr_filemap_first_file(map, ckpt, rank);
           file_elem != NULL;
           file_elem = scr_hash_elem_next(file_elem))
      {
        /* get the filename */
        char* file = scr_hash_elem_key(file_elem);

        /* if we failed to read any file, delete them all 
         * otherwise add them all to the keep_map */
        if (missing_file) {
          /* inform user on what we're doing */
          scr_dbg(1, "Deleting file: CheckpointID %d, Rank %d, File: %s",
                  ckpt, rank, file
          );

          /* delete the file */
          unlink(file);

          /* delete the meta file */
          scr_incomplete(file);
        } else {
          /* keep this file */
          scr_filemap_copy_file(keep_map, map, ckpt, rank, file);
        }
      }
    }
  }

  /* clear our current map, merge the keep_map into it, and write the map to disk */
  scr_filemap_clear(map);
  scr_filemap_merge(map, keep_map);
  scr_filemap_write(scr_map_file, map);

  /* free the keep_map object */
  scr_filemap_delete(keep_map);

  return SCR_SUCCESS;
}

/* returns true iff each file in the filemap can be read */
static int scr_check_files(scr_filemap* map, int checkpoint_id)
{
  /* for each file of each rank for specified checkpoint id */
  int failed_read = 0;
  scr_hash_elem* rank_elem;
  for (rank_elem = scr_filemap_first_rank_by_checkpoint(map, checkpoint_id);
       rank_elem != NULL;
       rank_elem = scr_hash_elem_next(rank_elem))
  {
    int rank = scr_hash_elem_key_int(rank_elem);
    scr_hash_elem* file_elem;
    for (file_elem = scr_filemap_first_file(map, checkpoint_id, rank);
         file_elem != NULL;
         file_elem = scr_hash_elem_next(file_elem))
    {
      char* file = scr_hash_elem_key(file_elem);
      /* check that we can read the file */
      if (access(file, R_OK) < 0) {
        failed_read = 1;
      }

      /* check that we can read meta file for the file */
      scr_meta* meta = scr_meta_new();
      if (scr_meta_read(file, meta) != SCR_SUCCESS) {
        failed_read = 1;
      } else {
        /* check that the file is complete */
        if (scr_meta_is_complete(meta) != SCR_SUCCESS) {
          failed_read = 1;
        }
      }
      scr_meta_delete(meta);
    }
  }

  /* if we failed to read a file, assume the set is incomplete */
  if (failed_read) {
    /* TODO: want to unlink all files in this case? */
    return SCR_FAILURE;
  }
  return SCR_SUCCESS;
}

/*
=========================================
File Copy Functions
=========================================
*/

static int scr_swap_file_names(const char* file_send, int rank_send,
                                     char* file_recv, size_t size_recv, int rank_recv,
                               const char* dir_recv, MPI_Comm comm)
{
  int rc = SCR_SUCCESS;

  /* determine whether we have a file to send */
  int have_outgoing = 0;
  if (rank_send != MPI_PROC_NULL &&
      file_send != NULL &&
      strcmp(file_send, "") != 0)
  {
    have_outgoing = 1;
  }

  /* determine whether we are expecting to receive a file */
  int have_incoming = 0;
  if (rank_recv != MPI_PROC_NULL &&
      dir_recv != NULL &&
      strcmp(dir_recv, "") != 0)
  {
    have_incoming = 1;
  }

  /* exchange file names with partners */
  char file_recv_orig[SCR_MAX_FILENAME] = "";
  int num_req = 0;;
  MPI_Request request[2];
  MPI_Status  status[2];
  if (have_incoming) {
    MPI_Irecv(file_recv_orig, SCR_MAX_FILENAME, MPI_BYTE, rank_recv, 0, comm, &request[num_req]);
    num_req++;
  }
  if (have_outgoing) {
    MPI_Isend((char*)file_send, strlen(file_send)+1, MPI_BYTE, rank_send, 0, comm, &request[num_req]);
    num_req++;
  }
  if (num_req > 0) {
    MPI_Waitall(num_req, request, status);
  }

  /* define the path to store our partner's file */
  if (have_incoming) {
    /* set full path to filename */
    char path[SCR_MAX_FILENAME] = "";
    char name[SCR_MAX_FILENAME] = "";
    scr_split_path(file_recv_orig, path, name);
    scr_build_path(file_recv, size_recv, dir_recv, name);
  }

  return SCR_SUCCESS;
}

/* scr_swap_files -- copy or move a file from one node to another
 * COPY_FILES
 *   if file_me != NULL, send file_me to rank_send, who will make a copy,
 *   copy file from rank_recv if there is one to receive
 * MOVE_FILES
 *   if file_me != NULL, move file_me to rank_send
 *   save file from rank_recv if there is one to receive
 *   To conserve space (i.e., RAM disc), if file_me exists,
 *   any incoming file will overwrite file_me in place, one block at a time.
 *   It is then truncated and renamed according the size and name of the incoming file,
 *   or it is deleted (moved) if there is no incoming file.
 */
static int scr_swap_files(int swap_type,
                  const char* file_send, int rank_send,
                  const char* file_recv, int rank_recv,
                  MPI_Comm comm)
{
  int rc = SCR_SUCCESS;
  MPI_Request request[2];
  MPI_Status  status[2];

  /* determine whether we have a file to send */
  int have_outgoing = 0;
  if (rank_send != MPI_PROC_NULL &&
      file_send != NULL &&
      strcmp(file_send, "") != 0)
  {
    have_outgoing = 1;
  }

  /* determine whether we are expecting to receive a file */
  int have_incoming = 0;
  if (rank_recv != MPI_PROC_NULL &&
      file_recv != NULL &&
      strcmp(file_recv, "") != 0)
  {
    have_incoming = 1;
  }

  /* remove the completion marker for partner's file */
  if (have_incoming) {
    scr_incomplete(file_recv);
  }

  /* allocate MPI send buffer */
  char *buf_send = NULL;
  if (have_outgoing) {
    buf_send = (char*) scr_align_malloc(scr_mpi_buf_size, scr_page_size);
    if (buf_send == NULL) {
      scr_err("Allocating memory: malloc(%ld) errno=%d %m @ %s:%d",
              scr_mpi_buf_size, errno, __FILE__, __LINE__
      );
      return SCR_FAILURE;
    }
  }

  /* allocate MPI recv buffer */
  char *buf_recv = NULL;
  if (have_incoming) {
    buf_recv = (char*) scr_align_malloc(scr_mpi_buf_size, scr_page_size);
    if (buf_recv == NULL) {
      scr_err("Allocating memory: malloc(%ld) errno=%d %m @ %s:%d",
              scr_mpi_buf_size, errno, __FILE__, __LINE__
      );
      if (buf_send != NULL) {
        scr_align_free(buf_send);
        buf_send = NULL;
      }
      return SCR_FAILURE;
    }
  }

  /* read in the metadata for our file, we don't send yet because we may update the CRC value */
  scr_meta* meta_send = NULL;
  if (have_outgoing) {
    meta_send = scr_meta_new();
    scr_meta_read(file_send, meta_send);
  }

  /* initialize crc values */
  uLong crc32_send = crc32(0L, Z_NULL, 0);
  uLong crc32_recv = crc32(0L, Z_NULL, 0);

  /* exchange files */
  if (swap_type == COPY_FILES) {
    /* open the file to send: read-only mode */
    int fd_send = -1;
    if (have_outgoing) {
      fd_send = scr_open(file_send, O_RDONLY);
      if (fd_send < 0) {
        scr_abort(-1, "Opening file for send: scr_open(%s, O_RDONLY) errno=%d %m @ %s:%d",
                file_send, errno, __FILE__, __LINE__
        );
      }
    }

    /* open the file to recv: truncate, write-only mode */
    int fd_recv = -1;
    if (have_incoming) {
      fd_recv = scr_open(file_recv, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
      if (fd_recv < 0) {
        scr_abort(-1, "Opening file for recv: scr_open(%s, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR) errno=%d %m @ %s:%d",
                file_recv, errno, __FILE__, __LINE__
        );
      }
    }

    /* exchange file chunks */
    int nread, nwrite;
    int sending = 0;
    if (have_outgoing) {
      sending = 1;
    }
    int receiving = 0;
    if (have_incoming) {
      receiving = 1;
    }
    while (sending || receiving) {
      /* if we are still receiving a file, post a receive */
      if (receiving) {
        MPI_Irecv(buf_recv, scr_mpi_buf_size, MPI_BYTE, rank_recv, 0, comm, &request[0]);
      }

      /* if we are still sending a file, read a chunk, send it, and wait */
      if (sending) {
        nread = scr_read(file_send, fd_send, buf_send, scr_mpi_buf_size);
        if (scr_crc_on_copy && nread > 0) {
          crc32_send = crc32(crc32_send, (const Bytef*) buf_send, (uInt) nread);
        }
        if (nread < 0) {
          nread = 0;
        }
        MPI_Isend(buf_send, nread, MPI_BYTE, rank_send, 0, comm, &request[1]);
        MPI_Wait(&request[1], &status[1]);
        if (nread < scr_mpi_buf_size) {
          sending = 0;
        }
      }

      /* if we are still receiving a file, wait on our receive to complete and write the data */
      if (receiving) {
        MPI_Wait(&request[0], &status[0]);
        MPI_Get_count(&status[0], MPI_BYTE, &nwrite);
        if (scr_crc_on_copy && nwrite > 0) {
          crc32_recv = crc32(crc32_recv, (const Bytef*) buf_recv, (uInt) nwrite);
        }
        scr_write(file_recv, fd_recv, buf_recv, nwrite);
        if (nwrite < scr_mpi_buf_size) {
          receiving = 0;
        }
      }
    }

    /* close the files */
    if (have_outgoing) {
      scr_close(file_send, fd_send);
    }
    if (have_incoming) {
      scr_close(file_recv, fd_recv);
    }

    /* set crc field on our file if it hasn't been set already */
    if (scr_crc_on_copy && have_outgoing) {
      uLong meta_send_crc;
      if (scr_meta_get_crc32(meta_send, &meta_send_crc) != SCR_SUCCESS) {
        scr_meta_set_crc32(meta_send, crc32_send);
        scr_complete(file_send, meta_send);
      } else {
        /* TODO: we could check that the crc on the sent file matches and take some action if not */
      }
    }
  } else if (swap_type == MOVE_FILES) {
    /* since we'll overwrite our send file in place with the recv file, which may be larger,
     * we need to keep track of how many bytes we've sent and whether we've sent them all */
    unsigned long filesize_send = 0;

    /* open our file */
    int fd = -1;
    if (have_outgoing) {
      /* we'll overwrite our send file (or just read it if there is no incoming) */
      filesize_send = scr_filesize(file_send);
      fd = scr_open(file_send, O_RDWR);
      if (fd < 0) {
        /* TODO: skip writes and return error? */
        scr_abort(-1, "Opening file for send/recv: scr_open(%s, O_RDWR) errno=%d %m @ %s:%d",
                file_send, errno, __FILE__, __LINE__
        );
      }
    } else if (have_incoming) {
      /* we'll write our recv file from scratch */
      fd = scr_open(file_recv, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
      if (fd < 0) {
        /* TODO: skip writes and return error? */
        scr_abort(-1, "Opening file for recv: scr_open(%s, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR) errno=%d %m @ %s:%d",
                file_recv, errno, __FILE__, __LINE__
        );
      }
    }

    /* exchange file chunks */
    int sending = 0;
    if (have_outgoing) {
      sending = 1;
    }
    int receiving = 0;
    if (have_incoming) {
      receiving = 1;
    }
    int nread, nwrite;
    off_t read_pos = 0, write_pos = 0;
    while (sending || receiving) {
      if (receiving) {
        /* prepare a buffer to receive up to scr_mpi_buf_size bytes */
        MPI_Irecv(buf_recv, scr_mpi_buf_size, MPI_BYTE, rank_recv, 0, comm, &request[0]);
      }

      if (sending) {
        /* compute number of bytes to read */
        unsigned long count = filesize_send - read_pos;
        if (count > scr_mpi_buf_size) {
          count = scr_mpi_buf_size;
        }

        /* read a chunk of up to scr_mpi_buf_size bytes into buf_send */
        lseek(fd, read_pos, SEEK_SET); /* seek to read position */
        nread = scr_read(file_send, fd, buf_send, count);
        if (scr_crc_on_copy && nread > 0) {
          crc32_send = crc32(crc32_send, (const Bytef*) buf_send, (uInt) nread);
        }
        if (nread < 0) {
          nread = 0;
        }
        read_pos += (off_t) nread; /* update read pointer */

        /* send chunk (if nread is smaller than scr_mpi_buf_size, then we've read the whole file) */
        MPI_Isend(buf_send, nread, MPI_BYTE, rank_send, 0, comm, &request[1]);
        MPI_Wait(&request[1], &status[1]);

        /* check whether we've read the whole file */
        if (filesize_send == read_pos && count < scr_mpi_buf_size) {
          sending = 0;
        }
      }

      if (receiving) {
        /* count the number of bytes received */
        MPI_Wait(&request[0], &status[0]);
        MPI_Get_count(&status[0], MPI_BYTE, &nwrite);
        if (scr_crc_on_copy && nwrite > 0) {
          crc32_recv = crc32(crc32_recv, (const Bytef*) buf_recv, (uInt) nwrite);
        }

        /* write those bytes to file (if nwrite is smaller than scr_mpi_buf_size, then we've received the whole file) */
        lseek(fd, write_pos, SEEK_SET); /* seek to write position */
        scr_write(file_recv, fd, buf_recv, nwrite);
        write_pos += (off_t) nwrite; /* update write pointer */

        /* if nwrite is smaller than scr_mpi_buf_size, then assume we've received the whole file */
        if (nwrite < scr_mpi_buf_size) {
          receiving = 0;
        }
      }
    }

    /* close file and cleanup */
    if (have_outgoing && have_incoming) {
      /* sent and received a file; close it, truncate it to write size, rename it, and remove its completion marker */
      scr_close(file_send, fd);
      truncate(file_send, write_pos);
      rename(file_send, file_recv);
      scr_incomplete(file_send);
    } else if (have_outgoing) {
      /* only sent a file; close it, delete it, and remove its completion marker */
      scr_close(file_send, fd);
      unlink(file_send);
      scr_incomplete(file_send);
    } else if (have_incoming) {
      /* only received a file; just need to close it */
      scr_close(file_recv, fd);
    }

    if (scr_crc_on_copy && have_outgoing) {
      uLong meta_send_crc;
      if (scr_meta_get_crc32(meta_send, &meta_send_crc) != SCR_SUCCESS) {
        /* we transfer this meta data across below, so may as well update these fields so we can use them */
        scr_meta_set_crc32(meta_send, crc32_send);
        /* do not complete file send, we just deleted it above */
      } else {
        /* TODO: we could check that the crc on the sent file matches and take some action if not */
      }
    }
  } else {
    scr_err("Unknown file transfer type: %d @ %s:%d", swap_type, __FILE__, __LINE__);
    return SCR_FAILURE;
  } /* end file copy / move */

  /* free the MPI buffers */
  if (have_outgoing) {
    scr_align_free(buf_send);
    buf_send = NULL;
  }
  if (have_incoming) {
    scr_align_free(buf_recv);
    buf_recv = NULL;
  }

  /* exchange meta file info with partners */
  scr_meta* meta_recv = scr_meta_new();
  scr_hash_sendrecv(meta_send, rank_send, meta_recv, rank_recv, comm);

  /* delete our send meta data object */
  if (have_outgoing) {
    scr_meta_delete(meta_send);
  }

  /* mark received file as complete */
  if (have_incoming) {
    /* check that our written file is the correct size */
    unsigned long filesize_wrote = scr_filesize(file_recv);
    if (scr_meta_check_filesize(meta_recv, filesize_wrote) != SCR_SUCCESS) {
      scr_meta_set_complete(meta_recv, 0);
      rc = SCR_FAILURE;
    }

    /* check that there was no corruption in receiving the file */
    if (scr_crc_on_copy) {
      /* if we computed crc during the copy, and crc is set in the received meta data
       * check that our computed value matches */
      uLong crc32_recv_meta;
      if (scr_meta_get_crc32(meta_recv, &crc32_recv_meta) == SCR_SUCCESS) {
        if (crc32_recv != crc32_recv_meta) {
          scr_meta_set_complete(meta_recv, 0);
          rc = SCR_FAILURE;
        }
      }
    }

    scr_complete(file_recv, meta_recv);
  }

  /* delete our received meta data */
  scr_meta_delete(meta_recv);

  return rc;
}

/* copy files to a partner node */
static int scr_copy_partner(scr_filemap* map, const struct scr_ckptdesc* c, int checkpoint_id)
{
  int rc = SCR_SUCCESS;

  /* get a list of our files */
  int numfiles = 0;
  char** files = NULL;
  scr_filemap_list_files(map, checkpoint_id, scr_my_rank_world, &numfiles, &files);

  /* first, determine how many files we'll be sending and receiving with our partners */
  MPI_Status status;
  int send_num = numfiles;
  int recv_num = 0;
  MPI_Sendrecv(&send_num, 1, MPI_INT, c->rhs_rank, 0, &recv_num, 1, MPI_INT, c->lhs_rank, 0, c->comm, &status);

  /* record how many files our partner will send */
  scr_filemap_set_expected_files(map, checkpoint_id, c->lhs_rank_world, recv_num);

  /* remember which node our partner is on (needed for drain) */
  scr_filemap_set_tag(map, checkpoint_id, c->lhs_rank_world, SCR_FILEMAP_KEY_PARTNER, c->lhs_hostname);

  /* record partner's checkpoint descriptor hash */
  scr_hash* lhs_desc_hash = scr_hash_new();
  scr_hash* my_desc_hash  = scr_hash_new();
  scr_ckptdesc_store_to_hash(c, my_desc_hash);
  scr_hash_sendrecv(my_desc_hash, c->rhs_rank, lhs_desc_hash, c->lhs_rank, c->comm);
  scr_filemap_set_desc(map, checkpoint_id, c->lhs_rank_world, lhs_desc_hash);
  scr_hash_delete(my_desc_hash);
  scr_hash_delete(lhs_desc_hash);

  /* store this info in our filemap before we receive any files */
  scr_filemap_write(scr_map_file, map);

  /* define directory to receive partner file in */
  char ckpt_dir[SCR_MAX_FILENAME];
  scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);

  /* for each potential file, step through a call to swap */
  while (send_num > 0 || recv_num > 0) {
    /* assume we won't send or receive in this step */
    int send_rank = MPI_PROC_NULL;
    int recv_rank = MPI_PROC_NULL;

    /* if we have a file left to send, get the filename and destination rank */
    char* file = NULL;
    if (send_num > 0) {
      int i = numfiles - send_num;
      file = files[i];
      send_rank = c->rhs_rank;
      send_num--;
    }

    /* if we have a file left to receive, get the rank */
    if (recv_num > 0) {
      recv_rank = c->lhs_rank;
      recv_num--;
    }

    /* exhange file names with partners */
    char file_partner[SCR_MAX_FILENAME];
    scr_swap_file_names(file, send_rank, file_partner, sizeof(file_partner), recv_rank, ckpt_dir, c->comm);

    /* if we'll receive a file, record the name of our partner's file in the filemap */
    if (recv_rank != MPI_PROC_NULL) {
      scr_filemap_add_file(map, checkpoint_id, c->lhs_rank_world, file_partner);
      scr_filemap_write(scr_map_file, map);
    }

    /* exhange files with partners */
    if (scr_swap_files(COPY_FILES, file, send_rank, file_partner, recv_rank, c->comm) != SCR_SUCCESS) {
      rc = SCR_FAILURE;
    }
  }

  /* free our list of files */
  if (files != NULL) {
    free(files);
    files = NULL;
  }

  return rc;
}

/* set the ranks array in the header */
static int scr_copy_xor_header_set_ranks(scr_hash* header, MPI_Comm comm, MPI_Comm comm_world)
{
  scr_hash_unset(header, SCR_KEY_COPY_XOR_RANKS);
  scr_hash_unset(header, SCR_KEY_COPY_XOR_GROUP);

  /* record the total number of ranks in comm_world */
  int ranks_world;
  MPI_Comm_size(comm_world, &ranks_world);
  scr_hash_set_kv_int(header, SCR_KEY_COPY_XOR_RANKS, ranks_world);

  /* create a new empty hash to track group info for this xor set */
  scr_hash* hash = scr_hash_new();
  scr_hash_set(header, SCR_KEY_COPY_XOR_GROUP, hash);

  /* record the total number of ranks in the xor communicator */
  int ranks_comm;
  MPI_Comm_size(comm, &ranks_comm);
  scr_hash_set_kv_int(hash, SCR_KEY_COPY_XOR_GROUP_RANKS, ranks_comm);

  /* record mapping of rank in xor group to corresponding world rank */
  int i, rank;
  if (ranks_comm > 0) {
    /* map ranks in comm to ranks in scr_comm_world */
    MPI_Group group, group_world;
    MPI_Comm_group(comm, &group);
    MPI_Comm_group(comm_world, &group_world);
    for (i=0; i < ranks_comm; i++) {
      MPI_Group_translate_ranks(group, 1, &i, group_world, &rank);
      scr_hash_setf(hash, NULL, "%s %d %d", SCR_KEY_COPY_XOR_GROUP_RANK, i, rank);
    }
    MPI_Group_free(&group);
    MPI_Group_free(&group_world);
  }

  return SCR_SUCCESS;
}

/* reduce-scatter XOR file of checkpoint files of ranks in same XOR set */
static int scr_copy_xor(scr_filemap* map, const struct scr_ckptdesc* c, int checkpoint_id)
{
  int rc = SCR_SUCCESS;
  int tmp_rc;
  int i;

  /* allocate buffer to read a piece of my file */
  char* send_buf = (char*) scr_align_malloc(scr_mpi_buf_size, scr_page_size);
  if (send_buf == NULL) {
    scr_abort(-1, "Allocating memory for send buffer: malloc(%d) errno=%d %m @ %s:%d",
            scr_mpi_buf_size, errno, __FILE__, __LINE__
    );
  }

  /* allocate buffer to read a piece of the recevied chunk file */
  char* recv_buf = (char*) scr_align_malloc(scr_mpi_buf_size, scr_page_size);
  if (recv_buf == NULL) {
    scr_abort(-1, "Allocating memory for recv buffer: malloc(%d) errno=%d %m @ %s:%d",
            scr_mpi_buf_size, errno, __FILE__, __LINE__
    );
  }

  /* count the number of files I have and allocate space in structures for each of them */
  int num_files = scr_filemap_num_files(map, checkpoint_id, scr_my_rank_world);
  int* fds = NULL;
  char** filenames = NULL;
  unsigned long* filesizes = NULL;
  if (num_files > 0) {
    fds       = (int*)           malloc(num_files * sizeof(int));
    filenames = (char**)         malloc(num_files * sizeof(char*));
    filesizes = (unsigned long*) malloc(num_files * sizeof(unsigned long));
    if (fds == NULL || filenames == NULL || filesizes == NULL) {
      scr_abort(-1, "Failed to allocate file arrays @ %s:%d",
                __FILE__, __LINE__
      );
    }
  }

  /* record partner's checkpoint descriptor hash in our filemap */
  scr_hash* lhs_desc_hash = scr_hash_new();
  scr_hash* my_desc_hash  = scr_hash_new();
  scr_ckptdesc_store_to_hash(c, my_desc_hash);
  scr_hash_sendrecv(my_desc_hash, c->rhs_rank, lhs_desc_hash, c->lhs_rank, c->comm);
  scr_filemap_set_desc(map, checkpoint_id, c->lhs_rank_world, lhs_desc_hash);
  scr_hash_delete(my_desc_hash);
  scr_hash_delete(lhs_desc_hash);

  /* allocate a new xor file header hash, record the global ranks of the
   * processes in our xor group, and record the checkpoint id */
  scr_hash* header = scr_hash_new();
  scr_copy_xor_header_set_ranks(header, c->comm, scr_comm_world);
  scr_hash_set_kv_int(header, SCR_KEY_COPY_XOR_CKPT, checkpoint_id);

  /* open each file, get the filesize of each, and read the meta data of each */
  scr_hash* current_files = scr_hash_new();
  int file_count = 0;
  unsigned long my_bytes = 0;
  scr_hash_elem* file_elem;
  for (file_elem = scr_filemap_first_file(map, checkpoint_id, scr_my_rank_world);
       file_elem != NULL;
       file_elem = scr_hash_elem_next(file_elem))
  {
    /* get the filename */
    filenames[file_count] = scr_hash_elem_key(file_elem);

    /* get the filesize of this file and add the byte count to the total */
    filesizes[file_count] = scr_filesize(filenames[file_count]);
    my_bytes += filesizes[file_count];

    /* read the meta data for this file and insert it into the current_files hash */
    scr_meta* file_hash = scr_meta_new();
    scr_meta_read(filenames[file_count], file_hash);
    scr_hash_setf(current_files, file_hash, "%d", file_count);

    /* open the file */
    fds[file_count]  = scr_open(filenames[file_count], O_RDONLY);
    if (fds[file_count] < 0) {
      /* TODO: try again? */
      scr_abort(-1, "Opening checkpoint file for copying: scr_open(%s, O_RDONLY) errno=%d %m @ %s:%d",
                filenames[file_count], errno, __FILE__, __LINE__
      );
    }

    file_count++;
  }

  /* set total number of files we have, plus our rank */
  scr_hash* current_hash = scr_hash_new();
  scr_hash_set_kv_int(current_hash, SCR_KEY_COPY_XOR_RANK,  scr_my_rank_world);
  scr_hash_set_kv_int(current_hash, SCR_KEY_COPY_XOR_FILES, file_count);
  scr_hash_set(current_hash, SCR_KEY_COPY_XOR_FILE, current_files);

  /* exchange file info with partners and add data to our header */
  scr_hash* partner_hash = scr_hash_new();
  scr_hash_sendrecv(current_hash, c->rhs_rank, partner_hash, c->lhs_rank, c->comm);
  scr_hash_set(header, SCR_KEY_COPY_XOR_CURRENT, current_hash);
  scr_hash_set(header, SCR_KEY_COPY_XOR_PARTNER, partner_hash);

  /* allreduce to get maximum filesize */
  unsigned long max_bytes;
  MPI_Allreduce(&my_bytes, &max_bytes, 1, MPI_UNSIGNED_LONG, MPI_MAX, c->comm);

  /* TODO: use unsigned long integer arithmetic (with proper byte padding) instead of char to speed things up */

  /* compute chunk size according to maximum file length and number of ranks in xor set */
  /* if filesize doesn't divide evenly, then add one byte to chunk_size */
  /* TODO: check that ranks > 1 for this divide to be safe (or at partner selection time) */
  size_t chunk_size = max_bytes / (unsigned long) (c->ranks - 1);
  if ((c->ranks - 1) * chunk_size < max_bytes) {
    chunk_size++;
  }

  /* TODO: need something like this to handle 0-byte files? */
  if (chunk_size == 0) {
    chunk_size++;
  }

  /* record the checkpoint_id and the chunk size in the xor chunk header */
  scr_hash_setf(header, NULL, "%s %lu", SCR_KEY_COPY_XOR_CHUNK, chunk_size);

  /* set chunk filenames of form:  <xor_rank+1>_of_<xor_ranks>_in_<group_id>.xor */
  char my_chunk_file[SCR_MAX_FILENAME];
  char ckpt_dir[SCR_MAX_FILENAME];
  scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);
  sprintf(my_chunk_file,  "%s/%d_of_%d_in_%d.xor", ckpt_dir, c->my_rank+1, c->ranks, c->group_id);

  /* record chunk file in filemap before creating it */
  scr_filemap_add_file(map, checkpoint_id, scr_my_rank_world, my_chunk_file);
  scr_filemap_write(scr_map_file, map);

  /* open my chunk file */
  int fd_chunk = scr_open(my_chunk_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd_chunk < 0) {
    /* TODO: try again? */
    scr_abort(-1, "Opening XOR chunk file for writing: scr_open(%s) errno=%d %m @ %s:%d",
            my_chunk_file, errno, __FILE__, __LINE__
    );
  }

  /* write out the xor chunk header */
  scr_hash_write_fd(my_chunk_file, fd_chunk, header);
  scr_hash_delete(header);

  MPI_Request request[2];
  MPI_Status  status[2];

  /* XOR Reduce_scatter */
  size_t nread = 0;
  while (nread < chunk_size) {
    size_t count = chunk_size - nread;
    if (count > scr_mpi_buf_size) {
      count = scr_mpi_buf_size;
    }

    int chunk_id;
    for(chunk_id = c->ranks-1; chunk_id >= 0; chunk_id--) {
      /* read the next set of bytes for this chunk from my file into send_buf */
      if (chunk_id > 0) {
        int chunk_id_rel = (c->my_rank + c->ranks + chunk_id) % c->ranks;
        if (chunk_id_rel > c->my_rank) {
          chunk_id_rel--;
        }
        unsigned long offset = chunk_size * (unsigned long) chunk_id_rel + nread;
        if (scr_read_pad_n(num_files, filenames, fds,
                           send_buf, count, offset, filesizes) != SCR_SUCCESS)
        {
          rc = SCR_FAILURE;
        }
      } else {
        memset(send_buf, 0, count);
      }

      /* TODO: XORing with unsigned long would be faster here (if chunk size is multiple of this size) */
      /* merge the blocks via xor operation */
      if (chunk_id < c->ranks-1) {
        for (i = 0; i < count; i++) {
          send_buf[i] ^= recv_buf[i];
        }
      }

      if (chunk_id > 0) {
        /* not our chunk to write, forward it on and get the next */
        MPI_Irecv(recv_buf, count, MPI_BYTE, c->lhs_rank, 0, c->comm, &request[0]);
        MPI_Isend(send_buf, count, MPI_BYTE, c->rhs_rank, 0, c->comm, &request[1]);
        MPI_Waitall(2, request, status);
      } else {
        /* write send block to send chunk file */
        if (scr_write_attempt(my_chunk_file, fd_chunk, send_buf, count) != count) {
          rc = SCR_FAILURE;
        }
      }
    }

    nread += count;
  }

  /* close my chunkfile, with fsync */
  if (scr_close(my_chunk_file, fd_chunk) != SCR_SUCCESS) {
    rc = SCR_FAILURE;
  }

  /* close my checkpoint files */
  for (i=0; i < num_files; i++) {
    scr_close(filenames[i], fds[i]);
  }

  /* free the buffers */
  if (filesizes != NULL) {
    free(filesizes);
    filesizes = NULL;
  }
  if (filenames != NULL) {
    /* in this case, we don't free each name, since we copied the pointer to the string in the filemap */
    free(filenames);
    filenames = NULL;
  }
  if (fds != NULL) {
    free(fds);
    fds = NULL;
  }
  scr_align_free(send_buf);
  scr_align_free(recv_buf);

  /* TODO: need to check for errors */
  /* write meta file for xor chunk */
  unsigned long my_chunk_file_size = scr_filesize(my_chunk_file);
  scr_meta* meta = scr_meta_new();
  scr_meta_set(meta, my_chunk_file, SCR_META_FILE_XOR, my_chunk_file_size, checkpoint_id, scr_my_rank_world, scr_ranks_world, 1);
  scr_complete(my_chunk_file, meta);
  scr_meta_delete(meta);

  /* if crc_on_copy is set, compute and store CRC32 value for chunk file */
  if (scr_crc_on_copy) {
    scr_compute_crc(my_chunk_file);
    /* TODO: would be nice to save this CRC in our partner's XOR file so we can check correctness on a rebuild */
  }

  return rc;
}

/* apply redundancy scheme to file and return number of bytes copied in bytes parameter */
int scr_copy_files(scr_filemap* map, const struct scr_ckptdesc* c, int checkpoint_id, double* bytes)
{
  /* initialize to 0 */
  *bytes = 0.0;

  /* step through each of my files for the latest checkpoint to scan for any incomplete files */
  int valid = 1;
  double my_bytes = 0.0;
  scr_hash_elem* file_elem;
  for (file_elem = scr_filemap_first_file(map, checkpoint_id, scr_my_rank_world);
       file_elem != NULL;
       file_elem = scr_hash_elem_next(file_elem))
  {
    /* get the filename */
    char* file = scr_hash_elem_key(file_elem);

    /* check the file */
    if (!scr_bool_have_file(map, checkpoint_id, scr_my_rank_world, file, scr_ranks_world)) {
      scr_dbg(2, "scr_copy_files: File determined to be invalid: %s", file);
      valid = 0;
    }

    /* add up the number of bytes on our way through */
    my_bytes += (double) scr_filesize(file);

    /* if crc_on_copy is set, compute crc and update meta file (PARTNER does this during the copy) */
    if (scr_crc_on_copy && c->copy_type != SCR_COPY_PARTNER) {
      scr_compute_crc(file);
    }
  }

  /* determine whether everyone's files are good */
  int all_valid = scr_alltrue(valid);
  if (!all_valid) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "scr_copy_files: Exiting copy since one or more checkpoint files is invalid");
    }
    return SCR_FAILURE;
  }

  /* start timer */
  time_t timestamp_start;
  double time_start;
  if (scr_my_rank_world == 0) {
    timestamp_start = scr_log_seconds();
    time_start = MPI_Wtime();
  }

  /* apply the redundancy scheme */
  int rc = SCR_FAILURE;
  switch (c->copy_type) {
  case SCR_COPY_LOCAL:
    rc = SCR_SUCCESS;
    break;
  case SCR_COPY_PARTNER:
    rc = scr_copy_partner(map, c, checkpoint_id);
    break;
  case SCR_COPY_XOR:
    rc = scr_copy_xor(map, c, checkpoint_id);
    break;
  }

  /* record the number of files this task wrote during this checkpoint 
   * (needed to remember when a task writes 0 files) */
  int num_files = scr_filemap_num_files(map, checkpoint_id, scr_my_rank_world);
  scr_filemap_set_expected_files(map, checkpoint_id, scr_my_rank_world, num_files);
  scr_filemap_write(scr_map_file, map);

  /* determine whether everyone succeeded in their copy */
  int valid_copy = (rc == SCR_SUCCESS);
  if (!valid_copy) {
    scr_err("scr_copy_files failed with return code %d @ %s:%d", rc, __FILE__, __LINE__);
  }
  int all_valid_copy = scr_alltrue(valid_copy);
  rc = all_valid_copy ? SCR_SUCCESS : SCR_FAILURE;

  /* add up total number of bytes */
  MPI_Allreduce(&my_bytes, bytes, 1, MPI_DOUBLE, MPI_SUM, scr_comm_world);

  /* stop timer and report performance info */
  if (scr_my_rank_world == 0) {
    double time_end = MPI_Wtime();
    double time_diff = time_end - time_start;
    double bw = *bytes / (1024.0 * 1024.0 * time_diff);
    scr_dbg(1, "scr_copy_files: %f secs, %e bytes, %f MB/s, %f MB/s per proc",
            time_diff, *bytes, bw, bw/scr_ranks_world
    );

    /* log data on the copy in the database */
    if (scr_log_enable) {
      char ckpt_dir[SCR_MAX_FILENAME];
      scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);
      scr_log_transfer("COPY", c->base, ckpt_dir, &checkpoint_id, &timestamp_start, &time_diff, bytes);
    }
  }

  return rc;
}

/*
=========================================
Flush and fetch functions
=========================================
*/

/* read in the summary file from dir assuming file is using version 4 format or earlier, convert to version 5 hash */
static int scr_summary_read_v4_to_v5(const char* dir, scr_hash* summary_hash)
{
  /* check that we have a pointer to a hash */
  if (summary_hash == NULL) {
    return SCR_FAILURE;
  }

  /* check whether we can read the summary file */
  char summary_file[SCR_MAX_FILENAME];
  if (scr_build_path(summary_file, sizeof(summary_file), dir, "scr_summary.txt") != SCR_SUCCESS) {
    scr_err("Failed to build full filename for summary file @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* check whether we can read the file before we actually try,
   * we take this step to avoid printing an error in scr_hash_read */
  if (access(summary_file, R_OK) < 0) {
    return SCR_FAILURE;
  }

  /* open the summary file */
  FILE* fs = fopen(summary_file, "r");
  if (fs == NULL) {
    scr_err("Opening summary file for read: fopen(%s, \"r\") errno=%d %m @ %s:%d",
            summary_file, errno, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* assume we have one file per rank */
  int num_records = scr_ranks_world;

  /* read the first line (all versions have at least one header line) */
  int linenum = 0;
  char line[2048];
  char field[2048];
  fgets(line, sizeof(line), fs);
  linenum++;

  /* get the summary file version number, if no number, assume version=1 */
  int version = 1;
  sscanf(line, "%s", field);
  if (strcmp(field, "Version:") == 0) {
    sscanf(line, "%s %d", field, &version);
  }

  /* all versions greater than 1, have two header lines, read and throw away the second */
  if (version > 1) {
    /* version 3 and higher writes the number of rows in the file (ranks may write 0 or more files) */
    if (version >= 3) {
      fgets(line, sizeof(line), fs);
      linenum++;
      sscanf(line, "%s %d", field, &num_records);
    }
    fgets(line, sizeof(line), fs);
    linenum++;
  }

  /* now we know how many records we'll be reading, so allocate space for them */
  if (num_records <= 0) {
    scr_err("No file records found in summary file %s, perhaps it is corrupt or incomplete @ %s:%d",
            summary_file, __FILE__, __LINE__
    );
    fclose(fs);
    return SCR_FAILURE;
  }

  /* set the version number in the summary hash, initialize a pointer to the checkpoint hash */
  scr_hash_set_kv_int(summary_hash, SCR_SUMMARY_KEY_VERSION, SCR_SUMMARY_FILE_VERSION_5);
  scr_hash* ckpt_hash = NULL;

  /* read the record for each rank */
  int i;
  int bad_values        =  0;
  int all_complete      =  1;
  int all_ranks         = -1;
  int all_checkpoint_id = -1;
  for(i=0; i < num_records; i++) {
    int expected_n, n;
    int rank, scr, ranks, pattern, complete, match_filesize, checkpoint_id;
    char filename[SCR_MAX_FILENAME];
    unsigned long exp_filesize, filesize;
    int crc_computed = 0;
    uLong crc = 0UL;

    /* read a line from the file, parse depending on version */
    if (version == 1) {
      expected_n = 10;
      n = fscanf(fs, "%d\t%d\t%d\t%d\t%d\t%d\t%lu\t%d\t%lu\t%s\n",
                 &rank, &scr, &ranks, &pattern, &checkpoint_id, &complete,
                 &exp_filesize, &match_filesize, &filesize, filename
      );
      linenum++;
    } else {
      expected_n = 11;
      n = fscanf(fs, "%d\t%d\t%d\t%d\t%d\t%lu\t%d\t%lu\t%s\t%d\t0x%lx\n",
                 &rank, &scr, &ranks, &checkpoint_id, &complete,
                 &exp_filesize, &match_filesize, &filesize, filename,
                 &crc_computed, &crc
      );
      linenum++;
    }

    /* check the return code returned from the read */
    if (n == EOF) {
      scr_err("Early EOF in summary file %s at line %d.  Only read %d of %d expected records @ %s:%d",
              summary_file, linenum, i, num_records, __FILE__, __LINE__
      );
      fclose(fs);
      scr_hash_unset_all(summary_hash);
      return SCR_FAILURE;
    } else if (n != expected_n) {
      scr_err("Invalid read of record %d in %s at line %d @ %s:%d",
              i, summary_file, linenum, __FILE__, __LINE__
      );
      fclose(fs);
      scr_hash_unset_all(summary_hash);
      return SCR_FAILURE;
    }

    /* TODO: check whether all files are complete, match expected size, number of ranks, checkpoint_id, etc */
    if (rank < 0 || rank >= scr_ranks_world) {
      bad_values = 1;
      scr_err("Invalid rank detected (%d) in a job with %d tasks in %s at line %d @ %s:%d",
              rank, scr_my_rank_world, summary_file, linenum, __FILE__, __LINE__
      );
    }

    /* chop to basename of filename */
    char* base = basename(filename);

    /* set the pointer to the checkpoint hash, if we haven't already */
    if (ckpt_hash == NULL) {
      /* get a pointer to the checkpoint hash */
      ckpt_hash = scr_hash_set_kv_int(summary_hash, SCR_SUMMARY_KEY_CKPT, checkpoint_id);
    }

    /* get a pointer to the hash for this rank, and then to the file for this rank */
    scr_hash* rank_hash = scr_hash_set_kv_int(ckpt_hash, SCR_SUMMARY_KEY_RANK, rank);
    scr_hash* file_hash = scr_hash_set_kv(    rank_hash, SCR_SUMMARY_KEY_FILE, base);

    /* set the file size, and the crc32 value if it was computed */
    scr_hash_setf(file_hash, NULL, "%s %lu", SCR_SUMMARY_KEY_SIZE, exp_filesize);
    if (crc_computed) {
      scr_hash_setf(file_hash, NULL, "%s %#lx", SCR_SUMMARY_KEY_CRC, crc);
    }

    /* if the file is incomplete, set the incomplete field for this file */
    if (! complete) {
      all_complete = 0;
      scr_hash_set_kv_int(file_hash, SCR_SUMMARY_KEY_COMPLETE, 0);
    }

    /* check that the checkpoint id matches all other checkpoint ids in the file */
    if (checkpoint_id != all_checkpoint_id) {
      if (all_checkpoint_id == -1) {
        all_checkpoint_id = checkpoint_id;
      } else {
        bad_values = 1;
        scr_err("Checkpoint id %d on record %d does not match expected checkpoint id %d in %s at line %d @ %s:%d",
                checkpoint_id, i, all_checkpoint_id, summary_file, linenum, __FILE__, __LINE__
        );
      }
    }

    /* check that the number of ranks matches all the number of ranks specified by all other records in the file */
    if (ranks != all_ranks) {
      if (all_ranks == -1) {
        all_ranks = ranks;
      } else {
        bad_values = 1;
        scr_err("Number of ranks %d on record %d does not match expected number of ranks %d in %s at line %d @ %s:%d",
                ranks, i, all_ranks, summary_file, linenum, __FILE__, __LINE__
        );
      }
    }
  }

  /* we've read in all of the records, now set the values for the complete field and the number of ranks field */
  if (ckpt_hash != NULL) {
    scr_hash_set_kv_int(ckpt_hash, SCR_SUMMARY_KEY_COMPLETE, all_complete);
    scr_hash_set_kv_int(ckpt_hash, SCR_SUMMARY_KEY_RANKS, all_ranks);
  }

  /* close the file */
  fclose(fs);

  /* if we found any problems while reading the file, clear the hash and return with an error */
  if (bad_values) {
    /* clear the hash, since we may have set bad values */
    scr_hash_unset_all(summary_hash);
    return SCR_FAILURE;
  }

  /* otherwise, return success */
  return SCR_SUCCESS;
}

/* read in the summary file from dir */
static int scr_summary_read_v5(const char* dir, scr_hash* summary_hash)
{
  /* check that we got a pointer to a hash */
  if (summary_hash == NULL) {
    return SCR_FAILURE;
  }

  /* build the filename for the summary file */
  char summary_file[SCR_MAX_FILENAME];
  if (scr_build_path(summary_file, sizeof(summary_file), dir, "summary.scr") != SCR_SUCCESS) {
    scr_err("Failed to build full filename for summary file @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* check whether we can read the file before we actually try,
   * we take this step to avoid printing an error in scr_hash_read */
  if (access(summary_file, R_OK) < 0) {
    return SCR_FAILURE;
  }

  /* read in the summary hash file */
  if (scr_hash_read(summary_file, summary_hash) != SCR_SUCCESS) {
    scr_err("Reading summary file %s @ %s:%d",
            summary_file, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* if we made it here, we successfully read the summary file as a hash */
  return SCR_SUCCESS;
}

/* read in the summary file from dir */
static int scr_summary_read(const char* dir, scr_hash* summary_hash, int* checkpoint_id)
{
  /* check that we have pointers to a hash and an integer */
  if (summary_hash == NULL || checkpoint_id == NULL) {
    return SCR_FAILURE;
  }

  /* clear the hash and initialize the checkpoint id */
  scr_hash_unset_all(summary_hash);
  *checkpoint_id = -1;

  /* attempt to read the summary file, assuming it is in version 5 format */
  if (scr_summary_read_v5(dir, summary_hash) != SCR_SUCCESS) {
    /* failed to read the summary file, try again, but now assume an older format */
    if (scr_summary_read_v4_to_v5(dir, summary_hash) != SCR_SUCCESS) {
      /* we still failed, don't report an error here, just return failure,
       * the read functions will report errors as needed */
      scr_err("Reading summary file in %s @ %s:%d",
              dir, __FILE__, __LINE__
      );
      return SCR_FAILURE;
    }
  }

  /* check that the summary file version is something we support */
  int supported_version = 0;
  char* version_str = scr_hash_elem_get_first_val(summary_hash, SCR_SUMMARY_KEY_VERSION);
  if (version_str != NULL) {
    int version = atoi(version_str);
    if (version == SCR_SUMMARY_FILE_VERSION_5) {
      supported_version = 1;
    }
  }
  if (! supported_version) {
    /* the old read format also failed, print an error and return failure */
    scr_err("Summary file version is not supported in %s @ %s:%d",
            dir, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* check that we have exactly one checkpoint */
  scr_hash* ckpt_hash = scr_hash_get(summary_hash, SCR_SUMMARY_KEY_CKPT);
  if (scr_hash_size(ckpt_hash) != 1) {
    scr_err("More than one checkpoint found in summary file in %s @ %s:%d",
            dir, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* get the first (and only) checkpoint id */
  char* ckpt_str = scr_hash_elem_get_first_val(summary_hash, SCR_META_KEY_CKPT);
  scr_hash* ckpt = scr_hash_get(ckpt_hash, ckpt_str);
  *checkpoint_id = atoi(ckpt_str);

  /* check that the complete string is set and is set to 1 */
  int set_is_complete = 0;
  char* complete_str = scr_hash_elem_get_first_val(ckpt, SCR_SUMMARY_KEY_COMPLETE);
  if (complete_str != NULL) {
    int complete = atoi(complete_str);
    if (complete == 1) {
      set_is_complete = 1;
    }
  }
  if (! set_is_complete) {
    return SCR_FAILURE;
  }

  /* read in the the number of ranks for this checkpoint */
  char* ranks_str = scr_hash_elem_get_first_val(ckpt, SCR_SUMMARY_KEY_RANKS);
  if (ranks_str == NULL) {
    scr_err("Failed to read number of ranks in summary file in %s @ %s:%d",
            dir, __FILE__, __LINE__
    );
    /* free the summary hash object */
    return SCR_FAILURE;
  }
  int ranks = atoi(ranks_str);

  /* check that the number of ranks matches the number we're currently running with */
  if (ranks != scr_ranks_world) {
    scr_err("Number of ranks %s that wrote checkpoint %s in %s does not match current number of ranks %d @ %s:%d",
            ranks_str, ckpt_str, dir, scr_ranks_world, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  return SCR_SUCCESS;
}

/* write out the summary file to dir */
static int scr_summary_write(const char* dir, int checkpoint_id, int all_complete, scr_hash* data)
{
  /* build the filename */
  char file[SCR_MAX_FILENAME];
  if (scr_build_path(file, sizeof(file), dir, "summary.scr") != SCR_SUCCESS) {
    scr_err("Failed to build full filename for summary file @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }


  /* create an empty hash to build our summary info */
  scr_hash* summary_hash = scr_hash_new();

  /* write the summary file version number */
  scr_hash_set_kv_int(summary_hash, SCR_SUMMARY_KEY_VERSION, SCR_SUMMARY_FILE_VERSION_5);

  /* write the checkpoint id */
  scr_hash* ckpt_hash = scr_hash_set_kv_int(summary_hash, SCR_SUMMARY_KEY_CKPT, checkpoint_id);

  /* write the number of ranks used to write this checkpoint */
  scr_hash_set_kv_int(ckpt_hash, SCR_SUMMARY_KEY_RANKS, scr_ranks_world);

  /* for each file, insert hash listing filename, then file size, crc, and incomplete flag under that */
  scr_hash_merge(ckpt_hash, data);
//  scr_hash_set(ckpt_hash, SCR_SUMMARY_KEY_RANK, data);

  /* mark whether the checkpoint set as a whole is complete */
  scr_hash_set_kv_int(ckpt_hash, SCR_SUMMARY_KEY_COMPLETE, all_complete);

  /* write the hash to a file */
  scr_hash_write(file, summary_hash);

  /* free the hash object */
  scr_hash_delete(summary_hash);

  /* TODO: ugly hack: subtract off scr_par_prefix */
  char* dir_tmp = strdup(dir);
  char* dir_base = basename(dir_tmp);

  /* mark the checkpoint as complete in the index file */
  scr_hash* index_hash = scr_hash_new();
  scr_index_read(scr_par_prefix, index_hash);
  scr_index_set_complete_key(index_hash, checkpoint_id, dir_base, all_complete);
  scr_index_write(scr_par_prefix, index_hash);
  scr_hash_delete(index_hash);

  /* TODO: free data for our ugly hack */
  free(dir_tmp);

  return SCR_SUCCESS;
}

/* returns true if the given checkpoint id needs to be flushed */
static int scr_bool_need_flush(int checkpoint_id)
{
  int need_flush = 0;

  if (scr_my_rank_local == 0) {
    /* read the flush file */
    scr_hash* hash = scr_hash_new();
    scr_hash_read(scr_flush_file, hash);

    /* if we have the checkpoint in cache, but not on the parallel file system, then it needs to be flushed */
    scr_hash* ckpt_hash = scr_hash_get_kv_int(hash, SCR_FLUSH_KEY_CKPT, checkpoint_id);
    scr_hash* in_cache = scr_hash_get_kv(ckpt_hash, SCR_FLUSH_KEY_LOCATION, SCR_FLUSH_KEY_LOCATION_CACHE);
    scr_hash* in_pfs   = scr_hash_get_kv(ckpt_hash, SCR_FLUSH_KEY_LOCATION, SCR_FLUSH_KEY_LOCATION_PFS);
    if (in_cache != NULL && in_pfs == NULL) {
      need_flush = 1;
    }

    /* free the hash object */
    scr_hash_delete(hash);
  }
  MPI_Bcast(&need_flush, 1, MPI_INT, 0, scr_comm_local);

  return need_flush;
}

/* adds a location for the specified checkpoint id to the flush file */
static int scr_flush_location_set(int checkpoint_id, const char* location)
{
  /* all master tasks write this file to their node */
  if (scr_my_rank_local == 0) {
    /* read the flush file into hash */
    scr_hash* hash = scr_hash_new();
    scr_hash_read(scr_flush_file, hash);

    /* set the location for this checkpoint */
    scr_hash* ckpt_hash = scr_hash_set_kv_int(hash, SCR_FLUSH_KEY_CKPT, checkpoint_id);
    scr_hash_set_kv(ckpt_hash, SCR_FLUSH_KEY_LOCATION, location);

    /* write the hash back to the flush file */
    scr_hash_write(scr_flush_file, hash);

    /* delete the hash */
    scr_hash_delete(hash);
  }
  return SCR_SUCCESS;
}

/* returns SCR_SUCCESS if checkpoint_id is at location */
static int scr_flush_location_test(int checkpoint_id, const char* location)
{
  /* all master tasks check their flush file */
  int at_location = 0;
  if (scr_my_rank_local == 0) {
    /* read the flush file into hash */
    scr_hash* hash = scr_hash_new();
    scr_hash_read(scr_flush_file, hash);

    /* check the location for this checkpoint */
    scr_hash* ckpt_hash = scr_hash_get_kv_int(hash, SCR_FLUSH_KEY_CKPT, checkpoint_id);
    scr_hash* value     = scr_hash_get_kv(ckpt_hash, SCR_FLUSH_KEY_LOCATION, location);
    if (value != NULL) {
      at_location = 1;
    }

    /* delete the hash */
    scr_hash_delete(hash);
  }
  MPI_Bcast(&at_location, 1, MPI_INT, 0, scr_comm_local);

  if (!at_location) {
    return SCR_FAILURE;
  }
  return SCR_SUCCESS;
}

/* removes a location for the specified checkpoint id from the flush file */
static int scr_flush_location_unset(int checkpoint_id, const char* location)
{
  /* all master tasks write this file to their node */
  if (scr_my_rank_local == 0) {
    /* read the flush file into hash */
    scr_hash* hash = scr_hash_new();
    scr_hash_read(scr_flush_file, hash);

    /* unset the location for this checkpoint */
    scr_hash* ckpt_hash = scr_hash_get_kv_int(hash, SCR_FLUSH_KEY_CKPT, checkpoint_id);
    scr_hash_unset_kv(ckpt_hash, SCR_FLUSH_KEY_LOCATION, location);

    /* write the hash back to the flush file */
    scr_hash_write(scr_flush_file, hash);

    /* delete the hash */
    scr_hash_delete(hash);
  }
  return SCR_SUCCESS;
}

/* checks whether the specified checkpoint id is currently being flushed */
static int scr_bool_is_flushing(int checkpoint_id)
{
  /* assume we are not flushing this checkpoint */
  int is_flushing = 0;

  /* have the master on each node check the flush file */
  if (scr_my_rank_local == 0) {
    /* read flush file into hash */
    scr_hash* hash = scr_hash_new();
    scr_hash_read(scr_flush_file, hash);

    /* attempt to look up the FLUSHING state for this checkpoint */
    scr_hash* ckpt_hash = scr_hash_get_kv_int(hash, SCR_FLUSH_KEY_CKPT, checkpoint_id);
    scr_hash* flushing_hash = scr_hash_get_kv(ckpt_hash, SCR_FLUSH_KEY_LOCATION, SCR_FLUSH_KEY_LOCATION_FLUSHING);
    if (flushing_hash != NULL) {
      is_flushing = 1;
    }

    /* delete the hash */
    scr_hash_delete(hash);
  }

  /* need every task to agree that this checkpoint is not being flushed */
  if (!scr_alltrue(is_flushing == 0)) {
    is_flushing = 1;
  }
  return is_flushing;
}

/* fetch file name in meta from dir and build new full path in newfile, return whether operation succeeded */
static int scr_fetch_a_file(const char* src_dir, const scr_meta* meta, const char* dst_dir,
                            char* newfile, size_t newfile_size)
{
  int success = SCR_SUCCESS;

  /* get the filename from the meta data */
  char* meta_filename;
  if (scr_meta_get_filename(meta, &meta_filename) != SCR_SUCCESS) {
    scr_err("Failed to read filename from meta data @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* build full path to file */
  char filename[SCR_MAX_FILENAME];
  if (scr_build_path(filename, sizeof(filename), src_dir, meta_filename) != SCR_SUCCESS) {
    scr_err("Failed to build full file name of target file for fetch @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* fetch the file */
  uLong crc;
  uLong* crc_p = NULL;
  if (scr_crc_on_flush) {
    crc_p = &crc;
  }
  success = scr_copy_to(filename, dst_dir, scr_file_buf_size, newfile, newfile_size, crc_p);

  /* check that crc matches crc stored in meta */
  uLong meta_crc;
  if (scr_meta_get_crc32(meta, &meta_crc) == SCR_SUCCESS) {
    if (success == SCR_SUCCESS && scr_crc_on_flush && crc != meta_crc) {
      success = SCR_FAILURE;
      scr_err("CRC32 mismatch detected when fetching file from %s to %s @ %s:%d",
              filename, newfile, __FILE__, __LINE__
      );

      /* delete the file -- it's corrupted */
      unlink(newfile);

      /* TODO: would be good to log this, but right now only rank 0 can write log entries */
      /*
      if (scr_log_enable) {
        time_t now = scr_log_seconds();
        scr_log_event("CRC32 MISMATCH", filename, NULL, &now, NULL);
      }
      */
    }
  }

  return success;
}

/* fetch files listed in hash for specified checkpoint id into specified checkpoint directory,
 * update filemap and fill in total number of bytes fetched,
 * returns SCR_SUCCESS if successful */
static int scr_fetch_files_list(scr_filemap* map, const scr_hash* list_hash, int checkpoint_id, const char* fetch_dir, const char* ckpt_dir, double* total_bytes)
{
  /* assume we'll succeed in fetching our files */
  int rc = SCR_SUCCESS;

  /* assume we don't have any files to fetch */
  int my_num_files = 0;
  *total_bytes = 0.0;

  /* lookup the file hash */
  scr_hash* file_hash = scr_hash_get(list_hash, SCR_SUMMARY_KEY_FILE);

  /* now iterate through the file list and fetch each file */
  scr_hash_elem* file_elem = NULL;
  for (file_elem = scr_hash_elem_first(file_hash);
       file_elem != NULL;
       file_elem = scr_hash_elem_next(file_elem))
  {
    /* get the filename */
    char* summary_filename = scr_hash_elem_key(file_elem);

    /* get a pointer to the hash for this file */
    scr_hash* hash = scr_hash_elem_hash(file_elem);

    /* check whether we are supposed to fetch this file */
    /* TODO: this is a hacky way to avoid reading a redundancy file back in
     * assuming that it's an original file, which breaks our redundancy computation
     * due to a name conflict on the file names */
    scr_hash_elem* no_fetch_hash = scr_hash_elem_get(hash, SCR_SUMMARY_KEY_NOFETCH);
    if (no_fetch_hash != NULL) {
      continue;
    }

    /* increment our file count */
    my_num_files++;

    /* split filename into path and name components */
    char path[SCR_MAX_FILENAME];
    char name[SCR_MAX_FILENAME];
    scr_split_path(summary_filename, path, name);

    /* build the destination file name */
    char newfile[SCR_MAX_FILENAME];
    scr_build_path(newfile, sizeof(newfile), ckpt_dir, name);
      
    /* add the file to our filemap and write it to disk before creating the file */
    scr_filemap_add_file(map, checkpoint_id, scr_my_rank_world, newfile);
    scr_filemap_write(scr_map_file, map);

    /* get the file size */
    unsigned long summary_filesize = 0;
    if (scr_hash_util_get_unsigned_long(hash, SCR_SUMMARY_KEY_SIZE, &summary_filesize) != SCR_SUCCESS) {
      scr_err("Failed to read file size from summary data @ %s:%d",
              __FILE__, __LINE__
      );
      rc = SCR_FAILURE;
      break;
    }

    /* add the filesize to our byte count */
    *total_bytes += (double) summary_filesize;

    /* check for a complete flag */
    int summary_complete = 1;
    if (scr_hash_util_get_int(hash, SCR_SUMMARY_KEY_COMPLETE, &summary_complete) != SCR_SUCCESS) {
      /* in summary file, the absence of a complete flag on a file implies the file is complete */
      summary_complete = 1;
    }

    /* create a new meta data object for this file */
    scr_meta* meta = scr_meta_new();

    /* set the meta data */
    scr_meta_set(meta, newfile, SCR_META_FILE_FULL, summary_filesize, checkpoint_id, scr_my_rank_world, scr_ranks_world, summary_complete);

    /* get the crc, if set, and add it to the meta data */
    uLong summary_crc;
    if (scr_hash_util_get_crc32(hash, SCR_SUMMARY_KEY_CRC, &summary_crc) == SCR_SUCCESS) {
      scr_meta_set_crc32(meta, summary_crc);
    }

    /* finally, fetch the file */
    if (scr_fetch_a_file(fetch_dir, meta, ckpt_dir, newfile, sizeof(newfile)) != SCR_SUCCESS) {
      rc = SCR_FAILURE;
    }

    /* mark the file as complete */
    scr_complete(newfile, meta);

    /* free the meta data object */
    scr_meta_delete(meta);
  }

  /* set the expected number of files for this checkpoint */
  scr_filemap_set_expected_files(map, checkpoint_id, scr_my_rank_world, my_num_files);
  scr_filemap_write(scr_map_file, map);

  return rc;
}

/* fetch files from parallel file system */
static int scr_fetch_files(scr_filemap* map, char* fetch_dir)
{
  int i, j;
  int rc = SCR_SUCCESS;
  int checkpoint_id = -1;
  double total_bytes = 0.0;

  /* start timer */
  time_t timestamp_start;
  double time_start;
  if (scr_my_rank_world == 0) {
    timestamp_start = scr_log_seconds();
    time_start = MPI_Wtime();
  }

  /* broadcast fetch directory */
  int dirsize = 0;
  if (scr_my_rank_world == 0) {
    dirsize = strlen(fetch_dir) + 1;
  }
  MPI_Bcast(&dirsize, 1, MPI_INT, 0, scr_comm_world);
  MPI_Bcast(fetch_dir, dirsize, MPI_CHAR, 0, scr_comm_world);

  /* if there is no directory, bail out with failure */
  if (strcmp(fetch_dir, "") == 0) {
    return SCR_FAILURE;
  }

  scr_hash* summary_hash = scr_hash_new();

  /* have rank 0 read summary file, if it exists */
  int read_summary = SCR_FAILURE;
  if (scr_my_rank_world == 0) {
    /* this may take a while, so tell user what we're doing */
    scr_dbg(1, "scr_fetch_files: Attempting fetch from %s", fetch_dir);

    /* build the fetch directory path */
    if (access(fetch_dir, R_OK) == 0) {
      /* log the fetch attempt */
      if (scr_log_enable) {
        time_t now = scr_log_seconds();
        scr_log_event("FETCH STARTED", fetch_dir, NULL, &now, NULL);
      }

      /* read data from the summary file */
      read_summary = scr_summary_read(fetch_dir, summary_hash, &checkpoint_id);
    } else {
      scr_err("scr_fetch_files: Failed to access directory %s @ %s:%d", fetch_dir, __FILE__, __LINE__);
    }
  }

  /* broadcast whether the summary file was read successfully */
  MPI_Bcast(&read_summary, 1, MPI_INT, 0, scr_comm_world);
  if (read_summary != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "scr_fetch_files: Failed to read summary file @ %s:%d", __FILE__, __LINE__);
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FETCH FAILED", fetch_dir, NULL, &now, &time_diff);
      }
    }
    scr_hash_delete(summary_hash);
    return SCR_FAILURE;
  }

  /* broadcast the checkpoint id */
  MPI_Bcast(&checkpoint_id, 1, MPI_INT, 0, scr_comm_world);
  if (checkpoint_id < 0) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "scr_fetch_files: Invalid checkpoint id in summary file @ %s:%d", __FILE__, __LINE__);
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FETCH FAILED", fetch_dir, NULL, &now, &time_diff);
      }
    }
    scr_hash_delete(summary_hash);
    return SCR_FAILURE;
  }

  /* delete any existing checkpoint files for this checkpoint id (do this before filemap_read) */
  scr_checkpoint_delete(map, checkpoint_id);

  /* get the checkpoint descriptor for this id */
  struct scr_ckptdesc* c = scr_ckptdesc_get(checkpoint_id, scr_nckptdescs, scr_ckptdescs);

  /* store our checkpoint descriptor hash in the filemap */
  scr_hash* my_desc_hash = scr_hash_new();
  scr_ckptdesc_store_to_hash(c, my_desc_hash);
  scr_filemap_set_desc(map, checkpoint_id, scr_my_rank_world, my_desc_hash);
  scr_hash_delete(my_desc_hash);

  /* write the filemap out before creating the directory */
  scr_filemap_write(scr_map_file, map);

  /* create the checkpoint directory */
  scr_checkpoint_dir_create(c, checkpoint_id);

  /* get the checkpoint directory */
  char ckpt_dir[SCR_MAX_FILENAME];
  scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);

  /* flow control rate of file reads from rank 0 by scattering file names to processes */
  int success = 1;
  if (scr_my_rank_world == 0) {
    /* sort the rank hash by rank id */
    scr_hash* ckpt_hash = scr_hash_get_kv_int(summary_hash, SCR_SUMMARY_KEY_CKPT, checkpoint_id);
    scr_hash* ranks_hash = scr_hash_get(ckpt_hash, SCR_SUMMARY_KEY_RANK);
    scr_hash_sort_int(ranks_hash, SCR_HASH_SORT_ASCENDING);

    /* lookup the hash belonging to our rank */
    scr_hash* rank_hash = scr_hash_get_kv_int(ckpt_hash, SCR_SUMMARY_KEY_RANK, 0);

    /* fetch these files into the checkpoint directory */
    if (scr_fetch_files_list(map, rank_hash, checkpoint_id, fetch_dir, ckpt_dir, &total_bytes) != SCR_SUCCESS) {
      success = 0;
    }

    /* clear the hash for this element (speeds lookup for later ranks) */
    scr_hash_unset_kv_int(ckpt_hash, SCR_SUMMARY_KEY_RANK, 0);

    /* now, have a sliding window of w processes read simultaneously */
    int w = scr_fetch_width;
    if (w > scr_ranks_world-1) {
      w = scr_ranks_world-1;
    }

    /* allocate MPI_Request arrays and an array of ints */
    int* bytes            = (int*)         malloc(w * sizeof(double));
    MPI_Request* req_recv = (MPI_Request*) malloc(w * sizeof(MPI_Request));
    MPI_Status status;
    if (bytes == NULL || req_recv == NULL) {
      scr_abort(-1, "scr_fetch_files: Failed to allocate memory for flow control @ %s:%d",
                __FILE__, __LINE__
      );
    }

    int outstanding = 0;
    int index = 0;
    i = 1;
    while (i < scr_ranks_world || outstanding > 0) {
      /* issue up to w outstanding sends and receives */
      while (i < scr_ranks_world && outstanding < w) {
        /* post a receive for the response message we'll get back when rank i is done */
        MPI_Irecv(&bytes[index], 1, MPI_DOUBLE, i, 0, scr_comm_world, &req_recv[index]);

        /* TODO: would be more efficient to scatter this data */
        /* lookup hash for this rank, send it to the rank, and then unset it from the summary hash */
        scr_hash* rank_hash = scr_hash_get_kv_int(ckpt_hash, SCR_SUMMARY_KEY_RANK, i);
        scr_hash_send(rank_hash, i, scr_comm_world);
        scr_hash_unset_kv_int(ckpt_hash, SCR_SUMMARY_KEY_RANK, i);

        /* update the number of outstanding requests */
        outstanding++;
        index++;
        i++;
      }

      /* wait to hear back from any rank */
      MPI_Waitany(w, req_recv, &index, &status);

      /* TODO: want to check success code from processes here
       * (e.g., we could abort read early if rank 1 has trouble?) */

      /* add bytes to our total */
      total_bytes += bytes[index];

      /* one less request outstanding now */
      outstanding--;
    }

    /* free the MPI_Request arrays */
    if (req_recv != NULL) {
      free(req_recv);
      req_recv = NULL;
    }
    if (bytes != NULL) {
      free(bytes);
      bytes = NULL;
    }
  } else {
    /* receive our file data from rank 0 */
    scr_hash* rank_hash = scr_hash_new();
    scr_hash_recv(rank_hash, 0, scr_comm_world);

    /* fetch these files into the checkpoint directory */
    if (scr_fetch_files_list(map, rank_hash, checkpoint_id, fetch_dir, ckpt_dir, &total_bytes) != SCR_SUCCESS) {
      success = 0;
    }

    /* delete our file data hash object */
    scr_hash_delete(rank_hash);

    /* tell rank 0 that we're done and send total number of bytes we read */
    MPI_Send(&total_bytes, 1, MPI_DOUBLE, 0, 0, scr_comm_world);
  }

  /* free the hash holding the summary file data */
  scr_hash_delete(summary_hash);

  /* check that all processes copied their file successfully */
  if (!scr_alltrue(success)) {
    /* someone failed, so let's delete the partial checkpoint */
    scr_checkpoint_delete(map, checkpoint_id);

    if (scr_my_rank_world == 0) {
      scr_dbg(1, "scr_fetch_files: One or more processes failed to read its files @ %s:%d",
              __FILE__, __LINE__
      );
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FETCH FAILED", fetch_dir, &checkpoint_id, &now, &time_diff);
      }
    }
    return SCR_FAILURE;
  }

  /* apply redundancy scheme */
  double bytes_copied = 0.0;
  rc = scr_copy_files(map, c, checkpoint_id, &bytes_copied);
  if (rc == SCR_SUCCESS) {
    /* set the checkpoint id */
    scr_checkpoint_id = checkpoint_id;

    /* update our flush file to indicate this checkpoint is in cache as well as the parallel file system */
    /* TODO: should we place SCR_FLUSH_KEY_LOCATION_PFS before scr_copy_files? */
    scr_flush_location_set(checkpoint_id, SCR_FLUSH_KEY_LOCATION_CACHE);
    scr_flush_location_set(checkpoint_id, SCR_FLUSH_KEY_LOCATION_PFS);
    scr_flush_location_unset(checkpoint_id, SCR_FLUSH_KEY_LOCATION_FLUSHING);
  } else {
    /* something went wrong, so delete this checkpoint from the cache */
    scr_checkpoint_delete(scr_map, scr_checkpoint_id);
  }

  /* stop timer, compute bandwidth, and report performance */
  if (scr_my_rank_world == 0) {
    double time_end = MPI_Wtime();
    double time_diff = time_end - time_start;
    double bw = total_bytes / (1024.0 * 1024.0 * time_diff);
    scr_dbg(1, "scr_fetch_files: %f secs, %e bytes, %f MB/s, %f MB/s per proc",
            time_diff, total_bytes, bw, bw/scr_ranks_world
    );

    /* log data on the fetch to the database */
    if (scr_log_enable) {
      time_t now = scr_log_seconds();
      if (rc == SCR_SUCCESS) {
        scr_log_event("FETCH SUCCEEDED", fetch_dir, &checkpoint_id, &now, &time_diff);
      } else {
        scr_log_event("FETCH FAILED", fetch_dir, &checkpoint_id, &now, &time_diff);
      }

      char ckpt_dir[SCR_MAX_FILENAME];
      scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);
      scr_log_transfer("FETCH", fetch_dir, ckpt_dir, &checkpoint_id,
                       &timestamp_start, &time_diff, &total_bytes
      );
    }
  }

  return rc;
}

/* returns true if the named file needs to be flushed, 0 otherwise */
static int scr_bool_flush_file(const char* file)
{
  /* assume we need to flush this file */
  int flush = 1;

  /* read meta info for file */
  scr_meta* meta = scr_meta_new();
  if (scr_meta_read(file, meta) == SCR_SUCCESS) {
    /* don't flush XOR files */
    if (scr_meta_check_filetype(meta, SCR_META_FILE_XOR) == SCR_SUCCESS) {
      flush = 0;
    }
  } else {
    /* TODO: print error */
  }
  scr_meta_delete(meta);

  return flush;
}

/* create and return the name of a subdirectory under the prefix directory for the specified checkpoint id */
static int scr_flush_dir_create(int checkpoint_id, char* dir)
{
  /* have rank 0 create the checkpoint directory */
  int dirsize = 0;
  if (scr_my_rank_world == 0) {
    /* get the current time */
    time_t now;
    now = time(NULL);

    /* format timestamp */
    char timestamp[SCR_MAX_FILENAME];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H:%M:%S", localtime(&now));

    /* build the directory name */
    char dirname[SCR_MAX_FILENAME];
    sprintf(dirname, "scr.%s.%s.%d", timestamp, scr_jobid, checkpoint_id);

    /* add the directory to our index file, and record the flush timestamp */
    scr_hash* index_hash = scr_hash_new();
    scr_index_read(scr_par_prefix, index_hash);
    scr_index_add_checkpoint_dir(index_hash, checkpoint_id, dirname);
    scr_index_mark_flushed(index_hash, checkpoint_id, dirname);
    scr_index_write(scr_par_prefix, index_hash);
    scr_hash_delete(index_hash);

    /* create the directory, set dir to an empty string if mkdir fails */
    sprintf(dir, "%s/%s", scr_par_prefix, dirname);
    if (scr_mkdir(dir, S_IRWXU) != SCR_SUCCESS) {
      /* failed to create the directory */
      scr_err("scr_flush_dir_create: Failed to make checkpoint directory mkdir(%s) %m errno=%d @ %s:%d",
              dir, errno, __FILE__, __LINE__
      );

      /* set dir to an empty string to indicate failure */
      strcpy(dir, "");
    }

    /* compute the size of the directory string, including the terminating NULL character */
    dirsize = strlen(dir) + 1;
  }

  /* broadcast the directory name from rank 0 */
  MPI_Bcast(&dirsize, 1, MPI_INT, 0, scr_comm_world);
  MPI_Bcast(dir, dirsize, MPI_BYTE, 0, scr_comm_world);

  /* check whether directory was created ok, and bail out if not */
  if (strcmp(dir, "") == 0) {
    return SCR_FAILURE;
  }

  /* otherwise, the directory was created, and we're good to go */
  return SCR_SUCCESS;
}

/* flushes file named in src_file to dst_dir and fills in meta based on flush, returns success of flush */
static int scr_flush_a_file(const char* src_file, const char* dst_dir, scr_meta* meta)
{
  int flushed = SCR_SUCCESS;
  int tmp_rc;

  /* get full path of file to copy */
  char file[SCR_MAX_FILENAME];
  strcpy(file, src_file);

  /* break file into path and name components */
  char path[SCR_MAX_FILENAME];
  char name[SCR_MAX_FILENAME];
  scr_split_path(file, path, name);

  /* fill in meta with source file info */
  if (scr_meta_read(file, meta) != SCR_SUCCESS) {
    /* TODO: print error */
  }

  /* get meta data file name for file */
  char metafile[SCR_MAX_FILENAME];
  scr_meta_name(metafile, file);

  /* copy file */
  int crc_valid = 0;
  uLong crc;
  uLong* crc_p = NULL;
  if (scr_crc_on_flush) {
    crc_valid = 1;
    crc_p = &crc;
  }
  char my_flushed_file[SCR_MAX_FILENAME];
  tmp_rc = scr_copy_to(file, dst_dir, scr_file_buf_size,
                       my_flushed_file, sizeof(my_flushed_file), crc_p
  );
  if (tmp_rc != SCR_SUCCESS) {
    crc_valid = 0;
    flushed = SCR_FAILURE;
  }
  scr_dbg(2, "scr_flush_a_file: Read and copied %s to %s with success code %d @ %s:%d",
          file, my_flushed_file, tmp_rc, __FILE__, __LINE__
  );

  /* if file has crc32, check it against the one computed during the copy, otherwise if scr_crc_on_flush is set, record crc32 */
  if (crc_valid) {
    uLong crc_meta;
    if (scr_meta_get_crc32(meta, &crc_meta) == SCR_SUCCESS) {
      if (crc != crc_meta) { 
        /* detected a crc mismatch during the copy */

        /* TODO: unlink the copied file */
        /* unlink(my_flushed_file); */

        /* mark the file as invalid */
        scr_meta_set_complete(meta, 0);
        scr_meta_write(file, meta);

        flushed = SCR_FAILURE;
        scr_err("scr_flush_a_file: CRC32 mismatch detected when flushing file %s to %s @ %s:%d",
                file, my_flushed_file, __FILE__, __LINE__
        );

        /* TODO: would be good to log this, but right now only rank 0 can write log entries */
        /*
        if (scr_log_enable) {
          time_t now = scr_log_seconds();
          scr_log_event("CRC32 MISMATCH", my_flushed_file, NULL, &now, NULL);
        }
        */
      }
    } else {
      /* the crc was not already in the metafile, but we just computed it, so set it */
      scr_meta_set_crc32(meta, crc);
      scr_meta_write(file, meta);
    }
  }

  /* copy corresponding .scr file */
  char my_flushed_metafile[SCR_MAX_FILENAME];
  tmp_rc = scr_copy_to(metafile, dst_dir, scr_file_buf_size,
                       my_flushed_metafile, sizeof(my_flushed_metafile), NULL
  );
  if (tmp_rc != SCR_SUCCESS) {
    flushed = SCR_FAILURE;
  }
  scr_dbg(2, "scr_flush_a_file: Read and copied %s to %s with success code %d @ %s:%d",
          metafile, my_flushed_metafile, tmp_rc, __FILE__, __LINE__
  );

  /* TODO: check that written filesize matches expected filesize */

  /* fill out meta data, set complete field based on flush success */
  /* (we don't update the meta file here, since perhaps the file in cache is ok and only the flush failed) */
  int complete = (flushed == SCR_SUCCESS);
  scr_meta_set_complete(meta, complete);

  return flushed;
}

static int    scr_flush_async_in_progress = 0;       /* tracks whether an async flush is currently underway */
static int    scr_flush_async_checkpoint_id = -1;    /* tracks the id of the checkpoint being flushed */
static time_t scr_flush_async_timestamp_start;       /* records the time the async flush started */
static double scr_flush_async_time_start;            /* records the time the async flush started */
static char   scr_flush_async_dir[SCR_MAX_FILENAME]; /* records the directory the async flush is writing to */
static scr_hash* scr_flush_async_hash = NULL; /* tracks list of files written with flush */
static double scr_flush_async_bytes = 0.0;           /* records the total number of bytes to be flushed */
static int    scr_flush_async_num_files = 0;         /* records the number of files this process must flush */

/* queues file to flushed to dst_dir in hash, returns size of file in bytes */
static int scr_flush_async_file_enqueue(scr_hash* hash, const char* file, const char* dst_dir, double* bytes)
{
  /* start with 0 bytes written for this file */
  *bytes = 0.0;

  /* break file into path and name components */
  char path[SCR_MAX_FILENAME];
  char name[SCR_MAX_FILENAME];
  scr_split_path(file, path, name);

  /* create dest_file using dest_dir and name */
  char dest_file[SCR_MAX_FILENAME];
  scr_build_path(dest_file, sizeof(dest_file), dst_dir, name);

  /* look up the filesize of the file */
  unsigned long filesize = scr_filesize(file);

  /* add this file to the hash, and add its filesize to the number of bytes written */
  scr_hash* file_hash = scr_hash_set_kv(hash, SCR_TRANSFER_KEY_FILES, file);
  if (file_hash != NULL) {
    scr_hash_setf(file_hash, NULL, "%s %s",  "DESTINATION", dest_file);
    scr_hash_setf(file_hash, NULL, "%s %lu", "SIZE",        filesize);
    scr_hash_setf(file_hash, NULL, "%s %lu", "WRITTEN",     0);
  }
  *bytes += (double) filesize;
 
  /* get meta data file name for file */
  char metafile[SCR_MAX_FILENAME];
  scr_meta_name(metafile, file);

  /* look up the filesize of the metafile */
  unsigned long metasize = scr_filesize(metafile);

  /* break file into path and name components */
  char metapath[SCR_MAX_FILENAME];
  char metaname[SCR_MAX_FILENAME];
  scr_split_path(metafile, metapath, metaname);

  /* create dest_file using dest_dir and name */
  char dest_metafile[SCR_MAX_FILENAME];
  scr_build_path(dest_metafile, sizeof(dest_metafile), dst_dir, metaname);

  /* add the metafile to the transfer hash, and add its filesize to the number of bytes written */
  file_hash = scr_hash_set_kv(hash, SCR_TRANSFER_KEY_FILES, metafile);
  if (file_hash != NULL) {
    scr_hash_setf(file_hash, NULL, "%s %s",  "DESTINATION", dest_metafile);
    scr_hash_setf(file_hash, NULL, "%s %lu", "SIZE",        metasize);
    scr_hash_setf(file_hash, NULL, "%s %lu", "WRITTEN",     0);
  }
  *bytes += (double) metasize;
 
  return SCR_SUCCESS;
}

/* given a hash, test whether the files in that hash have completed their flush */
static int scr_flush_async_file_test(const scr_hash* hash, double* bytes)
{
  /* initialize bytes to 0 */
  *bytes = 0.0;

  /* get the FILES hash */
  scr_hash* files_hash = scr_hash_get(hash, SCR_TRANSFER_KEY_FILES);
  if (files_hash == NULL) {
    /* can't tell whether this flush has completed */
    return SCR_FAILURE;
  }

  /* assume we're done, look for a file that says we're not */
  int transfer_complete = 1;

  /* for each file, check whether the WRITTEN field matches the SIZE field,
   * which indicates the file has completed its transfer */
  scr_hash_elem* elem;
  for (elem = scr_hash_elem_first(files_hash);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get the file name */
    char* file = scr_hash_elem_key(elem);

    /* get the hash for this file */
    scr_hash* file_hash = scr_hash_elem_hash(elem);
    if (file_hash == NULL) {
      transfer_complete = 0;
      continue;
    }

    /* check whether this file is complete */
    /* lookup the strings for the size and bytes written */
    char* size    = scr_hash_elem_get_first_val(file_hash, "SIZE");
    char* written = scr_hash_elem_get_first_val(file_hash, "WRITTEN");
    if (size == NULL || written == NULL) {
      transfer_complete = 0;
      continue;
    }
  
    /* convert the size and bytes written strings to numbers */
    off_t size_count    = strtoul(size,    NULL, 0);
    off_t written_count = strtoul(written, NULL, 0);
    if (written_count < size_count) {
      transfer_complete = 0;
    }

    /* add up number of bytes written */
    *bytes += (double) written_count;
  }

  /* return our decision */
  if (transfer_complete) {
    return SCR_SUCCESS;
  }
  return SCR_FAILURE;
}

/* dequeues files listed in hash2 from hash1 */
static int scr_flush_async_file_dequeue(scr_hash* hash1, scr_hash* hash2)
{
  /* for each file listed in hash2, remove it from hash1 */
  scr_hash* file_hash = scr_hash_get(hash2, SCR_TRANSFER_KEY_FILES);
  if (file_hash != NULL) {
    scr_hash_elem* elem;
    for (elem = scr_hash_elem_first(file_hash);
         elem != NULL;
         elem = scr_hash_elem_next(elem))
    {
      /* get the filename, and dequeue it */
      char* file = scr_hash_elem_key(elem);
      scr_hash_unset_kv(hash1, SCR_TRANSFER_KEY_FILES, file);

      /* get meta data file name for file, and dequeue it */
      char metafile[SCR_MAX_FILENAME];
      scr_meta_name(metafile, file);
      scr_hash_unset_kv(hash1, SCR_TRANSFER_KEY_FILES, metafile);
    }
  }

  return SCR_SUCCESS;
}

/* start an asynchronous flush from cache to parallel file system under SCR_PREFIX */
static int scr_flush_async_start(scr_filemap* map, int checkpoint_id)
{
  int tmp_rc;

  /* if user has disabled flush, return failure */
  if (scr_flush <= 0) {
    return SCR_FAILURE;
  }

  /* if we don't need a flush, return right away with success */
  if (!scr_bool_need_flush(checkpoint_id)) {
    return SCR_SUCCESS;
  }

  /* if scr_par_prefix is not set, return right away with an error */
  if (strcmp(scr_par_prefix, "") == 0) {
    return SCR_FAILURE;
  }

  /* this may take a while, so tell user what we're doing */
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "scr_flush_async_start: Initiating flush of checkpoint %d", checkpoint_id);
  }

  /* make sure all processes make it this far before progressing */
  MPI_Barrier(scr_comm_world);

  /* start timer */
  if (scr_my_rank_world == 0) {
    scr_flush_async_timestamp_start = scr_log_seconds();
    scr_flush_async_time_start = MPI_Wtime();

    /* log the start of the flush */
    if (scr_log_enable) {
      scr_log_event("ASYNC FLUSH STARTED", NULL, &checkpoint_id, &scr_flush_async_timestamp_start, NULL);
    }
  }

  /* mark that we've started a flush */
  scr_flush_async_in_progress = 1;
  scr_flush_async_checkpoint_id = checkpoint_id;
  scr_flush_location_set(checkpoint_id, SCR_FLUSH_KEY_LOCATION_FLUSHING);

  /* get a new hash to record our file list */
  scr_flush_async_hash = scr_hash_new();
  scr_flush_async_num_files = 0;
  scr_flush_async_bytes = 0.0;

  /* read in the filemap to get the checkpoint file names */
  int have_files = 1;
  if (have_files && (scr_check_files(map, checkpoint_id) != SCR_SUCCESS)) {
    scr_err("scr_flush_async_start: One or more files is missing @ %s:%d", __FILE__, __LINE__);
    have_files = 0;
  }
  if (!scr_alltrue(have_files)) {
    if (scr_my_rank_world == 0) {
      scr_err("scr_flush_async_start: One or more processes are missing their files @ %s:%d",
              __FILE__, __LINE__
      );
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - scr_flush_async_time_start;
        time_t now = scr_log_seconds();
        scr_log_event("ASYNC FLUSH FAILED", "Missing files in cache", &checkpoint_id, &now, &time_diff);
      }
    }

    /* TODO: could lead to a memory leak in scr_flush_async_hash */

    return SCR_FAILURE;
  }

  /* create the checkpoint directory */
  if (scr_flush_dir_create(checkpoint_id, scr_flush_async_dir) != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_err("scr_flush_async_start: Failed to create checkpoint directory @ %s:%d",
              __FILE__, __LINE__
      );
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - scr_flush_async_time_start;
        time_t now = scr_log_seconds();
        scr_log_event("ASYNC FLUSH FAILED", "Failed to create directory", &checkpoint_id, &now, &time_diff);
      }
    }

    /* TODO: could lead to a memory leak in scr_flush_async_hash */

    return SCR_FAILURE;
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "scr_flush_async_start: Flushing to %s", scr_flush_async_dir);
  }

  /* add each of my files to the transfer file list */
  double my_bytes = 0.0;
  scr_hash_elem* elem;
  for (elem = scr_filemap_first_file(map, checkpoint_id, scr_my_rank_world);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get the filename */
    char* file = scr_hash_elem_key(elem);

    /* enqueue this file, and add its byte count to the total */
    if (scr_bool_flush_file(file)) {
      double file_bytes = 0.0;
      scr_flush_async_file_enqueue(scr_flush_async_hash, file, scr_flush_async_dir, &file_bytes);
      my_bytes += file_bytes;
      scr_flush_async_num_files++;
    }
  }

  /* have master on each node write the transfer file, everyone else sends data to him */
  if (scr_my_rank_local == 0) {
    /* receive hash data from other processes on the same node and merge with our data */
    int i;
    for (i=1; i < scr_ranks_local; i++) {
      scr_hash* h = scr_hash_new();
      scr_hash_recv(h, i, scr_comm_local);
      scr_hash_merge(scr_flush_async_hash, h);
      scr_hash_delete(h);
    }

    /* get a hash to store file data */
    scr_hash* hash = scr_hash_new();

    /* open transfer file with lock */
    int fd = -1;
    scr_hash_lock_open_read(scr_transfer_file, &fd, hash);

    /* merge our data to the file data */
    scr_hash_merge(hash, scr_flush_async_hash);

    char* value = NULL;

    /* set BW if it's not already set */
    value = scr_hash_elem_get_first_val(hash, SCR_TRANSFER_KEY_BW);
    if (value == NULL) {
      double bw = (double) scr_flush_async_bw / (double) scr_ranks_level;
      scr_hash_unset(hash, SCR_TRANSFER_KEY_BW);
      scr_hash_setf(hash, NULL, "%s %f", SCR_TRANSFER_KEY_BW, bw);
    }

    /* set PERCENT if it's not already set */
    value = scr_hash_elem_get_first_val(hash, SCR_TRANSFER_KEY_PERCENT);
    if (value == NULL) {
      scr_hash_unset(hash, SCR_TRANSFER_KEY_PERCENT);
      scr_hash_setf(hash, NULL, "%s %f", SCR_TRANSFER_KEY_PERCENT, scr_flush_async_percent);
    }

    /* set the RUN command */
    scr_hash_unset(hash, SCR_TRANSFER_KEY_COMMAND);
    scr_hash_set_kv(hash, SCR_TRANSFER_KEY_COMMAND, SCR_TRANSFER_KEY_COMMAND_RUN);

    /* unset the DONE flag */
    scr_hash_unset_kv(hash, SCR_TRANSFER_KEY_FLAG, SCR_TRANSFER_KEY_FLAG_DONE);

    /* close the transfer file and release the lock */
    scr_hash_write_close_unlock(scr_transfer_file, &fd, hash);

    /* delete the hash */
    scr_hash_delete(hash);
  } else {
    /* send our transfer hash data to the master on this node */
    scr_hash_send(scr_flush_async_hash, 0, scr_comm_local);
  }

  /* get the total number of bytes to write */
  MPI_Allreduce(&my_bytes, &scr_flush_async_bytes, 1, MPI_DOUBLE, MPI_SUM, scr_comm_world);

  /* TODO: start transfer thread / process */

  /* make sure all processes have started before we leave */
  MPI_Barrier(scr_comm_world);

  return SCR_SUCCESS;
}

/* writes the specified command to the transfer file */
static int scr_flush_async_command_set(char* command)
{
  /* have the master on each node write this command to the file */
  if (scr_my_rank_local == 0) {
    /* get a hash to store file data */
    scr_hash* hash = scr_hash_new();

    /* read the file */
    int fd = -1;
    scr_hash_lock_open_read(scr_transfer_file, &fd, hash);

    /* set the command */
    scr_hash_unset(hash, SCR_TRANSFER_KEY_COMMAND);
    scr_hash_set_kv(hash, SCR_TRANSFER_KEY_COMMAND, command);

    /* write the hash back */
    scr_hash_write_close_unlock(scr_transfer_file, &fd, hash);

    /* delete the hash */
    scr_hash_delete(hash);
  }

  return SCR_SUCCESS;
}

/* waits until all transfer processes are in the specified state */
static int scr_flush_async_state_wait(char* state)
{
  /* wait until each process matches the state */
  int all_valid = 0;
  while (!all_valid) {
    /* assume we match the asked state */
    int valid = 1;

    /* have the master on each node check the state in the transfer file */
    if (scr_my_rank_local == 0) {
      /* get a hash to store file data */
      scr_hash* hash = scr_hash_new();

      /* open transfer file with lock */
      scr_hash_read_with_lock(scr_transfer_file, hash);

      /* check for the specified state */
      scr_hash* state_hash = scr_hash_get_kv(hash, SCR_TRANSFER_KEY_STATE, state);
      if (state_hash == NULL) {
        valid = 0;
      }

      /* delete the hash */
      scr_hash_delete(hash);
    }

    /* check whether everyone is at the specified state */
    if (scr_alltrue(valid)) {
      all_valid = 1;
    }

    /* if we're not there yet, sleep for sometime and they try again */
    if (!all_valid) {
      usleep(10*1000*1000);
    }
  }

  return SCR_SUCCESS;
}

/* removes all files from the transfer file */
static int scr_flush_async_file_clear_all()
{
  /* have the master on each node clear the FILES field */
  if (scr_my_rank_local == 0) {
    /* get a hash to store file data */
    scr_hash* hash = scr_hash_new();

    /* read the file */
    int fd = -1;
    scr_hash_lock_open_read(scr_transfer_file, &fd, hash);

    /* clear the FILES entry */
    scr_hash_unset(hash, SCR_TRANSFER_KEY_FILES);

    /* write the hash back */
    scr_hash_write_close_unlock(scr_transfer_file, &fd, hash);

    /* delete the hash */
    scr_hash_delete(hash);
  }

  return SCR_SUCCESS;
}

/* stop an ongoing asynchronous flush for a specified checkpoint */
static int scr_flush_async_stop()
{
  int tmp_rc;

  /* if user has disabled flush, return failure */
  if (scr_flush <= 0) {
    return SCR_FAILURE;
  }

  /* this may take a while, so tell user what we're doing */
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "scr_flush_async_stop_all: Stopping flush");
  }

  /* write stop command to transfer file */
  scr_flush_async_command_set(SCR_TRANSFER_KEY_COMMAND_STOP);

  /* wait until all tasks know the transfer is stopped */
  scr_flush_async_state_wait(SCR_TRANSFER_KEY_STATE_STOP);

  /* remove the files list from the transfer file */
  scr_flush_async_file_clear_all();

  /* remove FLUSHING state from flush file */
  scr_flush_async_in_progress = 0;
  /*
  scr_flush_location_unset(checkpoint_id, SCR_FLUSH_KEY_LOCATION_FLUSHING);
  */

  /* clear internal flush_async variables to indicate there is no flush */
  if (scr_flush_async_hash != NULL) {
    scr_hash_delete(scr_flush_async_hash);
  }

  /* make sure all processes have made it this far before we leave */
  MPI_Barrier(scr_comm_world);

  return SCR_SUCCESS;
}

/* check whether the flush from cache to parallel file system has completed */
static int scr_flush_async_test(scr_filemap* map, int checkpoint_id, double* bytes)
{
  /* initialize bytes to 0 */
  *bytes = 0.0;

  /* if user has disabled flush, return failure */
  if (scr_flush <= 0) {
    return SCR_FAILURE;
  }

  /* test that all of our files for this checkpoint are still here */
  int have_files = 1;
  if (have_files && (scr_check_files(map, checkpoint_id) != SCR_SUCCESS)) {
    scr_err("scr_flush_async_test: One or more files is missing @ %s:%d", __FILE__, __LINE__);
    have_files = 0;
  }
  if (!scr_alltrue(have_files)) {
    if (scr_my_rank_world == 0) {
      scr_err("scr_flush_async_test: One or more processes are missing their files @ %s:%d",
              __FILE__, __LINE__
      );
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - scr_flush_async_time_start;
        time_t now = scr_log_seconds();
        scr_log_event("ASYNC FLUSH TEST FAILED", "Missing files in cache", &checkpoint_id, &now, &time_diff);
      }
    }
    return SCR_FAILURE;
  }

  /* assume the transfer is complete */
  int transfer_complete = 1;

  /* have master on each node check whether the flush is complete */
  double bytes_written = 0.0;
  if (scr_my_rank_local == 0) {
    /* create a hash to hold the transfer file data */
    scr_hash* hash = scr_hash_new();

    /* read transfer file with lock */
    if (scr_hash_read_with_lock(scr_transfer_file, hash) == SCR_SUCCESS) {
      /* test each file listed in the transfer hash */
      if (scr_flush_async_file_test(hash, &bytes_written) != SCR_SUCCESS) {
        transfer_complete = 0;
      }
    } else {
      /* failed to read the transfer file, can't determine whether the flush is complete */
      transfer_complete = 0;
    }

    /* free the hash */
    scr_hash_delete(hash);
  }

  /* compute the total number of bytes written */
  MPI_Allreduce(&bytes_written, bytes, 1, MPI_DOUBLE, MPI_SUM, scr_comm_world);

  /* determine whether the transfer is complete on all tasks */
  if (scr_alltrue(transfer_complete)) {
    return SCR_SUCCESS;
  }
  return SCR_FAILURE;
}

/* complete the flush from cache to parallel file system has completed */
static int scr_flush_async_complete(scr_filemap* map, int checkpoint_id)
{
  int tmp_rc;
  int i;

  /* if user has disabled flush, return failure */
  if (scr_flush <= 0) {
    return SCR_FAILURE;
  }

  /* read in the filemap to get the checkpoint file names */
  int have_files = 1;
  if (have_files && (scr_check_files(map, checkpoint_id) != SCR_SUCCESS)) {
    scr_err("scr_flush_async_complete: One or more files is missing @ %s:%d", __FILE__, __LINE__);
    have_files = 0;
  }
  if (!scr_alltrue(have_files)) {
    if (scr_my_rank_world == 0) {
      scr_err("scr_flush_async_complete: One or more processes are missing their files @ %s:%d",
              __FILE__, __LINE__
      );
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - scr_flush_async_time_start;
        time_t now = scr_log_seconds();
        scr_log_event("ASYNC FLUSH COMPLETE FAILED", "Missing files in cache", &checkpoint_id, &now, &time_diff);
      }
    }
    return SCR_FAILURE;
  }

  /* TODO: have master tell each rank on node whether its files were written successfully */

  /* allocate structure to hold metadata info */
  scr_hash* data = scr_hash_new();
  if (data == NULL) {
    scr_err("scr_flush_async_complete: Failed to malloc data structure to write out summary file @ file %s:%d",
            __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }

  /* set our rank */
  scr_hash* rank_hash = scr_hash_set_kv_int(data, SCR_SUMMARY_KEY_RANK, scr_my_rank_world);

  /* fill in metadata info for the files this process flushed */
  double total_bytes = 0.0;
  scr_hash_elem* elem = NULL;
  for (elem = scr_filemap_first_file(map, checkpoint_id, scr_my_rank_world);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get the filename */
    char* file = scr_hash_elem_key(elem);

    /* if we flushed this file, read its metadata info */
    if (scr_bool_flush_file(file)) {
      /* record the filename in the hash, and get reference to a hash for this file */
      char path[SCR_MAX_FILENAME];
      char name[SCR_MAX_FILENAME];
      scr_split_path(file, path, name);
      scr_hash* file_hash = scr_hash_set_kv(rank_hash, SCR_SUMMARY_KEY_FILE, name);

      /* read meta data for this file */
      scr_hash* flush_meta = scr_hash_new();
      scr_meta_read(file, flush_meta);

      /* TODO: check that this file was written successfully */

      /* successfully flushed this file, record the filesize */
      unsigned long flush_filesize = 0;
      if (scr_meta_get_filesize(flush_meta, &flush_filesize) == SCR_SUCCESS) {
        scr_hash_setf(file_hash, NULL, "%s %lu", SCR_SUMMARY_KEY_SIZE, flush_filesize);
        total_bytes += (double) flush_filesize;
      }

      /* record the crc32 if one was computed */
      uLong flush_crc32;
      if (scr_meta_get_crc32(flush_meta, &flush_crc32) == SCR_SUCCESS) {
        scr_hash_setf(file_hash, NULL, "%s %#lx", SCR_SUMMARY_KEY_CRC, flush_crc32);
      }

      /* delete our temporary meta data object */
      scr_hash_delete(flush_meta);
    }
  }

  /* gather metadata info from all tasks for all files to rank 0 */
  int flushed = SCR_SUCCESS;
  if (scr_my_rank_world == 0) {
    /* flow control with a sliding window of w processes */
    int w = scr_flush_width;
    if (w > scr_ranks_world-1) {
      w = scr_ranks_world-1;
    }

    /* allocate MPI_Request arrays and an array of ints */
    int*         ranks    = (int*)         malloc(w * sizeof(int));
    double*      bytes    = (double*)      malloc(w * sizeof(double));
    MPI_Request* req_recv = (MPI_Request*) malloc(w * sizeof(MPI_Request));
    MPI_Request* req_send = (MPI_Request*) malloc(w * sizeof(MPI_Request));
    MPI_Status status;

    i = 1;
    int outstanding = 0;
    int index = 0;
    while (i < scr_ranks_world || outstanding > 0) {
      /* issue up to w outstanding sends and receives */
      while (i < scr_ranks_world && outstanding < w) {
        /* record which rank we assign to this slot */
        ranks[index] = i;

        /* post a receive for the response message we'll get back when rank i is done */
        MPI_Irecv(&bytes[index], 1, MPI_DOUBLE, i, 0, scr_comm_world, &req_recv[index]);

        /* post a send to tell rank i to start */
        int start = 1;
        MPI_Isend(&start, 1, MPI_INT, i, 0, scr_comm_world, &req_send[index]);

        /* update the number of outstanding requests */
        i++;
        outstanding++;
        index++;
      }

      /* wait to hear back from any rank */
      MPI_Waitany(w, req_recv, &index, &status);

      /* once we hear back from a rank, the send to that rank should also be done, so complete it */
      MPI_Wait(&req_send[index], &status);

      /* receive the meta data from this rank */
      scr_hash* incoming_hash = scr_hash_new();
      scr_hash_recv(incoming_hash, ranks[index], scr_comm_world);
      scr_hash_merge(data, incoming_hash);
      scr_hash_delete(incoming_hash);

      /* add the number of bytes written by this rank */
      total_bytes += bytes[index];

      /* one less request outstanding now */
      outstanding--;
    }

    /* free the MPI_Request arrays */
    if (req_send != NULL) {
      free(req_send);
      req_send = NULL;
    }
    if (req_recv != NULL) {
      free(req_recv);
      req_recv = NULL;
    }
    if (bytes != NULL) {
      free(bytes);
      bytes = NULL;
    }
    if (ranks != NULL) {
      free(ranks);
      ranks = NULL;
    }
  } else {
    /* receive signal to start */
    int start = 0;
    MPI_Status status;
    MPI_Recv(&start, 1, MPI_INT, 0, 0, scr_comm_world, &status);

    /* flush each of my files and fill in meta data structures */
    scr_hash_elem* elem = NULL;
    for (elem = scr_filemap_first_file(map, checkpoint_id, scr_my_rank_world);
         elem != NULL;
         elem = scr_hash_elem_next(elem))
    {
      /* get the filename */
      char* file = scr_hash_elem_key(elem);

      /* flush this file if needed */
      if (scr_bool_flush_file(file)) {
        /* record the filename in the hash, and get reference to a hash for this file */
        char path[SCR_MAX_FILENAME];
        char name[SCR_MAX_FILENAME];
        scr_split_path(file, path, name);
        scr_hash* file_hash = scr_hash_set_kv(rank_hash, SCR_SUMMARY_KEY_FILE, name);

        /* read meta data for this file */
        scr_hash* flush_meta = scr_hash_new();
        scr_meta_read(file, flush_meta);

        /* TODO: check that this file was written successfully */

        /* successfully flushed this file, record the filesize */
        unsigned long flush_filesize = 0;
        if (scr_meta_get_filesize(flush_meta, &flush_filesize) == SCR_SUCCESS) {
          scr_hash_setf(file_hash, NULL, "%s %lu", SCR_SUMMARY_KEY_SIZE, flush_filesize);
          total_bytes += (double) flush_filesize;
        }

        /* record the crc32 if one was computed */
        uLong flush_crc32;
        if (scr_meta_get_crc32(flush_meta, &flush_crc32) == SCR_SUCCESS) {
          scr_hash_setf(file_hash, NULL, "%s %#lx", SCR_SUMMARY_KEY_CRC, flush_crc32);
        }

        /* delete our temporary meta data object */
        scr_hash_delete(flush_meta);
      }
    }

    /* send total bytes to rank 0 */
    MPI_Send(&total_bytes, 1, MPI_DOUBLE, 0, 0, scr_comm_world);

    /* would be better to do this as a reduction-type of operation */
    scr_hash_send(data, 0, scr_comm_world);
  }

  /* determine whether everyone wrote their files ok */
  int all_success = scr_alltrue((flushed == SCR_SUCCESS));

  if (scr_my_rank_world == 0) {
    if (all_success) {
      /* everyone wrote their files ok, now write out summary file */
      int wrote_summary = scr_summary_write(scr_flush_async_dir, checkpoint_id, all_success, data);
      if (wrote_summary != SCR_SUCCESS) {
        flushed = SCR_FAILURE;
      }

      /* if we also wrote the summary file ok, update the current symlink */
      if (flushed == SCR_SUCCESS) {
        /* TODO: update 'flushed' depending on symlink update */

        /* build path to current symlink */
        char current[SCR_MAX_FILENAME];
        scr_build_path(current, sizeof(current), scr_par_prefix, SCR_CURRENT_LINK);

        /* if current exists, unlink it */
        if (access(current, F_OK) == 0) {
          unlink(current);
        }

        /* create new current to point to new directory */
        char target_path[SCR_MAX_FILENAME];
        char target_name[SCR_MAX_FILENAME];
        scr_split_path(scr_flush_async_dir, target_path, target_name);
        symlink(target_name, current);
      } else {
        /* someone failed to write a file */
        flushed = SCR_FAILURE;
      }
    }
  }

  /* have rank 0 broadcast whether the entire flush succeeded, including summary file and symlink update */
  MPI_Bcast(&flushed, 1, MPI_INT, 0, scr_comm_world);

  /* mark set as flushed to the parallel file system */
  if (flushed == SCR_SUCCESS) {
    scr_flush_location_set(checkpoint_id, SCR_FLUSH_KEY_LOCATION_PFS);
  }

  /* mark that we've stopped the flush */
  scr_flush_async_in_progress = 0;
  scr_flush_location_unset(checkpoint_id, SCR_FLUSH_KEY_LOCATION_FLUSHING);

  /* have master on each node remove files from the transfer file */
  if (scr_my_rank_local == 0) {
    /* get a hash to read from the file */
    scr_hash* transfer_hash = scr_hash_new();

    /* lock the transfer file, open it, and read it into the hash */
    int fd = -1;
    scr_hash_lock_open_read(scr_transfer_file, &fd, transfer_hash);

    /* remove files from the list */
    scr_flush_async_file_dequeue(transfer_hash, scr_flush_async_hash);

    /* set the STOP command */
    scr_hash_unset(transfer_hash, SCR_TRANSFER_KEY_COMMAND);
    scr_hash_set_kv(transfer_hash, SCR_TRANSFER_KEY_COMMAND, SCR_TRANSFER_KEY_COMMAND_STOP);

    /* write the hash back to the file */
    scr_hash_write_close_unlock(scr_transfer_file, &fd, transfer_hash);

    /* delete the hash */
    scr_hash_delete(transfer_hash);
  }

  /* free the file list for this checkpoint */
  scr_hash_delete(scr_flush_async_hash);

  /* free data structures */
  scr_hash_delete(data);

  /* stop timer, compute bandwidth, and report performance */
  if (scr_my_rank_world == 0) {
    double time_end = MPI_Wtime();
    double time_diff = time_end - scr_flush_async_time_start;
    double bw = scr_flush_async_bytes / (1024.0 * 1024.0 * time_diff);
    scr_dbg(1, "scr_flush_async_complete: %f secs, %e bytes, %f MB/s, %f MB/s per proc",
            time_diff, scr_flush_async_bytes, bw, bw/scr_ranks_world
    );

    /* log messages about flush */
    if (flushed == SCR_SUCCESS) {
      /* the flush worked, print a debug message */
      scr_dbg(1, "scr_flush_async_complete: Flush of checkpoint %d succeeded", checkpoint_id);

      /* log details of flush */
      if (scr_log_enable) {
        time_t now = scr_log_seconds();
        scr_log_event("ASYNC FLUSH SUCCEEDED", scr_flush_async_dir, &checkpoint_id, &now, &time_diff);

        /* lookup the checkpoint directory for this checkpoint */
        char* ckpt_dir = scr_ckptdesc_dir_from_filemap(map, checkpoint_id, scr_my_rank_world);
        scr_log_transfer("ASYNC FLUSH", ckpt_dir, scr_flush_async_dir, &checkpoint_id,
                         &scr_flush_async_timestamp_start, &time_diff, &scr_flush_async_bytes
        );
        if (ckpt_dir != NULL) {
          free(ckpt_dir);
        }
      }
    } else {
      /* the flush failed, this is more serious so print an error message */
      scr_err("scr_flush_async_complete: Flush failed");

      /* log details of flush */
      if (scr_log_enable) {
        time_t now = scr_log_seconds();
        scr_log_event("ASYNC FLUSH FAILED", scr_flush_async_dir, &checkpoint_id, &now, &time_diff);
      }
    }
  }

  return flushed;
}

/* wait until the checkpoint currently being flushed completes */
static int scr_flush_async_wait(scr_filemap* map)
{
  if (scr_flush_async_in_progress) {
    while (scr_bool_is_flushing(scr_flush_async_checkpoint_id)) {
      /* test whether the flush has completed, and if so complete the flush */
      double bytes = 0.0;
      if (scr_flush_async_test(map, scr_flush_async_checkpoint_id, &bytes) == SCR_SUCCESS) {
        /* complete the flush */
        scr_flush_async_complete(map, scr_flush_async_checkpoint_id);
      } else {
        /* otherwise, sleep to get out of the way */
        if (scr_my_rank_world == 0) {
          scr_dbg(1, "Flush of checkpoint %d is %d%% complete",
                  scr_flush_async_checkpoint_id, (int) (bytes / scr_flush_async_bytes * 100.0)
          );
        }
        usleep(10*1000*1000);
      }
    }
  }

  return SCR_SUCCESS;
}

/* flush files listed in filemap for specified checkpoint_id to specified directory,
 * record files in hash and fill in number of bytes flushed,
 * returns SCR_SUCCESS if successful */
static int scr_flush_files_list(scr_filemap* map, scr_hash* hash, int checkpoint_id, const char* flush_dir, double* total_bytes)
{
  /* assume we will succeed in this flush */
  int rc = SCR_SUCCESS;

  /* assume we won't flush any files */
  *total_bytes = 0.0;

  /* flush each of my files, fill in meta data structure, add to the byte count */
  scr_hash_elem* elem = NULL;
  for (elem = scr_filemap_first_file(map, checkpoint_id, scr_my_rank_world);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get the filename */
    char* file = scr_hash_elem_key(elem);

    /* flush this file if needed */
    if (scr_bool_flush_file(file)) {
      /* record the filename in the hash, and get reference to a hash for this file */
      char path[SCR_MAX_FILENAME];
      char name[SCR_MAX_FILENAME];
      scr_split_path(file, path, name);
      scr_hash* file_hash = scr_hash_set_kv(hash, SCR_SUMMARY_KEY_FILE, name);

      /* allocate a new meta data object */
      scr_meta* meta = scr_meta_new();

      /* flush the file and fill in the meta data for this file */
      if (scr_flush_a_file(file, flush_dir, meta) != SCR_SUCCESS) {
        /* the flush failed */
        rc = SCR_FAILURE;

        /* explicitly mark incomplete files */
        scr_hash_set_kv_int(file_hash, SCR_SUMMARY_KEY_COMPLETE, 0);
      } else {
        /* successfully flushed this file, record the filesize */
        unsigned long filesize = 0;
        if (scr_meta_get_filesize(meta, &filesize) == SCR_SUCCESS) {
          scr_hash_setf(file_hash, NULL, "%s %lu", SCR_SUMMARY_KEY_SIZE, filesize);
          *total_bytes += (double) filesize;
        }

        /* record the crc32 if one was computed */
        uLong crc = 0;
        if (scr_meta_get_crc32(meta, &crc) == SCR_SUCCESS) {
          scr_hash_setf(file_hash, NULL, "%s %#lx", SCR_SUMMARY_KEY_CRC, crc);
        }
      }

      /* delete our temporary meta data object */
      scr_meta_delete(meta);
    }
  }

  return rc;
}

/* flush files from cache to parallel file system under SCR_PREFIX */
static int scr_flush_files(scr_filemap* map, int checkpoint_id)
{
  int flushed = SCR_SUCCESS;
  int i;
  int tmp_rc;

  /* if user has disabled flush, return failure */
  if (scr_flush <= 0) {
    return SCR_FAILURE;
  }

  /* if we don't need a flush, return right away with success */
  if (!scr_bool_need_flush(checkpoint_id)) {
    return SCR_SUCCESS;
  }

  /* if scr_par_prefix is not set, return right away with an error */
  if (strcmp(scr_par_prefix, "") == 0) {
    return SCR_FAILURE;
  }

  /* this may take a while, so tell user what we're doing */
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "scr_flush_files: Initiating flush of checkpoint %d", checkpoint_id);
  }

  /* make sure all processes make it this far before progressing */
  MPI_Barrier(scr_comm_world);

  /* start timer */
  time_t timestamp_start;
  double time_start;
  if (scr_my_rank_world == 0) {
    timestamp_start = scr_log_seconds();
    time_start = MPI_Wtime();
  }

  /* log the flush start */
  if (scr_my_rank_world == 0) {
    if (scr_log_enable) {
      time_t now = scr_log_seconds();
      scr_log_event("FLUSH STARTED", NULL, &checkpoint_id, &now, NULL);
    }
  }

  /* check that we have all of our files */
  int have_files = 1;
  if (have_files && (scr_check_files(map, checkpoint_id) != SCR_SUCCESS)) {
    scr_err("scr_flush_files: One or more files is missing @ %s:%d", __FILE__, __LINE__);
    have_files = 0;
  }
  if (!scr_alltrue(have_files)) {
    if (scr_my_rank_world == 0) {
      scr_err("scr_flush_files: One or more processes are missing their files @ %s:%d", __FILE__, __LINE__);
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FLUSH FAILED", "Missing files in cache", &checkpoint_id, &now, &time_diff);
      }
    }
    return SCR_FAILURE;
  }

  /* if we are flushing something asynchronously, wait on it */
  if (scr_flush_async_in_progress) {
    scr_flush_async_wait(map);
    
    /* the flush we just waited on could be the requested checkpoint, so perhaps we're already done */
    if (!scr_bool_need_flush(checkpoint_id)) {
      return SCR_SUCCESS;
    }
  }

  /* create the checkpoint directory */
  char dir[SCR_MAX_FILENAME];
  if (scr_flush_dir_create(checkpoint_id, dir) != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_err("scr_flush_files: Failed to create checkpoint directory @ %s:%d", __FILE__, __LINE__);
      if (scr_log_enable) {
        double time_end = MPI_Wtime();
        double time_diff = time_end - time_start;
        time_t now = scr_log_seconds();
        scr_log_event("FLUSH FAILED", "Failed to create directory", &checkpoint_id, &now, &time_diff);
      }
    }
    return SCR_FAILURE;
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "scr_flush_files: Flushing to %s", dir);
  }

  /* allocate structure to hold summary file info */
  scr_hash* data = scr_hash_new();
  if (data == NULL) {
    scr_err("scr_flush_files: Failed to malloc data structure to write out summary file @ file %s:%d",
            __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }

  /* set our rank */
  scr_hash* rank_hash = scr_hash_set_kv_int(data, SCR_SUMMARY_KEY_RANK, scr_my_rank_world);

  /* flow control the write among processes, and gather meta data to rank 0 */
  double total_bytes = 0.0;
  if (scr_my_rank_world == 0) {
    /* flush each of my files, fill in meta data structure, add to the byte count */
    scr_flush_files_list(map, rank_hash, checkpoint_id, dir, &total_bytes);

    /* now, have a sliding window of w processes write simultaneously */
    int w = scr_flush_width;
    if (w > scr_ranks_world-1) {
      w = scr_ranks_world-1;
    }

    /* allocate MPI_Request arrays and an array of ints */
    int*         ranks    = (int*)         malloc(w * sizeof(int));
    double*      bytes    = (double*)      malloc(w * sizeof(double));
    MPI_Request* req_recv = (MPI_Request*) malloc(w * sizeof(MPI_Request));
    MPI_Request* req_send = (MPI_Request*) malloc(w * sizeof(MPI_Request));
    MPI_Status status;

    i = 1;
    int outstanding = 0;
    int index = 0;
    while (i < scr_ranks_world || outstanding > 0) {
      /* issue up to w outstanding sends and receives */
      while (i < scr_ranks_world && outstanding < w) {
        /* record which rank we assign to this slot */
        ranks[index] = i;

        /* post a receive for the response message we'll get back when rank i is done */
        MPI_Irecv(&bytes[index], 1, MPI_DOUBLE, i, 0, scr_comm_world, &req_recv[index]);

        /* post a send to tell rank i to start */
        int start = 1;
        MPI_Isend(&start, 1, MPI_INT, i, 0, scr_comm_world, &req_send[index]);

        /* update the number of outstanding requests */
        i++;
        outstanding++;
        index++;
      }

      /* wait to hear back from any rank */
      MPI_Waitany(w, req_recv, &index, &status);

      /* someone responded, the send to this rank should also be done, so complete it */
      MPI_Wait(&req_send[index], &status);

      /* receive the meta data from this rank */
      scr_hash* incoming_hash = scr_hash_new();
      scr_hash_recv(incoming_hash, ranks[index], scr_comm_world);
      scr_hash_merge(data, incoming_hash);
      scr_hash_delete(incoming_hash);

      /* add the number of bytes written by this rank */
      total_bytes += bytes[index];

      /* one less request outstanding now */
      outstanding--;
    }

    /* free the MPI_Request arrays */
    if (req_send != NULL) {
      free(req_send);
      req_send = NULL;
    }
    if (req_recv != NULL) {
      free(req_recv);
      req_recv = NULL;
    }
    if (bytes != NULL) {
      free(bytes);
      bytes = NULL;
    }
    if (ranks != NULL) {
      free(ranks);
      ranks = NULL;
    }
  } else {
    /* receive signal to start */
    int start = 0;
    MPI_Status status;
    MPI_Recv(&start, 1, MPI_INT, 0, 0, scr_comm_world, &status);

    /* flush each of my files and fill in meta data structures */
    scr_flush_files_list(map, rank_hash, checkpoint_id, dir, &total_bytes);

    /* send total bytes to rank 0 */
    MPI_Send(&total_bytes, 1, MPI_DOUBLE, 0, 0, scr_comm_world);

    /* would be better to do this as a reduction-type of operation */
    scr_hash_send(data, 0, scr_comm_world);
  }

  /* determine whether everyone wrote their files ok */
  int all_success = scr_alltrue((flushed == SCR_SUCCESS));

  if (scr_my_rank_world == 0) {
    if (all_success) {
      /* everyone wrote their files ok, now write out summary file */
      if (scr_summary_write(dir, checkpoint_id, all_success, data) != SCR_SUCCESS) {
        flushed = SCR_FAILURE;
      }

      /* if we also wrote the summary file ok, update the current symlink */
      if (flushed == SCR_SUCCESS) {
        /* TODO: update 'flushed' depending on symlink update */

        /* build path to current symlink */
        char current[SCR_MAX_FILENAME];
        scr_build_path(current, sizeof(current), scr_par_prefix, SCR_CURRENT_LINK);

        /* if current exists, unlink it */
        if (access(current, F_OK) == 0) {
          unlink(current);
        }

        /* create new current to point to new directory */
        char target_path[SCR_MAX_FILENAME];
        char target_name[SCR_MAX_FILENAME];
        scr_split_path(dir, target_path, target_name);
        symlink(target_name, current);
      } else {
        /* someone failed to write a file */
        flushed = SCR_FAILURE;
      }
    }
  }

  /* have rank 0 broadcast whether the entire flush succeeded, including summary file and symlink update */
  MPI_Bcast(&flushed, 1, MPI_INT, 0, scr_comm_world);

  /* mark this checkpoint as flushed to the parallel file system */
  if (flushed == SCR_SUCCESS) {
    scr_flush_location_set(checkpoint_id, SCR_FLUSH_KEY_LOCATION_PFS);
  }

  /* free data structures */
  scr_hash_delete(data);

  /* stop timer, compute bandwidth, and report performance */
  if (scr_my_rank_world == 0) {
    double time_end = MPI_Wtime();
    double time_diff = time_end - time_start;
    double bw = total_bytes / (1024.0 * 1024.0 * time_diff);
    scr_dbg(1, "scr_flush_files: %f secs, %e bytes, %f MB/s, %f MB/s per proc",
            time_diff, total_bytes, bw, bw/scr_ranks_world
    );

    /* log messages about flush */
    if (flushed == SCR_SUCCESS) {
      /* the flush worked, print a debug message */
      scr_dbg(1, "scr_flush_files: Flush of checkpoint %d succeeded", checkpoint_id);

      /* log details of flush */
      if (scr_log_enable) {
        time_t now = scr_log_seconds();
        scr_log_event("FLUSH SUCCEEDED", dir, &checkpoint_id, &now, &time_diff);

        /* lookup the checkpoint descriptor for this checkpoint */
        char* ckpt_dir = scr_ckptdesc_dir_from_filemap(map, checkpoint_id, scr_my_rank_world);
        scr_log_transfer("FLUSH", ckpt_dir, dir, &checkpoint_id, &timestamp_start, &time_diff, &total_bytes);
        if (ckpt_dir != NULL) {
          free(ckpt_dir);
        }
      }
    } else {
      /* the flush failed, this is more serious so print an error message */
      scr_err("scr_flush_files: Flush of checkpoint %d failed", checkpoint_id);

      /* log details of flush */
      if (scr_log_enable) {
        time_t now = scr_log_seconds();
        scr_log_event("FLUSH FAILED", dir, &checkpoint_id, &now, &time_diff);
      }
    }
  }

  return flushed;
}

/* check whether a flush is needed, and execute flush if so */
static int scr_check_flush(scr_filemap* map)
{
  /* check whether user has enabled SCR auto-flush feature */
  if (scr_flush > 0) {
    /* every scr_flush checkpoints, flush the checkpoint set */
    if (scr_checkpoint_id > 0 && scr_checkpoint_id % scr_flush == 0) {
      if (scr_flush_async) {
        /* check that we don't start an async flush if one is already in progress */
        if (scr_flush_async_in_progress) {
          /* we need to flush the current checkpoint, however, another flush is ongoing,
           * so wait for this other flush to complete before starting the next one */
          scr_flush_async_wait(map);
        }

        /* start an async flush on the current checkpoint id */
        scr_flush_async_start(map, scr_checkpoint_id);
      } else {
        /* synchronously flush the current checkpoint */
        scr_flush_files(map, scr_checkpoint_id);
      }
    }
  }
  return SCR_SUCCESS;
}

/*
=========================================
Halt logic
=========================================
*/

/* writes a halt file to indicate that the SCR should exit job at first opportunity */
static int scr_halt(const char* reason)
{
  /* copy in reason if one was given */
  if (reason != NULL) {
    scr_hash_unset(scr_halt_hash, SCR_HALT_KEY_EXIT_REASON);
    scr_hash_set_kv(scr_halt_hash, SCR_HALT_KEY_EXIT_REASON, reason);
  }

  /* log the halt condition */
  int* ckpt = NULL;
  if (scr_checkpoint_id > 0) {
    ckpt = &scr_checkpoint_id;
  }
  scr_log_halt(reason, ckpt);

  /* and write out the halt file */
  return scr_halt_sync_and_decrement(scr_halt_file, scr_halt_hash, 0);
}

/* returns the number of seconds remaining in the time allocation */
static int scr_seconds_remaining()
{
  /* returning a negative number tells the caller this functionality is disabled */
  int secs = -1;

  /* call libyogrt if we have it */
  #ifdef HAVE_LIBYOGRT
    secs = yogrt_remaining();
    if (secs < 0) {
      secs = 0;
    }
  #endif /* HAVE_LIBYOGRT */

  return secs;
}

/* check whether we should halt the job */
static int scr_bool_check_halt_and_decrement(int halt_cond, int decrement)
{
  /* assume we don't have to halt */
  int need_to_halt = 0;

  /* only rank 0 reads the halt file */
  if (scr_my_rank_world == 0) {
    /* TODO: all epochs are stored in ints, should be in unsigned ints? */
    /* get current epoch seconds */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int now = tv.tv_sec;

    /* locks halt file, reads it to pick up new values, decrements the
     * checkpoint counter, writes it out, and unlocks it */
    scr_halt_sync_and_decrement(scr_halt_file, scr_halt_hash, decrement);

    char* value = NULL;

    /* set halt seconds to value found in our halt hash */
    int halt_seconds = 0;
    value = scr_hash_elem_get_first_val(scr_halt_hash, SCR_HALT_KEY_SECONDS);
    if (value != NULL) {
      halt_seconds = atoi(value);
    }

    /* if halt secs enabled, check the remaining time */
    if (halt_seconds > 0) {
      int remaining = scr_seconds_remaining();
      if (remaining >= 0 && remaining <= halt_seconds) {
        if (halt_cond == SCR_TEST_AND_HALT) {
          scr_dbg(0, "Job exiting: Reached time limit: (seconds remaining = %d) <= (SCR_HALT_SECONDS = %d).",
                  remaining, halt_seconds
          );
          scr_halt("TIME_LIMIT");
        }
        need_to_halt = 1;
      }
    }

    /* check whether a reason has been specified */
    value = scr_hash_elem_get_first_val(scr_halt_hash, SCR_HALT_KEY_EXIT_REASON);
    if (value != NULL) {
      if (strcmp(value, "") != 0) {
        /* since value points at the EXIT_REASON string in the halt hash, and since
         * scr_halt() resets this value, we need to copy the current reason */
        char* tmp_value = strdup(value);
        if (halt_cond == SCR_TEST_AND_HALT && tmp_value != NULL) {
          scr_dbg(0, "Job exiting: Reason: %s.", tmp_value);
          scr_halt(tmp_value);
        }
        if (tmp_value != NULL) {
          free(tmp_value);
          tmp_value = NULL;
        }
        need_to_halt = 1;
      }
    }

    /* check whether we are out of checkpoints */
    value = scr_hash_elem_get_first_val(scr_halt_hash, SCR_HALT_KEY_CHECKPOINTS);
    if (value != NULL) {
      int checkpoints_left = atoi(value);
      if (checkpoints_left == 0) {
        if (halt_cond == SCR_TEST_AND_HALT) {
          scr_dbg(0, "Job exiting: No more checkpoints remaining.");
          scr_halt("NO_CHECKPOINTS_LEFT");
        }
        need_to_halt = 1;
      }
    }

    /* check whether we need to exit before a specified time */
    value = scr_hash_elem_get_first_val(scr_halt_hash, SCR_HALT_KEY_EXIT_BEFORE);
    if (value != NULL) {
      int exit_before = atoi(value);
      if (now >= (exit_before - halt_seconds)) {
        if (halt_cond == SCR_TEST_AND_HALT) {
          time_t time_now  = (time_t) now;
          time_t time_exit = (time_t) exit_before - halt_seconds;
          char str_now[256];
          char str_exit[256];
          strftime(str_now,  sizeof(str_now),  "%c", localtime(&time_now));
          strftime(str_exit, sizeof(str_exit), "%c", localtime(&time_exit));
          scr_dbg(0, "Job exiting: Current time (%s) is past ExitBefore-HaltSeconds time (%s).",
                  str_now, str_exit
          );
          scr_halt("EXIT_BEFORE_TIME");
        }
        need_to_halt = 1;
      }
    }

    /* check whether we need to exit after a specified time */
    value = scr_hash_elem_get_first_val(scr_halt_hash, SCR_HALT_KEY_EXIT_AFTER);
    if (value != NULL) {
      int exit_after = atoi(value);
      if (now >= exit_after) {
        if (halt_cond == SCR_TEST_AND_HALT) {
          time_t time_now  = (time_t) now;
          time_t time_exit = (time_t) exit_after;
          char str_now[256];
          char str_exit[256];
          strftime(str_now,  sizeof(str_now),  "%c", localtime(&time_now));
          strftime(str_exit, sizeof(str_exit), "%c", localtime(&time_exit));
          scr_dbg(0, "Job exiting: Current time (%s) is past ExitAfter time (%s).", str_now, str_exit);
          scr_halt("EXIT_AFTER_TIME");
        }
        need_to_halt = 1;
      }
    }
  }

  MPI_Bcast(&need_to_halt, 1, MPI_INT, 0, scr_comm_world);
  if (need_to_halt && halt_cond == SCR_TEST_AND_HALT) {
    /* handle any async flush */
    if (scr_flush_async_in_progress) {
      if (scr_flush_async_checkpoint_id == scr_checkpoint_id) {
        /* we're going to sync flush this same checkpoint below, so kill it */
        scr_flush_async_stop(scr_map);
      } else {
        /* the async flush is flushing a different checkpoint, so wait for it */
        scr_flush_async_wait(scr_map);
      }
    }

    /* flush files if needed */
    scr_flush_files(scr_map, scr_checkpoint_id);

    /* sync up tasks before exiting (don't want tasks to exit so early that
     * runtime kills others after timeout) */
    MPI_Barrier(scr_comm_world);

    /* and exit the job */
    exit(0);
  }

  return need_to_halt;
}

/*
=========================================
Distribute and file rebuild functions
=========================================
*/

/* returns true if a an XOR file is found for this rank for the given checkpoint id,
 * sets xor_file to full filename */
static int scr_bool_have_xor_file(scr_filemap* map, int checkpoint_id, char* xor_file)
{
  int rc = 0;

  /* find the name of my xor chunk file: read filemap and check filetype of each file */
  scr_hash_elem* file_elem = NULL;
  for (file_elem = scr_filemap_first_file(map, checkpoint_id, scr_my_rank_world);
       file_elem != NULL;
       file_elem = scr_hash_elem_next(file_elem))
  {
    /* get the filename */
    char* file = scr_hash_elem_key(file_elem);

    /* read the meta for this file */
    scr_meta* meta = scr_meta_new();
    scr_meta_read(file, meta);

    /* if the filetype of this file is an XOR fule, copy the filename and bail out */
    char* filetype = NULL;
    if (scr_meta_get_filetype(meta, &filetype) == SCR_SUCCESS) {
      if (strcmp(filetype, SCR_META_FILE_XOR) == 0) {
        strcpy(xor_file, file);
        rc = 1;
        scr_meta_delete(meta);
        break;
      }
    }

    /* free the meta data for this file and go on to the next */
    scr_meta_delete(meta);
  }

  return rc;
}

/* given a filename to my XOR file, a failed rank in my xor set,
 * rebuild file and return new filename and current checkpoint id to caller */
static int scr_rebuild_xor(scr_filemap* map, const struct scr_ckptdesc* c, int checkpoint_id, int root)
{
  int rc = SCR_SUCCESS;
  int i;
  MPI_Status  status[2];

  int fd_chunk = 0;
  char full_chunk_filename[SCR_MAX_FILENAME] = "";
  char path[SCR_MAX_FILENAME] = "";
  char name[SCR_MAX_FILENAME] = "";

  int* fds = NULL;
  char** filenames = NULL;
  unsigned long* filesizes = NULL;

  /* allocate hash object to read in (or receive) the header of the XOR file */
  scr_hash* header = scr_hash_new();

  int num_files = -1;
  scr_hash* current_hash = NULL;
  if (root != c->my_rank) {
    /* lookup name of xor file */
    if (!scr_bool_have_xor_file(map, checkpoint_id, full_chunk_filename)) {
      scr_abort(-1, "Missing XOR file %s @ %s:%d",
              full_chunk_filename, __FILE__, __LINE__
      );
    }

    /* open our xor file for reading */
    fd_chunk = scr_open(full_chunk_filename, O_RDONLY);
    if (fd_chunk < 0) {
      scr_abort(-1, "Opening XOR file for reading in XOR rebuild: scr_open(%s, O_RDONLY) errno=%d %m @ %s:%d",
              full_chunk_filename, errno, __FILE__, __LINE__
      );
    }

    /* read in the xor chunk header */
    scr_hash_read_fd(full_chunk_filename, fd_chunk, header);

    /* lookup number of files this process wrote */
    current_hash = scr_hash_get(header, SCR_KEY_COPY_XOR_CURRENT);
    if (scr_hash_util_get_int(current_hash, SCR_KEY_COPY_XOR_FILES, &num_files) != SCR_SUCCESS) {
      scr_abort(-1, "Failed to read number of files from XOR file header: %s @ %s:%d",
              full_chunk_filename, __FILE__, __LINE__
      );
    }

    /* allocate arrays to hold file descriptors, filenames, and filesizes for each of our files */
    if (num_files > 0) {
      fds       = (int*)           malloc(num_files * sizeof(int));
      filenames = (char**)         malloc(num_files * sizeof(char*));
      filesizes = (unsigned long*) malloc(num_files * sizeof(unsigned long));
      if (fds == NULL || filenames == NULL || filesizes == NULL) {
        scr_abort(-1, "Failed to allocate file arrays @ %s:%d",
                  __FILE__, __LINE__
        );
      }
    }

    /* get path from chunk file */
    scr_split_path(full_chunk_filename, path, name);

    /* open each of our files */
    for (i=0; i < num_files; i++) {
      /* lookup meta data for this file from header hash */
      scr_hash* meta_tmp = scr_hash_get_kv_int(current_hash, SCR_KEY_COPY_XOR_FILE, i);
      if (meta_tmp == NULL) {
        scr_abort(-1, "Failed to find file %d in XOR file header %s @ %s:%d",
                  i, full_chunk_filename, __FILE__, __LINE__
        );
      }

      /* get pointer to filename for this file */
      char* filename = NULL;
      if (scr_meta_get_filename(meta_tmp, &filename) != SCR_SUCCESS) {
        scr_abort(-1, "Failed to read filename for file %d in XOR file header %s @ %s:%d",
                  i, full_chunk_filename, __FILE__, __LINE__
        );
      }

      /* create full path to the file */
      char full_file[SCR_MAX_FILENAME];
      scr_build_path(full_file, sizeof(full_file), path, filename);

      /* copy the full filename */
      filenames[i] = strdup(full_file);
      if (filenames[i] == NULL) {
        scr_abort(-1, "Failed to copy filename during rebuild @ %s:%d",
                  full_file, __FILE__, __LINE__
        );
      }

      /* lookup the filesize */
      if (scr_meta_get_filesize(meta_tmp, &filesizes[i]) != SCR_SUCCESS) {
        scr_abort(-1, "Failed to read file size for file %s in XOR file header during rebuild @ %s:%d",
                  full_file, __FILE__, __LINE__
        );
      }

      /* open the file for reading */
      fds[i] = scr_open(full_file, O_RDONLY);
      if (fds[i] < 0) {
        /* TODO: try again? */
        scr_abort(-1, "Opening checkpoint file for reading in XOR rebuild: scr_open(%s, O_RDONLY) errno=%d %m @ %s:%d",
                  full_file, errno, __FILE__, __LINE__
        );
      }
    }

    /* if failed rank is to my left, i have the meta for his files, send him the header */
    if (root == c->lhs_rank) {
      scr_hash_send(header, c->lhs_rank, c->comm);
    }

    /* if failed rank is to my right, send him my file info so he can write his XOR header */
    if (root == c->rhs_rank) {
      scr_hash_send(current_hash, c->rhs_rank, c->comm);
    }
  } else {
    /* receive the header from right-side partner;
     * includes number of files and meta data for my files, as well as, 
     * the checkpoint id and the chunk size */
    scr_hash_recv(header, c->rhs_rank, c->comm);

    /* rename PARTNER to CURRENT in our header */
    current_hash = scr_hash_new();
    scr_hash* old_hash = scr_hash_get(header, SCR_KEY_COPY_XOR_PARTNER);
    scr_hash_merge(current_hash, old_hash);
    scr_hash_unset(header, SCR_KEY_COPY_XOR_CURRENT);
    scr_hash_unset(header, SCR_KEY_COPY_XOR_PARTNER);
    scr_hash_set(header, SCR_KEY_COPY_XOR_CURRENT, current_hash);

    /* receive number of files our left-side partner has and allocate an array of
     * meta structures to store info */
    scr_hash* partner_hash = scr_hash_new();
    scr_hash_recv(partner_hash, c->lhs_rank, c->comm);
    scr_hash_set(header, SCR_KEY_COPY_XOR_PARTNER, partner_hash);

    /* get the number of files */
    if (scr_hash_util_get_int(current_hash, SCR_KEY_COPY_XOR_FILES, &num_files) != SCR_SUCCESS) {
      scr_abort(-1, "Failed to read number of files from XOR file header during rebuild @ %s:%d",
              __FILE__, __LINE__
      );
    }
    if (num_files > 0) {
      fds       = (int*)           malloc(num_files * sizeof(int));
      filenames = (char**)         malloc(num_files * sizeof(char*));
      filesizes = (unsigned long*) malloc(num_files * sizeof(unsigned long));
      if (fds == NULL || filenames == NULL || filesizes == NULL) {
        scr_abort(-1, "Failed to allocate file arrays @ %s:%d",
                  __FILE__, __LINE__
        );
      }
    }

    /* set chunk filename of form:  <xor_rank+1>_of_<xorset_size>_in_<level_partion>x<xorset_size>.xor */
    char ckpt_dir[SCR_MAX_FILENAME];
    scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);
    sprintf(full_chunk_filename, "%s/%d_of_%d_in_%d.xor", ckpt_dir, c->my_rank+1, c->ranks, c->group_id);

    /* split file into path and name */
    scr_split_path(full_chunk_filename, path, name);

    /* record our chunk file and each of our checkpoint files in the filemap before creating */
    scr_filemap_add_file(map, checkpoint_id, scr_my_rank_world, full_chunk_filename);
    for (i=0; i < num_files; i++) {
      /* lookup meta data for this file from header hash */
      scr_hash* meta_tmp = scr_hash_get_kv_int(current_hash, SCR_KEY_COPY_XOR_FILE, i);
      if (meta_tmp == NULL) {
        scr_abort(-1, "Failed to find file %d in XOR file header %s @ %s:%d",
                  i, full_chunk_filename, __FILE__, __LINE__
        );
      }

      /* get pointer to filename for this file */
      char* filename = NULL;
      if (scr_meta_get_filename(meta_tmp, &filename) != SCR_SUCCESS) {
        scr_abort(-1, "Failed to read filename for file %d in XOR file header %s @ %s:%d",
                  i, full_chunk_filename, __FILE__, __LINE__
        );
      }

      /* get the filename */
      char full_file[SCR_MAX_FILENAME];
      scr_build_path(full_file, sizeof(full_file), path, filename);

      /* copy the filename */
      filenames[i] = strdup(full_file);
      if (filenames[i] == NULL) {
        scr_abort(-1, "Failed to copy filename during rebuild @ %s:%d",
                  full_file, __FILE__, __LINE__
        );
      }

      /* get the filesize */
      if (scr_meta_get_filesize(meta_tmp, &filesizes[i]) != SCR_SUCCESS) {
        scr_abort(-1, "Failed to read file size for file %s in XOR file header during rebuild @ %s:%d",
                  full_file, __FILE__, __LINE__
        );
      }

      /* add the file to our filemap */
      scr_filemap_add_file(map, checkpoint_id, scr_my_rank_world, full_file);
    }
    scr_filemap_set_expected_files(map, checkpoint_id, scr_my_rank_world, num_files + 1);
    scr_filemap_write(scr_map_file, map);

    /* open my chunk file for writing */
    fd_chunk = scr_open(full_chunk_filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd_chunk < 0) {
      /* TODO: try again? */
      scr_abort(-1, "Opening XOR chunk file for writing in XOR rebuild: scr_open(%s) errno=%d %m @ %s:%d",
                full_chunk_filename, errno, __FILE__, __LINE__
      );
    }

    /* open each of my files for writing */
    for (i=0; i < num_files; i++) {
      /* open my file for writing */
      fds[i] = scr_open(filenames[i], O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
      if (fds[i] < 0) {
        /* TODO: try again? */
        scr_abort(-1, "Opening checkpoint file for writing in XOR rebuild: scr_open(%s) errno=%d %m @ %s:%d",
                  filenames[i], errno, __FILE__, __LINE__
        );
      }
    }

    /* write XOR chunk file header */
    scr_hash_write_fd(full_chunk_filename, fd_chunk, header);
  }

  /* read the chunk size used to compute the xor data */
  unsigned long chunk_size;
  if (scr_hash_util_get_unsigned_long(header, SCR_KEY_COPY_XOR_CHUNK, &chunk_size) != SCR_SUCCESS) {
    scr_abort(-1, "Failed to read chunk size from XOR file header %s @ %s:%d",
            full_chunk_filename, __FILE__, __LINE__
    );
  }

  /* allocate buffer to read a piece of my file */
  char* send_buf = (char*) scr_align_malloc(scr_mpi_buf_size, scr_page_size);
  if (send_buf == NULL) {
    scr_abort(-1, "Allocating memory for send buffer: malloc(%d) errno=%d %m @ %s:%d",
            scr_mpi_buf_size, errno, __FILE__, __LINE__
    );
  }

  /* allocate buffer to read a piece of the recevied chunk file */
  char* recv_buf = (char*) scr_align_malloc(scr_mpi_buf_size, scr_page_size);
  if (recv_buf == NULL) {
    scr_abort(-1, "Allocating memory for recv buffer: malloc(%d) errno=%d %m @ %s:%d",
            scr_mpi_buf_size, errno, __FILE__, __LINE__
    );
  }

  /* Pipelined XOR Reduce to root */
  unsigned long offset = 0;
  int chunk_id;
  for (chunk_id = 0; chunk_id < c->ranks; chunk_id++) {
    size_t nread = 0;
    while (nread < chunk_size) {
      size_t count = chunk_size - nread;
      if (count > scr_mpi_buf_size) {
        count = scr_mpi_buf_size;
      }

      if (root != c->my_rank) {
        /* read the next set of bytes for this chunk from my file into send_buf */
        if (chunk_id != c->my_rank) {
          /* for this chunk, read data from the logical file */
          if (scr_read_pad_n(num_files, filenames, fds,
                             send_buf, count, offset, filesizes) != SCR_SUCCESS)
          {
            /* read failed, make sure we fail this rebuild */
            rc = SCR_FAILURE;
          }
          offset += count;
        } else {
          /* for this chunk, read data from the XOR file */
          if (scr_read_attempt(full_chunk_filename, fd_chunk, send_buf, count) != count) {
            /* read failed, make sure we fail this rebuild */
            rc = SCR_FAILURE;
          }
        }

        /* if not start of pipeline, receive data from left and xor with my own */
        if (root != c->lhs_rank) {
          int i;
          MPI_Recv(recv_buf, count, MPI_BYTE, c->lhs_rank, 0, c->comm, &status[0]);
          for (i = 0; i < count; i++) {
            send_buf[i] ^= recv_buf[i];
          }
        }

        /* send data to right-side partner */
        MPI_Send(send_buf, count, MPI_BYTE, c->rhs_rank, 0, c->comm);
      } else {
        /* root of rebuild, just receive incoming chunks and write them out */
        MPI_Recv(recv_buf, count, MPI_BYTE, c->lhs_rank, 0, c->comm, &status[0]);

        /* if this is not my xor chunk, write data to normal file, otherwise write to my xor chunk */
        if (chunk_id != c->my_rank) {
          /* for this chunk, write data to the logical file */
          if (scr_write_pad_n(num_files, filenames, fds,
                              recv_buf, count, offset, filesizes) != SCR_SUCCESS)
          {
            /* write failed, make sure we fail this rebuild */
            rc = SCR_FAILURE;
          }
          offset += count;
        } else {
          /* for this chunk, write data from the XOR file */
          if (scr_write_attempt(full_chunk_filename, fd_chunk, recv_buf, count) != count) {
            /* write failed, make sure we fail this rebuild */
            rc = SCR_FAILURE;
          }
        }
      }

      nread += count;
    }
  }

  /* close my chunkfile */
  if (scr_close(full_chunk_filename, fd_chunk) != SCR_SUCCESS) {
    rc = SCR_FAILURE;
  }

  /* close my checkpoint files */
  for (i=0; i < num_files; i++) {
    if (scr_close(filenames[i], fds[i]) != SCR_SUCCESS) {
      rc = SCR_FAILURE;
    }
  }

  /* if i'm the rebuild rank, truncate my file, and complete my file and xor chunk */
  if (root == c->my_rank) {
    /* complete each of our files and mark each as complete */
    for (i=0; i < num_files; i++) {
      /* TODO: need to check for errors, check that file is really valid */

      /* fill out meta info for our file and complete it */
      scr_hash* meta_tmp = scr_hash_get_kv_int(current_hash, SCR_KEY_COPY_XOR_FILE, i);
      scr_complete(filenames[i], meta_tmp);

      /* if crc_on_copy is set, compute and store CRC32 value for each file */
      if (scr_crc_on_copy) {
        /* check for mismatches here, in case we failed to rebuild the file correctly */
        if (scr_compute_crc(filenames[i]) != SCR_SUCCESS) {
          scr_err("Failed to verify CRC32 after rebuild on file %s @ %s:%d",
                  filenames[i], __FILE__, __LINE__
          );

          /* make sure we fail this rebuild */
          rc = SCR_FAILURE;
        }
      }
    }

    /* create meta data for chunk and complete it */
    unsigned long full_chunk_filesize = scr_filesize(full_chunk_filename);
    scr_meta* meta_chunk = scr_meta_new();
    scr_meta_set(meta_chunk, full_chunk_filename, SCR_META_FILE_XOR, full_chunk_filesize, checkpoint_id, scr_my_rank_world, scr_ranks_world, 1);
    scr_complete(full_chunk_filename, meta_chunk);
    scr_meta_delete(meta_chunk);

    /* if crc_on_copy is set, compute and store CRC32 value for chunk file */
    if (scr_crc_on_copy) {
      /* TODO: would be nice to check for mismatches here, but we did not save this value in the partner XOR file */
      scr_compute_crc(full_chunk_filename);
    }
  }

  /* free the buffers */
  scr_align_free(recv_buf);
  scr_align_free(send_buf);
  if (filesizes != NULL) {
    free(filesizes);
    filesizes = NULL;
  }
  if (filenames != NULL) {
    /* free each of the filenames we strdup'd */
    for (i=0; i < num_files; i++) {
      if (filenames[i] != NULL) {
        free(filenames[i]);
        filenames[i] = NULL;
      }
    }
    free(filenames);
    filenames = NULL;
  }
  if (fds != NULL) {
    free(fds);
    fds = NULL;
  }
  scr_hash_delete(header);

  return rc;
}

/* given a checkpoint id, check whether files can be rebuilt via xor and execute the rebuild if needed */
static int scr_attempt_rebuild_xor(scr_filemap* map, const struct scr_ckptdesc* c, int checkpoint_id)
{
  /* check whether we have our files */
  int have_my_files = scr_bool_have_files(map, checkpoint_id, scr_my_rank_world);

  /* check whether we have our XOR file */
  char xor_file[SCR_MAX_FILENAME];
  if (!scr_bool_have_xor_file(map, checkpoint_id, xor_file)) {
    have_my_files = 0;
  }

  /* TODO: check whether each of the files listed in our xor file exists? */

  /* check whether I have my full checkpoint file, assume I don't */
  int need_rebuild = 1;
  if (have_my_files) {
    need_rebuild = 0;
  }

  /* count how many in my xor set need to rebuild */
  int total_rebuild;
  MPI_Allreduce(&need_rebuild, &total_rebuild, 1, MPI_INT, MPI_SUM, c->comm); 

  /* check whether all sets can rebuild, if not, bail out */
  int set_can_rebuild = (total_rebuild <= 1);
  if (!scr_alltrue(set_can_rebuild)) {
    if (scr_my_rank_world == 0) {
      scr_err("Cannot rebuild missing files @ %s:%d", __FILE__, __LINE__);
    }
    return SCR_FAILURE;
  }

  /* it's possible to rebuild; rebuild if we need to */
  int rc = SCR_SUCCESS;
  if (total_rebuild > 0) {
    /* someone in my set needs to rebuild, determine who */
    int tmp_rank = need_rebuild ? c->my_rank : -1;
    int rebuild_rank;
    MPI_Allreduce(&tmp_rank, &rebuild_rank, 1, MPI_INT, MPI_MAX, c->comm);

    /* rebuild */
    if (need_rebuild) {
      scr_dbg(1, "Rebuilding file from XOR segments");
    }
    rc = scr_rebuild_xor(map, c, checkpoint_id, rebuild_rank);
  }

  /* check whether all sets rebuilt ok */
  if (!scr_alltrue(rc == SCR_SUCCESS)) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "One or more processes failed to rebuild its files @ %s:%d", __FILE__, __LINE__);
    }
    return SCR_FAILURE;
  }

  return SCR_SUCCESS;
}

/* given a filemap, a checkpoint_id, and a rank, unlink those files and remove them from the map */
static int scr_unlink_rank(scr_filemap* map, int ckpt, int rank)
{
  /* delete each file and remove its metadata file */
  scr_hash_elem* file_elem;
  for (file_elem = scr_filemap_first_file(map, ckpt, rank);
       file_elem != NULL;
       file_elem = scr_hash_elem_next(file_elem))
  {
    char* file = scr_hash_elem_key(file_elem);
    if (file != NULL) {
      scr_dbg(2, "Delete file Checkpoint %d, Rank %d, File %s", ckpt, rank, file);

      /* delete the file */
      unlink(file);

      /* delete the meta file */
      scr_incomplete(file);

      /* remove the file from the map */
      scr_filemap_remove_file(map, ckpt, rank, file);
    }
  }

  /* unset the expected number of files for this rank */
  scr_filemap_unset_expected_files(map, ckpt, rank);

  /* write the new filemap to disk */
  scr_filemap_write(scr_map_file, map);

  return SCR_SUCCESS;
}

/* since on a restart we may end up with more or fewer ranks on a node than the previous run,
 * rely on the master to read in and distribute the filemap to other ranks on the node */
static int scr_scatter_filemaps(scr_filemap* my_map)
{
  /* allocate empty send hash */
  scr_hash* send_hash = scr_hash_new();

  /* if i'm the master on this node, read in all filemaps */
  if (scr_my_rank_local == 0) {
    /* create an empty filemap */
    scr_filemap* all_map = scr_filemap_new();

    /* read in the master map */
    scr_hash* hash = scr_hash_new();
    scr_hash_read(scr_master_map_file, hash);

    /* for each filemap listed in the master map */
    scr_hash_elem* elem;
    for (elem = scr_hash_elem_first(scr_hash_get(hash, "Filemap"));
         elem != NULL;
         elem = scr_hash_elem_next(elem))
    {
      /* get the filename of this filemap */
      char* file = scr_hash_elem_key(elem);

      /* read in the filemap */
      scr_filemap* tmp_map = scr_filemap_new();
      scr_filemap_read(file, tmp_map);

      /* merge it with the all_map */
      scr_filemap_merge(all_map, tmp_map);

      /* delete filemap */
      scr_filemap_delete(tmp_map);

      /* delete the file */
      unlink(file);
    }

    /* free the hash object */
    scr_hash_delete(hash);

    /* write out new local 0 filemap */
    if (scr_filemap_num_ranks(all_map) > 0) {
      scr_filemap_write(scr_map_file, all_map);
    }

    /* get global rank of each rank on this node */
    int* ranks = (int*) malloc(scr_ranks_local * sizeof(int));
    if (ranks == NULL) {
      scr_abort(-1,"Failed to allocate memory to record local rank list @ %s:%d",
         __FILE__, __LINE__
      );
    }
    MPI_Gather(&scr_my_rank_world, 1, MPI_INT, ranks, 1, MPI_INT, 0, scr_comm_local);

    /* for each rank on this node, send them their own file data if we have it */
    int i;
    for (i=0; i < scr_ranks_local; i++) {
      int rank = ranks[i];
      if (scr_filemap_have_rank(all_map, rank)) {
        /* extract the filemap for this rank */
        scr_filemap* tmp_map = scr_filemap_extract_rank(all_map, rank);

        /* get a reference to the hash object that we'll send to this rank,
         * and merge this filemap into it */
        scr_hash* tmp_hash = scr_hash_getf(send_hash, "%d", i);
        if (tmp_hash == NULL) {
          /* if we don't find an existing entry in the send_hash,
           * create an empty hash and insert it */
          scr_hash* empty_hash = scr_hash_new();
          scr_hash_setf(send_hash, empty_hash, "%d", i);
          tmp_hash = empty_hash;
        }
        scr_hash_merge(tmp_hash, tmp_map);

        /* delete the filemap for this rank */
        scr_filemap_delete(tmp_map);
      }
    }

    /* free our rank list */
    if (ranks != NULL) {
      free(ranks);
      ranks = NULL;
    }

    /* now just round robin the remainder across the set (load balancing) */
    int num;
    int* remaining_ranks = NULL;
    scr_filemap_list_ranks(all_map, &num, &remaining_ranks);

    int j = 0;
    while (j < num) {
      /* pick a rank in scr_ranks_local to send to */
      i = j % scr_ranks_local;

      /* extract the filemap for this rank */
      scr_filemap* tmp_map = scr_filemap_extract_rank(all_map, remaining_ranks[j]);

      /* get a reference to the hash object that we'll send to this rank,
       * and merge this filemap into it */
      scr_hash* tmp_hash = scr_hash_getf(send_hash, "%d", i);
      if (tmp_hash == NULL) {
        /* if we don't find an existing entry in the send_hash,
         * create an empty hash and insert it */
        scr_hash* empty_hash = scr_hash_new();
        scr_hash_setf(send_hash, empty_hash, "%d", i);
        tmp_hash = empty_hash;
      }
      scr_hash_merge(tmp_hash, tmp_map);

      /* delete the filemap for this rank */
      scr_filemap_delete(tmp_map);
      j++;
    }

    if (remaining_ranks != NULL) {
      free(remaining_ranks);
      remaining_ranks = NULL;
    }

    /* delete the filemap */
    scr_filemap_delete(all_map);

    /* write out the new master filemap */
    hash = scr_hash_new();
    char file[SCR_MAX_FILENAME];
    for (i=0; i < scr_ranks_local; i++) {
      sprintf(file, "%s/filemap_%d.scrinfo", scr_cntl_prefix, i);
      scr_hash_set_kv(hash, "Filemap", file);
    }
    scr_hash_write(scr_master_map_file, hash);
    scr_hash_delete(hash);
  } else {
    /* send our global rank to the master */
    MPI_Gather(&scr_my_rank_world, 1, MPI_INT, NULL, 1, MPI_INT, 0, scr_comm_local);
  }

  /* receive our filemap from master */
  scr_hash* recv_hash = scr_hash_new();
  scr_hash_exchange(send_hash, recv_hash, scr_comm_local);

  /* merge map sent from master into our map */
  scr_hash* map_from_master = scr_hash_getf(recv_hash, "%d", 0);
  if (map_from_master != NULL) {
    scr_hash_merge(my_map, map_from_master);
  }

  /* write out our local filemap */
  if (scr_filemap_num_ranks(my_map) > 0) {
    scr_filemap_write(scr_map_file, my_map);
  }

  /* free off our send and receive hashes */
  scr_hash_delete(recv_hash);
  scr_hash_delete(send_hash);

  return SCR_SUCCESS;
}

/* this transfers checkpoint descriptors for the given checkpoint id */
static int scr_distribute_ckptdescs(scr_filemap* map, int checkpoint_id, struct scr_ckptdesc* c)
{
  int i;

  /* create a new hash and copy our checkpoint descriptors to it */
  scr_hash* send_hash = scr_hash_new();

  /* for this checkpoint, get list of ranks we have data for */
  int  nranks = 0;
  int* ranks = NULL;
  scr_filemap_list_ranks_by_checkpoint(map, checkpoint_id, &nranks, &ranks);

  /* for each rank we have files for, check whether we also have its checkpoint descriptor */
  int invalid_rank_found = 0;
  for (i=0; i < nranks; i++) {
    /* get the rank id */
    int rank = ranks[i];

    /* check that the rank is within range */
    if (rank < 0 || rank >= scr_ranks_world) {
      scr_err("Invalid rank id %d in world of %d @ %s:%d",
         rank, scr_ranks_world, __FILE__, __LINE__
      );
      invalid_rank_found = 1;
    }

    /* lookup the checkpoint descriptor hash for this rank */
    scr_hash* desc = scr_hash_new();
    scr_filemap_get_desc(map, checkpoint_id, rank, desc);

    /* if this descriptor has entries, add it to our send hash, delete the hash otherwise */
    if (scr_hash_size(desc) > 0) {
      scr_hash_setf(send_hash, desc, "%d", rank);
    } else {
      scr_hash_delete(desc);
    }
  }

  /* free off our list of ranks */
  if (ranks != NULL) {
    free(ranks);
    ranks = NULL;
  }

  /* check that we didn't find an invalid rank on any process */
  if (!scr_alltrue(invalid_rank_found == 0)) {
    scr_hash_delete(send_hash);
    return SCR_FAILURE;
  }

  /* create an empty hash to receive any incoming descriptors */
  /* exchange descriptors with other ranks */
  scr_hash* recv_hash = scr_hash_new();
  scr_hash_exchange(send_hash, recv_hash, scr_comm_world);

  /* check that everyone can get their descriptor */
  int num_desc = scr_hash_size(recv_hash);
  if (!scr_alltrue(num_desc > 0)) {
    scr_hash_delete(recv_hash);
    scr_hash_delete(send_hash);
    scr_dbg(2, "Cannot find process that has my checkpoint descriptor @ %s:%d", __FILE__, __LINE__);
    return SCR_FAILURE;
  }

  /* just go with the first checkpoint descriptor in our list -- they should all be the same */
  scr_hash_elem* desc_elem = scr_hash_elem_first(recv_hash);
  scr_hash* desc_hash = scr_hash_elem_hash(desc_elem);

  /* record the descriptor in our filemap */
  scr_filemap_set_desc(map, checkpoint_id, scr_my_rank_world, desc_hash);
  scr_filemap_write(scr_map_file, map);

  /* TODO: at this point, we could delete descriptors for other ranks for this checkpoint */

  /* read our checkpoint descriptor from the map */
  scr_ckptdesc_create_from_filemap(map, checkpoint_id, scr_my_rank_world, c);

  /* free off our send and receive hashes */
  scr_hash_delete(recv_hash);
  scr_hash_delete(send_hash);

  return SCR_SUCCESS;
}

/* this moves all files in the cache to make them accessible to new rank mapping */
static int scr_distribute_files(scr_filemap* map, const struct scr_ckptdesc* c, int checkpoint_id)
{
  int i, round;
  int rc = SCR_SUCCESS;

  /* clean out any incomplete files before we start */
  scr_clean_files(map);

  /* for this checkpoint, get list of ranks we have data for */
  int  nranks = 0;
  int* ranks = NULL;
  scr_filemap_list_ranks_by_checkpoint(map, checkpoint_id, &nranks, &ranks);

  /* walk backwards through the list of ranks, and set our start index to the rank which
   * is the first rank that is equal to or higher than our own rank -- when we assign round
   * ids below, this offsetting helps distribute the load */
  int start_index = 0;
  int invalid_rank_found = 0;
  for (i = nranks-1; i >= 0; i--) {
    int rank = ranks[i];

    /* pick the first rank whose rank id is equal to or higher than our own */
    if (rank >= scr_my_rank_world) {
      start_index = i;
    }

    /* while we're at it, check that the rank is within range */
    if (rank < 0 || rank >= scr_ranks_world) {
      scr_err("Invalid rank id %d in world of %d @ %s:%d",
         rank, scr_ranks_world, __FILE__, __LINE__
      );
      invalid_rank_found = 1;
    }
  }

  /* check that we didn't find an invalid rank on any process */
  if (!scr_alltrue(invalid_rank_found == 0)) {
    if (ranks != NULL) {
      free(ranks);
      ranks = NULL;
    }
    return SCR_FAILURE;
  }

  /* allocate array to record the rank we can send to in each round */
  int* have_rank_by_round = (int*) malloc(sizeof(int) * nranks);
  int* send_flag_by_round = (int*) malloc(sizeof(int) * nranks);
  if (have_rank_by_round == NULL || send_flag_by_round == NULL) {
    scr_abort(-1,"Failed to allocate memory to record rank id by round @ %s:%d",
       __FILE__, __LINE__
    );
  }

  /* check that we have all of the files for each rank, and determine the round we can send them */
  scr_hash* send_hash = scr_hash_new();
  scr_hash* recv_hash = scr_hash_new();
  for (round = 0; round < nranks; round++) {
    /* get the rank id */
    int index = (start_index + round) % nranks;
    int rank = ranks[index];

    /* record the rank indexed by the round number */
    have_rank_by_round[round] = rank;

    /* assume we won't be sending to this rank in this round */
    send_flag_by_round[round] = 0;

    /* if we have files for this rank, specify the round we can send those files in */
    if (scr_bool_have_files(map, checkpoint_id, rank)) {
      scr_hash_setf(send_hash, NULL, "%d %d", rank, round);
    }
  }
  scr_hash_exchange(send_hash, recv_hash, scr_comm_world);

  /* search for the minimum round we can get our files */
  int retrieve_rank  = -1;
  int retrieve_round = -1;
  scr_hash_elem* elem = NULL;
  for (elem = scr_hash_elem_first(recv_hash);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get the rank id */
    int rank = scr_hash_elem_key_int(elem);

    /* get the round id */
    scr_hash* round_hash = scr_hash_elem_hash(elem);
    scr_hash_elem* round_elem = scr_hash_elem_first(round_hash);
    char* round_str = scr_hash_elem_key(round_elem);
    int round = atoi(round_str);

    /* record this round and rank number of it's less than the current round */
    if (round < retrieve_round || retrieve_round == -1) {
      retrieve_round = round;
      retrieve_rank  = rank;
    }
  }

  /* done with the round hashes, free them off */
  scr_hash_delete(recv_hash);
  scr_hash_delete(send_hash);

  /* free off our list of ranks */
  if (ranks != NULL) {
    free(ranks);
    ranks = NULL;
  }

  /* for some redundancy schemes, we know at this point whether we can recover all files */
  int can_get_files = (retrieve_rank != -1);
  if (c->copy_type != SCR_COPY_XOR && !scr_alltrue(can_get_files)) {
    /* print a debug message indicating which rank is missing files */
    if (!can_get_files) {
      scr_dbg(2, "Cannot find process that has my checkpoint files @ %s:%d", __FILE__, __LINE__);
    }
    return SCR_FAILURE;
  }

  /* get the maximum retrieve round */
  int max_rounds = 0;
  MPI_Allreduce(&retrieve_round, &max_rounds, 1, MPI_INT, MPI_MAX, scr_comm_world);

  /* tell destination which round we'll take our files in */
  send_hash = scr_hash_new();
  recv_hash = scr_hash_new();
  if (retrieve_rank != -1) {
    scr_hash_setf(send_hash, NULL, "%d %d", retrieve_rank, retrieve_round);
  }
  scr_hash_exchange(send_hash, recv_hash, scr_comm_world);

  /* determine which ranks want to fetch their files from us */
  for(elem = scr_hash_elem_first(recv_hash);
      elem != NULL;
      elem = scr_hash_elem_next(elem))
  {
    /* get the round id */
    scr_hash* round_hash = scr_hash_elem_hash(elem);
    scr_hash_elem* round_elem = scr_hash_elem_first(round_hash);
    char* round_str = scr_hash_elem_key(round_elem);
    int round = atoi(round_str);

    /* record whether this rank wants its files from us */
    if (round >= 0 && round < nranks) {
      send_flag_by_round[round] = 1;
    }
  }

  /* done with the round hashes, free them off */
  scr_hash_delete(recv_hash);
  scr_hash_delete(send_hash);

  int tmp_rc = 0;

  /* get the path for this checkpoint */
  char ckpt_dir[SCR_MAX_FILENAME];
  scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);

  /* run through rounds and exchange files */
  for (round = 0; round <= max_rounds; round++) {
    /* assume we don't need to send or receive any files this round */
    int send_rank = MPI_PROC_NULL;
    int recv_rank = MPI_PROC_NULL;
    int send_num  = 0;
    int recv_num  = 0;

    /* check whether I can potentially send to anyone in this round */
    if (round < nranks) {
      /* have someone's files, check whether they are asking for them this round */
      if (send_flag_by_round[round]) {
        /* need to send files this round, remember to whom and how many */
        int dst_rank = have_rank_by_round[round];
        send_rank = dst_rank;
        send_num  = scr_filemap_num_files(map, checkpoint_id, dst_rank);
      }
    }

    /* if I'm supposed to get my files this round, set the recv_rank */
    if (retrieve_round == round) {
      recv_rank = retrieve_rank;
    }

    /* TODO: another special case is to just move files if the processes are on the same node */

    /* if i'm sending to myself, just move (rename) each file */
    if (send_rank == scr_my_rank_world) {
      /* get our file list */
      int numfiles = 0;
      char** files = NULL;
      scr_filemap_list_files(map, checkpoint_id, send_rank, &numfiles, &files);

      /* iterate over and rename each file */
      for (i=0; i < numfiles; i++) {
        /* get the existing filename and split into path and name components */
        char* file = files[i];
        char path[SCR_MAX_FILENAME];
        char name[SCR_MAX_FILENAME];
        scr_split_path(file, path, name);

        /* build the new filename */
        char newfile[SCR_MAX_FILENAME];
        scr_build_path(newfile, sizeof(newfile), ckpt_dir, name);

        /* build the name of the existing and new meta files */
        char metafile[SCR_MAX_FILENAME];
        char newmetafile[SCR_MAX_FILENAME];
        scr_meta_name(metafile,    file);
        scr_meta_name(newmetafile, newfile);

        /* if the new file name is different from the old name, rename it */
        if (strcmp(file, newfile) != 0) {
          /* record the new filename to our map and write it to disk */
          scr_filemap_add_file(map, checkpoint_id, send_rank, newfile);
          scr_filemap_write(scr_map_file, map);

          /* rename the file */
          scr_dbg(2, "Round %d: rename(%s, %s)", round, file, newfile);
          tmp_rc = rename(file, newfile);
          if (tmp_rc != 0) {
            /* TODO: to cross mount points, if tmp_rc == EXDEV, open new file, copy, and delete orig */
            scr_err("Moving checkpoint file: rename(%s, %s) %m errno=%d @ %s:%d",
                    file, newfile, errno, __FILE__, __LINE__
            );
            rc = SCR_FAILURE;
          }

          /* rename the meta file */
          scr_dbg(2, "rename(%s, %s)", metafile, newmetafile);
          tmp_rc = rename(metafile, newmetafile);
          if (tmp_rc != 0) {
            /* TODO: to cross mount points, if tmp_rc == EXDEV, open new file, copy, and delete orig */
            scr_err("Moving checkpoint file: rename(%s, %s) %m errno=%d @ %s:%d",
                    metafile, newmetafile, errno, __FILE__, __LINE__
            );
            rc = SCR_FAILURE;
          }

          /* remove the old name from the filemap and write it to disk */
          scr_filemap_remove_file(map, checkpoint_id, send_rank, file);
          scr_filemap_write(scr_map_file, map);
        }
      }

      /* free the list of filename pointers */
      if (files != NULL) {
        free(files);
        files = NULL;
      }
    } else {
      /* if we have files for this round, but the correspdonding rank doesn't need them, delete the files */
      if (round < nranks && send_rank == MPI_PROC_NULL) {
        int dst_rank = have_rank_by_round[round];
        scr_unlink_rank(map, checkpoint_id, dst_rank);
      }

      /* sending to and/or recieving from another node */
      if (send_rank != MPI_PROC_NULL || recv_rank != MPI_PROC_NULL) {
        /* remember the send rank for later, we'll set it to -1 eventually */
        int filemap_send_rank = send_rank;

        /* have someone to send to or receive from */
        int have_outgoing = 0;
        int have_incoming = 0;
        if (send_rank != MPI_PROC_NULL) {
          have_outgoing = 1;
        }
        if (recv_rank != MPI_PROC_NULL) {
          have_incoming = 1;
        }

        /* first, determine how many files I will be receiving and tell how many I will be sending */
        MPI_Request request[2];
        MPI_Status  status[2];
        int num_req = 0;
        if (have_incoming) {
          MPI_Irecv(&recv_num, 1, MPI_INT, recv_rank, 0, scr_comm_world, &request[num_req]);
          num_req++;
        }
        if (have_outgoing) {
          MPI_Isend(&send_num, 1, MPI_INT, send_rank, 0, scr_comm_world, &request[num_req]);
          num_req++;
        }
        if (num_req > 0) {
          MPI_Waitall(num_req, request, status);
        }

        /* record how many files I will receive (need to distinguish between 0 files and not knowing) */
        if (have_incoming) {
          scr_filemap_set_expected_files(map, checkpoint_id, scr_my_rank_world, recv_num);
        }

        /* turn off send or receive flags if the file count is 0, nothing else to do */
        if (send_num == 0) {
          have_outgoing = 0;
          send_rank = MPI_PROC_NULL;
        }
        if (recv_num == 0) {
          have_incoming = 0;
          recv_rank = MPI_PROC_NULL;
        }

        /* get our file list for the destination */
        int numfiles = 0;
        char** files = NULL;
        if (have_outgoing) {
          scr_filemap_list_files(map, checkpoint_id, send_rank, &numfiles, &files);
        }

        /* while we have a file to send or receive ... */
        while (have_incoming || have_outgoing) {
          /* get the filename */
          char* file = NULL;
          if (have_outgoing) {
            file = files[numfiles - send_num];
          }

          /* exhange file names with partners */
          char file_partner[SCR_MAX_FILENAME];
          scr_swap_file_names(file, send_rank, file_partner, sizeof(file_partner), recv_rank,
                              ckpt_dir, scr_comm_world
          );

          /* if we'll receive a file, record the name of our file in the filemap and write it to disk */
          if (recv_rank != MPI_PROC_NULL) {
            scr_filemap_add_file(map, checkpoint_id, scr_my_rank_world, file_partner);
            scr_filemap_write(scr_map_file, map);
          }

          /* either sending or receiving a file this round, since we move files,
           * it will be deleted or overwritten */
          char newfile[SCR_MAX_FILENAME];
          if (scr_swap_files(MOVE_FILES, file, send_rank, file_partner, recv_rank, scr_comm_world)
                != SCR_SUCCESS)
          {
            scr_err("Swapping checkpoint files: %s to %d, %s from %d @ %s:%d",
                    file, send_rank, file_partner, recv_rank, __FILE__, __LINE__
            );
            rc = SCR_FAILURE;
          }

          /* if we sent a file, remove its name from the filemap and write it to disk */
          if (send_rank != MPI_PROC_NULL) {
            scr_filemap_remove_file(map, checkpoint_id, send_rank, file);
            scr_filemap_write(scr_map_file, map);
          }

          /* if we received a file, decrement receive count */
          if (have_incoming) {
            recv_num--;
            if (recv_num == 0) {
              have_incoming = 0;
              recv_rank = MPI_PROC_NULL;
            }
          }

          /* if we sent a file, get the next filename and decrement our send count */
          if (have_outgoing) {
            send_num--;
            if (send_num == 0) {
              have_outgoing = 0;
              send_rank = MPI_PROC_NULL;
            }
          }
        }

        /* free our file list */
        if (files != NULL) {
          free(files);
          files = NULL;
        }

        /* if we sent to someone, remove those files from the filemap */
        if (filemap_send_rank != MPI_PROC_NULL) {
          scr_filemap_remove_rank_by_checkpoint(map, checkpoint_id, filemap_send_rank);
          scr_filemap_write(scr_map_file, map);
        }
      }
    }
  }

  /* if we have more rounds than max rounds, delete the remainder of our files */
  for (round = max_rounds+1; round < nranks; round++) {
    /* have someone's files for this round, so delete them */
    int dst_rank = have_rank_by_round[round];
    scr_unlink_rank(map, checkpoint_id, dst_rank);
  }

  if (send_flag_by_round != NULL) {
    free(send_flag_by_round);
    send_flag_by_round = NULL;
  }
  if (have_rank_by_round != NULL) {
    free(have_rank_by_round);
    have_rank_by_round = NULL;
  }

  /* write out new filemap and free the memory resources */
  scr_filemap_write(scr_map_file, map);

  /* clean out any incomplete files */
  scr_clean_files(map);

  /* TODO: if the exchange or redundancy rebuild failed, we should also delete any *good* files we received */

  /* return whether distribute succeeded, it does not ensure we have all of our files,
   * only that the transfer completed without failure */
  return rc;
}

int scr_rebuild_files(scr_filemap* map, const struct scr_ckptdesc* c, int checkpoint_id)
{
  int rc = SCR_SUCCESS;

  /* for xor, need to call rebuild_xor here */
  if (c->copy_type == SCR_COPY_XOR) {
    rc = scr_attempt_rebuild_xor(map, c, checkpoint_id);
  }

  /* check that rebuild worked */
  if (rc != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "Missing checkpoints files @ %s:%d",
              __FILE__, __LINE__
      );
    }
    return SCR_FAILURE;
  }

  /* at this point, we should have all of our files, check that they're all here */

  /* check whether everyone has their files */
  int have_my_files = scr_bool_have_files(map, checkpoint_id, scr_my_rank_world);
  if (!scr_alltrue(have_my_files)) {
    if (scr_my_rank_world == 0) {
      scr_dbg(1, "Missing checkpoints files @ %s:%d",
              __FILE__, __LINE__
      );
    }
    return SCR_FAILURE;
  }

  /* for LOCAL and PARTNER, we need to apply the copy to complete the rebuild */
  if (c->copy_type == SCR_COPY_LOCAL || c->copy_type == SCR_COPY_PARTNER) {
    double bytes_copied = 0.0;
    rc = scr_copy_files(map, c, checkpoint_id, &bytes_copied);
  }

  return rc;
}

/* given a filename, return the full path to the file which the user should write to */
static int scr_route_file(int checkpoint_id, const char* file, char* newfile, int n)
{
  /* check that we got a file and newfile to write to */
  if (file == NULL || strcmp(file, "") == 0 || newfile == NULL) {
    return SCR_FAILURE;
  }

  /* check that user's filename is not too long */
  if (strlen(file) >= SCR_MAX_FILENAME) {
    scr_abort(-1, "file name (%s) is longer than SCR_MAX_FILENAME (%d) @ %s:%d",
              file, SCR_MAX_FILENAME, __FILE__, __LINE__
    );
  }

  /* split user's filename into path and name components */
  char path[SCR_MAX_FILENAME];
  char name[SCR_MAX_FILENAME];
  scr_split_path(file, path, name);

  /* lookup the checkpoint directory */
  char ckpt_dir[SCR_MAX_FILENAME];
  struct scr_ckptdesc* c = scr_ckptdesc_get(checkpoint_id, scr_nckptdescs, scr_ckptdescs);
  scr_checkpoint_dir(c, checkpoint_id, ckpt_dir);

  /* build the composed name */
  if (scr_build_path(newfile, n, ckpt_dir, name) != SCR_SUCCESS) {
    /* abort if the new name is longer than our buffer */
    scr_abort(-1, "file name (%s/%s) is longer than n (%d) @ %s:%d",
              ckpt_dir, name, n, __FILE__, __LINE__
    );
  }

  return SCR_SUCCESS;
}

/* read in environment variables */
static int scr_get_params()
{
  char* value;
  scr_hash* tmp;
  double d;
  unsigned long long ull;

  /* user may want to disable SCR at runtime, read env var to avoid reading config files */
  if ((value = getenv("SCR_ENABLE")) != NULL) {
    scr_enabled = atoi(value);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* read in our configuration parameters */
  scr_param_init();

  /* check enabled parameter again, this time including settings from config files */
  if ((value = scr_param_get("SCR_ENABLE")) != NULL) {
    scr_enabled = atoi(value);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    scr_param_finalize();
    return SCR_FAILURE;
  }

  /* set debug verbosity level */
  if ((value = scr_param_get("SCR_DEBUG")) != NULL) {
    scr_debug = atoi(value);
  }

  /* set logging */
  if ((value = scr_param_get("SCR_LOG_ENABLE")) != NULL) {
    scr_log_enable = atoi(value);
  }

  /* read username from SCR_USER_NAME, if not set, try USER from environment */
  if ((value = scr_param_get("SCR_USER_NAME")) != NULL) {
    scr_username = strdup(value);
    if (scr_username == NULL) {
      scr_abort(-1, "Failed to allocate memory to record username (%s) @ %s:%d",
              value, __FILE__, __LINE__
      );
    }
  } else if ((value = getenv("USER")) != NULL) {
    scr_username = strdup(value);
    if (scr_username == NULL) {
      scr_abort(-1, "Failed to allocate memory to record username (%s) @ %s:%d",
              value, __FILE__, __LINE__
      );
    }
  }

  /* read jobid from SCR_JOB_ID, if not set, try SLURM_JOBID from environment */
  if ((value = scr_param_get("SCR_JOB_ID")) != NULL) {
    scr_jobid = strdup(value);
    if (scr_jobid == NULL) {
      scr_abort(-1, "Failed to allocate memory to record jobid (%s) @ %s:%d",
              value, __FILE__, __LINE__
      );
    }
  } else if ((value = getenv("SLURM_JOBID")) != NULL) {
    scr_jobid = strdup(value);
    if (scr_jobid == NULL) {
      scr_abort(-1, "Failed to allocate memory to record jobid (%s) @ %s:%d",
              value, __FILE__, __LINE__
      );
    }
  }

  /* read job name from SCR_JOB_NAME */
  if ((value = scr_param_get("SCR_JOB_NAME")) != NULL) {
    scr_jobname = strdup(value);
    if (scr_jobname == NULL) {
      scr_abort(-1, "Failed to allocate memory to record jobname (%s) @ %s:%d",
              value, __FILE__, __LINE__
      );
    }
  }

  /* override default base control directory */
  if ((value = scr_param_get("SCR_CNTL_BASE")) != NULL) {
    strcpy(scr_cntl_base, value);
  }

  /* override default base directory for checkpoint cache */
  if ((value = scr_param_get("SCR_CACHE_BASE")) != NULL) {
    strcpy(scr_cache_base, value);
  }

  /* set maximum number of checkpoints to keep in cache */
  if ((value = scr_param_get("SCR_CACHE_SIZE")) != NULL) {
    scr_cache_size = atoi(value);
  }

  /* fill in a hash of cache descriptors */
  scr_cachedesc_hash = scr_hash_new();
  tmp = scr_param_get_hash(SCR_CONFIG_KEY_CACHEDESC);
  if (tmp != NULL) {
    scr_hash_set(scr_cachedesc_hash, SCR_CONFIG_KEY_CACHEDESC, tmp);
  } else {
    /* fill in info for one CACHE type */
    tmp = scr_hash_set_kv(scr_cachedesc_hash, SCR_CONFIG_KEY_CACHEDESC, "0");
    scr_hash_setf(tmp, NULL, "%s %s", SCR_CONFIG_KEY_BASE, scr_cache_base);
    scr_hash_setf(tmp, NULL, "%s %d", SCR_CONFIG_KEY_SIZE, scr_cache_size);
  }
  
  /* select copy method */
  if ((value = scr_param_get("SCR_COPY_TYPE")) != NULL) {
    if (strcasecmp(value, "local") == 0) {
      scr_copy_type = SCR_COPY_LOCAL;
    } else if (strcasecmp(value, "partner") == 0) {
      scr_copy_type = SCR_COPY_PARTNER;
    } else if (strcasecmp(value, "xor") == 0) {
      scr_copy_type = SCR_COPY_XOR;
    } else {
      scr_copy_type = SCR_COPY_FILE;
    }
  }

  /* specify the number of tasks in xor set */
  if ((value = scr_param_get("SCR_SET_SIZE")) != NULL) {
    scr_set_size = atoi(value);
  }

  /* number of nodes between partners */
  if ((value = scr_param_get("SCR_HOP_DISTANCE")) != NULL) {
    scr_hop_distance = atoi(value);
  }

  /* fill in a hash of checkpoint descriptors */
  scr_ckptdesc_hash = scr_hash_new();
  if (scr_copy_type == SCR_COPY_LOCAL) {
    /* fill in info for one LOCAL checkpoint */
    tmp = scr_hash_set_kv(scr_ckptdesc_hash, SCR_CONFIG_KEY_CKPTDESC, "0");
    scr_hash_setf(tmp, NULL, "%s %s", SCR_CONFIG_KEY_BASE,            scr_cache_base);
    scr_hash_setf(tmp, NULL, "%s %s", SCR_CONFIG_KEY_TYPE,            "LOCAL");
  } else if (scr_copy_type == SCR_COPY_PARTNER) {
    /* fill in info for one PARTNER checkpoint */
    tmp = scr_hash_set_kv(scr_ckptdesc_hash, SCR_CONFIG_KEY_CKPTDESC, "0");
    scr_hash_setf(tmp, NULL, "%s %s", SCR_CONFIG_KEY_BASE,            scr_cache_base);
    scr_hash_setf(tmp, NULL, "%s %s", SCR_CONFIG_KEY_TYPE,            "PARTNER");
    scr_hash_setf(tmp, NULL, "%s %d", SCR_CONFIG_KEY_HOP_DISTANCE,    scr_hop_distance);
  } else if (scr_copy_type == SCR_COPY_XOR) {
    /* fill in info for one XOR checkpoint */
    tmp = scr_hash_set_kv(scr_ckptdesc_hash, SCR_CONFIG_KEY_CKPTDESC, "0");
    scr_hash_setf(tmp, NULL, "%s %s", SCR_CONFIG_KEY_BASE,            scr_cache_base);
    scr_hash_setf(tmp, NULL, "%s %s", SCR_CONFIG_KEY_TYPE,            "XOR");
    scr_hash_setf(tmp, NULL, "%s %d", SCR_CONFIG_KEY_HOP_DISTANCE,    scr_hop_distance);
    scr_hash_setf(tmp, NULL, "%s %d", SCR_CONFIG_KEY_SET_SIZE,        scr_set_size);
  } else {
    /* read info from our configuration files */
    tmp = scr_param_get_hash(SCR_CONFIG_KEY_CKPTDESC);
    if (tmp != NULL) {
      scr_hash_set(scr_ckptdesc_hash, SCR_CONFIG_KEY_CKPTDESC, tmp);
    } else {
      scr_abort(-1, "Failed to define checkpoints @ %s:%d",
              __FILE__, __LINE__
      );
    }
  }

  /* if job has fewer than SCR_HALT_SECONDS remaining after completing a checkpoint, halt it */
  if ((value = scr_param_get("SCR_HALT_SECONDS")) != NULL) {
    scr_halt_seconds = atoi(value);
  }

  /* set MPI buffer size (file chunk size) */
  if ((value = scr_param_get("SCR_MPI_BUF_SIZE")) != NULL) {
    if (scr_abtoull(value, &ull) == SCR_SUCCESS) {
      scr_mpi_buf_size = (size_t) ull;
    } else {
      scr_err("Failed to read SCR_MPI_BUF_SIZE successfully @ %s:%d", __FILE__, __LINE__);
    }
  }

  /* whether to distribute files in filemap to ranks in SCR_Init */
  if ((value = scr_param_get("SCR_DISTRIBUTE")) != NULL) {
    scr_distribute = atoi(value);
  }

  /* whether to fetch files from the parallel file system in SCR_Init */
  if ((value = scr_param_get("SCR_FETCH")) != NULL) {
    scr_fetch = atoi(value);
  }

  /* specify number of processes to read files simultaneously */
  if ((value = scr_param_get("SCR_FETCH_WIDTH")) != NULL) {
    scr_fetch_width = atoi(value);
  }

  /* specify how often we should flush files */
  if ((value = scr_param_get("SCR_FLUSH")) != NULL) {
    scr_flush = atoi(value);
  }

  /* specify number of processes to write files simultaneously */
  if ((value = scr_param_get("SCR_FLUSH_WIDTH")) != NULL) {
    scr_flush_width = atoi(value);
  }

  /* specify whether to always flush latest checkpoint from cache on restart */
  if ((value = scr_param_get("SCR_FLUSH_ON_RESTART")) != NULL) {
    scr_flush_on_restart = atoi(value);
  }

  /* set to 1 if code must be restarted from the parallel file system */
  if ((value = scr_param_get("SCR_GLOBAL_RESTART")) != NULL) {
    scr_global_restart = atoi(value);
  }

  /* specify whether to use asynchronous flush */
  if ((value = scr_param_get("SCR_FLUSH_ASYNC")) != NULL) {
    scr_flush_async = atoi(value);
  }

  /* bandwidth limit imposed during async flush (in bytes/sec) */
  if ((value = scr_param_get("SCR_FLUSH_ASYNC_BW")) != NULL) {
    if (scr_atod(value, &d) == SCR_SUCCESS) {
      scr_flush_async_bw = d;
    } else {
      scr_err("Failed to read SCR_FLUSH_ASYNC_BW successfully @ %s:%d", __FILE__, __LINE__);
    }
  }

  /* bandwidth limit imposed during async flush (in bytes/sec) */
  if ((value = scr_param_get("SCR_FLUSH_ASYNC_PERCENT")) != NULL) {
    if (scr_atod(value, &d) == SCR_SUCCESS) {
      scr_flush_async_percent = d;
    } else {
      scr_err("Failed to read SCR_FLUSH_ASYNC_PERCENT successfully @ %s:%d", __FILE__, __LINE__);
    }
  }

  /* set file copy buffer size (file chunk size) */
  if ((value = scr_param_get("SCR_FILE_BUF_SIZE")) != NULL) {
    if (scr_abtoull(value, &ull) == SCR_SUCCESS) {
      scr_file_buf_size = (size_t) ull;
    } else {
      scr_err("Failed to read SCR_FILE_BUF_SIZE successfully @ %s:%d", __FILE__, __LINE__);
    }
  }

  /* specify whether to compute CRC on redundancy copy */
  if ((value = scr_param_get("SCR_CRC_ON_COPY")) != NULL) {
    scr_crc_on_copy = atoi(value);
  }

  /* specify whether to compute CRC on fetch and flush */
  if ((value = scr_param_get("SCR_CRC_ON_FLUSH")) != NULL) {
    scr_crc_on_flush = atoi(value);
  }

  /* specify whether to compute and check CRC when deleting a file */
  if ((value = scr_param_get("SCR_CRC_ON_DELETE")) != NULL) {
    scr_crc_on_delete = atoi(value);
  }

  /* override default checkpoint interval (number of times to call Need_checkpoint between checkpoints) */
  if ((value = scr_param_get("SCR_CHECKPOINT_INTERVAL")) != NULL) {
    scr_checkpoint_interval = atoi(value);
  }

  /* override default minimum number of seconds between checkpoints */
  if ((value = scr_param_get("SCR_CHECKPOINT_SECONDS")) != NULL) {
    scr_checkpoint_seconds = atoi(value);
  }

  /* override default maximum allowed checkpointing overhead */
  if ((value = scr_param_get("SCR_CHECKPOINT_OVERHEAD")) != NULL) {
    if (scr_atod(value, &d) == SCR_SUCCESS) {
      scr_checkpoint_overhead = d;
    } else {
      scr_err("Failed to read SCR_CHECKPOINT_OVERHEAD successfully @ %s:%d", __FILE__, __LINE__);
    }
  }

  /* override default scr_par_prefix (parallel file system prefix) */
  if ((value = scr_param_get("SCR_PREFIX")) != NULL) {
    strcpy(scr_par_prefix, value);
  }

  /* if user didn't set with SCR_PREFIX, pick up the current working directory as a default */
  /* TODO: wonder whether this convenience will cause more problems than its worth?
   * may lead to writing large checkpoint file sets to the executable directory, which may not be a parallel file system */
  if (strcmp(scr_par_prefix, "") == 0) {
    if (getcwd(scr_par_prefix, sizeof(scr_par_prefix)) == NULL) {
      scr_abort(-1, "Problem reading current working directory (getcwd() errno=%d %m) @ %s:%d",
              errno, __FILE__, __LINE__
      );
    }
  }

  /* connect to the SCR log database if enabled */
  /* NOTE: We do this inbetween our existing calls to scr_param_init and scr_param_finalize,
   * since scr_log_init itself calls param_init to read the db username and password from the
   * config file, which in turn requires a bcast.  However, only rank 0 calls scr_log_init(),
   * so the bcast would fail if scr_param_init really had to read the config file again. */
  if (scr_my_rank_world == 0 && scr_log_enable) {
    if (scr_log_init() != SCR_SUCCESS) {
      scr_err("Failed to initialize SCR logging, disabling logging @ %s:%d",
              __FILE__, __LINE__
      );
      scr_log_enable = 0;
    }
  }

  /* done reading parameters, can release the data structures now */
  scr_param_finalize();

  return SCR_SUCCESS;
}

/*
=========================================
User interface functions
=========================================
*/

int SCR_Init()
{
  int i;

  /* check whether user has disabled library via environment variable */
  char* value = NULL;
  if ((value = getenv("SCR_ENABLE")) != NULL) {
    scr_enabled = atoi(value);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* NOTE: SCR_ENABLE can also be set in a config file, but to read a config file,
   * we must at least create scr_comm_world and call scr_get_params() */

  /* create a context for the library */
  MPI_Comm_dup(MPI_COMM_WORLD, &scr_comm_world);

  /* find our rank and the size of our world */
  MPI_Comm_rank(scr_comm_world, &scr_my_rank_world);
  MPI_Comm_size(scr_comm_world, &scr_ranks_world);

  /* get my hostname */
  if (gethostname(scr_my_hostname, sizeof(scr_my_hostname)) != 0) {
    scr_err("Call to gethostname failed @ %s:%d",
            __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }

  /* get the page size (used to align communication buffers) */
  scr_page_size = getpagesize();
  if (scr_page_size <= 0) {
    scr_err("Call to getpagesize failed @ %s:%d",
            __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }

  /* read our configuration: environment variables, config file, etc. */
  scr_get_params();

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    /* we dup'd comm_world to broadcast parameters in scr_get_params,
     * need to free it here */
    MPI_Comm_free(&scr_comm_world);

    return SCR_FAILURE;
  }

  /* check that some required parameters are set */
  if (scr_username == NULL || scr_jobid == NULL) {
    scr_abort(-1,
              "Jobid or username is not set; you may need to manually set SCR_JOB_ID or SCR_USER_NAME @ %s:%d",
              __FILE__, __LINE__
    );
  }

  /* create a scr_comm_local communicator to hold all tasks on the same node */
#ifdef HAVE_LIBGCS
  /* determine the length of the maximum hostname (including terminating NULL character),
   * and check that our own buffer is at least as big */
  int my_hostname_len = strlen(scr_my_hostname) + 1;
  int max_hostname_len = 0;
  MPI_Allreduce(&my_hostname_len, &max_hostname_len, 1, MPI_INT, MPI_MAX, scr_comm_world);
  if (max_hostname_len > sizeof(scr_my_hostname)) {
    scr_err("Hostname is too long on some process @ %s:%d",
            __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }

  /* split ranks based on hostname */
  GCS_Comm_split(
      scr_comm_world,
      scr_my_hostname, max_hostname_len, GCS_CMP_STR,
      NULL, 0, GCS_IGNORE_KEY,
      &scr_comm_local
  );
#else /* HAVE_LIBGCS */
  /* TODO: maybe a better way to identify processes on the same node?
   * TODO: could improve scalability here using a parallel sort and prefix scan
   * TODO: need something to work on systems with IPv6
   * Assumes: same int(IP) ==> same node 
   *   1. Get IP address as integer data type
   *   2. Allgather IP addresses from all processes
   *   3. Set color id to process with highest rank having the same IP */

  /* get IP address as integer data type */
  struct hostent *hostent;
  hostent = gethostbyname(scr_my_hostname);
  if (hostent == NULL) {
    scr_err("Fetching host information: gethostbyname(%s) @ %s:%d",
            scr_my_hostname, __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }
  int host_id = (int) ((struct in_addr *) hostent->h_addr_list[0])->s_addr;

  /* gather all host_id values */
  int* host_ids = (int*) malloc(scr_ranks_world * sizeof(int));
  if (host_ids == NULL) {
    scr_err("Can't allocate memory to determine which processes are on the same node @ %s:%d",
            __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }
  MPI_Allgather(&host_id, 1, MPI_INT, host_ids, 1, MPI_INT, scr_comm_world);

  /* set host_index to the highest rank having the same host_id as we do */
  int host_index = 0;
  for (i=0; i < scr_ranks_world; i++) {
    if (host_ids[i] == host_id) {
      host_index = i;
    }
  }
  free(host_ids);

  /* finally create the communicator holding all ranks on the same node */
  MPI_Comm_split(scr_comm_world, host_index, scr_my_rank_world, &scr_comm_local);
#endif /* HAVE_LIBGCS */

  /* find our position in the local communicator */
  MPI_Comm_rank(scr_comm_local, &scr_my_rank_local);
  MPI_Comm_size(scr_comm_local, &scr_ranks_local);

  /* Based on my local rank, create communicators consisting of all tasks at same local rank level */
  MPI_Comm_split(scr_comm_world, scr_my_rank_local, scr_my_rank_world, &scr_comm_level);

  /* find our position in the level communicator */
  MPI_Comm_rank(scr_comm_level, &scr_my_rank_level);
  MPI_Comm_size(scr_comm_level, &scr_ranks_level);

  /* setup checkpoint descriptors */
  if (scr_ckptdesc_create_list() != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_err("Failed to prepare one or more checkpoint descriptors @ %s:%d",
              __FILE__, __LINE__
      );
    }
  }

  /* check that we have an enabled checkpoint descriptor with interval of one */
  int found_one = 0;
  for (i=0; i < scr_nckptdescs; i++) {
    /* check that we have at least one descriptor enabled with an interval of one */
    if (scr_ckptdescs[i].enabled && scr_ckptdescs[i].interval == 1) {
      found_one = 1;
    }
  }
  if (!found_one) {
    if (scr_my_rank_world == 0) {
      scr_abort(-1, "Failed to find an enabled checkpoint descriptor with interval 1 @ %s:%d",
              __FILE__, __LINE__
      );
    }
  }

  /* register this job in the logging database */
  if (scr_my_rank_world == 0 && scr_log_enable) {
    if (scr_username != NULL && scr_jobname != NULL) {
      time_t job_start = scr_log_seconds();
      if (scr_log_job(scr_username, scr_jobname, job_start) == SCR_SUCCESS) {
        /* record the start time for this run */
        scr_log_run(job_start);
      } else {
        scr_err("Failed to log job for username %s and jobname %s, disabling logging @ %s:%d",
                scr_username, scr_jobname, __FILE__, __LINE__
        );
        scr_log_enable = 0;
      }
    } else {
      scr_err("Failed to read username or jobname from environment, disabling logging @ %s:%d",
              __FILE__, __LINE__
      );
      scr_log_enable = 0;
    }
  }

  /* build the control directory name: CNTL_BASE/username/scr.jobid */
  int cntldir_str_len = strlen(scr_cntl_base) + 1 + strlen(scr_username) + strlen("/scr.") + strlen(scr_jobid);
  scr_cntl_prefix = (char*) malloc(cntldir_str_len + 1);
  if (scr_cntl_prefix == NULL) {
    scr_abort(-1, "Failed to allocate buffer to store control prefix @ %s:%d",
              __FILE__, __LINE__
    );
  }
  sprintf(scr_cntl_prefix, "%s/%s/scr.%s", scr_cntl_base, scr_username, scr_jobid);

  /* the master on each node creates the control directory */
  if (scr_my_rank_local == 0) {
    scr_dbg(2, "Creating control directory: %s", scr_cntl_prefix);
    if (scr_mkdir(scr_cntl_prefix, S_IRWXU | S_IRWXG) != SCR_SUCCESS) {
      scr_abort(-1, "Failed to create control directory: %s @ %s:%d",
              scr_cntl_prefix, __FILE__, __LINE__
      );
    }
    /* TODO: open permissions to control directory so other users (admins) can halt the job? */
    /*
    mode_t mode = umask(0000);
    scr_mkdir(scr_cntl_prefix, S_IRWXU | S_IRWXG | S_IRWXO);
    umask(mode);
    */
  }

  /* TODO: should we check for access and required space in cache directore at this point? */

  /* create the checkpoint directories */
  if (scr_my_rank_local == 0) {
    for (i=0; i < scr_nckptdescs; i++) {
      /* TODO: if checkpoints can be enabled at run time, we'll need to create them all up front */
      if (scr_ckptdescs[i].enabled) {
        scr_dbg(2, "Creating cache directory: %s", scr_ckptdescs[i].directory);
        if (scr_mkdir(scr_ckptdescs[i].directory, S_IRWXU | S_IRWXG) != SCR_SUCCESS) {
          scr_abort(-1, "Failed to create cache directory: %s @ %s:%d",
                    scr_ckptdescs[i].directory, __FILE__, __LINE__
          );
        }
      }
    }
  }

  /* TODO: should we check for access and required space in cache directore at this point? */

  /* ensure that the control and checkpoint directories are ready on our node */
  MPI_Barrier(scr_comm_local);

  /* build the file names using the control directory prefix */
  scr_build_path(scr_halt_file,  sizeof(scr_halt_file),  scr_cntl_prefix, "halt.scrinfo");
  scr_build_path(scr_flush_file, sizeof(scr_flush_file), scr_cntl_prefix, "flush.scrinfo");
  scr_build_path(scr_nodes_file, sizeof(scr_nodes_file), scr_cntl_prefix, "nodes.scrinfo");
  sprintf(scr_map_file, "%s/filemap_%d.scrinfo", scr_cntl_prefix, scr_my_rank_local);
  sprintf(scr_master_map_file, "%s/filemap.scrinfo", scr_cntl_prefix);
  sprintf(scr_transfer_file, "%s/transfer.scrinfo", scr_cntl_prefix);

  /* TODO: continue draining a checkpoint if one is in progress from the previous run,
   * for now, just delete the transfer file so we'll start over from scratch */
  if (scr_my_rank_local == 0) {
    unlink(scr_transfer_file);
  }

  /* TODO: should we also record the list of nodes and / or MPI rank to node mapping? */
  /* record the number of nodes being used in this job to the nodes file */
  int num_nodes = 0;
  MPI_Allreduce(&scr_ranks_level, &num_nodes, 1, MPI_INT, MPI_MAX, scr_comm_world);
  if (scr_my_rank_local == 0) {
    scr_hash* nodes_hash = scr_hash_new();
    scr_hash_setf(nodes_hash, NULL, "%s %d", SCR_NODES_KEY_NODES, num_nodes);
    scr_hash_write(scr_nodes_file, nodes_hash);
    scr_hash_delete(nodes_hash);
  }

  /* initialize halt info before calling scr_bool_check_halt_and_decrement
   * and set the halt seconds in our halt data structure,
   * this will be overridden if a value is already set in the halt file */
  scr_halt_hash = scr_hash_new();

  /* record the halt seconds if they are set */
  if (scr_halt_seconds > 0) {
    scr_hash_setf(scr_halt_hash, NULL, "%s %lu", SCR_HALT_KEY_SECONDS, scr_halt_seconds);
  }

  /* sync everyone up */
  MPI_Barrier(scr_comm_world);

  /* now all processes are initialized (be careful when moving this line up or down) */
  scr_initialized = 1;

  /* since we may be shuffling files around, stop any ongoing async flush */
  if (scr_flush_async) {
    scr_flush_async_stop();
  }

  /* exit right now if we need to halt */
  scr_bool_check_halt_and_decrement(SCR_TEST_AND_HALT, 0);

  int rc = SCR_FAILURE;

  double time_start, time_end, time_diff;

  /* if the code is restarting from the parallel file system, disable fetch and enable flush_on_restart */
  if (scr_global_restart) {
    scr_flush_on_restart = 1;
    scr_fetch = 0;
  }

  /* if scr_fetch or scr_flush is enabled, check that scr_par_prefix is set */
  if ((scr_fetch != 0 || scr_flush > 0) && strcmp(scr_par_prefix, "") == 0) {
    if (scr_my_rank_world == 0) {
      scr_halt("SCR_INIT_FAILED");
      scr_abort(-1, "SCR_PREFIX must be set to use SCR_FETCH or SCR_FLUSH @ %s:%d"
                __FILE__, __LINE__
      );
    }
    MPI_Barrier(scr_comm_world);
  }

  /* allocate a new global filemap object */
  scr_map = scr_filemap_new();

  /* master on each node reads all filemaps and distributes them to other ranks
   * on the node, if any */
  scr_scatter_filemaps(scr_map);

  /* attempt to distribute files for a restart */
  if (rc != SCR_SUCCESS && scr_distribute) {
    int distribute_attempted = 0;

    /* start timer */
    time_t time_t_start;
    if (scr_my_rank_world == 0) {
      time_t_start = scr_log_seconds();
      time_start = MPI_Wtime();
    }

    /* TODO: also attempt to recover checkpoints which we were in the middle of flushing */
    /* start from most recent checkpoint and work backwards */
    int max_id;
    time_t now;
    do {
      /* clean incomplete files from our cache */
      scr_clean_files(scr_map);

      /* find the maximum latest checkpoint id across all ranks */
      int checkpoint_id = scr_filemap_latest_checkpoint(scr_map);
      MPI_Allreduce(&checkpoint_id, &max_id, 1, MPI_INT, MPI_MAX, scr_comm_world);

      if (max_id != -1) {
        /* remember that we tried to distribute and rebuild at least one checkpoint */
        distribute_attempted = 1;
        
        /* log the attempt */
        if (scr_my_rank_world == 0) {
          scr_dbg(1, "Attempting to distribute and rebuild checkpoint %d", max_id);
          if (scr_log_enable) {
            time_t now = scr_log_seconds();
            scr_log_event("REBUILD STARTED", NULL, &max_id, &now, NULL);
          }
        }

        /* read descriptor for this checkpoint from flush file */
        int rebuild_succeeded = 0;
        struct scr_ckptdesc ckptdesc;
        if (scr_distribute_ckptdescs(scr_map, max_id, &ckptdesc) == SCR_SUCCESS) {
          /* create a directory for this checkpoint */
          scr_checkpoint_dir_create(&ckptdesc, max_id);

          /* distribute the files for the this checkpoint */
          scr_distribute_files(scr_map, &ckptdesc, max_id);

          /* rebuild files for this checkpoint */
          rc = scr_rebuild_files(scr_map, &ckptdesc, max_id);
          if (rc == SCR_SUCCESS) {
            /* rebuild succeeded, update scr_checkpoint_id to the latest checkpoint
             * and set max_id to break the loop */
            rebuild_succeeded = 1;
            scr_checkpoint_id = max_id;
            max_id = -1;

            /* update our flush file to indicate this checkpoint is in cache */
            scr_flush_location_set(scr_checkpoint_id, SCR_FLUSH_KEY_LOCATION_CACHE);

            /* if anyone has marked this checkpoint as flushed, have everyone mark it as flushed */
            int in_pfs = 0;
            if (scr_flush_location_test(scr_checkpoint_id, SCR_FLUSH_KEY_LOCATION_PFS) == SCR_SUCCESS) {
              in_pfs = 1;
            }
            if (!scr_alltrue(in_pfs == 0)) {
              scr_flush_location_set(scr_checkpoint_id, SCR_FLUSH_KEY_LOCATION_PFS);
            }

            /* TODO: would like to restore flushing status to checkpoints that were in the middle of a flush,
             * but we need to better manage the transfer file to do this, so for now just forget about flushing
             * this checkpoint */
            scr_flush_location_unset(scr_checkpoint_id, SCR_FLUSH_KEY_LOCATION_FLUSHING);
          }

          /* free checkpoint descriptor */
          scr_ckptdesc_free(&ckptdesc);
        }

        /* if the distribute or rebuild failed, delete the checkpoint */
        if (!rebuild_succeeded) {
          /* log that we failed */
          if (scr_my_rank_world == 0) {
            scr_dbg(1, "Failed to distribute and rebuild checkpoint %d", max_id);
            if (scr_log_enable) {
              time_t now = scr_log_seconds();
              scr_log_event("REBUILD FAILED", NULL, &max_id, &now, NULL);
            }
          }

          /* rebuild failed, delete this checkpoint */
          scr_checkpoint_delete(scr_map, max_id);
        } else {
          /* rebuid worked, log success */
          if (scr_my_rank_world == 0) {
            scr_dbg(1, "Rebuilt checkpoint %d", scr_checkpoint_id);
            if (scr_log_enable) {
              time_t now = scr_log_seconds();
              scr_log_event("REBUILD SUCCEEDED", NULL, &scr_checkpoint_id, &now, NULL);
            }
          }
        }
      }
    } while (max_id != -1);

    /* TODO: may want to keep cache_size sets around, but we need to rebuild each one of them */
    /* TODO: don't delete a checkpoint if it is being flushed */
    /* delete all checkpoints up to most recent */
    if (scr_checkpoint_id != 0) {
      if (scr_my_rank_world == 0) {
        scr_dbg(1, "Deleting excess checkpoints");
      }

      /* find the maximum number of checkpoints across all ranks */
      int max_num_checkpoints = 0;
      int num_checkpoints = scr_filemap_num_checkpoints(scr_map);
      MPI_Allreduce(&num_checkpoints, &max_num_checkpoints, 1, MPI_INT, MPI_MAX, scr_comm_world);

      /* while this maximum number is greater than 1, find the oldest checkpoint and delete it */
      while (max_num_checkpoints > 1) {
        /* find the oldest checkpoint across all ranks */
        int min_id = max_id;
        int checkpoint_id = scr_filemap_oldest_checkpoint(scr_map, -1);
        if (checkpoint_id == -1) {
          checkpoint_id = max_id;
        }
        MPI_Allreduce(&checkpoint_id, &min_id, 1, MPI_INT, MPI_MIN, scr_comm_world);

        /* if this oldest checkpoint is not the latest (last one), delete it */
        if (min_id != scr_checkpoint_id) {
          scr_checkpoint_delete(scr_map, min_id);
        }

        /* find the maximum number of checkpoints across all ranks again */
        max_num_checkpoints = 0;
        num_checkpoints = scr_filemap_num_checkpoints(scr_map);
        MPI_Allreduce(&num_checkpoints, &max_num_checkpoints, 1, MPI_INT, MPI_MAX, scr_comm_world);
      }
    }

    /* stop timer and report performance */
    if (scr_my_rank_world == 0) {
      time_end = MPI_Wtime();
      time_diff = time_end - time_start;

      if (distribute_attempted) {
        if (rc == SCR_SUCCESS) {
          scr_dbg(1, "Scalable restart succeeded for checkpoint %d, took %f secs",
                  scr_checkpoint_id, time_diff
          );
          if (scr_log_enable) {
            time_t now = scr_log_seconds();
            scr_log_event("RESTART SUCCEEDED", NULL, &scr_checkpoint_id, &time_t_start, &time_diff);
          }
        } else {
          scr_dbg(1, "Scalable restart failed, took %f secs", time_diff);
          if (scr_log_enable) {
            time_t now = scr_log_seconds();
            scr_log_event("RESTART FAILED", NULL, NULL, &time_t_start, &time_diff);
          }
        }
      }
    }

    /* TODO: need to make the flush file specific to each checkpoint */
    /* if distribute succeeds, check whether we should flush on restart */
    if (rc == SCR_SUCCESS) {
      if (scr_flush_on_restart) {
        /* always flush on restart if scr_flush_on_restart is set */
        scr_flush_files(scr_map, scr_checkpoint_id);
      } else {
        /* otherwise, flush only if we need to flush */
        scr_check_flush(scr_map);
      }
    }
  }

  /* TODO: there is some risk here of cleaning the cache when we shouldn't if given a badly placed nodeset
   * for a restart job step within an allocation with lots of spares. */
  /* if the distribute fails, or if the code must restart from the parallel file system, clear the cache */
  if (rc != SCR_SUCCESS || scr_global_restart) {
    scr_unlink_all(scr_map);
    scr_checkpoint_id = 0;
  }

  /* attempt to fetch files from parallel file system into cache */
  int fetch_attempted = 0;
  if (rc != SCR_SUCCESS && scr_fetch) {
    /* start timer */
    if (scr_my_rank_world == 0) {
      time_start = MPI_Wtime();
    }

    /* build the filename for the current symlink */
    char scr_current[SCR_MAX_FILENAME];
    scr_build_path(scr_current, sizeof(scr_current), scr_par_prefix, SCR_CURRENT_LINK);

    /* have rank 0 read the index file */
    scr_hash* index_hash = NULL;
    int read_index_file = 0;
    if (scr_my_rank_world == 0) {
      /* create an empty hash to store our index */
      index_hash = scr_hash_new();

      /* read the index file */
      if (scr_index_read(scr_par_prefix, index_hash) == SCR_SUCCESS) {
        /* remember that we read the index file ok, so we know we can write to it later
         * this way we don't overwrite an existing index file just because the read happened to fail */
        read_index_file = 1;
      }
    }

    /* now start fetching, we keep trying until we exhaust all valid checkpoints */
    char target[SCR_MAX_FILENAME];
    char fetch_dir[SCR_MAX_FILENAME];
    int current_checkpoint_id = -1;
    int continue_fetching = 1;
    while (continue_fetching) {
      /* initialize our target and fetch directory values to empty strings */
      strcpy(target, "");
      strcpy(fetch_dir, "");

      /* rank 0 determines the directory to fetch from */
      if (scr_my_rank_world == 0) {
        /* read the target of the current symlink if there is one */
        if (access(scr_current, R_OK) == 0) {
          int target_len = readlink(scr_current, target, sizeof(target)-1);
          if (target_len >= 0) {
            target[target_len] = '\0';
          }
        }

        /* if we read the index file, lookup the checkpoint id */
        if (read_index_file) {
          int next_checkpoint_id = -1;
          if (strcmp(target, "") != 0) {
            /* we have a subdirectory name, lookup the checkpoint id corresponding to this directory */
            scr_index_get_checkpoint_id_by_dir(index_hash, target, &next_checkpoint_id);
          } else {
            /* otherwise, just get the most recent complete checkpoint (that's older than the current id) */
            scr_index_most_recent_complete(index_hash, current_checkpoint_id, &next_checkpoint_id, target);
          }
          current_checkpoint_id = next_checkpoint_id;
        }

        /* if we have a subdirectory (target) name, build the full fetch directory */
        if (strcmp(target, "") != 0) {
          /* record that we're attempting a fetch of this checkpoint */
          if (read_index_file && current_checkpoint_id != -1) {
            scr_index_mark_fetched(index_hash, current_checkpoint_id, target);
            scr_index_write(scr_par_prefix, index_hash);
          }

          /* we have a subdirectory, now build the full path */
          scr_build_path(fetch_dir, sizeof(fetch_dir), scr_par_prefix, target);
        }
      }

      /* now attempt to fetch the checkpoint */
      rc = scr_fetch_files(scr_map, fetch_dir);
      if (rc == SCR_SUCCESS) {
        /* we succeeded in fetching this checkpoint, set current to point to it, and stop fetching */
        if (scr_my_rank_world == 0) {
          symlink(target, scr_current);
        }
        continue_fetching = 0;
      } else {
        /* fetch failed, delete the current symlink */
        unlink(scr_current);

        /* if we had a fetch directory, mark it as failed so we don't try it again */
        if (strcmp(fetch_dir, "") != 0) {
          if (scr_my_rank_world == 0) {
            if (read_index_file && current_checkpoint_id != -1 && strcmp(target, "") != 0) {
              scr_index_mark_failed(index_hash, current_checkpoint_id, target);
              scr_index_write(scr_par_prefix, index_hash);
            }
          }
        } else {
          /* we ran out of valid checkpoints in the index file, bail out of the loop */
          continue_fetching = 0;
        }
      }
    }

    /* delete the index hash */
    if (scr_my_rank_world == 0) {
      scr_hash_delete(index_hash);
    }

    /* stop timer for fetch */
    if (scr_my_rank_world == 0) {
      time_end = MPI_Wtime();
      time_diff = time_end - time_start;
      scr_dbg(1, "scr_fetch_files: return code %d, %f secs", rc, time_diff);
    }
  }

  /* TODO: there is some risk here of cleaning the cache when we shouldn't if given a badly placed nodeset
     for a restart job step within an allocation with lots of spares. */
  /* if the fetch fails, lets clear the cache */
  if (rc != SCR_SUCCESS) {
    scr_unlink_all(scr_map);
    scr_checkpoint_id = 0;
  }

  /* both the distribute and the fetch failed */
  if (rc != SCR_SUCCESS) {
    /* if a fetch was attempted but we failed, print a warning */
    if (scr_my_rank_world == 0 && fetch_attempted) {
      scr_err("Failed to fetch checkpoint set into cache @ %s:%d", __FILE__, __LINE__);
    }
    rc = SCR_SUCCESS;
  }

  /* sync everyone before returning to ensure that subsequent calls to SCR functions are valid */
  MPI_Barrier(scr_comm_world);

  /* start the clocks for measuring the compute time and time of last checkpoint */
  if (scr_my_rank_world == 0) {
    /* set the checkpoint end time, we use this time in Need_checkpoint */
    scr_time_checkpoint_end = MPI_Wtime();

    /* start the clocks for measuring the compute time */
    scr_timestamp_compute_start = scr_log_seconds();
    scr_time_compute_start = MPI_Wtime();

    /* log the start time of this compute phase */
    if (scr_log_enable) {
      int compute_id = scr_checkpoint_id + 1;
      scr_log_event("COMPUTE STARTED", NULL, &compute_id, &scr_timestamp_compute_start, NULL);
    }
  }

  /* all done, ready to go */
  return rc;
}

/* Close down and clean up */
int SCR_Finalize()
{ 
  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d", __FILE__, __LINE__);
    return SCR_FAILURE;
  }

  if (scr_my_rank_world == 0) {
    /* stop the clock for measuring the compute time */
    scr_time_compute_end = MPI_Wtime();

    /* if we reach SCR_Finalize, assume that we should not restart the job */
    scr_halt("SCR_FINALIZE_CALLED");
  }

  /* handle any async flush */
  if (scr_flush_async_in_progress) {
    if (scr_flush_async_checkpoint_id == scr_checkpoint_id) {
      /* we're going to sync flush this same checkpoint below, so kill it */
      scr_flush_async_stop();
    } else {
      /* the async flush is flushing a different checkpoint, so wait for it */
      scr_flush_async_wait(scr_map);
    }
  }

  /* flush checkpoint set if we need to */
  if (scr_bool_need_flush(scr_checkpoint_id)) {
    scr_flush_files(scr_map, scr_checkpoint_id);
  }

  /* disconnect from database */
  if (scr_my_rank_world == 0 && scr_log_enable) {
    scr_log_finalize();
  }

  /* free off the memory allocated for our checkpoint descriptors */
  scr_ckptdesc_free_list();

  /* delete the cache descriptor and checkpoint descriptor hashes */
  scr_hash_delete(scr_cachedesc_hash);
  scr_hash_delete(scr_ckptdesc_hash);

  /* free off our global filemap object */
  scr_filemap_delete(scr_map);

  /* free off the library's communicators */
  MPI_Comm_free(&scr_comm_level);
  MPI_Comm_free(&scr_comm_local);
  MPI_Comm_free(&scr_comm_world);

  /* free memory allocated for variables */
  if (scr_username) {
    free(scr_username);
    scr_username = NULL;
  }
  if (scr_jobid) {
    free(scr_jobid);
    scr_jobid    = NULL;
  }
  if (scr_jobname) {
    free(scr_jobname);
    scr_jobname  = NULL;
  }

  /* free off the memory we allocated for our cntl prefix */
  if (scr_cntl_prefix != NULL) {
    free(scr_cntl_prefix);
    scr_cntl_prefix = NULL;
  }

  /* we're no longer in an initialized state */
  scr_initialized = 0;

  return SCR_SUCCESS;
}

/* sets flag to 1 if a checkpoint should be taken, flag is set to 0 otherwise */
int SCR_Need_checkpoint(int* flag)
{
  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    *flag = 0;
    return SCR_FAILURE;
  }

  /* say no if not initialized */
  if (! scr_initialized) {
    *flag = 0;
    scr_abort(-1, "SCR has not been initialized @ %s:%d", __FILE__, __LINE__);
    return SCR_FAILURE;
  }

  /* track the number of times a user has called SCR_Need_checkpoint */
  scr_need_checkpoint_id++;

  /* assume we don't need to checkpoint */
  *flag = 0;

  /* check whether a halt condition is active (don't halt, just be sure to return 1 in this case) */
  if (!*flag && scr_bool_check_halt_and_decrement(SCR_TEST_BUT_DONT_HALT, 0)) {
    *flag = 1;
  }

  /* have rank 0 make the decision and broadcast the result */
  if (scr_my_rank_world == 0) {
    /* TODO: account for MTBF, time to flush, etc. */
    /* if we don't need to halt, check whether we can afford to checkpoint */

    /* if checkpoint interval is set, check the current checkpoint id */
    if (!*flag && scr_checkpoint_interval > 0 && scr_need_checkpoint_id % scr_checkpoint_interval == 0) {
      *flag = 1;
    }

    /* if checkpoint seconds is set, check the time since the last checkpoint */
    if (!*flag && scr_checkpoint_seconds > 0) {
      double now_seconds = MPI_Wtime();
      if ((int)(now_seconds - scr_time_checkpoint_end) >= scr_checkpoint_seconds) {
        *flag = 1;
      }
    }

    /* check whether we can afford to checkpoint based on the max allowed checkpoint overhead, if set */
    if (!*flag && scr_checkpoint_overhead > 0) {
      /* TODO: could init the cost estimate via environment variable or stats from previous run */
      if (scr_time_checkpoint_count == 0) {
        /* if we haven't taken a checkpoint, we need to take one in order to get a cost estimate */
        *flag = 1;
      } else if (scr_time_checkpoint_count > 0) {
        /* based on average time of checkpoint, current time, and time that last checkpoint ended,
         * determine overhead of checkpoint if we took one right now */
        double now = MPI_Wtime();
        double avg_cost = scr_time_checkpoint_total / (double) scr_time_checkpoint_count;
        double percent_cost = avg_cost / (now - scr_time_checkpoint_end + avg_cost) * 100.0;

        /* if our current percent cost is less than allowable overhead,
         * indicate that it's time for a checkpoint */
        if (percent_cost < scr_checkpoint_overhead) {
          *flag = 1;
        }
      }
    }

    /* no way to determine whether we need to checkpoint, so always say yes */
    if (!*flag &&
        scr_checkpoint_interval <= 0 &&
        scr_checkpoint_seconds <= 0 &&
        scr_checkpoint_overhead <= 0)
    {
      *flag = 1;
    }
  }

  /* rank 0 broadcasts the decision */
  MPI_Bcast(flag, 1, MPI_INT, 0, scr_comm_world);

  return SCR_SUCCESS;
}

/* informs SCR that a fresh checkpoint set is about to start */
int SCR_Start_checkpoint()
{
  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }
  
  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d", __FILE__, __LINE__);
    return SCR_FAILURE;
  }

  /* bail out if user called Start_checkpoint twice without Complete_checkpoint in between */
  if (scr_in_checkpoint) {
    scr_abort(-1,
            "SCR_Complete_checkpoint must be called before SCR_Start_checkpoint is called again @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* make sure everyone is ready to start before we delete any existing checkpoints */
  MPI_Barrier(scr_comm_world);

  /* set the checkpoint flag to indicate we have entered a new checkpoint */
  scr_in_checkpoint = 1;

  /* stop clock recording compute time */
  if (scr_my_rank_world == 0) {
    /* stop the clock for measuring the compute time */
    scr_time_compute_end = MPI_Wtime();

    /* log the end of this compute phase */
    if (scr_log_enable) {
      int compute_id = scr_checkpoint_id + 1;
      double time_diff = scr_time_compute_end - scr_time_compute_start;
      time_t now = scr_log_seconds();
      scr_log_event("COMPUTE COMPLETED", NULL, &compute_id, &now, &time_diff);
    }
  }

  /* increment our checkpoint counter */
  scr_checkpoint_id++;

  /* get the checkpoint descriptor for this checkpoint id */
  struct scr_ckptdesc* c = scr_ckptdesc_get(scr_checkpoint_id, scr_nckptdescs, scr_ckptdescs);

  /* start the clock to record how long it takes to checkpoint */
  if (scr_my_rank_world == 0) {
    scr_timestamp_checkpoint_start = scr_log_seconds();
    scr_time_checkpoint_start = MPI_Wtime();

    /* log the start of this checkpoint phase */
    if (scr_log_enable) {
      scr_log_event("CHECKPOINT STARTED", c->base, &scr_checkpoint_id, &scr_timestamp_checkpoint_start, NULL);
    }
  }

  /* get an ordered list of the checkpoints currently in cache */
  int nckpts;
  int* ckpts = NULL;
  scr_filemap_list_checkpoints(scr_map, &nckpts, &ckpts);

  /* lookup the number of checkpoints we're allowed to keep in the base for this checkpoint */
  int size = scr_cachedesc_size(c->base);

  int i;
  char* base = NULL;

  /* run through each of our checkpoints and count how many we have in this base */
  int nckpts_base = 0;
  for (i=0; i < nckpts; i++) {
    /* if this checkpoint is not currently flushing, delete it */
    base = scr_ckptdesc_base_from_filemap(scr_map, ckpts[i], scr_my_rank_world);
    if (base != NULL) {
      if (strcmp(base, c->base) == 0) {
        nckpts_base++;
      }
      free(base);
    }
  }

  /* run through and delete checkpoints from base until we make room for the current one */
  int flushing = -1;
  for (i=0; i < nckpts && nckpts_base >= size; i++) {
    base = scr_ckptdesc_base_from_filemap(scr_map, ckpts[i], scr_my_rank_world);
    if (base != NULL) {
      if (strcmp(base, c->base) == 0) {
        if (!scr_bool_is_flushing(ckpts[i])) {
          /* this checkpoint is in our base, and it's not being flushed, so delete it */
          scr_checkpoint_delete(scr_map, ckpts[i]);
          nckpts_base--;
        } else if (flushing == -1) {
          /* this checkpoint is in our base, but we're flushing it, don't delete it */
          flushing = ckpts[i];
        }
      }
      free(base);
    }
  }

  /* if we still don't have room and we're flushing, the checkpoint we need to delete
   * must be flushing, so wait for it to finish */
  if (nckpts_base >= size && flushing != -1) {
    /* TODO: we could increase the transfer bandwidth to reduce our wait time */

    /* wait for this checkpoint to complete its flush */
    scr_flush_async_wait(scr_map);

    /* alright, this checkpoint is no longer flushing, so we can delete it now and continue on */
    scr_checkpoint_delete(scr_map, flushing);
    nckpts_base--;
  }

  /* free the list of checkpoint */
  if (ckpts != NULL) {
    free(ckpts);
    ckpts = NULL;
  }

  /* store the checkpoint descriptor in the filemap, so if we die before completing
   * the checkpoint, we'll have a record of the new directory we're about to create */
  scr_hash* my_desc_hash = scr_hash_new();
  scr_ckptdesc_store_to_hash(c, my_desc_hash);
  scr_filemap_set_desc(scr_map, scr_checkpoint_id, scr_my_rank_world, my_desc_hash);
  scr_filemap_write(scr_map_file, scr_map);
  scr_hash_delete(my_desc_hash);

  /* make directory in cache to store files for this checkpoint */
  scr_checkpoint_dir_create(c, scr_checkpoint_id);

  /* print a debug message to indicate we've started the checkpoint */
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "Starting checkpoint %d", scr_checkpoint_id);
  }

  return SCR_SUCCESS;
}

/* given a filename, return the full path to the file which the user should write to */
int SCR_Route_file(const char* file, char* newfile)
{
  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }
  
  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d", __FILE__, __LINE__);
    return SCR_FAILURE;
  }

  /* route the file */
  int n = SCR_MAX_FILENAME;
  if (scr_route_file(scr_checkpoint_id, file, newfile, n) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  /* TODO: to avoid duplicates, check that the file is not already in the filemap,
   * at the moment duplicates just overwrite each other, so there's no harm */
  /* if we are in a new checkpoint, record this file in our filemap,
   * otherwise, we are likely in a restart, so check whether the file exists */
  if (scr_in_checkpoint) {
    scr_filemap_add_file(scr_map, scr_checkpoint_id, scr_my_rank_world, newfile);
    scr_filemap_write(scr_map_file, scr_map);
  } else {
    /* if we can't read the file, return an error */
    if (access(newfile, R_OK) < 0) {
      return SCR_FAILURE;
    }
  }

  return SCR_SUCCESS;
}

/* completes the checkpoint set and marks it as valid or not */
int SCR_Complete_checkpoint(int valid)
{
  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d", __FILE__, __LINE__);
    return SCR_FAILURE;
  }

  /* bail out if user called Start_checkpoint twice without Complete_checkpoint in between */
  if (! scr_in_checkpoint) {
    scr_abort(-1,
            "SCR_Start_checkpoint must be called before SCR_Complete_checkpoint @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* mark each file as complete or not */
  scr_hash_elem* elem;
  for (elem = scr_filemap_first_file(scr_map, scr_checkpoint_id, scr_my_rank_world);
       elem != NULL;
       elem = scr_hash_elem_next(elem))
  {
    /* get the filename */
    char* file = scr_hash_elem_key(elem);

    /* fill out meta info for our file */
    unsigned long filesize = scr_filesize(file);
    scr_meta* meta = scr_meta_new();
    scr_meta_set(meta, file, SCR_META_FILE_FULL, filesize,
                 scr_checkpoint_id, scr_my_rank_world, scr_ranks_world, valid
    );

    /* mark the file as complete */
    scr_complete(file, meta);
    scr_meta_delete(meta);
  }

  /* apply redundancy scheme */
  double bytes_copied = 0.0;
  struct scr_ckptdesc* c = scr_ckptdesc_get(scr_checkpoint_id, scr_nckptdescs, scr_ckptdescs);
  int rc = scr_copy_files(scr_map, c, scr_checkpoint_id, &bytes_copied);

  /* record the cost of the checkpoint and log its completion */
  if (scr_my_rank_world == 0) {
    /* stop the clock for this checkpoint */
    scr_time_checkpoint_end = MPI_Wtime();

    /* compute and record the cost for this checkpoint */
    double cost = scr_time_checkpoint_end - scr_time_checkpoint_start;
    if (cost < 0) {
      scr_err("Checkpoint end time (%f) is less than start time (%f) @ %s:%d",
              scr_time_checkpoint_end, scr_time_checkpoint_start, __FILE__, __LINE__
      );
      cost = 0;
    }
    scr_time_checkpoint_total += cost;
    scr_time_checkpoint_count++;

    /* log data on the checkpoint in the database */
    if (scr_log_enable) {
      /* log the end of this checkpoint phase */
      double time_diff = scr_time_checkpoint_end - scr_time_checkpoint_start;
      time_t now = scr_log_seconds();
      scr_log_event("CHECKPOINT COMPLETED", c->base, &scr_checkpoint_id, &now, &time_diff);

      /* log the transfer details */
      char ckpt_dir[SCR_MAX_FILENAME];
      scr_checkpoint_dir(c, scr_checkpoint_id, ckpt_dir);
      scr_log_transfer("CHECKPOINT", c->base, ckpt_dir, &scr_checkpoint_id,
                       &scr_timestamp_checkpoint_start, &cost, &bytes_copied
      );
    }

    /* print out a debug message with the result of the copy */
    scr_dbg(1, "Completed checkpoint %d with return code %d",
            scr_checkpoint_id, rc
    );
  }

  /* if copy is good, check whether we need to flush or halt,
   * otherwise delete the checkpoint to conserve space */
  if (rc == SCR_SUCCESS) {
    /* check_flush may start an async flush, whereas check_halt will call sync flush,
     * so place check_flush after check_halt */
    scr_flush_location_set(scr_checkpoint_id, SCR_FLUSH_KEY_LOCATION_CACHE);
    scr_bool_check_halt_and_decrement(SCR_TEST_AND_HALT, 1);
    scr_check_flush(scr_map);
  } else {
    /* something went wrong, so delete this checkpoint from the cache */
    scr_checkpoint_delete(scr_map, scr_checkpoint_id);
  }

  /* if we have an async flush ongoing, take this chance to check whether it's completed */
  if (scr_flush_async_in_progress) {
    double bytes = 0.0;
    if (scr_flush_async_test(scr_map, scr_flush_async_checkpoint_id, &bytes) == SCR_SUCCESS) {
      /* async flush has finished, go ahead and complete it */
      scr_flush_async_complete(scr_map, scr_flush_async_checkpoint_id);
    } else {
      /* not done yet, just print a progress message to the screen */
      if (scr_my_rank_world == 0) {
        scr_dbg(1, "Flush of checkpoint %d is %d%% complete",
                scr_flush_async_checkpoint_id, (int) (bytes / scr_flush_async_bytes * 100.0)
        );
      }
    }
  }

  /* make sure everyone is ready before we exit */
  MPI_Barrier(scr_comm_world);

  /* unset the checkpoint flag to indicate we have exited the current checkpoint */
  scr_in_checkpoint = 0;

  /* start the clock for measuring the compute time */
  if (scr_my_rank_world == 0) {
    scr_timestamp_compute_start = scr_log_seconds();
    scr_time_compute_start = MPI_Wtime();

    /* log the start time of this compute phase */
    if (scr_log_enable) {
      int compute_id = scr_checkpoint_id + 1;
      scr_log_event("COMPUTE STARTED", NULL, &compute_id, &scr_timestamp_compute_start, NULL);
    }
  }

  return rc;
}