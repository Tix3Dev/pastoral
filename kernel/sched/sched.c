#include <sched/sched.h>
#include <int/apic.h>
#include <vector.h>
#include <cpu.h>
#include <mm/pmm.h>
#include <string.h>
#include <debug.h>
#include <elf.h>
#include <mm/mmap.h>
#include <types.h>
#include <errno.h>
#include <fs/fd.h>
#include <drivers/terminal.h>
#include <time.h>

static struct hash_table task_list;
static struct hash_table session_list;

static struct bitmap pid_bitmap = {
	.data = NULL,
	.size = 0,
	.resizable = true
};

static struct bitmap sid_bitmap = {
	.data = NULL,
	.size = 0,
	.resizable = true
};

char sched_lock;

// does not lock **remember**
struct sched_task *sched_translate_pid(pid_t pid) {
	return hash_table_search(&task_list, &pid, sizeof(pid));
}

// does not lock **remember**
struct sched_thread *sched_translate_tid(pid_t pid, tid_t tid) {
	struct sched_task *task = sched_translate_pid(pid);
	if(task == NULL) {
		return NULL;
	}

	return hash_table_search(&task->thread_list, &tid, sizeof(tid));
}

struct sched_thread *find_next_thread(struct sched_task *task) {
	struct sched_thread *ret = NULL;

	for(size_t i = 0, cnt = 0; i < task->thread_list.capacity; i++) {
		if(task->thread_list.data[i] == NULL) {
			continue;
		}

		struct sched_thread *next_thread = task->thread_list.data[i];
		next_thread->idle_cnt++;

		if(next_thread->status == TASK_WAITING && cnt < next_thread->idle_cnt) {
			cnt = next_thread->idle_cnt;
			ret = next_thread;
		}
	}

	return ret;
}

struct sched_task *find_next_task() {
	struct sched_task *ret = NULL;

	for(size_t i = 0, cnt = 0; i < task_list.capacity; i++) {
		if(task_list.data[i] == NULL) {
			continue;
		}

		struct sched_task *next_task = task_list.data[i];
		next_task->idle_cnt++;

		if(next_task->status == TASK_WAITING && cnt < next_task->idle_cnt) {
			cnt = next_task->idle_cnt;
			ret = next_task;
		}
	}

	return ret;
}

void sched_idle() {
	xapic_write(XAPIC_EOI_OFF, 0);
	spinrelease(&sched_lock);

	asm volatile ("sti");

	for(;;) {
		asm volatile ("hlt");
	}
}

