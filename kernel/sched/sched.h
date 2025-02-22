#pragma once

#include <fs/fd.h>
#include <vector.h>
#include <types.h>
#include <mm/vmm.h>
#include <cpu.h>
#include <bitmap.h>
#include <hash.h>
#include <elf.h>
#include <sched/signal.h>

#define EVENT_PROC_EXIT 0
#define EVENT_FD_READ 1
#define EVENT_FD_WRITE 2
#define EVENT_TIMER_TRIGGER 3
#define EVENT_SIGNAL 4
#define EVENT_HDA_CMD 5

struct event_trigger {
	struct sched_task *agent_task;
	struct sched_thread *agent_thread;

	struct event *event;
	int event_type;
};

struct event {
	struct sched_task *task;
	struct sched_thread *thread;

	VECTOR(struct event_trigger*) triggers;

	struct timespec *timespec;
	struct event_trigger *timer_trigger;

	int pending;
	char lock;
};

struct sched_thread {
	tid_t tid;
	pid_t pid;

	size_t status;
	size_t idle_cnt;
	size_t user_stack;
	size_t kernel_stack;
	size_t user_gs_base;
	size_t user_fs_base;
	size_t kernel_stack_size;
	size_t user_stack_size;
	size_t errno;

	char sig_lock;
	sigset_t sigmask;
	struct event sigwait;
	struct signal_queue signal_queue;

	struct registers regs;
};

struct process_group;
struct session;

struct sched_task {
	char fd_lock;
	struct hash_table fd_list;
	struct bitmap fd_bitmap;

	char tid_lock;
	struct hash_table thread_list;
	struct bitmap tid_bitmap;

	struct event *event;
	volatile int event_waiting;

	struct event_trigger *exit_trigger;
	struct event_trigger *last_trigger;

	struct vfs_node *cwd;

	pid_t pid;
	pid_t ppid;

	pid_t pgid;
	struct process_group *group;
	pid_t sid;
	struct session *session;

	int has_execved;

	size_t idle_cnt;
	size_t status;
	int process_status;

	uid_t real_uid;
	uid_t effective_uid;
	uid_t saved_uid;

	gid_t real_gid;
	gid_t effective_gid;
	gid_t saved_gid;

	mode_t umask;

	char sig_lock;
	struct sigaction sigactions[SIGNAL_MAX];

	VECTOR(struct sched_task*) children;

	struct page_table *page_table;
};

struct process_group {
	pid_t pgid;
	pid_t sid;

	pid_t pid_leader;
	struct sched_task *leader;

	VECTOR(struct sched_task*) process_list;
};

struct session {
	pid_t sid;
	pid_t pgid_leader;

	struct bitmap pgid_bitmap;

	struct hash_table group_list;
};

struct sched_arguments {
	int envp_cnt;
	int argv_cnt;

	char **argv;
	char **envp;
};

struct sched_task *sched_translate_pid(pid_t pid);
struct sched_thread *sched_translate_tid(pid_t pid, tid_t tid);
struct sched_task *sched_default_task();
struct sched_thread *sched_default_thread(struct sched_task *task);
struct sched_task *sched_task_exec(const char *path, uint16_t cs, struct sched_arguments *arguments, int status);
struct sched_thread *sched_thread_exec(struct sched_task *task, uint64_t rip, uint16_t cs, struct aux *aux, struct sched_arguments *arguments);

void reschedule(struct registers *regs, void *ptr);
void sched_dequeue(struct sched_task *task, struct sched_thread *thread);
void sched_dequeue_and_yield(struct sched_task *task, struct sched_thread *thread);
void sched_requeue(struct sched_task *task, struct sched_thread *thread);
void sched_requeue_and_yield(struct sched_task *task, struct sched_thread *thread);
void sched_yield();

int event_append_trigger(struct event *event, struct event_trigger *trigger);
int event_wait(struct event *event, int event_type);
int event_create_timer(struct event *event, struct timespec *timespec);
int event_fire(struct event_trigger *trigger);

int task_create_session(struct sched_task *task);

extern char sched_lock;

#define CURRENT_TASK ({ \
	sched_translate_pid(CORE_LOCAL->pid); \
})

#define CURRENT_THREAD ({ \
	sched_translate_tid(CORE_LOCAL->pid, CORE_LOCAL->tid); \
})

#define TASK_RUNNING 0
#define TASK_WAITING 1
#define TASK_YIELD 2

#define THREAD_KERNEL_STACK_SIZE 0x4000
#define THREAD_USER_STACK_SIZE 0x10000

#define TASK_MAX_PRIORITY ~(0ull)
#define TASK_MIN_PRIORITY ~(0)
