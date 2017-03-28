#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "dccthread.h"
#include "dlist.h"

//------------------------------------------------------------//
//----------------- Inicializando estruturas -----------------//
//------------------------------------------------------------//

typedef struct dccthread{
	ucontext_t context;
	char name[DCCTHREAD_MAX_NAME_SIZE];
	dccthread_t *waiting_for; //ponteiro para uma thread que esta esperando .
							  //NULL caso não espere ninguém.
	unsigned int yielded; //0 se nao, 1 se sim.
	int id;
	unsigned int is_in_ready_list;	// se não, 1 se sim.
	unsigned int is_in_waiting_list;
	int stimerid;
} dccthread_t;

struct dlist *ready_list;
struct dlist *waiting_list;

dccthread_t *manager;
dccthread_t *principal;

int counter_thread = 0;
int sleeptid = 1;
timer_t timerid;
struct sigevent sigev;
struct sigaction sact;
struct itimerspec its;
struct sigevent sleep_ev;
struct sigaction sleep_act;

//------------------------------------------------------------//
//------------------ Funções de dccthread.h ------------------//
//------------------------------------------------------------//
int cmp_wait_for(const void *e1, const void *e2, void *userdata){
	dccthread_t* e_list = (dccthread_t*) e1;
	dccthread_t* e_exit = (dccthread_t*) e2;
	if(e_exit == e_list->waiting_for)
		return 0;
	else
		return 1;
}

int cmp_wait_for_sleep(const void *e1, const void *e2, void *userdata){
	dccthread_t* e_list = (dccthread_t*) e1;
	dccthread_t* e_dummy = (dccthread_t*) e2;
	if(e_list->stimerid == e_dummy->stimerid)
		return 0;
	else
		return 1;
}

static void timer_catcher(int sig, siginfo_t *si, void *uc){
    dccthread_yield();
}

static void sleep_catcher(int sig, siginfo_t *si, void *uc){
	sigprocmask(SIG_BLOCK,&sact.sa_mask,NULL);
	ucontext_t* ucp = (ucontext_t*) uc;
	dccthread_t* dummy = (dccthread_t*) malloc (sizeof(dccthread_t));
	dummy->stimerid = si->si_timerid;
   	dccthread_t* t_dependent = (dccthread_t*) dlist_find_remove(waiting_list, dummy, cmp_wait_for_sleep,NULL);

	if(t_dependent != NULL){
		t_dependent->is_in_waiting_list = 0;
		t_dependent->is_in_ready_list = 1;
		dlist_push_right(ready_list, t_dependent);
	}
	setcontext( ucp );
}

void dccthread_init(void (*func)(int), int param){
	sact.sa_flags = SA_SIGINFO;
	sact.sa_sigaction = timer_catcher;
	sigaction(SIGUSR1,&sact,NULL);
	sigev.sigev_notify = SIGEV_SIGNAL;
	sigev.sigev_signo = SIGUSR1;
	sigemptyset(&sact.sa_mask);
	sigaddset(&sact.sa_mask, SIGUSR1);
	timer_create(CLOCK_PROCESS_CPUTIME_ID, &sigev, &timerid);
	its.it_value.tv_nsec = 10000000;
	its.it_interval.tv_nsec = its.it_value.tv_nsec;

    sleep_act.sa_flags = SA_SIGINFO;
	sleep_act.sa_sigaction = sleep_catcher;
	sigaction(SIGUSR2,&sleep_act,NULL);
	sigemptyset(&sleep_act.sa_mask);
	//sigaddset(&sleep_act.sa_mask, SIGUSR2);
	sleep_ev.sigev_notify = SIGEV_SIGNAL;
	sleep_ev.sigev_signo = SIGUSR2;



	ready_list = dlist_create();
	waiting_list = dlist_create();

	manager = (dccthread_t *) malloc (sizeof(dccthread_t));

	getcontext(&manager->context);
    manager->context.uc_link = NULL;
	manager->context.uc_stack.ss_sp = malloc (THREAD_STACK_SIZE);
	manager->context.uc_stack.ss_size = THREAD_STACK_SIZE;
	manager->context.uc_stack.ss_flags = 0;
	manager->id = -1;
	manager->yielded = 0;
	strcpy(manager->name, "gerente");
	manager->waiting_for = NULL;

	principal = dccthread_create("main", func, param);

	sigprocmask(SIG_BLOCK,&sact.sa_mask,NULL);
	while(!dlist_empty(ready_list)){
		dccthread_t *aux = (dccthread_t *) malloc (sizeof(dccthread_t));
		aux = ready_list->head->data;
		timer_settime(timerid,0, &its, NULL);
		swapcontext(&manager->context, &aux->context);
		dlist_pop_left(ready_list);
		if(aux->yielded == 0)aux->is_in_ready_list = 0;
		//free(aux);
	}
	exit(EXIT_SUCCESS);
}