void reschedule(struct registers *regs, void*) {
	if(__atomic_test_and_set(&sched_lock, __ATOMIC_ACQUIRE)) {
		return;
	}

	struct sched_task *next_task = find_next_task();
	if(next_task == NULL) {
		if(CORE_LOCAL->tid != -1 && CORE_LOCAL->pid != -1) {
			spinrelease(&sched_lock);
			return;
		}
		sched_idle();
	}

	struct sched_thread *next_thread = find_next_thread(next_task);
	if(next_thread == NULL) {
		if(CORE_LOCAL->tid != -1 && CORE_LOCAL->pid != -1) {
			spinrelease(&sched_lock);
			return;
		}
		sched_idle();
	}

	if(CORE_LOCAL->tid != -1 && CORE_LOCAL->pid != -1) {
		struct sched_task *last_task = sched_translate_pid(CORE_LOCAL->pid);
		if(last_task == NULL) {
			sched_idle();
		}

		struct sched_thread *last_thread = sched_translate_tid(CORE_LOCAL->pid, CORE_LOCAL->tid);
		if(last_thread == NULL) {
			sched_idle();
		}

		if(last_thread->status != TASK_YIELD) {
			last_thread->status = TASK_WAITING;
		}

		if(last_task->status != TASK_YIELD) {
			last_task->status = TASK_WAITING;
		}

		last_thread->errno = CORE_LOCAL->errno;
		last_thread->regs = *regs;
		last_thread->user_fs_base = get_user_fs();
		last_thread->user_gs_base = get_user_gs();
		last_thread->user_stack = CORE_LOCAL->user_stack;
	}

	CORE_LOCAL->pid = next_task->pid;
	CORE_LOCAL->tid = next_thread->tid;
	CORE_LOCAL->errno = next_thread->errno;
	CORE_LOCAL->kernel_stack = next_thread->kernel_stack;
	CORE_LOCAL->user_stack = next_thread->user_stack;

	CORE_LOCAL->page_table = next_task->page_table;

	vmm_init_page_table(CORE_LOCAL->page_table);

	next_thread->idle_cnt = 0;
	next_task->idle_cnt = 0;
	next_task->status = TASK_RUNNING;
	next_thread->status = TASK_RUNNING;

	set_user_fs(next_thread->user_fs_base);
	set_user_gs(next_thread->user_gs_base);

	next_task->event_waiting = 0;

	for(size_t i = 0; i < SIGNAL_MAX; i++) {
		if(next_thread->signal_queue.sigpending & (1 << i)) {
			struct signal *signal = &next_thread->signal_queue.queue[i];
			struct sigaction *action = signal->sigaction;

			next_thread->regs.rsp -= 128;
			next_thread->regs.rsp &= -16ll;

			if(action->sa_flags & SA_SIGINFO) {
				next_thread->regs.rsp -= sizeof(struct siginfo);
				struct siginfo *siginfo = (void*)next_thread->regs.rsp;

				// TODO use a proper ucontext
				next_thread->regs.rsp -= sizeof(struct registers);
				struct registers *ucontext = (void*)next_thread->regs.rsp;

				next_thread->regs.rip = (uint64_t)action->handler.sa_sigaction;
				next_thread->regs.rdi = signal->signum;
				next_thread->regs.rsi = (uint64_t)siginfo;
				next_thread->regs.rdx = (uint64_t)ucontext; 
			} else {
				next_thread->regs.rip = (uint64_t)action->handler.sa_sigaction;
				next_thread->regs.rdi = signal->signum;
			}

			next_thread->signal_queue.sigpending &= ~(1 << i);
			
			break;
		}
	}

	if(next_thread->regs.cs & 0x3) {
		swapgs();
	}

	xapic_write(XAPIC_EOI_OFF, 0);
	spinrelease(&sched_lock);

	asm volatile (
		"mov %0, %%rsp\n\t"
		"pop %%r15\n\t"
		"pop %%r14\n\t"
		"pop %%r13\n\t"
		"pop %%r12\n\t"
		"pop %%r11\n\t"
		"pop %%r10\n\t"
		"pop %%r9\n\t"
		"pop %%r8\n\t"
		"pop %%rsi\n\t"
		"pop %%rdi\n\t"
		"pop %%rbp\n\t"
		"pop %%rdx\n\t"
		"pop %%rcx\n\t"
		"pop %%rbx\n\t"
		"pop %%rax\n\t"
		"addq $16, %%rsp\n\t"
		"iretq\n\t"
		:: "r" (&next_thread->regs)
	);
}

void sched_dequeue(struct sched_task *task, struct sched_thread *thread) {
	spinlock(&sched_lock);

	task->status = TASK_YIELD;
	thread->status = TASK_YIELD;

	spinrelease(&sched_lock);
}


void sched_dequeue_and_yield(struct sched_task *task, struct sched_thread *thread) {
	asm volatile ("cli");

	sched_dequeue(task, thread);

	xapic_write(XAPIC_ICR_OFF + 0x10, CORE_LOCAL->apic_id << 24);
	xapic_write(XAPIC_ICR_OFF, 32);

	asm volatile ("sti");

	for(;;) {
		asm volatile ("hlt");
	}
}

void sched_requeue(struct sched_task *task, struct sched_thread *thread) {
	spinlock(&sched_lock);

	task->status = TASK_WAITING;
	task->idle_cnt = TASK_MAX_PRIORITY;

	thread->status = TASK_WAITING;
	thread->idle_cnt = TASK_MAX_PRIORITY;

	spinrelease(&sched_lock);
}

void sched_requeue_and_yield(struct sched_task *task, struct sched_thread *thread) {
	asm volatile ("cli");

	sched_requeue(task, thread);

	xapic_write(XAPIC_ICR_OFF + 0x10, CORE_LOCAL->apic_id << 24);
	xapic_write(XAPIC_ICR_OFF, 32);

	asm volatile ("sti");

	for(;;) {
		asm volatile ("hlt");
	}
}

void sched_yield() {
	xapic_write(XAPIC_ICR_OFF + 0x10, CORE_LOCAL->apic_id << 24);
	xapic_write(XAPIC_ICR_OFF, 32);

	for(;;) {
		asm volatile ("hlt");
	}
}

struct sched_task *sched_default_task() {
	struct sched_task *task = alloc(sizeof(struct sched_task));

