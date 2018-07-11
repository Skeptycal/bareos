/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2013 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Manipulation routines for Job Control Records and
 *  handling of last_jobs_list.
 *
 *  Kern E. Sibbald, December 2000
 *
 *  These routines are thread safe.
 *
 *  The job list routines were re-written in May 2005 to
 *  eliminate the global lock while traversing the list, and
 *  to use the dlist subroutines.  The locking is now done
 *  on the list each time the list is modified or traversed.
 *  That is it is "micro-locked" rather than globally locked.
 *  The result is that there is one lock/unlock for each entry
 *  in the list while traversing it rather than a single lock
 *  at the beginning of a traversal and one at the end.  This
 *  incurs slightly more overhead, but effectively eliminates
 *  the possibilty of race conditions.  In addition, with the
 *  exception of the global locking of the list during the
 *  re-reading of the config file, no recursion is needed.
 *
 */

#include "include/bareos.h"
#include "include/jcr.h"
#include "lib/edit.h"

const int debuglevel = 3400;

/* External variables we reference */

/* External referenced functions */
void FreeBregexps(alist *bregexps);

/* Forward referenced functions */
extern "C" void TimeoutHandler(int sig);
static void JcrTimeoutCheck(watchdog_t *self);
#ifdef TRACE_JCR_CHAIN
static void b_lock_jcr_chain(const char *filen, int line);
static void b_unlock_jcr_chain(const char *filen, int line);
#define lock_jcr_chain() b_lock_jcr_chain(__FILE__, __LINE__);
#define unlock_jcr_chain() b_unlock_jcr_chain(__FILE__, __LINE__);
#else
static void lock_jcr_chain();
static void unlock_jcr_chain();
#endif

int num_jobs_run;
dlist *last_jobs = NULL;
const int max_last_jobs = 10;

static dlist *jcrs = NULL;            /* JobControlRecord chain */
static int watch_dog_timeout = 0;

static pthread_mutex_t jcr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t job_start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t last_jobs_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef HAVE_WIN32
static bool tsd_initialized = false;
static pthread_key_t jcr_key;         /* Pointer to jcr for each thread */
#else
#ifdef PTHREAD_ONCE_KEY_NP
static pthread_key_t jcr_key = PTHREAD_ONCE_KEY_NP;
#else
static pthread_key_t jcr_key;         /* Pointer to jcr for each thread */
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
#endif
#endif

static char Job_status[]     = "Status Job=%s JobStatus=%d\n";

void LockJobs()
{
   P(job_start_mutex);
}

void UnlockJobs()
{
   V(job_start_mutex);
}

void InitLastJobsList()
{
   JobControlRecord *jcr = NULL;
   struct s_last_job *job_entry = NULL;
   if (!last_jobs) {
      last_jobs = New(dlist(job_entry, &job_entry->link));
   }
   if (!jcrs) {
      jcrs = New(dlist(jcr, &jcr->link));
   }
}

void TermLastJobsList()
{
   if (last_jobs) {
      LockLastJobsList();
      while (!last_jobs->empty()) {
         void *je = last_jobs->first();
         last_jobs->remove(je);
         free(je);
      }
      delete last_jobs;
      last_jobs = NULL;
      UnlockLastJobsList();
   }
   if (jcrs) {
      delete jcrs;
      jcrs = NULL;
   }
}

bool ReadLastJobsList(int fd, uint64_t addr)
{
   struct s_last_job *je, job;
   uint32_t num;
   bool ok = true;

   Dmsg1(100, "read_last_jobs seek to %d\n", (int)addr);
   if (addr == 0 || lseek(fd, (boffset_t)addr, SEEK_SET) < 0) {
      return false;
   }
   if (read(fd, &num, sizeof(num)) != sizeof(num)) {
      return false;
   }
   Dmsg1(100, "Read num_items=%d\n", num);
   if (num > 4 * max_last_jobs) {  /* sanity check */
      return false;
   }
   LockLastJobsList();
   for ( ; num; num--) {
      if (read(fd, &job, sizeof(job)) != sizeof(job)) {
         BErrNo be;
         Pmsg1(000, "Read job entry. ERR=%s\n", be.bstrerror());
         ok = false;
         break;
      }
      if (job.JobId > 0) {
         je = (struct s_last_job *)malloc(sizeof(struct s_last_job));
         memcpy((char *)je, (char *)&job, sizeof(job));
         if (!last_jobs) {
            InitLastJobsList();
         }
         last_jobs->append(je);
         if (last_jobs->size() > max_last_jobs) {
            je = (struct s_last_job *)last_jobs->first();
            last_jobs->remove(je);
            free(je);
         }
      }
   }
   UnlockLastJobsList();
   return ok;
}

uint64_t WriteLastJobsList(int fd, uint64_t addr)
{
   struct s_last_job *je;
   uint32_t num;
   ssize_t status;

   Dmsg1(100, "write_last_jobs seek to %d\n", (int)addr);
   if (lseek(fd, (boffset_t)addr, SEEK_SET) < 0) {
      return 0;
   }
   if (last_jobs) {
      LockLastJobsList();
      /*
       * First record is number of entires
       */
      num = last_jobs->size();
      if (write(fd, &num, sizeof(num)) != sizeof(num)) {
         BErrNo be;
         Pmsg1(000, "Error writing num_items: ERR=%s\n", be.bstrerror());
         goto bail_out;
      }
      foreach_dlist(je, last_jobs) {
         if (write(fd, je, sizeof(struct s_last_job)) != sizeof(struct s_last_job)) {
            BErrNo be;
            Pmsg1(000, "Error writing job: ERR=%s\n", be.bstrerror());
            goto bail_out;
         }
      }
      UnlockLastJobsList();
   }

   /*
    * Return current address
    */
   status = lseek(fd, 0, SEEK_CUR);
   if (status < 0) {
      status = 0;
   }
   return status;

bail_out:
   UnlockLastJobsList();
   return 0;
}

