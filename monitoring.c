


#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "arch/timing.h"

#include "monitoring.h"
#include "worker.h"
#include "task.h"
#include "stream.h"


#define MON_TASKNAME_MAXLEN  32

struct monctx_t {
  int           wid;         /** worker id */
  FILE         *outfile;     /** where to write the monitoring data to */
  unsigned int  disp;        /** count how often a task has been dispatched */
  int           debug_level;
  unsigned int  wait_cnt;
  timing_t      wait_total;
  timing_t      wait_current;
};


struct mon_task_t {
  char name[MON_TASKNAME_MAXLEN];
  monctx_t *ctx;
  unsigned long tid;
  unsigned long flags; /** monitoring flags */
  unsigned long disp;  /** dispatch counter */
  struct {
    timing_t creat; /** task creation time */
    timing_t total; /** total execution time of the task */
    timing_t start; /** start time of last dispatch */
    timing_t stop;  /** stop time of last dispatch */
  } times;
  mon_stream_t *dirty_list; /** head of dirty stream list */
  char blockon;     /** for convenience: tracking if blocked 
                        on read or write or any */
};


struct mon_stream_t {
  mon_task_t   *montask;     /** Invariant: != NULL */
  mon_stream_t *dirty;       /** for maintaining a dirty stream list */
  char          mode;        /** either 'r' or 'w' */
  char          state;       /** one if IOCR */
  unsigned int  sid;         /** copy of the stream uid */
  unsigned long counter;     /** number of items processed */
  unsigned int  event_flags; /** events "?!*" */
};

/**
 * The state of a stream descriptor
 */
#define ST_INUSE    'I'
#define ST_OPENED   'O'
#define ST_CLOSED   'C'
#define ST_REPLACED 'R'

/**
 * The event_flags of a stream descriptor
 */
#define ST_MOVED    (1<<0)
#define ST_WAKEUP   (1<<1)
#define ST_BLOCKON  (1<<2)

/**
 * This special value indicates the end of the dirty list chain.
 * NULL cannot be used as NULL indicates that the SD is not dirty.
 */
#define ST_DIRTY_END   ((mon_stream_t *)-1)



/**
 * Prefix/postfix for monitoring outfiles
 */
#define MON_PFIX_LEN  16
static char monitoring_prefix[MON_PFIX_LEN];
static char monitoring_postfix[MON_PFIX_LEN];

#define MON_NAME_LEN  31

/**
 * Reference timestamp
 */
static timing_t monitoring_begin = TIMING_INITIALIZER;



/*****************************************************************************
 * HELPER FUNCTIONS
 ****************************************************************************/

/**
 * Macros for checking flags
 */
#define FLAG_TIMES(mt)    (mt->flags & LPEL_MON_TASK_TIMES)
#define FLAG_STREAMS(mt)  (mt->flags & LPEL_MON_TASK_STREAMS)

/**
 * Print a time in usec
 */
static inline void PrintTiming( const timing_t *t, FILE *file)
{
  if (t->tv_sec == 0) {
    (void) fprintf( file, "%lu ", t->tv_nsec / 1000);
  } else {
    (void) fprintf( file, "%lu%06lu ",
        (unsigned long) t->tv_sec, (t->tv_nsec / 1000)
        );
  }
}


/**
 * Add a stream monitor object to the dirty list of its task.
 * It is only added to the dirty list once.
 */
static inline void MarkDirty( mon_stream_t *ms)
{
  mon_task_t *mt = ms->montask;
  /*
   * only add if not dirty yet
   */
  if ( ms->dirty == NULL ) {
    /*
     * Set the dirty ptr of ms to the dirty_list ptr of montask
     * and the dirty_list ptr of montask to ms, i.e.,
     * insert the ms at the front of the dirty_list.
     * Initially, dirty_list of montask is empty (ST_DIRTY_END, != NULL)
     */
    ms->dirty = mt->dirty_list;
    mt->dirty_list = ms;
  }
}


