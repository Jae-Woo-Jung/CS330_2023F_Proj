#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include <string.h>
#include "userprog/process.h"
#ifdef VM
#include "vm/file.h"
#endif
#ifdef EFILESYS
#include "filesys/directory.h"
#include "filesys/inode.h"
#endif
void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* Big lock for filesystem. */
struct lock filesys_lock;

static bool fd_cmp(const struct list_elem *a, const struct list_elem *b,
					void *aux UNUSED) {
	int a_fd, b_fd;
	a_fd = list_entry(a, struct fd_list_elem, elem)->fd;
	b_fd = list_entry(b, struct fd_list_elem, elem)->fd;
	return a_fd < b_fd;
}

static struct file * fd_to_file(int fd) {
	struct list_elem *e;
	struct file *ret = NULL;

	for (e = list_begin(thread_current()->fd_list);
		e != list_end(thread_current()->fd_list); e=list_next(e)) {
		if (fd == list_entry(e, struct fd_list_elem, elem)->fd) {
			ret = list_entry(e, struct fd_list_elem, elem)->file_ptr;
			return ret;
		}
	}
	return NULL;
}

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	g_fd_nr = 3;
	lock_init(&syscall_lock);
	lock_init(&filesys_lock);
}

static void
error_die (void) {
	SyS_exit(-1);
}

static bool
validate_ptr (const void *p, size_t size, bool writable) {
	if (p == NULL || !is_user_vaddr (p))
		return false;

	struct thread *current = thread_current ();
	void *ptr = pg_round_down (p);
	for (; ptr <= pg_round_down (p + size); ptr += PGSIZE) {
		#ifndef VM
		uint64_t *pte = pml4e_walk (current->pml4, (uint64_t) ptr, 0);
		if (pte == NULL ||
				is_kern_pte(pte) ||
				(writable && !is_writable (pte)))
			return false;
		#else
		struct page* page = spt_find_page(&current->spt, ptr);
		if (page == NULL || (writable && !page->writable))
			return false;
		#endif
	}

	return true;
}

