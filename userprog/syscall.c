#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>

#include "devices/input.h"
#include "devices/shutdown.h"

#include "filesys/file.h"
#include "filesys/filesys.h"

#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#include "userprog/pagedir.h"
#include "userprog/process.h"

#define MAX_ARGS 3
static void syscall_handler (struct intr_frame *);
void fetch_args (uint32_t* esp, uint32_t* args_, int num);
void check_vaddr (const void* vaddr);
void exit_ (int status);
int exec_ (const char * file_name);
bool create_(const char *file, uint32_t initial_size);
bool remove_(const char *file);
int open_(const char *file);
int filesize_(int fd_num);
struct fd* search_fd(int fd_num);
int read_(int fd_num, void *buffer, uint32_t size);
void check_buffer(const uint8_t* buffer, uint32_t size);
int write_(int fd_num, const void *buffer, uint32_t size);
void seek_(int fd_num, uint32_t position);
uint32_t tell_(int fd_num);
void close_(int fd_num);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
	uint32_t* esp = f->esp;
	check_vaddr(esp);
	uint32_t syscall_num = *esp;
	esp++;
	uint32_t args[MAX_ARGS];
	switch (syscall_num) {
		case SYS_HALT:
			shutdown_power_off();
			break;
		case SYS_EXIT:
			fetch_args (esp, args, 1);
			exit_((int)args[0]);
			break;
		case SYS_EXEC:
			fetch_args (esp, args, 1);
			f->eax = exec_((const char *)args[0]);
			break;
		case SYS_WAIT:
			fetch_args (esp, args, 1);
			f->eax = process_wait((tid_t)args[0]);
			break;
		case SYS_CREATE:
			fetch_args (esp, args, 2);
			f->eax = create_((const char*)args[0], args[1]);
			break;
		case SYS_REMOVE:
			fetch_args (esp, args, 1);
			f->eax = remove_((const char*)args[0]);
			break;
		case SYS_OPEN:
			fetch_args (esp, args, 1);
			f->eax = open_((const char*)args[0]);
			break;
		case SYS_FILESIZE:
			fetch_args (esp, args, 1);
			f->eax = filesize_((int)args[0]);
			break;
		case SYS_READ:
			fetch_args (esp, args, 3);
			f->eax = read_((int)args[0], (void *)args[1], args[2]);
			break;
		case SYS_WRITE:
			fetch_args (esp, args, 3);
			f->eax = write_((int)args[0], (const void*)args[1], args[2]);
			break;
		case SYS_SEEK:
			fetch_args (esp, args, 2);
			seek_((int)args[0], args[1]);
			break;
		case SYS_TELL:
			fetch_args (esp, args, 1);
			f->eax = tell_((int)args[0]);
			break;
		case SYS_CLOSE:
			fetch_args (esp, args, 1);
			close_((int)args[0]);
			break;
		default:
			printf("Unimplemented system call%d", syscall_num);
			break;
	}
}

void
exit_ (int status) {
	thread_current()->child_node->exit_status = status;
	thread_exit();
}

int
exec_ (const char *file_name) {
	check_vaddr((const void*) file_name);
	struct thread *cur = thread_current();
	cur->in_exec = true;
	int pid = process_execute(file_name);
	cur->in_exec = false;
	return pid;
}

bool
create_(const char *file, uint32_t initial_size) {
	check_vaddr((const void*)file);
	lock_acquire(filesys_lock);
	bool status = filesys_create(file, initial_size);
	lock_release(filesys_lock);
	return status;
}

bool
remove_(const char *file) {
	check_vaddr((const void*)file);
	lock_acquire(filesys_lock);
	bool status = filesys_remove(file);
	lock_release(filesys_lock);
	return status;
}

int
open_(const char *file) {
	check_vaddr((const void*)file);
	lock_acquire(filesys_lock);
	struct file* f = filesys_open(file);
	lock_release(filesys_lock);
	if (f == NULL) {
		return -1;
	}
	struct thread *cur = thread_current();
	struct fd *fd = malloc(sizeof(struct fd));
	fd->f = f;
	fd->fd_num = cur->fd_count;
	cur->fd_count++;
	list_push_back(&cur->fd_table, &fd->elem);
	return fd->fd_num;
}

int
filesize_(int fd_num) {
	struct fd *fd = search_fd(fd_num);
	lock_acquire(filesys_lock);
	int len = file_length(fd->f);
	lock_release(filesys_lock);
	return len;
}

int
read_(int fd_num, void *buffer, uint32_t size) {
	if (fd_num == 1) return -1;
	check_buffer(buffer, size);
	if (fd_num == 0) {
		uint8_t *tmp = buffer;
		for (uint32_t i = 0; i < size; i++) {
			*(tmp + i) = input_getc();
		}
		return size;
	}
	struct fd* fd = search_fd(fd_num);
	if (fd == NULL) return -1;
	lock_acquire(filesys_lock);
	int num = file_read(fd->f, buffer, size);
	lock_release(filesys_lock);
	return num;
}

int
write_(int fd_num, const void *buffer, uint32_t size) {
	if (fd_num == 0) return -1;
	check_buffer(buffer, size);
	if (fd_num == 1) {
		putbuf(buffer, size);
		return size;
	}
	struct fd *fd = search_fd (fd_num);
	if (fd == NULL) return -1;
	lock_acquire(filesys_lock);
	int num = file_write(fd->f, buffer, size);
	lock_release(filesys_lock);
	return num;
}

void
seek_ (int fd_num, uint32_t size) {
	struct fd* fd = search_fd(fd_num);
	lock_acquire(filesys_lock);
	file_seek(fd->f, size);
	lock_release(filesys_lock);
}

uint32_t
tell_ (int fd_num) {
	struct fd* fd = search_fd(fd_num);
	lock_acquire(filesys_lock);
	uint32_t pos = file_tell(fd->f);
	lock_release(filesys_lock);
	return pos;
}

void
close_ (int fd_num) {
	struct fd* fd = search_fd(fd_num);
	if (fd == NULL) exit_(-1);
	lock_acquire(filesys_lock);
	file_close(fd->f);
	lock_release(filesys_lock);
	list_remove(&fd->elem);
	free(fd);
}

struct fd*
search_fd(int fd_num) {
	struct thread *cur = thread_current();
	struct list_elem *e;
	struct fd *fd = NULL;
	for (e = list_begin(&cur->fd_table); e != list_end(&cur->fd_table); e = list_next(e)) {
	struct fd *tmp = list_entry(e, struct fd, elem);
		if (tmp->fd_num == fd_num) {
			fd = tmp;
			break;
		}
	}
	return fd;
}

void
fetch_args (uint32_t* esp, uint32_t* args, int num) {
	for (int i = 0; i < num; i++) {
		check_vaddr(esp);
		*(args + i) = *esp;
		esp++;
	}
}

void
check_vaddr (const void* vaddr) {
	if (vaddr == NULL || !is_user_vaddr(vaddr) ||
		pagedir_get_page (thread_current()->pagedir, vaddr) == NULL) {
		exit_(-1);
	}
}

void
check_buffer(const uint8_t* buffer, uint32_t size) {
	check_vaddr(buffer);
	check_vaddr(buffer + size);
}