	task->pid = bitmap_alloc(&pid_bitmap);
	task->status = TASK_YIELD;
	task->fd_bitmap.resizable = true;

	task->event = alloc(sizeof(struct event));
	task->exit_trigger = alloc(sizeof(struct event_trigger));

	task->real_uid = 0;
	task->effective_uid = 0;
	task->saved_uid = 0;

	task->real_gid = 0;
	task->effective_gid = 0;
	task->saved_gid = 0;

	task->umask = 022;

	task->tid_bitmap = (struct bitmap) {
		.data = NULL,
		.size = 0,
		.resizable = true
	};

	if(CURRENT_TASK != NULL) {
		task->ppid = CURRENT_TASK->pid;
	} else {
		task->ppid = -1;
	}

	hash_table_push(&task_list, &task->pid, task, sizeof(task->pid));

	return task;
}

struct sched_thread *sched_default_thread(struct sched_task *task) {
	struct sched_thread *thread = alloc(sizeof(struct sched_thread));

	thread->pid = task->pid;
	thread->tid = bitmap_alloc(&task->tid_bitmap);
	thread->status = TASK_YIELD;

	thread->kernel_stack = pmm_alloc(DIV_ROUNDUP(THREAD_KERNEL_STACK_SIZE, PAGE_SIZE), 1) + THREAD_KERNEL_STACK_SIZE + HIGH_VMA;

	hash_table_push(&task->thread_list, &thread->tid, thread, sizeof(thread->tid));

	return thread;
}

static uint64_t sched_arg_placement(struct sched_arguments *arguments, uint64_t *ptr, struct aux *aux) {
	uint64_t rsp = (uint64_t)ptr;

	for(size_t i = 0; i < arguments->envp_cnt; i++) {
		char *element = arguments->envp[i];
		ptr = (uint64_t*)((void*)ptr - (strlen(element) + 1));
		strcpy((void*)ptr, element);
	}

	for(size_t i = 0; i < arguments->argv_cnt; i++) {
		char *element = arguments->argv[i];
		ptr = (uint64_t*)((void*)ptr - (strlen(element) + 1));
		strcpy((void*)ptr, element);
	}

	ptr = (uint64_t*)((uintptr_t)ptr - ((uintptr_t)ptr & 0xf)); // align 16

	if((arguments->argv_cnt + arguments->envp_cnt + 1) & 1) {
		ptr--;
	}

	ptr -= 10;

	ptr[0] = ELF_AT_PHNUM; ptr[1] = aux->at_phnum;
	ptr[2] = ELF_AT_PHENT; ptr[3] = aux->at_phent;
	ptr[4] = ELF_AT_PHDR;  ptr[5] = aux->at_phdr;
	ptr[6] = ELF_AT_ENTRY; ptr[7] = aux->at_entry;
	ptr[8] = 0; ptr[9] = 0;

	*(--ptr) = 0;
	ptr -= arguments->envp_cnt;

	for(size_t i = 0; i < arguments->envp_cnt; i++) {
		rsp -= strlen(arguments->envp[i]) + 1;
		ptr[i] = rsp;
	}

	*(--ptr) = 0;
	ptr -= arguments->argv_cnt;

	for(size_t i = 0; i < arguments->argv_cnt; i++) {
		rsp -= strlen(arguments->argv[i]) + 1;
		ptr[i] = rsp;
	}

	*(--ptr) = arguments->argv_cnt;

	return (uint64_t)ptr;
}

struct sched_thread *sched_thread_exec(struct sched_task *task, uint64_t rip, uint16_t cs, struct aux *aux, struct sched_arguments *arguments) {
	struct sched_thread *thread = sched_default_thread(task);

	thread->regs.rip = rip;
	thread->regs.cs = cs;
	thread->regs.rflags = 0x202;

	thread->user_gs_base = 0;
	thread->user_fs_base = 0;

	if(cs & 0x3) {
		thread->regs.ss = cs - 8;
		thread->user_stack = (uint64_t)mmap(	task->page_table,
												NULL,
												THREAD_USER_STACK_SIZE,
												MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_USER,
												MMAP_MAP_ANONYMOUS,
												0,
												0
										   ) + THREAD_USER_STACK_SIZE;
		thread->regs.rsp = sched_arg_placement(arguments, (void*)thread->user_stack, aux);
	} else {
		thread->regs.ss = cs + 8;
		thread->regs.rsp = thread->kernel_stack;
	}