static bool
validate_string (const void *p) {
	if (p == NULL || !is_user_vaddr (p))
		return false;
	struct thread *current = thread_current ();
	void *ptr = pg_round_down (p);
	for (; ; ptr += PGSIZE) {
		uint64_t *pte = pml4e_walk (current->pml4, (uint64_t) ptr, 0);
		if (pte == NULL || is_kern_pte(pte))
			return false;

		for (; *(char *)p != 0; p++);
		if (*(char *)p == 0)
			return true;
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	memcpy(&thread_current()->parent_if, f, sizeof(struct intr_frame));

	switch (f->R.rax) {
		case SYS_HALT:
			SyS_halt();
			break;
		case SYS_EXIT:
			SyS_exit(f->R.rdi);
			break;
		case SYS_FORK:
			f->R.rax = SyS_fork(f->R.rdi);
			break;
		case SYS_EXEC:
			f->R.rax = SyS_exec(f->R.rdi);
			break;
		case SYS_WAIT:
			f->R.rax = SyS_wait(f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = SyS_create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = SyS_remove(f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = SyS_open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = SyS_filesize(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = SyS_read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = SyS_write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			SyS_seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = SyS_tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			SyS_close(f->R.rdi);
			break;
#ifdef VM
		case SYS_MMAP:
			f->R.rax = SyS_mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
			break;
		case SYS_MUNMAP:
			SyS_munmap(f->R.rdi);
			break;
#endif
#ifdef EFILESYS
		case SYS_CHDIR:
			f->R.rax = SyS_chdir(f->R.rdi);
			break;
		case SYS_MKDIR:
			f->R.rax = SyS_mkdir(f->R.rdi);
			break;
		case SYS_READDIR:
			f->R.rax = SyS_readdir(f->R.rdi, f->R.rsi);
			break;
		case SYS_ISDIR:
			f->R.rax = SyS_isdir(f->R.rdi);
			break;
		case SYS_INUMBER:
			f->R.rax = SyS_inumber(f->R.rdi);
			break;
		case SYS_SYMLINK:
			f->R.rax = SyS_symlink(f->R.rdi, f->R.rsi);
			break;
#endif
		default:
			printf ("system call!\n");
			thread_exit ();
			break;

	}
	
}

void check_address (void *addr) {
	if (!is_user_vaddr(addr))
		SyS_exit(-1);
}

void SyS_halt (void) {
	power_off();
}

void SyS_exit (int status) {
	struct thread *t = thread_current();
	t->exit_status = status;

	printf("%s: exit(%d)\n", t->name, status); 
	thread_exit();
}

pid_t SyS_fork (const char *thread_name) {
	if (!validate_string (thread_name))
		error_die ();

	lock_acquire(&filesys_lock);
	pid_t child_pid = (pid_t) process_fork(thread_name, &thread_current()->parent_if);
	lock_release(&filesys_lock);

	if (child_pid == TID_ERROR)
		return TID_ERROR;

	struct thread *child = NULL;
	for (struct list_elem *e = list_begin(&thread_current()->child_list);
			e != list_end(&thread_current()->child_list); e=list_next(e)) {
		struct thread *tmp = list_entry(e, struct thread, child_elem);
		if (tmp->tid == child_pid) {
			child = tmp;
			break;
		}
	}

	if (child == NULL) {
		return TID_ERROR;
	} else {
		sema_down(&child->_do_fork_sema);

		if (child->exit_status == TID_ERROR)
			return TID_ERROR;
	}	
	return child_pid;
}

int SyS_exec (const char *cmd_line){
	if (!validate_string (cmd_line))
		error_die ();
	
	unsigned long memsz = strlen(cmd_line) + 1;
	char *copy = (char *) palloc_get_multiple(0, (memsz + (PGSIZE - 1)) / PGSIZE);
	strlcpy(copy, cmd_line, memsz);

	int result = process_exec(copy);
	if (result == -1)
		error_die ();
	
	thread_current() -> exit_status = result;
	return result;
}

int SyS_wait (pid_t pid) {
	return process_wait((tid_t) pid);
}

bool SyS_create (const char *file, unsigned initial_size) {
	bool ret;
	if (!validate_string (file) || !strcmp (file, ""))
		error_die ();

	if (file == NULL)
		SyS_exit(-1);

	lock_acquire(&filesys_lock);
	ret = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
	return ret;
}

bool SyS_remove (const char *file) {
	bool ret;
	if (!validate_string (file))
		error_die ();

	if (file == NULL)
		return false;

	lock_acquire(&filesys_lock);
	ret = filesys_remove(file);
	lock_release(&filesys_lock);
	return ret;
}

int SyS_open (const char *file) {
	struct fd_list_elem *fd_elem = NULL;
	struct file *file_open = NULL;

	if (!validate_string (file))
		error_die ();

	if (file == NULL)
		return -1;

	lock_acquire(&filesys_lock);
	file_open = filesys_open(file);
	lock_release(&filesys_lock);
	if (file_open == NULL)
		return -1;

	fd_elem = malloc(sizeof(struct fd_list_elem));
	if (!fd_elem)
		return -1;

	lock_acquire(&filesys_lock);
	fd_elem->fd = g_fd_nr;
	fd_elem->file_ptr = file_open;
	g_fd_nr++;
	lock_release(&filesys_lock);

	list_insert_ordered(thread_current()->fd_list, &fd_elem->elem, fd_cmp, NULL);
	return fd_elem->fd;
}

int SyS_filesize (int fd) {
	struct file *file_ptr = fd_to_file(fd);

	if (file_ptr == NULL)
		return -1;
	else
		return file_length(file_ptr);
}

int SyS_read (int fd, void *buffer, unsigned size) {
	if (!validate_ptr (buffer, size, true))
		error_die ();
	unsigned long count = 0;

	if (fd == 0) {
		lock_acquire(&syscall_lock);
		for (count = 0; count < size; count++) {
			((char *)buffer)[count] = input_getc();
		}
		lock_release(&syscall_lock);

		return count; 
	} else if (fd == 1) {
		SyS_exit(-1);
		return -1;
	} else {
		struct file *fd_file = fd_to_file(fd);

		#ifdef VM
		if (spt_find_page(&thread_current()->spt, buffer) != NULL
			&& spt_find_page(&thread_current()->spt, buffer)->writable == 0)
			SyS_exit(-1);
		#endif

		if (fd_file != NULL) {
			lock_acquire(&filesys_lock);
			count = file_read(fd_file, buffer, size);
			lock_release(&filesys_lock);
			return count;
		} else {
			SyS_exit(-1);
			return -1;
		}
	}
}

int SyS_write(int fd, const void *buffer, unsigned size) {
	int ret;
	if (!validate_ptr (buffer, size, false))
		error_die ();

	if (fd == 1) {
		lock_acquire(&syscall_lock);
		putbuf(buffer, size);
		lock_release(&syscall_lock);

		return size;
	} else if (fd == 0) {
		return -1;
	} else {
		struct file *fd_file = fd_to_file(fd);
		if (fd_file == NULL || file_is_reg(fd_file) == false)
			return -1;

		lock_acquire(&filesys_lock);
		ret = file_write(fd_file, buffer, size);
		lock_release(&filesys_lock);
		return ret;
	}
}

void SyS_close (int fd) {
	struct fd_list_elem * close_fd_list_elem = NULL;
	struct list_elem *e;

	for (e = list_begin(thread_current()->fd_list);
			e != list_end(thread_current()->fd_list); e=list_next(e)) {
		struct fd_list_elem *tmp = list_entry(e, struct fd_list_elem, elem);
		if (fd == tmp->fd) {
			close_fd_list_elem = tmp;
			break;
		}
	}

	if (close_fd_list_elem == NULL) {
		SyS_exit(-1);
	} else {
		list_remove(e);
		lock_acquire(&filesys_lock);
		file_close(close_fd_list_elem->file_ptr);
		lock_release(&filesys_lock);
		free(close_fd_list_elem); 
	}
}

void SyS_seek (int fd, unsigned position) {
	struct file *fd_file = fd_to_file(fd);
	if (fd_file == NULL || file_is_reg(fd_file) == false)
		 return;
	lock_acquire(&filesys_lock);
	file_seek(fd_file, position);
	lock_release(&filesys_lock);
}

unsigned SyS_tell (int fd) {
	struct file *fd_file = fd_to_file(fd);
	unsigned ret = 0;
	if (fd_file == NULL || file_is_reg(fd_file) == false)
		 return ret;

	lock_acquire(&filesys_lock);
	ret = (unsigned) file_tell(fd_to_file(fd));
	lock_release(&filesys_lock);
	return ret;
}

#ifdef VM
void *SyS_mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
	if ( is_kernel_vaddr(addr) )
		return NULL;

	if (addr == NULL || (pg_round_down(addr)!= addr))
		return NULL;

	if (length <= 0 || length >= KERN_BASE)
		return NULL;

	if (fd <= 1)
		return NULL;

	if (pg_round_down(offset) != offset)
		return NULL;

	struct file *fd_file = fd_to_file(fd);
	if (fd_file == NULL)
		return NULL;

	if (file_length(fd_file) == 0 || file_length(fd_file) <= offset)
		return NULL;

	return do_mmap(addr, length, writable, fd_file, offset);
}

void SyS_munmap (void *addr) {
	if (pg_round_down(addr) != addr)
		return;

	struct page *page = spt_find_page(&thread_current()->spt, addr);

	if (page == NULL)
		return;

	if (page->operations->type != VM_FILE)
		return;

	if (!page->file.load_info.is_first_page)
		return;

	do_munmap(addr);
}
#endif

#ifdef EFILESYS
bool SyS_chdir (const char *dir) {
	bool ret;
	check_address(dir);

	if (strlen(dir) == 0)
		return false;

	lock_acquire(&filesys_lock);
	ret = dir_chdir(dir);
	lock_release(&filesys_lock);
	return ret;
}

bool SyS_mkdir (const char *dir) {
	bool ret;
	check_address(dir);

	lock_acquire(&filesys_lock);
	ret = dir_mkdir(dir);
	lock_release(&filesys_lock);
	return ret;
}

bool SyS_readdir (int fd, char *name) {
	bool ret;
	check_address(name);

	struct file *file = fd_to_file(fd);
	if (file == NULL || file_is_dir(file) == false)
		return false;

	lock_acquire(&filesys_lock);
	ret = dir_readdir((struct dir *) file, name);
	lock_release(&filesys_lock);
	return ret;
}

bool SyS_isdir (int fd) {
	struct file *file = fd_to_file(fd);
	bool ret;

	lock_acquire(&filesys_lock);
	ret = file_is_dir(file);
	lock_release(&filesys_lock);
	return ret;
}

int SyS_inumber (int fd) {
	struct file *file = fd_to_file(fd);
	int ret;

	lock_acquire(&filesys_lock);
	ret = inode_get_inumber(file_get_inode(file));
	lock_release(&filesys_lock);
	return ret;
}

int SyS_symlink (const char *target, const char *linkpath) {
	int ret;
	check_address(target);
	check_address(linkpath);

	lock_acquire(&filesys_lock);
	ret = filesys_symlink(target, linkpath);
	lock_release(&filesys_lock);
	return ret;
}
#endif