dccthread_t * dccthread_create(const char *name,void (*func)(int ), int param){


	dccthread_t *new_thread;
	new_thread = (dccthread_t*) malloc (sizeof(dccthread_t));
    getcontext(&new_thread->context);

	sigprocmask(SIG_BLOCK,&sact.sa_mask,NULL);
	new_thread->context.uc_link = &manager->context;
	new_thread->context.uc_stack.ss_sp = malloc (THREAD_STACK_SIZE);
	new_thread->context.uc_stack.ss_size = THREAD_STACK_SIZE;
	new_thread->context.uc_stack.ss_flags = 0;
	new_thread->id = counter_thread;

	counter_thread++;
	new_thread->yielded = 0;
	strcpy(new_thread->name, name);
	new_thread->waiting_for = NULL;
	new_thread->is_in_ready_list = 1;
	new_thread->is_in_waiting_list = 0;

	//Inserindo na dlist de prontos:
	dlist_push_right(ready_list, new_thread);

	makecontext(&new_thread->context, (void*) func, 1, param);
	sigprocmask(SIG_UNBLOCK,&sact.sa_mask,NULL);
	return new_thread;
}

void dccthread_yield(void){
	sigprocmask(SIG_BLOCK,&sact.sa_mask,NULL);
	dccthread_t *current_context = dccthread_self();
	current_context->yielded = 1;
	dlist_push_right(ready_list, current_context);
	swapcontext(&current_context->context, &manager->context);
	sigprocmask(SIG_UNBLOCK,&sact.sa_mask,NULL);
}

void dccthread_exit(void){
	dccthread_t* current = dccthread_self();
	sigprocmask(SIG_BLOCK,&sact.sa_mask,NULL);
	dccthread_t* t_dependent = (dccthread_t*) dlist_find_remove(waiting_list, current, cmp_wait_for,NULL);
	if(t_dependent != NULL){
		t_dependent->is_in_waiting_list = 0;
		t_dependent->is_in_ready_list = 1;
		dlist_push_right(ready_list, t_dependent);
	}
	sigprocmask(SIG_UNBLOCK,&sact.sa_mask,NULL);
	setcontext(&manager->context);
}

/* `dccthread_wait` blocks the current thread until thread `tid`
 * terminates. */
void dccthread_wait(dccthread_t *tid){
	sigprocmask(SIG_BLOCK,&sact.sa_mask,NULL);
	dccthread_t *current = dccthread_self();
	if(tid->is_in_ready_list || tid->is_in_waiting_list){
		current->is_in_ready_list = 0;
		current->is_in_waiting_list = 1;
		current->waiting_for = tid;
		dlist_push_right(waiting_list, current);
		swapcontext(&current->context, &manager->context);
	}
	sigprocmask(SIG_UNBLOCK,&sact.sa_mask,NULL);
}

/* `dccthread_sleep` stops the current thread for the time period
 * specified in `ts`. */
void dccthread_sleep(struct timespec ts){
	sigprocmask(SIG_BLOCK,&sact.sa_mask,NULL);
	timer_t tsleepid;
	struct itimerspec its;
	timer_create(CLOCK_REALTIME, &sleep_ev, &tsleepid);
	its.it_value = ts;

	dccthread_t* current = dccthread_self();
	current->stimerid = sleeptid;
	sleeptid++;
	current->is_in_ready_list = 0;
	current->is_in_waiting_list = 1;
	dlist_push_right(waiting_list, current);

	timer_settime(tsleepid,0, &its, NULL);
	swapcontext(&current->context, &manager->context);
	sigprocmask(SIG_UNBLOCK,&sact.sa_mask,NULL);
}

/* `dccthread_self` returns the current thread's handle. */
dccthread_t * dccthread_self(void){
	return ready_list->head->data;
}

/* `dccthread_name` returns a pointer to the string containing the
 * name of thread `tid`.  the returned string is owned and managed
 * by the library. */
const char * dccthread_name(dccthread_t *tid){
	return tid->name;
}