/**
 * Print the dirty list of the task
 */
static void PrintDirtyList(mon_task_t *mt)
{
  mon_stream_t *ms, *next;
  FILE *file = mt->ctx->outfile;

  ms = mt->dirty_list;

  while (ms != ST_DIRTY_END) {
    /* all elements in the dirty list must belong to same task */
    assert( ms->montask == mt );

    /* now print */
    (void) fprintf( file,
        "%u,%c,%c,%lu,%c%c%c;",
        ms->sid, ms->mode, ms->state, ms->counter,
        ( ms->event_flags & ST_BLOCKON) ? '?':'-',
        ( ms->event_flags & ST_WAKEUP) ? '!':'-',
        ( ms->event_flags & ST_MOVED ) ? '*':'-'
        );


    /* get the next dirty entry, and clear the link in the current entry */
    next = ms->dirty;

    /* update/reset states */
    switch (ms->state) {
      case ST_OPENED:
      case ST_REPLACED:
        ms->state = ST_INUSE;
        /* fall-through */
      case ST_INUSE:
        ms->dirty = NULL;
        ms->event_flags = 0;
        break;

      case ST_CLOSED:
        /* eventually free the mon_stream_t of the closed stream */
        free(ms);
        break;

      default: assert(0);
    }
    ms = next;
  }

  /* dirty list of task is empty */
  mt->dirty_list = ST_DIRTY_END;
}

/*****************************************************************************
 * PUBLIC FUNCTIONS
 ****************************************************************************/


/**
 * Initialize the monitoring module
 *
 * @param prefix    prefix of the monitoring context files
 * @param postfix   postfix of the monitoring context files
 * @pre             prefix == NULL  ||  strlen(prefix)  <= MON_PFIX_LEN
 * @pre             postfix == NULL ||  strlen(postfix) <= MON_PFIX_LEN
 */
void LpelMonInit(char *prefix, char *postfix)
{

  /* store the prefix */
  (void) memset(monitoring_prefix,  0, MON_PFIX_LEN);
  if ( prefix != NULL ) {
    (void) strncpy( monitoring_prefix, prefix, MON_PFIX_LEN);
  }

  /* store the postfix */
  (void) memset(monitoring_postfix, 0, MON_PFIX_LEN);
  if ( postfix != NULL ) {
    (void) strncpy( monitoring_postfix, postfix, MON_PFIX_LEN);
  }

  /* initialize timing */
  TIMESTAMP(&monitoring_begin);
}


/**
 * Cleanup the monitoring module 
 */
void LpelMonCleanup(void)
{
  /* NOP */
}



/**
 * Create a monitoring context (for a worker)
 *
 * @param wid   worker id
 * @param name  name of monitoring context,
 *              filename where the information is logged
 * @pre         name != NULL && strlen(name) <= MON_NAME_LEN
 *
 * @return a newly created monitoring context
 */
monctx_t *LpelMonContextCreate(int wid, char *name)
{
  int buflen = MON_PFIX_LEN*2 + MON_NAME_LEN + 1;
  char fname[buflen];

  monctx_t *mon = (monctx_t *) malloc( sizeof(monctx_t));

  mon->wid = wid;

  /* build filename */
  memset(fname, 0, buflen);
  strncat(fname, monitoring_prefix, MON_PFIX_LEN);
  strncat(fname, name, MON_NAME_LEN);
  strncat(fname, monitoring_postfix, MON_PFIX_LEN);

  /* open logfile */
  mon->outfile = fopen(fname, "w");
  assert( mon->outfile != NULL);

  /* default values */
  mon->disp = 0;
  mon->debug_level = 0; /* no debug printing */
  mon->wait_cnt = 0;
  TimingZero(&mon->wait_total);
  TimingZero(&mon->wait_current);


  /* start message */
  //FIXME
  if (wid<0) {
    //LpelMonDebug( wc->mon, "Wrapper %s started.\n", taskname);
  } else {
    //LpelMonDebug( wc->mon, "Worker %d started.\n", wc->wid);
  }

  return mon;
}


