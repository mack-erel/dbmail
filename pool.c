/*
 * 
 * pool.c Management of process pool
 * 
 * 
 * TODO:
 *
 * general: statistics bookkeeping
 * 
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "debug.h"
#include "serverchild.h"
#include "pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <time.h>

#define P_SIZE 100000

static Scoreboard_t *scoreboard;
static int shmid;
static int sb_lockfd;

extern int GeneralStopRequested;
extern ChildInfo_t childinfo;


/*
 *
 *
 * Scoreboard
 *
 *
 */

State_t state_new(void); 
State_t state_new(void)
{
	State_t s;
	s.pid = -1;
	s.ctime = time(0);
	s.status = STATE_NOOP;
	s.count = 0;
	s.client = "none";
	return s;
}

int set_lock(int type);
int set_lock(int type)
{
	struct flock lock;
	lock.l_type = type; /* F_RDLCK, F_WRLCK, F_UNLOCK */
	lock.l_start = 0;
	lock.l_whence = 0;
	lock.l_len = 1;
	return ( fcntl(sb_lockfd, F_SETLKW, &lock) );
}

void scoreboard_new(serverConfig_t * conf)
{
	if ((scoreboard = (Scoreboard_t *) malloc(P_SIZE)) == NULL)
		trace(TRACE_FATAL,
		      "%s,%s: malloc scoreboard failed",__FILE__,__FUNCTION__);
	if ((shmid = shmget(IPC_PRIVATE, P_SIZE, 0644 | IPC_CREAT)) == -1)
		trace(TRACE_FATAL, "%s,%s: shmget failed",__FILE__,__FUNCTION__);
	scoreboard = shmat(shmid, (void *) 0, 0);
	if (scoreboard == (Scoreboard_t *) (-1)) {
		trace(TRACE_FATAL, "%s,%s: scoreboard init failed",__FILE__,__FUNCTION__);
		scoreboard_delete();
	}
	scoreboard_lock_new();
	scoreboard->conf = conf;
	scoreboard_setup();
	scoreboard_conf_check();
}

void scoreboard_lock_new(void)
{
	if ( (sb_lockfd = open(SCOREBOARD_LOCK_FILE,O_RDWR|O_CREAT|O_TRUNC,0600)) < 0) {
		trace(TRACE_FATAL,
			"%s,%s, opening lockfile [%s] failed", __FILE__, __FUNCTION__,
			SCOREBOARD_LOCK_FILE);
	}
}

void scoreboard_setup(void) {	
	int i;
	scoreboard_wrlck();
	for (i = 0; i < HARD_MAX_CHILDREN; i++) {
		scoreboard->child[i] = state_new();
	}
	scoreboard_unlck();
}

void scoreboard_conf_check(void)
{
	/* some sanity checks on boundaries */
	scoreboard_wrlck();
	if (scoreboard->conf->maxChildren > HARD_MAX_CHILDREN)
		scoreboard->conf->maxChildren = HARD_MAX_CHILDREN;

	if (scoreboard->conf->maxChildren < scoreboard->conf->startChildren) {
		trace(TRACE_WARNING,
		      "%s,%s: MAXCHILDREN too small. Increasing to NCHILDREN [%d]",
		      __FILE__,__func__,scoreboard->conf->startChildren);
		scoreboard->conf->maxChildren = scoreboard->conf->startChildren;
	}

	if (scoreboard->conf->maxSpareChildren > scoreboard->conf->maxChildren) {
		trace(TRACE_WARNING,
		      "%s,%s: MAXSPARECHILDREN too small. Increasing to MAXCHILDREN [%d]",
		      __FILE__,__func__,scoreboard->conf->maxChildren);
		scoreboard->conf->maxSpareChildren = scoreboard->conf->maxChildren;
	}

	if (scoreboard->conf->maxSpareChildren < scoreboard->conf->minSpareChildren) {
		trace(TRACE_WARNING,
		      "%s,%s: MAXSPARECHILDREN too small. Increasing to MINSPARECHILDREN [%d]",
		      __FILE__,__func__,scoreboard->conf->minSpareChildren);
		scoreboard->conf->maxSpareChildren = scoreboard->conf->minSpareChildren;
	}
	scoreboard_unlck();
}