	return thread;
}

struct sched_task *sched_task_exec(const char *path, uint16_t cs, struct sched_arguments *arguments, int status) {
	spinlock(&sched_lock);

	struct sched_task *task = sched_default_task();

	task->page_table = alloc(sizeof(struct page_table));
	vmm_default_table(task->page_table);

	vmm_init_page_table(task->page_table);

	struct sched_task *current_task = CURRENT_TASK;
	struct sched_thread *current_thread = CURRENT_THREAD;

	CORE_LOCAL->pid = task->pid;

	int fd = fd_openat(AT_FDCWD, path, O_RDONLY, 0);
	if(fd == -1) {
		fd_close(fd);
		CORE_LOCAL->pid = current_task->pid;
		spinrelease(&sched_lock);
		return NULL;
	}

	char *ld_path = NULL;

	struct aux aux;
	if(elf_load(task->page_table, &aux, fd, 0, &ld_path) == -1) {
		fd_close(fd);
		CORE_LOCAL->pid = current_task->pid;
		spinrelease(&sched_lock);
		return NULL;
	}

	fd_close(fd);

	uint64_t entry_point = aux.at_entry;

	if(ld_path) {
		int ld_fd = fd_openat(AT_FDCWD, ld_path, O_RDONLY, 0);
		if(ld_fd == -1) {
			fd_close(ld_fd);
			CORE_LOCAL->pid = current_task->pid;
			spinrelease(&sched_lock);
			return NULL;
		}

		struct aux ld_aux;
		if(elf_load(task->page_table, &ld_aux, ld_fd, 0x40000000, NULL) == -1) {
			fd_close(ld_fd);
			CORE_LOCAL->pid = current_task->pid;
			spinrelease(&sched_lock);
			return NULL;
		}

		fd_close(ld_fd);

		entry_point = ld_aux.at_entry;
	}

	struct sched_thread *thread = sched_thread_exec(task, entry_point, cs, &aux, arguments);

	if(thread == NULL) {
		CORE_LOCAL->pid = current_task->pid;
		spinrelease(&sched_lock);
		return NULL;
	}

	CORE_LOCAL->pid = current_task->pid;

	vmm_init_page_table(current_task->page_table);

	task->event->task = task;
	task->event->thread = thread;

	task->exit_trigger->agent_task = task;
	task->exit_trigger->agent_thread = thread;
	task->exit_trigger->event_type = EVENT_PROC_EXIT;

	task->exit_trigger->event = alloc(sizeof(struct event));
	task->exit_trigger->event->task = current_task;
	task->exit_trigger->event->thread = current_thread;

	spinrelease(&sched_lock);

	task->status = status;
	thread->status = TASK_WAITING;

	return task;
}

int event_append_trigger(struct event *event, struct event_trigger *trigger) {
	if(event == NULL || trigger == NULL) {
		return -1;
	}

	spinlock(&event->lock);

	VECTOR_PUSH(event->triggers, trigger);

	spinrelease(&event->lock);

	return 0;
}

int event_wait(struct event *event, int event_type) {
	if(event == NULL) {
		return -1;
	}

	struct sched_task *task = CURRENT_TASK;

	asm volatile ("cli");

	if(event->pending) {
		event->pending--;
		return 0;
	}

	for(;;) {
		sched_dequeue(CURRENT_TASK, CURRENT_THREAD);

		task->event_waiting = 1;

		asm volatile ("sti");

		while(task->event_waiting);
		task->event_waiting = 0;

		struct event_trigger *trigger = task->last_trigger;

		if(trigger->event_type == event_type) {
			return 0;
		}
	}

	return 0;
}

int event_fire(struct event_trigger *trigger) {
	if(trigger == NULL || trigger->event == NULL) {
		return -1;
	}

	struct event *event = trigger->event;
	struct sched_task *task = event->task;

	asm volatile ("cli");

	spinlock(&event->lock);

	event->pending++;
	task->last_trigger = trigger;
	sched_requeue(event->task, event->thread);

	spinrelease(&event->lock);

	asm volatile ("sti");

	return 0;
}

int event_create_timer(struct event *event, struct timespec *timespec) {
	event->timespec = timespec;
	event->timer_trigger = alloc(sizeof(struct event_trigger));

	event->timer_trigger->event = event;
	event->timer_trigger->event_type = EVENT_TIMER_TRIGGER;

	struct timer *timer = alloc(sizeof(struct timer));
	timer->timespec = *timespec;

	VECTOR_PUSH(timer->triggers, event->timer_trigger);
	VECTOR_PUSH(timer_list, timer);

	return 0;
}

