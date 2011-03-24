/**
 * Main LPEL module
 *
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <sched.h>
#include <unistd.h>  /* sysconf() */
#include <sys/types.h> /* pid_t */
#include <sys/syscall.h>

#include <pthread.h> /* worker threads are OS threads */
#include <pcl.h>     /* tasks are executed in user-space with help of
                        GNU Portable Coroutine Library  */

#include "lpel_main.h"

/*!! link with -lcap */
#if defined(__linux__) && defined(LPEL_USE_CAPABILITIES)
#  include <sys/capability.h>
#endif

/* macro using syscall for gettid, as glibc doesn't provide a wrapper */
#define gettid() syscall( __NR_gettid )


#include "worker.h"



/* Keep copy of the (checked) configuration provided at LpelInit() */
lpel_config_t    _lpel_global_config;

/* test if flags are set in lpel config */
#define LPEL_ICFG(f)   ( (_lpel_global_config.flags & (f)) == (f) )

#ifdef __linux__
/* cpuset for others-threads */
static cpu_set_t cpuset_others;

/*
 * cpuset for workers = [0,proc_workers-1]
 * is only used if not FLAG_PINNED is set
 */
static cpu_set_t cpuset_workers;
#endif /* __linux__ */



/**
 * Get the number of available cores
 */
int LpelGetNumCores( int *result)
{
  int proc_avail = -1;
  /* query the number of CPUs */
  proc_avail = sysconf(_SC_NPROCESSORS_ONLN);
  if (proc_avail == -1) {
    /* _SC_NPROCESSORS_ONLN not available! */
    return LPEL_ERR_FAIL;
  }
  *result = proc_avail;
  return 0;
}

int LpelCanSetExclusive( int *result)
{
#if defined(__linux__) && defined(LPEL_USE_CAPABILITIES)
  cap_t caps;
  cap_flag_value_t cap;
  /* obtain caps of process */
  caps = cap_get_proc();
  if (caps == NULL) {
    return LPEL_ERR_FAIL;
  }
  cap_get_flag(caps, CAP_SYS_NICE, CAP_EFFECTIVE, &cap);
  *result = (cap == CAP_SET);
  return 0;
#else
  return LPEL_ERR_FAIL;
#endif
}





static int CheckConfig( void)
{
  lpel_config_t *cfg = &_lpel_global_config;
  int proc_avail;

  /* input sanity checks */
  if ( cfg->num_workers <= 0 ||  cfg->proc_workers <= 0 ) {
    return LPEL_ERR_INVAL;
  }
  if ( cfg->proc_others < 0 ) {
    return LPEL_ERR_INVAL;
  }

  /* check if there are enough processors (if we can check) */
  if (0 == LpelGetNumCores( &proc_avail)) {
    if (cfg->proc_workers + cfg->proc_others > proc_avail) {
      return LPEL_ERR_INVAL;
    }
    /* check exclusive flag sanity */
    if ( LPEL_ICFG( LPEL_FLAG_EXCLUSIVE) ) {
      /* check if we can do a 1-1 mapping */
      /*if ( (cfg->proc_others== 0) || (cfg->num_workers!=cfg->proc_workers) ) {
        return LPEL_ERR_INVAL;
      }*/
    }
  }

  /* additional flags for exclusive flag */
  if ( LPEL_ICFG( LPEL_FLAG_EXCLUSIVE) ) {
    int can_rt;
    /* pinned flag must also be set */
    if ( !LPEL_ICFG( LPEL_FLAG_PINNED) ) {
      return LPEL_ERR_INVAL;
    }
    /* check permissions to set exclusive (if we can check) */
    if ( 0==LpelCanSetExclusive(&can_rt) && !can_rt ) {
      return LPEL_ERR_EXCL;
    }
  }

  return 0;
}


static void CreateCpusets( void)
{
  #ifdef __linux__
  lpel_config_t *cfg = &_lpel_global_config;
  int  i;

  /* create the cpu_set for worker threads */
  CPU_ZERO( &cpuset_workers );
  for (i=0; i<cfg->proc_workers; i++) {
    CPU_SET(i, &cpuset_workers);
  }

  /* create the cpu_set for other threads */
  CPU_ZERO( &cpuset_others );
  if (cfg->proc_others == 0) {
    /* distribute on the workers */
    for (i=0; i<cfg->proc_workers; i++) {
      CPU_SET(i, &cpuset_others);
    }
  } else {
    /* set to proc_others */
    for( i=cfg->proc_workers;
        i<cfg->proc_workers+cfg->proc_others;
        i++ ) {
      CPU_SET(i, &cpuset_others);
    }
  }
  #endif /* __linux__ */
}



/**
 * Initialise the LPEL
 *
 *  num_workers, proc_workers > 0
 *  proc_others >= 0
 *
 *
 * EXCLUSIVE: only valid, if
 *       #proc_avail >= proc_workers + proc_others &&
 *       proc_others != 0 &&
 *       num_workers == proc_workers 
 *
 */
int LpelInit( lpel_config_t *cfg)
{
  workercfg_t worker_config;
  int res;

  /* store a local copy of cfg */
  _lpel_global_config = *cfg;
  
  /* check the config */
  res = CheckConfig();
  if (res!=0) return res;

  /* create the cpu affinity set for used threads */
  CreateCpusets();

  /* Init libPCL */
  co_thread_init();
 
  worker_config.node = _lpel_global_config.node;
  /* initialise workers */
  LpelWorkerInit( _lpel_global_config.num_workers, &worker_config);

  LpelWorkerSpawn();
  return 0;
}



void LpelStop(void)
{
  LpelWorkerTerminate();
}



/**
 * Cleans the LPEL up
 * - wait for the workers to finish
 * - free the data structures of worker threads
 */
void LpelCleanup(void)
{
  /* Cleanup scheduler */
  LpelWorkerCleanup();

  /* Cleanup libPCL */
  co_thread_cleanup();
}




/**
 * @pre core in [0, num_workers] or -1
 */
int LpelThreadAssign( int core)
{
  #ifdef __linux__
  lpel_config_t *cfg = &_lpel_global_config;
  pid_t tid;
  int res;

  /* get thread id */
  tid = gettid();

  if ( core == -1) {
    /* assign an others thread to others cpuset */
    res = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset_others);
    if( res != 0) return LPEL_ERR_ASSIGN;

  } else {
    /* assign a worker thread */
    assert( 0<=core && core<cfg->num_workers );

    if ( LPEL_ICFG(LPEL_FLAG_PINNED)) {
      /* assign to specified core */
      cpu_set_t cpuset;

      CPU_ZERO(&cpuset);
      CPU_SET( core % cfg->proc_workers, &cpuset);
      res = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
      if( res != 0) return LPEL_ERR_ASSIGN;

      /* make non-preemptible */
      if ( LPEL_ICFG(cfg->flags & LPEL_FLAG_EXCLUSIVE)) {
        struct sched_param param;
        param.sched_priority = 1; /* lowest real-time, TODO other? */
        if (0!=sched_setscheduler(tid, SCHED_FIFO, &param)) {
          /* we do best effort at this point */
          //return LPEL_ERR_EXCL;
        }
      }
    } else {
      /* assign along all workers */
      res = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset_workers);
      if( res != 0) return LPEL_ERR_ASSIGN;
    }
  }

  #endif /* __linux__ */
  return 0;
}