/**
 * Destroy a monitoring context
 *
 * @param mon the monitoring context to destroy
 */
void LpelMonContextDestroy(monctx_t *mon)
{
  /*FIXME exit message
    LpelMonDebug( wc->mon,
        "Worker %d exited. wait_cnt %u, wait_time %lu.%09lu\n",
        wc->wid,
        wc->wait_cnt, 
        (unsigned long) wc->wait_time.tv_sec, wc->wait_time.tv_nsec
        );
   */
  if ( mon->outfile != NULL) {
    int ret;
    ret = fclose( mon->outfile);
    assert(ret == 0);
  }

  free( mon);
}



mon_task_t *LpelMonTaskCreate(unsigned long tid, char *name, unsigned long flags)
{
  mon_task_t *mt = malloc( sizeof(mon_task_t) );

  /* zero out everything */
  memset(mt, 0, sizeof(mon_task_t));
  
  /* copy name and 0-terminate */
  if ( name != NULL ) {
    (void) strncpy(mt->name, name, MON_TASKNAME_MAXLEN);
    mt->name[MON_TASKNAME_MAXLEN-1] = '\0';
  }

  mt->ctx = NULL;
  mt->tid = tid;
  mt->flags = flags;
  mt->disp = 0;

  mt->dirty_list = ST_DIRTY_END;

  if FLAG_TIMES(mt) {
    timing_t tnow;
    TIMESTAMP(&tnow);
    TimingDiff(&mt->times.creat, &monitoring_begin, &tnow);
  }
  return mt;
}


void LpelMonTaskDestroy(mon_task_t *mt)
{
  assert( mt != NULL );
  free(mt);
}


char *LpelMonTaskGetName(mon_task_t *mt)
{
  return mt->name;
}


void LpelMonTaskAssign(mon_task_t *mt, monctx_t *ctx)
{
  assert( mt != NULL );
  assert( mt->ctx == NULL );
  mt->ctx = ctx;
}





/*****************************************************************************
 * CALLBACK FUNCTIONS
 ****************************************************************************/



void LpelMonWorkerWaitStart(monctx_t *mon)
{
  mon->wait_cnt++;
  TimingStart(&mon->wait_current);
}


void LpelMonWorkerWaitStop(monctx_t *mon)
{
  TimingEnd(&mon->wait_current);
  TimingAdd(&mon->wait_total, &mon->wait_current);
  /*FIXME print message?
  LpelMonitoringDebug( wc->mon,
      "worker %d waited (%u) for %lu.%09lu\n",
      wc->wid,
      wc->wait_cnt,
      (unsigned long) wtm.tv_sec, wtm.tv_nsec
      );
  */
}



void LpelMonTaskStart(mon_task_t *mt)
{
  assert( mt != NULL );
  if FLAG_TIMES(mt) {
    TIMESTAMP(&mt->times.start);
  }

  /* set blockon to any */
  mt->blockon = 'a';

  /* increment dispatch counter of task */
  mt->disp++;
  /* increment task dispatched counter of monitoring context */
  mt->ctx->disp++;
}