int task_create_session(struct sched_task *task) {
	if(task->group != NULL && task->group->pid_leader == task->pid) {
		set_errno(EPERM);
		return -1;
	}

	struct session *session = alloc(sizeof(struct session));
	struct process_group *group = alloc(sizeof(struct process_group));

	session->pgid_bitmap = (struct bitmap) {
		.data = NULL,
		.size = 0,
		.resizable = true
	};

	pid_t sid = bitmap_alloc(&sid_bitmap);
	pid_t pgid = bitmap_alloc(&session->pgid_bitmap);

	session->sid = sid;
	session->pgid_leader = pgid;

	group->pgid = pgid;
	group->sid = sid;
	group->pid_leader = task->pid;
	group->leader = task;
	VECTOR_PUSH(group->process_list, task);

	hash_table_push(&session->group_list, &session->pgid_leader, group, sizeof(session->pgid_leader));
	hash_table_push(&session_list, &session->sid, session, sizeof(session->sid));

	task->sid = sid;
	task->session = session;

	task->pgid = pgid;
	task->group = group;

	return sid;
}

int task_setpgid(struct sched_task *task, pid_t pgid) {
	if(task->pgid == pgid) {
		return 0;
	}

	if((CURRENT_TASK->sid != task->sid) || (task->group->pid_leader == task->pid) || task->has_execved) {
		set_errno(EPERM);
		return -1;
	}

	struct session *session = task->session;
	struct process_group *target_group;

	if(!BIT_TEST(session->pgid_bitmap.data, pgid)) {
		target_group = alloc(sizeof(struct process_group));

		target_group->pgid = pgid;
		target_group->sid = task->sid;
		target_group->pid_leader = task->pid;
		target_group->leader = task;
		VECTOR_PUSH(target_group->process_list, task);

		hash_table_push(&session->group_list, &target_group->pgid, target_group, sizeof(target_group->pgid));

		BIT_SET(session->pgid_bitmap.data, pgid);
	} else {
		target_group = hash_table_search(&session->group_list, &pgid, sizeof(pgid));
	}

	VECTOR_PUSH(target_group->process_list, task);

	task->pgid = pgid;

	return 0;
}

void syscall_waitpid(struct registers *regs) {
	int pid = regs->rdi;
	int *status = (int*)regs->rsi;
	int options = regs->rdx;

#ifndef SYSCALL_DEBUG
	print("syscall: waitpid: pid {%x}, status {%x}, options {%x}\n", pid, (uintptr_t)status, options);
#endif

	asm volatile ("cli");

	struct sched_task *current_task = CURRENT_TASK;

	VECTOR(struct sched_task*) process_list = { 0 };

	if(pid < -1) {
		// TODO implement process groups
	} else if(pid == -1) {
		for(size_t i = 0; i < current_task->children.length; i++) {
			VECTOR_PUSH(process_list, current_task->children.data[i]);
		}
	} else if(pid == 0) {
		// TODO implement process groups
	} else if(pid > 0) {
		VECTOR_PUSH(process_list, sched_translate_pid(pid));
	}

	for(size_t i = 0; i < process_list.length; i++) {
		struct sched_task *task = process_list.data[i];
		struct event_trigger *trigger = task->exit_trigger;

		trigger->event = current_task->event;

		event_append_trigger(current_task->event, task->exit_trigger);
	}

	event_wait(current_task->event, EVENT_PROC_EXIT);

	struct event_trigger *trigger = current_task->last_trigger;
	struct sched_task *agent = trigger->agent_task;

	if(status != NULL) {
		*status = agent->process_status;
	}

	regs->rax = agent->pid;
}