void scoreboard_release(pid_t pid)
{
	int slot;
	scoreboard_wrlck();
	slot = getKey(pid);
	scoreboard->child[slot] = state_new();
	scoreboard_unlck();
	
}
void scoreboard_delete()
{
	if (shmdt(scoreboard) == -1)
		trace(TRACE_FATAL,
		      "scoreboard_delete(): detach shared mem failed");
	if (shmctl(shmid, IPC_RMID, NULL) == -1)
		trace(TRACE_FATAL,
		      "scoreboard_delete(): delete shared mem segment failed");
	return;
}
int count_spare_children()
{
	int i, count;
	count = 0;
	
	scoreboard_rdlck();
	for (i = 0; i < scoreboard->conf->maxChildren; i++) {
		if (scoreboard->child[i].pid > 0
		    && scoreboard->child[i].status == STATE_IDLE)
			count++;
	}
	scoreboard_unlck();
	
	return count;
}

int count_children()
{
	int i, count;
	count = 0;
	
	scoreboard_rdlck();
	for (i = 0; i < scoreboard->conf->maxChildren; i++) {
		if (scoreboard->child[i].pid > 0)
			count++;
	}
	scoreboard_unlck();
	
	return count;
}

pid_t get_idle_spare()
{
	int i;
	pid_t idlepid = (pid_t) -1;
	
	scoreboard_rdlck();
	for (i = scoreboard->conf->maxChildren - 1; i >= 0; i--) {
		if ((scoreboard->child[i].pid > 0) 
		    && (scoreboard->child[i].status == STATE_IDLE)) {
			idlepid = scoreboard->child[i].pid;
		}
	}
	scoreboard_unlck();
	
	return idlepid;
}

int getKey(pid_t pid)
{
	int i;
	/* no locking here ! */
	for (i = 0; i < scoreboard->conf->maxChildren; i++) {
		if (scoreboard->child[i].pid == pid)
			return i;
	}
	trace(TRACE_FATAL,
	      "%s,%s: pid NOT found on scoreboard [%d]", __FILE__, __FUNCTION__, pid);
	return -1;
}

/*
 *
 *
 * Child
 *
 *
 */

int child_register()
{
	int i;
	trace(TRACE_MESSAGE, "%s,%s: register child [%d]",
		__FILE__, __FUNCTION__, getpid());
	
	scoreboard_wrlck();
	for (i = 0; i < scoreboard->conf->maxChildren; i++) {
		if (scoreboard->child[i].pid == -1)
			break;
		if (scoreboard->child[i].pid == getpid()) {
			trace(TRACE_ERROR,
			      "%s,%s: child already registered.",
			      __FILE__, __FUNCTION__);
			scoreboard_unlck();
			return -1;
		}
	}
	if (i == scoreboard->conf->maxChildren) {
		trace(TRACE_WARNING,
		      "%s,%s: no empty slot found",
		      __FILE__, __FUNCTION__);
		scoreboard_unlck();
		return -1;
	}
	
	scoreboard->child[i].pid = getpid();
	scoreboard->child[i].status = STATE_IDLE;
	scoreboard_unlck();

	trace(TRACE_INFO, "%s,%s: initializing child_state [%d] using slot [%d]",
		__FILE__, __FUNCTION__, getpid(), i);
	return 0;
}

void child_reg_connected()
{
	int key;
	pid_t pid;
	
	pid = getpid();

	scoreboard_wrlck();
	key = getKey(pid);
	scoreboard->child[key].status = STATE_CONNECTED;
	scoreboard_unlck();

	trace(TRACE_MESSAGE, "%s,%s: [%d]", __FILE__, __FUNCTION__,
			getpid());
}

void child_reg_disconnected()
{
	int key;
	pid_t pid;
	
	pid = getpid();

	scoreboard_wrlck();
	key = getKey(pid);
	scoreboard->child[key].status = STATE_IDLE;
	scoreboard_unlck();

	trace(TRACE_MESSAGE, "%s,%s: [%d]", __FILE__, __FUNCTION__,
		getpid());
}

void child_unregister()
{
	/*
	 *
	 * Set the state for this slot to WAIT
	 * so the parent process can do a waitpid()
	 *
	 */
	int key;
	pid_t pid;
	
	pid = getpid();

	scoreboard_wrlck();
	key = getKey(pid);
	scoreboard->child[key].status = STATE_WAIT;
	scoreboard_unlck();

	trace(TRACE_MESSAGE,
	      "%s,%s: child [%d] unregistered", __FILE__, __FUNCTION__,
	      getpid());
}


/*
 *
 *
 * Server
 *
 *
 */

