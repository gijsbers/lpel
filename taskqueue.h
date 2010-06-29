#ifndef _TASKQUEUE_H_
#define _TASKQUEUE_H_

#include "lpel.h"

/**
 * do not modify the fields
 */
typedef struct {
  task_t *head;
  task_t *tail;
  unsigned int count;
} taskqueue_t;

extern void TaskqueueInit(taskqueue_t *tq);
extern void TaskqueueAppend(taskqueue_t *tq, task_t *t);
extern task_t *TaskqueueRemove(taskqueue_t *tq);
extern void TaskqueueIterateRemove(taskqueue_t *tq, 
         bool (*cond)(task_t*), void (*action)(task_t*) );

#endif /* _TASKQUEUE_H_ */