void syscall_exit(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: exit\n");
#endif

	asm volatile ("cli");

	struct sched_task *task = CURRENT_TASK;
	if(task == NULL) {
		panic("");
	}

	for(size_t i = 0; i < task->fd_bitmap.size; i++) {
		if(BIT_TEST(task->fd_bitmap.data, i)) {
			fd_close(i);
		}
	}

	for(size_t i = 0; i < task->thread_list.capacity; i++) {
		struct sched_thread *thread = task->thread_list.data[i];

		if(thread) {
			thread->status = TASK_YIELD;
			hash_table_delete(&task->thread_list, &thread->tid, sizeof(thread->tid));
		}
	}

	struct page_table *page_table = task->page_table;

	/* TODO leaks inner pt levels */
	for(size_t i = 0; i < page_table->pages->capacity; i++) {
		struct page *page = page_table->pages->data[i];

		if(page) {
			hash_table_delete(page_table->pages, &page->vaddr, sizeof(page->vaddr));

			if((*page->reference) <= 1) { // shared page
				(*page->reference)--;
				continue;
			}

			pmm_free(page->paddr, 1);
		}
	}

	int status = regs->rdi;

	struct sched_task *parent = sched_translate_pid(1);

	for(size_t i = 0; i < task->children.length; i++) {
		struct sched_task *child = task->children.data[i];
		child->ppid = 1;
		VECTOR_PUSH(parent->children, child);
	}

	//VECTOR_REMOVE_BY_VALUE(sched_translate_pid(task->ppid)->children, task);

	task->process_status = status | 0x200;
	event_fire(task->exit_trigger);

	task->status = TASK_YIELD;

	hash_table_delete(&task_list, &task->pid, sizeof(task->pid));

	CORE_LOCAL->pid = -1;
	CORE_LOCAL->tid = -1;

	asm volatile ("sti");

	sched_yield();
}

void syscall_execve(struct registers *regs) {
	char *_path = (char*)regs->rdi;
	char **_argv = (char**)regs->rsi;
	char **_envp = (char**)regs->rdx;

	int envp_cnt = 0;
	for(;;envp_cnt++) {
		if(_envp[envp_cnt] == NULL) {
			break;
		}
	}

	int argv_cnt = 0;
	for(;;argv_cnt++) {
		if(_argv[argv_cnt] == NULL) {
			break;
		}
	}

	char *path = alloc(strlen(_path) + 1);
	char **argv = alloc(sizeof(char*) * argv_cnt);
	char **envp = alloc(sizeof(char*) * envp_cnt);

	strcpy(path, _path);

	for(size_t i = 0; i < envp_cnt; i++) {
		envp[i] = alloc(strlen(_envp[i]));
		strcpy(envp[i], _envp[i]);
	}

	for(size_t i = 0; i < argv_cnt; i++) {
		argv[i] = alloc(strlen(_argv[i]));
		strcpy(argv[i], _argv[i]);
	}

	struct sched_arguments arguments = {
		.envp_cnt = envp_cnt,
		.argv_cnt = argv_cnt,
		.argv = argv,
		.envp = envp
	};

#ifndef SYSCALL_DEBUG
	print("syscall: execve: path {%s}, argv {", path);

	for(size_t i = 0; i < argv_cnt; i++) {
		print("%s, ", argv[i]);
	}

	print("\b\b}, envp {");

	for(size_t i = 0; i < envp_cnt; i++) {
		print("%s, ", envp[i]);
	}

	print("\b\b}\n");
#endif
	struct sched_task *current_task = CURRENT_TASK;
	struct vfs_node *vfs_node = vfs_search_absolute(NULL, path, true);
	if(vfs_node == NULL) {
		set_errno(ENOENT);
		regs->rax = -1;
		return;
	}

	if(stat_has_access(vfs_node->asset->stat, current_task->effective_uid,
		current_task->effective_gid, X_OK) == -1) {
		set_errno(EACCES);
		regs->rax = -1;
		return;
	}

	bool is_suid = vfs_node->asset->stat->st_mode & S_ISUID ? true : false;
	bool is_sgid = vfs_node->asset->stat->st_mode & S_ISGID ? true : false;

	struct sched_task *task = sched_task_exec(path, 0x43, &arguments, TASK_WAITING);
	struct sched_thread *thread = sched_translate_tid(task->pid, 0);

	bitmap_dup(&current_task->fd_bitmap, &task->fd_bitmap);
	for(size_t i = 0; i < task->fd_bitmap.size; i++) {
		if(BIT_TEST(task->fd_bitmap.data, i)) {
			struct fd_handle *handle = fd_translate(i);
			if(handle->flags & O_CLOEXEC) {
				fd_close(i);
				continue;
			}

			hash_table_push(&task->fd_list, &handle->fd_number, handle, sizeof(handle->fd_number));
		}
	}

	hash_table_delete(&task_list, &current_task->pid, sizeof(current_task->pid));
	hash_table_delete(&task_list, &task->pid, sizeof(task->pid));

	task->cwd = current_task->cwd;
	task->pid = current_task->pid;
	task->ppid = current_task->ppid;
	task->exit_trigger = current_task->exit_trigger;

	thread->pid = task->pid;

	task->real_uid = current_task->real_uid;
	task->effective_uid = is_suid ? vfs_node->asset->stat->st_uid : current_task->effective_uid;
	task->saved_uid = task->effective_uid;

	task->real_gid = current_task->real_gid;
	task->effective_gid = is_sgid ? vfs_node->asset->stat->st_gid : current_task->effective_gid;
	task->saved_gid = task->effective_gid;

	task->pgid = current_task->pgid;
	task->group = current_task->group;
	task->sid = current_task->sid;
	task->session = current_task->session;

	task->umask = current_task->umask;

	task->has_execved = 1;

	CORE_LOCAL->pid = -1;
	CORE_LOCAL->tid = -1;

	hash_table_push(&task_list, &task->pid, task, sizeof(task->pid));

	sched_yield();
}