void manage_start_children()
{
	/* 
	 *
	 * startup the first batch of forked processes
	 *
	 */
	int i;
	for (i = 0; i < scoreboard->conf->startChildren; i++) {
		if (CreateChild(&childinfo) == -1) {
			manage_stop_children();
			trace(TRACE_FATAL,
			      "%s,%s: could not create children. Fatal.",
			      __FILE__, __FUNCTION__);
			exit(0);
		}
	}
}
void manage_restart_children() { 
	/* restart active children */
	int i;
	pid_t chpid;
	for (i=0; i< scoreboard->conf->maxChildren; i++) {
		chpid=scoreboard->child[i].pid;
		if (chpid == -1)
			continue;
		if (waitpid(chpid, NULL, WNOHANG|WUNTRACED) == chpid) {
			trace(TRACE_MESSAGE,"%s,%s: child [%d] exited. Restarting...",
				__FILE__, __FUNCTION__, chpid);
			scoreboard_release(chpid);			
			CreateChild(&childinfo);
		}
	}
	sleep(1);
}

void manage_stop_children()
{
	/* 
	 *
	 * cleanup all remaining forked processes
	 *
	 */
	int stillSomeAlive = 1;
	int i, cnt = 0;
	pid_t chpid;
	
	trace(TRACE_MESSAGE, "%s,%s: General stop requested. Killing children.. ",
			__FILE__,__func__);

	while (stillSomeAlive && cnt < 10) {
		stillSomeAlive = 0;
		cnt++;

		for (i = 0; i < scoreboard->conf->maxChildren; i++) {
			chpid = scoreboard->child[i].pid;
			if (chpid <= 0)
				continue;

			if (CheckChildAlive(chpid)) {
				trace(TRACE_INFO,
				      "%s,%s: child [%d] is still alive, sending SIGTERM",
				      __FILE__,__func__,(unsigned) chpid);
				kill(chpid, SIGTERM);
				usleep(1000);
			} else
				trace(TRACE_INFO,
				      "%s,%s: child [%d] is dead, zombie not yet cleaned",
				      __FILE__,__func__,(unsigned) chpid);


			if (waitpid(chpid, NULL, WNOHANG | WUNTRACED) == chpid) {
				trace(TRACE_INFO,
				      "%s,%s: child [%d] has exited, zombie cleaned up",
				      __FILE__,__func__,(unsigned) chpid);
				chpid = 0;
			} else {
				stillSomeAlive = 1;
				trace(TRACE_INFO,
				      "%s,%s: child [%d] hasn't provided exit status yet",
				      __FILE__,__func__,chpid);
			}
		}

		if (stillSomeAlive)
			usleep(500);
	}

	if (stillSomeAlive) {
		trace(TRACE_INFO,
		      "%s,%s: not all children terminated at SIGTERM, killing hard now",
		      __FILE__,__func__);

		for (i = 0; i < scoreboard->conf->maxChildren; i++) {
			chpid = scoreboard->child[i].pid;
			if (chpid != 0)
				kill(chpid, SIGKILL);;
		}
	}
}


void manage_spare_children()
{
	/* 
	 *
	 * manage spare children while running
	 *
	 */
	int somethingchanged;
	pid_t chpid;
	
	chpid = getpid();
	somethingchanged = 0;
	
	/* scale up */
	while ((count_children() < scoreboard->conf->maxChildren)
	       && (count_spare_children() <
		   scoreboard->conf->minSpareChildren)) {
		somethingchanged = 1;
		trace(TRACE_INFO,
		      "%s,%s: creating spare child", __FILE__,__func__);
		if ((chpid = CreateChild(&childinfo)) < 0) {
			trace(TRACE_ERROR,
			      "%s,%s: unable to start new child",
			      __FILE__,__func__);
			break;
		}
	}

	/* scale down */
	while ((count_children() > scoreboard->conf->startChildren)
	       && (count_spare_children() >
		   scoreboard->conf->maxSpareChildren)) {
		somethingchanged = 1;
		if ((chpid = get_idle_spare()) > 0) {
			trace(TRACE_INFO,
			      "%s,%s: killing overcomplete spare [%d]",
			      __FILE__,__func__,chpid);
			kill(chpid, SIGTERM);
			if (waitpid(chpid, NULL, 0) == chpid) {
				trace(TRACE_INFO,
				      "%s,%s: spare child [%u] has exited",
				      __FILE__,__func__,chpid);
			}
		} else {
			trace(TRACE_WARNING,
			      "%s,%s: unable to get pid for idle spare",
			      __FILE__,__func__);
		}
	}
	/* scoreboard */
	if (somethingchanged > 0) {
		trace(TRACE_MESSAGE,
		      "%s,%s: children [%d/%d], spares [%d (%d - %d)]",
		      __FILE__,__func__,
		      count_children(),
		      scoreboard->conf->maxChildren,
		      count_spare_children(),
		      scoreboard->conf->minSpareChildren,
		      scoreboard->conf->maxSpareChildren);
	}
	if (!count_children()) {
		trace(TRACE_WARNING,
		      "%s,%s: no children left ?. Aborting.",
		      __FILE__,__func__);
		GeneralStopRequested = 1;
	}
}