void LpelMonTaskStop(mon_task_t *mt, taskstate_t state)
{
  FILE *file = mt->ctx->outfile;
  timing_t et, norm_ts;
  assert( mt != NULL );


  if FLAG_TIMES(mt) {
    TIMESTAMP(&mt->times.stop);
    TimingDiff(&norm_ts, &monitoring_begin, &mt->times.stop);

    
    PrintTiming(&norm_ts, file);
  }

  /* print general info: name, disp.cnt, state */
  fprintf( file, "%lu ", mt->tid);
  if (strlen(mt->name) > 0) {
    fprintf( file, "%s ", mt->name);
  }
  
  fprintf( file, "disp %lu ", mt->disp);

  if ( state==TASK_BLOCKED) {
    fprintf( file, "st B%c ", mt->blockon);
  } else {
    fprintf( file, "st %c ", state);
  }

  /* print times */
  if FLAG_TIMES(mt) {
    fprintf( file, "et ");
    /* execution time */
    TimingDiff(&et, &mt->times.start, &mt->times.stop);
    /* update total execution time */
    TimingAdd(&mt->times.total, &et);

    PrintTiming( &et , file);
    if ( state == TASK_ZOMBIE) {
      fprintf( file, "creat ");
      PrintTiming( &mt->times.creat, file);
    }
  }

  /* print stream info */
  if FLAG_STREAMS(mt) {
    fprintf( file,"[" );
    
    /* print (and reset) dirty list */
    PrintDirtyList(mt);

    fprintf( file,"] " );
  }

  fprintf( file, "\n");
  //fflush( file);
}




mon_stream_t *LpelMonStreamOpen(mon_task_t *mt, unsigned int sid, char mode)
{
  
  if (mt && FLAG_STREAMS(mt)) {
    mon_stream_t *ms = malloc(sizeof(mon_stream_t));
    ms->sid = sid;
    ms->montask = mt;
    ms->mode = mode;
    ms->state = ST_OPENED;
    ms->counter = 0;
    ms->event_flags = 0;
    ms->dirty = NULL;

    MarkDirty(ms);

    return ms;
  }
  return NULL;
}

/**
 * @pre ms != NULL
 */
void LpelMonStreamClose(mon_stream_t *ms)
{
  assert( ms != NULL );
  ms->state = ST_CLOSED;
  MarkDirty(ms);
  /* do not free ms, as it will be kept until its monintoring
     information has been output via dirty list upon TaskStop() */
}



void LpelMonStreamReplace(mon_stream_t *ms, unsigned int new_sid)
{
  assert( ms != NULL );
  ms->state = ST_REPLACED;
  ms->sid = new_sid;
  MarkDirty(ms);
}


/**
 * @pre ms != NULL
 */
void LpelMonStreamMoved(mon_stream_t *ms, void *item)
{
  assert( ms != NULL );

  ms->counter++;
  ms->event_flags |= ST_MOVED;
  MarkDirty(ms);
}



/**
 * @pre ms != NULL
 */
void LpelMonStreamBlockon(mon_stream_t *ms)
{
  assert( ms != NULL );
  ms->event_flags |= ST_BLOCKON;
  MarkDirty(ms);

  /* track if blocked on reading or writing */
  switch(ms->mode) {
  case 'r': ms->montask->blockon = 'i'; break;
  case 'w': ms->montask->blockon = 'o'; break;
  default: assert(0);
  }
}


/**
 * @pre ms != NULL
 */
void LpelMonStreamWakeup(mon_stream_t *ms)
{
  assert( ms != NULL );
  ms->event_flags |= ST_WAKEUP;

  /* MarkDirty() not needed, as Moved()
   * event is called anyway
   */
  //MarkDirty(ms);
}


void LpelMonDebug( monctx_t *mon, const char *fmt, ...)
{
  timing_t tnow, ts;
  va_list ap;

  if (!mon) return;

  /* print current timestamp */
  //TODO check if timestamping required
  TIMESTAMP(&tnow);
  TimingDiff(&ts, &monitoring_begin, &tnow);
  PrintTiming( &ts, mon->outfile);
  fprintf( mon->outfile, "*** ");

  va_start(ap, fmt);
  vfprintf( mon->outfile, fmt, ap);
  fflush(mon->outfile);
  va_end(ap);
}


#if 0

static void PrintWorkerCtx( workerctx_t *wc, FILE *file)
{
  if ( wc->wid < 0) {
    (void) fprintf(file, "loop %u ", wc->loop);
  } else {
    (void) fprintf(file, "wid %d loop %u ", wc->wid, wc->loop);
  }
}



#endif