void syscall_fork(struct registers *regs) {
	spinlock(&sched_lock);

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] fork\n", CORE_LOCAL->pid);
#endif

	struct sched_task *current_task = CURRENT_TASK;
	if(current_task == NULL) {
		panic("");
	}

	struct sched_thread *current_thread = CURRENT_THREAD;
	if(current_thread == NULL) {
		panic("");
	}

	struct sched_task *task = alloc(sizeof(struct sched_task));
	struct sched_thread *thread = alloc(sizeof(struct sched_thread));

	task->pid = bitmap_alloc(&pid_bitmap);
	task->ppid = current_task->pid;
	task->status = TASK_WAITING;
	task->page_table = vmm_fork_page_table(current_task->page_table);
	task->cwd = current_task->cwd;

	task->real_uid = current_task->real_uid;
	task->effective_uid = current_task->effective_uid;
	task->saved_uid = current_task->saved_uid;

	task->real_gid = current_task->real_gid;
	task->effective_gid = current_task->effective_gid;
	task->saved_gid = current_task->saved_gid;

	task->umask = current_task->umask;

	task->pgid = current_task->pgid;
	task->group = current_task->group;
	task->sid = current_task->sid;
	task->session = current_task->session;

	task->event = alloc(sizeof(struct event));
	task->event->task = task;
	task->event->thread = thread;

	task->exit_trigger = alloc(sizeof(struct event_trigger));
	task->exit_trigger->agent_task = task;
	task->exit_trigger->agent_thread = thread;
	task->exit_trigger->event_type = EVENT_PROC_EXIT;

	task->exit_trigger->event = alloc(sizeof(struct event));
	task->exit_trigger->event->task = CURRENT_TASK;
	task->exit_trigger->event->thread = CURRENT_THREAD;

	task->tid_bitmap = (struct bitmap) {
		.data = NULL,
		.size = 0,
		.resizable = true
	};

	for(size_t i = 0; i < current_task->fd_list.capacity; i++) {
		struct fd_handle *handle = current_task->fd_list.data[i];
		if(handle) {
			struct fd_handle *new_handle = alloc(sizeof(struct fd_handle));
			*new_handle = *handle;
			file_get(new_handle->file_handle);
			hash_table_push(&task->fd_list, &new_handle->fd_number, new_handle, sizeof(new_handle->fd_number));
		}
	}

	bitmap_dup(&current_task->fd_bitmap, &task->fd_bitmap);

	thread->regs = *regs;
	thread->user_gs_base = current_thread->user_gs_base;
	thread->user_fs_base = current_thread->user_fs_base;
	thread->kernel_stack = pmm_alloc(DIV_ROUNDUP(THREAD_KERNEL_STACK_SIZE, PAGE_SIZE), 1) + THREAD_KERNEL_STACK_SIZE + HIGH_VMA;
	thread->user_stack = current_thread->user_stack;
	thread->status = TASK_WAITING;
	thread->tid = bitmap_alloc(&task->tid_bitmap);
	thread->pid = task->pid;

	hash_table_push(&task_list, &task->pid, task, sizeof(task->pid));
	hash_table_push(&task->thread_list, &thread->tid, thread, sizeof(thread->tid));

	regs->rax = task->pid;
	thread->regs.rax = 0;

	VECTOR_PUSH(current_task->children, task);

	spinrelease(&sched_lock);
}

void syscall_getpid(struct registers *regs) {
	regs->rax = CORE_LOCAL->pid;
}