void LockLastJobsList()
{
   P(last_jobs_mutex);
}

void UnlockLastJobsList()
{
   V(last_jobs_mutex);
}

/*
 * Get an ASCII representation of the Operation being performed as an english Noun
 */
const char *JobControlRecord::get_OperationName()
{
   switch(JobType_) {
   case JT_BACKUP:
      return _("Backup");
   case JT_VERIFY:
      return _("Verifying");
   case JT_RESTORE:
      return _("Restoring");
   case JT_ARCHIVE:
      return _("Archiving");
   case JT_COPY:
      return _("Copying");
   case JT_MIGRATE:
      return _("Migration");
   case JT_SCAN:
      return _("Scanning");
   case JT_CONSOLIDATE:
      return _("Consolidating");
   default:
      return _("Unknown operation");
   }
}

/*
 * Get an ASCII representation of the Action being performed either an english Verb or Adjective
 */
const char *JobControlRecord::get_ActionName(bool past)
{
   switch(JobType_) {
   case JT_BACKUP:
      return _("backup");
   case JT_VERIFY:
      return (past) ? _("verified") : _("verify");
   case JT_RESTORE:
      return (past) ? _("restored") : _("restore");
   case JT_ARCHIVE:
      return (past) ? _("archived") : _("archive");
   case JT_COPY:
      return (past) ? _("copied") : _("copy");
   case JT_MIGRATE:
      return (past) ? _("migrated") : _("migrate");
   case JT_SCAN:
      return (past) ? _("scanned") : _("scan");
   case JT_CONSOLIDATE:
      return (past) ? _("consolidated") : _("consolidate");
   default:
      return _("unknown action");
   }
}

bool JobControlRecord::JobReads()
{
   switch (JobType_) {
   case JT_VERIFY:
   case JT_RESTORE:
   case JT_COPY:
   case JT_MIGRATE:
      return true;
   case JT_BACKUP:
      if (JobLevel_ == L_VIRTUAL_FULL) {
         return true;
      }
      break;
   default:
      break;
   }
   return false;
}

/*
 * Push a job_callback_item onto the job end callback stack.
 */
void RegisterJobEndCallback(JobControlRecord *jcr, void JobEndCb(JobControlRecord *jcr, void *), void *ctx)
{
   job_callback_item *item;

   item = (job_callback_item *)malloc(sizeof(job_callback_item));

   item->JobEndCb = JobEndCb;
   item->ctx = ctx;

   jcr->job_end_callbacks.push((void *)item);
}

/*
 * Pop each job_callback_item and process it.
 */
static void CallJobEndCallbacks(JobControlRecord *jcr)
{
   job_callback_item *item;

   if (jcr->job_end_callbacks.size() > 0) {
      item = (job_callback_item *)jcr->job_end_callbacks.pop();
      while (item) {
         item->JobEndCb(jcr, item->ctx);
         free(item);
         item = (job_callback_item *)jcr->job_end_callbacks.pop();
      }
   }
}

/*
 * Create thread key for thread specific data.
 */
static void create_jcr_key()
{
   int status;

#ifdef PTHREAD_ONCE_KEY_NP
   status = pthread_key_create_once_np(&jcr_key, NULL);
#else
   status = pthread_key_create(&jcr_key, NULL);
#endif
   if (status != 0) {
      BErrNo be;
      Jmsg1(NULL, M_ABORT, 0, _("pthread key create failed: ERR=%s\n"),
            be.bstrerror(status));
   }
}

/*
 * Setup thread key for thread specific data.
 */
void setup_tsd_key()
{
#ifdef HAVE_WIN32
   P(jcr_lock);
   if (!tsd_initialized) {
      create_jcr_key();
      tsd_initialized = true;
   }
   V(jcr_lock);
#else
#ifdef PTHREAD_ONCE_KEY_NP
   create_jcr_key();
#else
   int status;

   status = pthread_once(&key_once, create_jcr_key);
   if (status != 0) {
      BErrNo be;
      Jmsg1(NULL, M_ABORT, 0, _("pthread_once failed. ERR=%s\n"), be.bstrerror(status));
   }
#endif
#endif
}

/*
 * Create a Job Control Record and link it into JobControlRecord chain
 * Returns newly allocated JobControlRecord
 *
 * Note, since each daemon has a different JobControlRecord, he passes us the size.
 */
