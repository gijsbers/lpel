#ifndef _TASK_H_
#define _TASK_H_


#include "arch/atomic.h"

#include "scheduler.h"


/**
 * If a stacksize <= 0 is specified,
 * use the default stacksize
 */
#define LPEL_TASK_ATTR_STACKSIZE_DEFAULT  8192  /* 8k stacksize*/


struct lpel_task_t;
struct mon_task_t;



/* task function signature */
typedef void (*lpel_taskfunc_t)( struct lpel_task_t *self, void *inarg);


typedef enum taskstate_t {
  TASK_CREATED = 'C',
  TASK_RUNNING = 'U',
  TASK_READY   = 'R',
  TASK_BLOCKED = 'B',
  TASK_ZOMBIE  = 'Z'
} taskstate_t;

typedef enum {
  BLOCKED_ON_INPUT  = 'i',
  BLOCKED_ON_OUTPUT = 'o',
  BLOCKED_ON_ANYIN  = 'a',
} taskstate_blocked_t;


/**
 * TASK CONTROL BLOCK
 */
typedef struct lpel_task_t {
  /** intrinsic pointers for organizing tasks in a list*/
  struct lpel_task_t *prev, *next;
  unsigned int uid;    /** unique identifier */
  int stacksize;       /** stacksize */
  taskstate_t state;   /** state */
  taskstate_blocked_t blocked_on; /** on which event the task is waiting */

  struct workerctx_t *worker_context;  /** worker context for this task */

  sched_task_t sched_info;

  /**
   * indicates the SD which points to the stream which has new data
   * and caused this task to be woken up
   */
  struct lpel_stream_desc_t *wakeup_sd;
  atomic_t poll_token;        /** poll token, accessed concurrently */

  /* ACCOUNTING INFORMATION */
  struct mon_task_t *mon;

  /* CODE */
  //FIXME mctx_t mctx;     /** machine context of the task*/
  lpel_taskfunc_t func; /** function of the task */
  void *inarg;          /** input argument  */
} lpel_task_t;




lpel_task_t *LpelTaskCreate( int worker, lpel_taskfunc_t func,
    void *inarg, int stacksize );

void LpelTaskDestroy( lpel_task_t *t);


void LpelTaskMonitor( lpel_task_t *t, char *name, unsigned long flags);
void LpelTaskRun( lpel_task_t *t);

void LpelTaskExit( lpel_task_t *ct);
void LpelTaskYield( lpel_task_t *ct);

unsigned int LpelTaskGetUID( lpel_task_t *t);

void LpelTaskBlock( lpel_task_t *ct, taskstate_blocked_t block_on);
void LpelTaskUnblock( lpel_task_t *ct, lpel_task_t *blocked);


#endif /* _TASK_H_ */