void syscall_getppid(struct registers *regs) {
	regs->rax = CURRENT_TASK->ppid;
}

void syscall_gettid(struct registers *regs) {
	regs->rax = CORE_LOCAL->tid;
}

void syscall_getuid(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] getuid\n", CORE_LOCAL->pid);
#endif
	regs->rax = CURRENT_TASK->real_uid;
}

void syscall_geteuid(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] geteuid\n", CORE_LOCAL->pid);
#endif
	regs->rax = CURRENT_TASK->effective_uid;
}

void syscall_getgid(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] getgid\n", CORE_LOCAL->pid);
#endif
	regs->rax = CURRENT_TASK->real_gid;
}

void syscall_getegid(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] getegid\n", CORE_LOCAL->pid);
#endif
	regs->rax = CURRENT_TASK->effective_gid;
}

void syscall_setuid(struct registers *regs) {
	uid_t uid = regs->rdi;
	struct sched_task *current_task = CURRENT_TASK;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] setuid: uid {%x}\n", CORE_LOCAL->pid, uid);
#endif

	if(current_task->effective_uid == 0) {
		current_task->real_uid = uid;
		current_task->effective_uid = uid;
		current_task->saved_uid = uid;
		regs->rax = 0;
		return;
	}

	if(current_task->real_uid == uid || current_task->effective_uid == uid || current_task->saved_uid == uid) {
		current_task->effective_uid = uid;
		regs->rax = 0;
		return;
	}

	set_errno(EPERM);
	regs->rax = -1;
}

void syscall_seteuid(struct registers *regs) {
	uid_t euid = regs->rdi;
	struct sched_task *current_task = CURRENT_TASK;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] seteuid: euid {%x}\n", CORE_LOCAL->pid, euid);
#endif

	if(current_task->real_uid == euid || current_task->effective_uid == euid || current_task->saved_uid == euid) {
		current_task->effective_uid = euid;
		regs->rax = 0;
		return;
	}

	set_errno(EPERM);
	regs->rax = -1;
}

void syscall_setgid(struct registers *regs) {
	gid_t gid = regs->rdi;
	struct sched_task *current_task = CURRENT_TASK;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] setgid: gid {%x}\n", CORE_LOCAL->pid, gid);
#endif

	if(current_task->effective_uid == 0) {
		current_task->real_gid = gid;
		current_task->effective_gid = gid;
		current_task->saved_gid = gid;
		regs->rax = 0;
		return;
	}

	if(current_task->real_gid == gid || current_task->effective_gid == gid || current_task->saved_gid == gid) {
		current_task->effective_gid = gid;
		regs->rax = 0;
		return;
	}

	set_errno(EPERM);
	regs->rax = -1;
}

void syscall_setegid(struct registers *regs) {
	uid_t egid = regs->rdi;
	struct sched_task *current_task = CURRENT_TASK;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] setegid: egid {%x}\n", CORE_LOCAL->pid, egid);
#endif

	if(current_task->real_gid == egid || current_task->effective_gid == egid || current_task->saved_gid == egid) {
		current_task->effective_gid = egid;
		regs->rax = 0;
		return;
	}

	set_errno(EPERM);
	regs->rax = -1;
}

void syscall_setpgid(struct registers *regs) {
	pid_t pid = regs->rdi;
	pid_t pgid = regs->rsi;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] setpgid: pid {%x}, pgid {%x}\n", CORE_LOCAL->pid, pid, pgid);
#endif

	struct sched_task *task = sched_translate_pid(pid);
	if(task == NULL) {
		set_errno(ESRCH);
		regs->rax = -1;
		return;
	}

	regs->rax = task_setpgid(task, pgid);
}

void syscall_getpgid(struct registers *regs) {
	pid_t pid = regs->rdi;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] getpgid: pid {%x}\n", CORE_LOCAL->pid, pid);
#endif

	struct sched_task *task = sched_translate_pid(pid);
	if(task == NULL) {
		set_errno(ESRCH);
		regs->rax = -1;
		return;
	}

	regs->rax = task->pgid;
}

void syscall_setsid(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] setsid\n", CORE_LOCAL->pid);
#endif

	struct sched_task *current_task = CURRENT_TASK;
	if(current_task == NULL) {
		panic("");
	}

	regs->rax = task_create_session(current_task);
}

void syscall_getsid(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] getsid\n", CORE_LOCAL->pid);
#endif

	regs->rax = CURRENT_TASK->sid;
}