JobControlRecord *new_jcr(int size, JCR_free_HANDLER *daemon_free_jcr)
{
   JobControlRecord *jcr;
   MessageQeueItem *item = NULL;
   struct sigaction sigtimer;
   int status;

   Dmsg0(debuglevel, "Enter new_jcr\n");

   setup_tsd_key();

   jcr = (JobControlRecord *)malloc(size);
   memset(jcr, 0, size);
   jcr = new(jcr) JobControlRecord(); /* placement new instead of memset */

   jcr->msg_queue = New (dlist(item, &item->link));
   if ((status = pthread_mutex_init(&jcr->msg_queue_mutex, NULL)) != 0) {
      BErrNo be;
      Jmsg(NULL, M_ABORT, 0, _("Could not init msg_queue mutex. ERR=%s\n"), be.bstrerror(status));
   }

   jcr->my_thread_id = pthread_self();
   jcr->job_end_callbacks.init(1, false);
   jcr->sched_time = time(NULL);
   jcr->initial_sched_time = jcr->sched_time;
   jcr->daemon_free_jcr = daemon_free_jcr;    /* plug daemon free routine */
   jcr->InitMutex();
   jcr->IncUseCount();
   jcr->VolumeName = GetPoolMemory(PM_FNAME);
   jcr->VolumeName[0] = 0;
   jcr->errmsg = GetPoolMemory(PM_MESSAGE);
   jcr->errmsg[0] = 0;
   jcr->comment = GetPoolMemory(PM_FNAME);
   jcr->comment[0] = 0;

   /*
    * Setup some dummy values
    */
   bstrncpy(jcr->Job, "*System*", sizeof(jcr->Job));
   jcr->JobId = 0;
   jcr->setJobType(JT_SYSTEM);           /* internal job until defined */
   jcr->setJobLevel(L_NONE);
   jcr->setJobStatus(JS_Created);        /* ready to run */
   sigtimer.sa_flags = 0;
   sigtimer.sa_handler = TimeoutHandler;
   sigfillset(&sigtimer.sa_mask);
   sigaction(TIMEOUT_SIGNAL, &sigtimer, NULL);

   /*
    * Locking jobs is a global lock that is needed
    * so that the Director can stop new jobs from being
    * added to the jcr chain while it processes a new
    * conf file and does the RegisterJobEndCallback().
    */
   LockJobs();
   lock_jcr_chain();
   if (!jcrs) {
      jcrs = New(dlist(jcr, &jcr->link));
   }
   jcrs->append(jcr);
   unlock_jcr_chain();
   UnlockJobs();

   return jcr;
}


/*
 * Remove a JobControlRecord from the chain
 *
 * NOTE! The chain must be locked prior to calling this routine.
 */
static void RemoveJcr(JobControlRecord *jcr)
{
   Dmsg0(debuglevel, "Enter RemoveJcr\n");
   if (!jcr) {
      Emsg0(M_ABORT, 0, _("NULL jcr.\n"));
   }
   jcrs->remove(jcr);
   Dmsg0(debuglevel, "Leave RemoveJcr\n");
}

/*
 * Free stuff common to all JCRs.  N.B. Be careful to include only
 * generic stuff in the common part of the jcr.
 */
static void FreeCommonJcr(JobControlRecord *jcr)
{
   Dmsg1(100, "FreeCommonJcr: %p \n", jcr);

   if (!jcr) {
      Dmsg0(100, "FreeCommonJcr: Invalid jcr\n");
   }

   /*
    * Uses jcr lock/unlock
    */
   RemoveJcrFromTsd(jcr);
   jcr->SetKillable(false);

   jcr->DestroyMutex();

   if (jcr->msg_queue) {
      delete jcr->msg_queue;
      jcr->msg_queue = nullptr;
      pthread_mutex_destroy(&jcr->msg_queue_mutex);
   }

   if (jcr->client_name) {
      FreePoolMemory(jcr->client_name);
      jcr->client_name = NULL;
   }

   if (jcr->attr) {
      FreePoolMemory(jcr->attr);
      jcr->attr = NULL;
   }

   if (jcr->sd_auth_key) {
      free(jcr->sd_auth_key);
      jcr->sd_auth_key = NULL;
   }

   if (jcr->VolumeName) {
      FreePoolMemory(jcr->VolumeName);
      jcr->VolumeName = NULL;
   }

   if (jcr->dir_bsock) {
      jcr->dir_bsock->close();
      delete jcr->dir_bsock;
      jcr->dir_bsock = NULL;
   }

   if (jcr->errmsg) {
      FreePoolMemory(jcr->errmsg);
      jcr->errmsg = NULL;
   }

   if (jcr->where) {
      free(jcr->where);
      jcr->where = NULL;
   }

   if (jcr->RegexWhere) {
      free(jcr->RegexWhere);
      jcr->RegexWhere = NULL;
   }

   if (jcr->where_bregexp) {
      FreeBregexps(jcr->where_bregexp);
      delete jcr->where_bregexp;
      jcr->where_bregexp = NULL;
   }

   if (jcr->cached_path) {
      FreePoolMemory(jcr->cached_path);
      jcr->cached_path = NULL;
      jcr->cached_pnl = 0;
   }

   if (jcr->id_list) {
      FreeGuidList(jcr->id_list);
      jcr->id_list = NULL;
   }

   if (jcr->comment) {
      FreePoolMemory(jcr->comment);
      jcr->comment = NULL;
   }

   free(jcr);
}

/*
 * Global routine to free a jcr
 */
#ifdef DEBUG
void b_free_jcr(const char *file, int line, JobControlRecord *jcr)
{
   struct s_last_job *je;

   Dmsg3(debuglevel, "Enter FreeJcr jid=%u from %s:%d\n", jcr->JobId, file, line);

#else

void FreeJcr(JobControlRecord *jcr)
{
   struct s_last_job *je;

   Dmsg3(debuglevel, "Enter FreeJcr jid=%u UseCount=%d Job=%s\n",
         jcr->JobId, jcr->UseCount(), jcr->Job);

#endif

   lock_jcr_chain();
   jcr->DecUseCount();              /* decrement use count */
   if (jcr->UseCount() < 0) {
      Jmsg2(jcr, M_ERROR, 0, _("JobControlRecord UseCount=%d JobId=%d\n"),
         jcr->UseCount(), jcr->JobId);
   }
   if (jcr->JobId > 0) {
      Dmsg3(debuglevel, "Dec FreeJcr jid=%u UseCount=%d Job=%s\n",
         jcr->JobId, jcr->UseCount(), jcr->Job);
   }
   if (jcr->UseCount() > 0) {          /* if in use */
      unlock_jcr_chain();
      return;
   }
   if (jcr->JobId > 0) {
      Dmsg3(debuglevel, "remove jcr jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
   }
   RemoveJcr(jcr);                   /* remove Jcr from chain */
   unlock_jcr_chain();

   DequeueMessages(jcr);
   CallJobEndCallbacks(jcr);                  /* call registered callbacks */

   Dmsg1(debuglevel, "End job=%d\n", jcr->JobId);

   /*
    * Keep some statistics
    */
   switch (jcr->getJobType()) {
   case JT_BACKUP:
   case JT_VERIFY:
   case JT_RESTORE:
   case JT_MIGRATE:
   case JT_COPY:
   case JT_ADMIN:
      /*
       * Keep list of last jobs, but not Console where JobId==0
       */
      if (jcr->JobId > 0) {
         LockLastJobsList();
         num_jobs_run++;
         je = (struct s_last_job *)malloc(sizeof(struct s_last_job));
         memset(je, 0, sizeof(struct s_last_job));  /* zero in case unset fields */
         je->Errors = jcr->JobErrors;
         je->JobType = jcr->getJobType();
         je->JobId = jcr->JobId;
         je->VolSessionId = jcr->VolSessionId;
         je->VolSessionTime = jcr->VolSessionTime;
         bstrncpy(je->Job, jcr->Job, sizeof(je->Job));
         je->JobFiles = jcr->JobFiles;
         je->JobBytes = jcr->JobBytes;
         je->JobStatus = jcr->JobStatus;
         je->JobLevel = jcr->getJobLevel();
         je->start_time = jcr->start_time;
         je->end_time = time(NULL);

         if (!last_jobs) {
            InitLastJobsList();
         }
         last_jobs->append(je);
         if (last_jobs->size() > max_last_jobs) {
            je = (struct s_last_job *)last_jobs->first();
            last_jobs->remove(je);
            free(je);
         }
         UnlockLastJobsList();
      }
      break;
   default:
      break;
   }

   CloseMsg(jcr);                    /* close messages for this job */

   if (jcr->daemon_free_jcr) {
      jcr->daemon_free_jcr(jcr);      /* call daemon free routine */
   }

   FreeCommonJcr(jcr);
   CloseMsg(NULL);                   /* flush any daemon messages */
   Dmsg0(debuglevel, "Exit FreeJcr\n");
}

void JobControlRecord::SetKillable(bool killable)
{
   lock();

   my_thread_killable = killable;
   if (killable) {
      my_thread_id = pthread_self();
   } else {
      memset(&my_thread_id, 0, sizeof(my_thread_id));
   }

   unlock();
}

void JobControlRecord::MyThreadSendSignal(int sig)
{
   lock();

   if (IsKillable() && !pthread_equal(my_thread_id, pthread_self())) {
      Dmsg1(800, "Send kill to jid=%d\n", JobId);
      pthread_kill(my_thread_id, sig);
   } else if (!IsKillable()) {
      Dmsg1(10, "Warning, can't send kill to jid=%d\n", JobId);
   }

   unlock();
}

/*
 * Remove jcr from thread specific data, but but make sure it is us who are attached.
 */
void RemoveJcrFromTsd(JobControlRecord *jcr)
{
   JobControlRecord *tjcr = get_jcr_from_tsd();

   if (tjcr == jcr) {
      SetJcrInTsd(INVALID_JCR);
   }
}

/*
 * Put this jcr in the thread specifc data
 */
void SetJcrInTsd(JobControlRecord *jcr)
{
   int status;

   status = pthread_setspecific(jcr_key, (void *)jcr);
   if (status != 0) {
      BErrNo be;
      Jmsg1(jcr, M_ABORT, 0, _("pthread_setspecific failed: ERR=%s\n"),
            be.bstrerror(status));
   }
}

/*
 * Give me the jcr that is attached to this thread
 */
JobControlRecord *get_jcr_from_tsd()
{
   JobControlRecord *jcr = (JobControlRecord *)pthread_getspecific(jcr_key);

   /*
    * Set any INVALID_JCR to NULL which the rest of BAREOS understands
    */
   if (jcr == INVALID_JCR) {
      jcr = NULL;
   }

   return jcr;
}

/*
 * Find which JobId corresponds to the current thread
 */
uint32_t GetJobidFromTsd()
{
   JobControlRecord *jcr = (JobControlRecord *)pthread_getspecific(jcr_key);
   uint32_t JobId = 0;

   if (jcr && jcr != INVALID_JCR) {
      JobId = (uint32_t)jcr->JobId;
   }

   return JobId;
}

/*
 * Given a JobId, find the JobControlRecord
 *
 * Returns: jcr on success
 *          NULL on failure
 */
JobControlRecord *get_jcr_by_id(uint32_t JobId)
{
   JobControlRecord *jcr;

   foreach_jcr(jcr) {
      if (jcr->JobId == JobId) {
         jcr->IncUseCount();
         Dmsg3(debuglevel, "Inc get_jcr jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
         break;
      }
   }
   endeach_jcr(jcr);

   return jcr;
}

/*
 * Given a thread id, find the JobId
 *
 * Returns: JobId on success
 *          0 on failure
 */
uint32_t GetJobidFromTid(pthread_t tid)
{
   JobControlRecord *jcr = NULL;
   bool found = false;

   foreach_jcr(jcr) {
      if (pthread_equal(jcr->my_thread_id, tid)) {
         found = true;
         break;
      }
   }
   endeach_jcr(jcr);

   if (found) {
      return jcr->JobId;
   }

   return 0;
}

/*
 * Given a SessionId and SessionTime, find the JobControlRecord
 *
 * Returns: jcr on success
 *          NULL on failure
 */
JobControlRecord *get_jcr_by_session(uint32_t SessionId, uint32_t SessionTime)
{
   JobControlRecord *jcr;

   foreach_jcr(jcr) {
      if (jcr->VolSessionId == SessionId &&
          jcr->VolSessionTime == SessionTime) {
         jcr->IncUseCount();
         Dmsg3(debuglevel, "Inc get_jcr jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
         break;
      }
   }
   endeach_jcr(jcr);

   return jcr;
}

/*
 * Given a Job, find the JobControlRecord compares on the number of
 * characters in Job thus allowing partial matches.
 *
 * Returns: jcr on success
 *          NULL on failure
 */
JobControlRecord *get_jcr_by_partial_name(char *Job)
{
   JobControlRecord *jcr;
   int len;

   if (!Job) {
      return NULL;
   }

   len = strlen(Job);
   foreach_jcr(jcr) {
      if (bstrncmp(Job, jcr->Job, len)) {
         jcr->IncUseCount();
         Dmsg3(debuglevel, "Inc get_jcr jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
         break;
      }
   }
   endeach_jcr(jcr);

   return jcr;
}


/*
 * Given a Job, find the JobControlRecord requires an exact match of names.
 *
 * Returns: jcr on success
 *          NULL on failure
 */
JobControlRecord *get_jcr_by_full_name(char *Job)
{
   JobControlRecord *jcr;

   if (!Job) {
      return NULL;
   }

   foreach_jcr(jcr) {
      if (bstrcmp(jcr->Job, Job)) {
         jcr->IncUseCount();
         Dmsg3(debuglevel, "Inc get_jcr jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
         break;
      }
   }
   endeach_jcr(jcr);

   return jcr;
}

static void UpdateWaitTime(JobControlRecord *jcr, int newJobStatus)
{
   bool enter_in_waittime;
   int oldJobStatus = jcr->JobStatus;

   switch (newJobStatus) {
   case JS_WaitFD:
   case JS_WaitSD:
   case JS_WaitMedia:
   case JS_WaitMount:
   case JS_WaitStoreRes:
   case JS_WaitJobRes:
   case JS_WaitClientRes:
   case JS_WaitMaxJobs:
   case JS_WaitPriority:
      enter_in_waittime = true;
      break;
   default:
      enter_in_waittime = false; /* not a Wait situation */
      break;
   }

   /*
    * If we were previously waiting and are not any more
    * we want to update the wait_time variable, which is
    * the start of waiting.
    */
   switch (oldJobStatus) {
   case JS_WaitFD:
   case JS_WaitSD:
   case JS_WaitMedia:
   case JS_WaitMount:
   case JS_WaitStoreRes:
   case JS_WaitJobRes:
   case JS_WaitClientRes:
   case JS_WaitMaxJobs:
   case JS_WaitPriority:
      if (!enter_in_waittime) { /* we get out the wait time */
         jcr->wait_time_sum += (time(NULL) - jcr->wait_time);
         jcr->wait_time = 0;
      }
      break;
   default:
      /*
       * If wait state is new, we keep current time for watchdog MaxWaitTime
       */
      if (enter_in_waittime) {
         jcr->wait_time = time(NULL);
      }
      break;
   }
}

/*
 * Priority runs from 0 (lowest) to 10 (highest)
 */
static int GetStatusPriority(int JobStatus)
{
   int priority = 0;

   switch (JobStatus) {
   case JS_Incomplete:
      priority = 10;
      break;
   case JS_ErrorTerminated:
   case JS_FatalError:
   case JS_Canceled:
      priority = 9;
      break;
   case JS_Error:
      priority = 8;
      break;
   case JS_Differences:
      priority = 7;
      break;
   }

   return priority;
}

/*
 * Send Job status to Director
 */
bool JobControlRecord::sendJobStatus()
{
   if (dir_bsock) {
      return dir_bsock->fsend(Job_status, Job, JobStatus);
   }

   return true;
}

/*
 * Set and send Job status to Director
 */
bool JobControlRecord::sendJobStatus(int newJobStatus)
{
   if (!is_JobStatus(newJobStatus)) {
      setJobStatus(newJobStatus);
      if (dir_bsock) {
         return dir_bsock->fsend(Job_status, Job, JobStatus);
      }
   }

   return true;
}

void JobControlRecord::setJobStarted()
{
   job_started = true;
   job_started_time = time(NULL);
}

void JobControlRecord::setJobStatus(int newJobStatus)
{
   int priority;
   int old_priority = 0;
   int oldJobStatus = ' ';

   if (JobStatus) {
      oldJobStatus = JobStatus;
      old_priority = GetStatusPriority(oldJobStatus);
   }
   priority = GetStatusPriority(newJobStatus);

   Dmsg2(800, "SetJcrJobStatus(%s, %c)\n", Job, newJobStatus);

   /*
    * Update wait_time depending on newJobStatus and oldJobStatus
    */
   UpdateWaitTime(this, newJobStatus);

   /*
    * For a set of errors, ... keep the current status
    * so it isn't lost. For all others, set it.
    */
   Dmsg2(800, "OnEntry JobStatus=%c newJobstatus=%c\n", oldJobStatus, newJobStatus);

   /*
    * If status priority is > than proposed new status, change it.
    * If status priority == new priority and both are zero, take the new status.
    * If it is not zero, then we keep the first non-zero "error" that occurred.
    */
   if (priority > old_priority || (
       priority == 0 && old_priority == 0)) {
      Dmsg4(800, "Set new stat. old: %c,%d new: %c,%d\n",
            oldJobStatus, old_priority, newJobStatus, priority);
      JobStatus = newJobStatus;     /* replace with new status */
   }

   if (oldJobStatus != JobStatus) {
      Dmsg2(800, "leave setJobStatus old=%c new=%c\n", oldJobStatus, newJobStatus);
//    GeneratePluginEvent(this, bEventStatusChange, NULL);
   }
}

#ifdef TRACE_JCR_CHAIN
static int lock_count = 0;
#endif

/*
 * Lock the chain
 */
#ifdef TRACE_JCR_CHAIN
static void b_lock_jcr_chain(const char *fname, int line)
#else
static void lock_jcr_chain()
#endif
{
#ifdef TRACE_JCR_CHAIN
   Dmsg3(debuglevel, "Lock jcr chain %d from %s:%d\n", ++lock_count, fname, line);
#endif
   P(jcr_lock);
}

/*
 * Unlock the chain
 */
#ifdef TRACE_JCR_CHAIN
static void b_unlock_jcr_chain(const char *fname, int line)
#else
static void unlock_jcr_chain()
#endif
{
#ifdef TRACE_JCR_CHAIN
   Dmsg3(debuglevel, "Unlock jcr chain %d from %s:%d\n", lock_count--, fname, line);
#endif
   V(jcr_lock);
}

/*
 * Start walk of jcr chain
 * The proper way to walk the jcr chain is:
 *    JobControlRecord *jcr;
 *    foreach_jcr(jcr) {
 *      ...
 *    }
 *    endeach_jcr(jcr);
 *
 * It is possible to leave out the endeach_jcr(jcr), but
 * in that case, the last jcr referenced must be explicitly
 * released with:
 *
 * FreeJcr(jcr);
 */
JobControlRecord *jcr_walk_start()
{
   JobControlRecord *jcr;
   lock_jcr_chain();
   jcr = (JobControlRecord *)jcrs->first();
   if (jcr) {
      jcr->IncUseCount();
      if (jcr->JobId > 0) {
         Dmsg3(debuglevel, "Inc walk_start jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
      }
   }
   unlock_jcr_chain();
   return jcr;
}

/*
 * Get next jcr from chain, and release current one
 */
JobControlRecord *jcr_walk_next(JobControlRecord *prev_jcr)
{
   JobControlRecord *jcr;

   lock_jcr_chain();
   jcr = (JobControlRecord *)jcrs->next(prev_jcr);
   if (jcr) {
      jcr->IncUseCount();
      if (jcr->JobId > 0) {
         Dmsg3(debuglevel, "Inc walk_next jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
      }
   }
   unlock_jcr_chain();
   if (prev_jcr) {
      FreeJcr(prev_jcr);
   }
   return jcr;
}

/*
 * Release last jcr referenced
 */
void JcrWalkEnd(JobControlRecord *jcr)
{
   if (jcr) {
      if (jcr->JobId > 0) {
         Dmsg3(debuglevel, "Free walk_end jid=%u UseCount=%d Job=%s\n",
            jcr->JobId, jcr->UseCount(), jcr->Job);
      }
      FreeJcr(jcr);
   }
}

/*
 * Return number of Jobs
 */
int JobCount()
{
   JobControlRecord *jcr;
   int count = 0;

   lock_jcr_chain();
   for (jcr = (JobControlRecord *)jcrs->first(); (jcr = (JobControlRecord *)jcrs->next(jcr)); ) {
      if (jcr->JobId > 0) {
         count++;
      }
   }
   unlock_jcr_chain();
   return count;
}


/*
 * Setup to call the timeout check routine every 30 seconds
 * This routine will check any timers that have been enabled.
 */
bool InitJcrSubsystem(int timeout)
{
   watchdog_t *wd = new_watchdog();

   watch_dog_timeout = timeout;
   wd->one_shot = false;
   wd->interval = 30;   /* FIXME: should be configurable somewhere, even
                         if only with a #define */
   wd->callback = JcrTimeoutCheck;

   RegisterWatchdog(wd);

   return true;
}

static void JcrTimeoutCheck(watchdog_t *self)
{
   JobControlRecord *jcr;
   BareosSocket *bs;
   time_t timer_start;

   Dmsg0(debuglevel, "Start JobControlRecord timeout checks\n");

   /* Walk through all JCRs checking if any one is
    * blocked for more than specified max time.
    */
   foreach_jcr(jcr) {
      Dmsg2(debuglevel, "JcrTimeoutCheck JobId=%u jcr=0x%x\n", jcr->JobId, jcr);
      if (jcr->JobId == 0) {
         continue;
      }
      bs = jcr->store_bsock;
      if (bs) {
         timer_start = bs->timer_start;
         if (timer_start && (watchdog_time - timer_start) > watch_dog_timeout) {
            bs->timer_start = 0;      /* turn off timer */
            bs->SetTimedOut();
            Qmsg(jcr, M_ERROR, 0, _(
                 "Watchdog sending kill after %d secs to thread stalled reading Storage daemon.\n"),
                 watchdog_time - timer_start);
            jcr->MyThreadSendSignal(TIMEOUT_SIGNAL);
         }
      }
      bs = jcr->file_bsock;
      if (bs) {
         timer_start = bs->timer_start;
         if (timer_start && (watchdog_time - timer_start) > watch_dog_timeout) {
            bs->timer_start = 0;      /* turn off timer */
            bs->SetTimedOut();
            Qmsg(jcr, M_ERROR, 0, _(
                 "Watchdog sending kill after %d secs to thread stalled reading File daemon.\n"),
                 watchdog_time - timer_start);
            jcr->MyThreadSendSignal(TIMEOUT_SIGNAL);
         }
      }
      bs = jcr->dir_bsock;
      if (bs) {
         timer_start = bs->timer_start;
         if (timer_start && (watchdog_time - timer_start) > watch_dog_timeout) {
            bs->timer_start = 0;      /* turn off timer */
            bs->SetTimedOut();
            Qmsg(jcr, M_ERROR, 0, _(
                 "Watchdog sending kill after %d secs to thread stalled reading Director.\n"),
                 watchdog_time - timer_start);
            jcr->MyThreadSendSignal(TIMEOUT_SIGNAL);
         }
      }
   }
   endeach_jcr(jcr);

   Dmsg0(debuglevel, "Finished JobControlRecord timeout checks\n");
}

/*
 * Return next JobId from comma separated list
 *
 * Returns:
 *   1 if next JobId returned
 *   0 if no more JobIds are in list
 *  -1 there is an error
 */
int GetNextJobidFromList(char **p, uint32_t *JobId)
{
   const int maxlen = 30;
   char jobid[maxlen+1];
   char *q = *p;

   jobid[0] = 0;
   for (int i=0; i<maxlen; i++) {
      if (*q == 0) {
         break;
      } else if (*q == ',') {
         q++;
         break;
      }
      jobid[i] = *q++;
      jobid[i+1] = 0;
   }
   if (jobid[0] == 0) {
      return 0;
   } else if (!Is_a_number(jobid)) {
      return -1;                      /* error */
   }
   *p = q;
   *JobId = str_to_int64(jobid);
   return 1;
}

/*
 * Timeout signal comes here
 */
extern "C" void TimeoutHandler(int sig)
{
   return;                            /* thus interrupting the function */
}

/*
 * Used to display specific daemon information after a fatal signal
 * (like BareosDb in the director)
 */
#define MAX_DBG_HOOK 10
static dbg_jcr_hook_t *dbg_jcr_hooks[MAX_DBG_HOOK];
static int dbg_jcr_handler_count;

void DbgJcrAddHook(dbg_jcr_hook_t *hook)
{
   ASSERT(dbg_jcr_handler_count < MAX_DBG_HOOK);
   dbg_jcr_hooks[dbg_jcr_handler_count++] = hook;
}

/*
 * !!! WARNING !!!
 *
 * This function should be used ONLY after a fatal signal. We walk through the
 * JobControlRecord chain without doing any lock, BAREOS should not be running.
 */
void DbgPrintJcr(FILE *fp)
{
   char ed1[50], buf1[128], buf2[128], buf3[128], buf4[128];
   if (!jcrs) {
      return;
   }

   fprintf(fp, "Attempt to dump current JCRs. njcrs=%d\n", jcrs->size());

   for (JobControlRecord *jcr = (JobControlRecord *)jcrs->first(); jcr ; jcr = (JobControlRecord *)jcrs->next(jcr)) {
      fprintf(fp, "threadid=%s JobId=%d JobStatus=%c jcr=%p name=%s\n",
              edit_pthread(jcr->my_thread_id, ed1, sizeof(ed1)),
              (int)jcr->JobId, jcr->JobStatus, jcr, jcr->Job);
      fprintf(fp, "threadid=%s killable=%d JobId=%d JobStatus=%c jcr=%p name=%s\n",
              edit_pthread(jcr->my_thread_id, ed1, sizeof(ed1)),
              jcr->IsKillable(), (int)jcr->JobId, jcr->JobStatus, jcr, jcr->Job);
      fprintf(fp, "\tUseCount=%i\n", jcr->UseCount());
      fprintf(fp, "\tJobType=%c JobLevel=%c\n",
              jcr->getJobType(), jcr->getJobLevel());
      bstrftime(buf1, sizeof(buf1), jcr->sched_time);
      bstrftime(buf2, sizeof(buf2), jcr->start_time);
      bstrftime(buf3, sizeof(buf3), jcr->end_time);
      bstrftime(buf4, sizeof(buf4), jcr->wait_time);
      fprintf(fp, "\tsched_time=%s start_time=%s\n\tend_time=%s wait_time=%s\n",
              buf1, buf2, buf3, buf4);
      fprintf(fp, "\tdb=%p db_batch=%p batch_started=%i\n",
              jcr->db, jcr->db_batch, jcr->batch_started);

      /*
       * Call all the jcr debug hooks
       */
      for(int i=0; i < dbg_jcr_handler_count; i++) {
         dbg_jcr_hook_t *hook = dbg_jcr_hooks[i];
         hook(jcr, fp);
      }
   }
}

JobControlRecord::JobControlRecord()
   : mutex(PTHREAD_MUTEX_INITIALIZER)
   , _use_count(0)
   , JobType_(0)
   , JobLevel_(0)
   , Protocol_(0)
   , my_thread_killable(false)
   , my_thread_id(0)
   , dir_bsock(nullptr)
   , store_bsock(nullptr)
   , file_bsock(nullptr)
   , daemon_free_jcr(nullptr)
   , msg_queue(nullptr)
   , msg_queue_mutex(PTHREAD_MUTEX_INITIALIZER)
   , dequeuing_msgs(false)
   , VolumeName(nullptr)
   , errmsg(nullptr)

   , JobId(0)
   , VolSessionId(0)
   , VolSessionTime(0)
   , JobFiles(0)
   , JobErrors(0)
   , JobWarnings(0)
   , LastRate(0)
   , JobBytes(0)
   , LastJobBytes(0)
   , ReadBytes(0)
   , FileId(0)
   , JobStatus(0)
   , JobPriority(0)
   , sched_time(0)
   , initial_sched_time(0)
   , start_time(0)
   , run_time(0)
   , last_time(0)
   , end_time(0)
   , wait_time_sum(0)
   , wait_time(0)
   , job_started_time(0)
   , client_name(nullptr)
   , JobIds(nullptr)
   , RestoreBootstrap(nullptr)
   , stime(nullptr)
   , sd_auth_key(nullptr)
   , jcr_msgs(nullptr)
   , ClientId(0)
   , where(nullptr)
   , RegexWhere(nullptr)
   , where_bregexp(nullptr)
   , cached_pnl(0)
   , cached_path(nullptr)
   , passive_client(false)
   , prefix_links(false)
   , gui(false)
   , authenticated(false)
   , cached_attribute(false)
   , batch_started(false)
   , cmd_plugin(false)
   , opt_plugin(false)
   , keep_path_list(false)
   , accurate(false)
   , HasBase(false)
   , rerunning(false)
   , job_started(false)
   , suppress_output(false)
   , cjcr(0)

   , buf_size(0)
#ifdef HAVE_WIN32
   , cp_thread(nullptr)
#endif
   , attr(nullptr)
   , db(nullptr)
   , db_batch(nullptr)
   , nb_base_files(0)
   , nb_base_files_used(0)

   , ar(nullptr)
   , id_list(nullptr)

   , plugin_ctx_list(nullptr)
   , plugin_ctx(nullptr)
   , plugin_sp(nullptr)
   , comment(nullptr)
   , max_bandwidth(0)
   , path_list(nullptr)

#ifdef DIRECTOR_DAEMON
   , SD_msg_chan(0)
   , SD_msg_chan_started(false)
   , start_wait(PTHREAD_COND_INITIALIZER)
   , term_wait(PTHREAD_COND_INITIALIZER)
   , nextrun_ready(PTHREAD_COND_INITIALIZER)
   , work_item(nullptr)
   , ua(nullptr)
   , restore_tree_root(nullptr)
   , bsr(nullptr)
   , backup_format(nullptr)
   , plugin_options(nullptr)
   , SDJobFiles(0)
   , SDJobBytes(0)
   , SDErrors(0)
   , SDJobStatus(0)
   , FDJobStatus(0)
   , DumpLevel(0)
   , ExpectedFiles(0)
   , MediaId(0)
   , FileIndex(0)
   , MaxRunSchedTime(0)
   , mig_jcr(nullptr)


   , fname(nullptr)
   , client_uname(nullptr)
   , FDSecureEraseCmd(nullptr)
   , SDSecureEraseCmd(nullptr)
   , vf_jobids(nullptr)
   , replace(0)
   , NumVols(0)
   , reschedule_count(0)
   , FDVersion(0)
   , spool_size(0)
   , sd_msg_thread_done(false)
   , IgnoreDuplicateJobChecking(false)
   , IgnoreLevelPoolOverides(false)
   , IgnoreClientConcurrency(false)
   , IgnoreStorageConcurrency(false)
   , spool_data(false)
   , acquired_resource_locks(false)
   , start_wait_inited(false)
   , term_wait_inited(false)
   , nextrun_ready_inited(false)
   , fn_printed(false)
   , needs_sd(false)
   , cloned(false)
   , unlink_bsr(false)
   , VSS(false)
   , Encrypt(false)
   , stats_enabled(false)
   , no_maxtime(false)
   , keep_sd_auth_key(false)
   , use_accurate_chksum(false)
   , sd_canceled(false)
   , remote_replicate(false)
   , RescheduleIncompleteJobs(false)
   , HasQuota(false)
   , HasSelectedJobs(false)
#endif /* DIRECTOR_DAEMON */
{
      Dmsg0(100, "Contruct JobControlRecord\n");
      job_end_callbacks.init();
      Job[0] = 0;
#ifdef DIRECTOR_DAEMON
      FSCreateTime[0] = 0;
      since[0] = 0;
      PrevJob[0] = 0;
      RestoreJobId = 0;
      MigrateJobId = 0;
      VerifyJobId = 0;
#endif
};

JobControlRecord::~JobControlRecord()
{
      Dmsg0(100, "Destruct JobControlRecord\n");
}

