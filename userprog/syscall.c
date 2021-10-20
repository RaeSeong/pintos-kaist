#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "threads/flags.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include <list.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "intrinsic.h"
#include "vm/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/*              system call by inkyu            */
void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int open (const char *file);
int filesize(int fd);
int exec(const *cmd_line);
int read (int fd , void *buffer, unsigned size);
int write (int fd , const void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
int wait (tid_t tid);
tid_t fork(const char *thread_name, struct intr_frame *f);
int dup2(int oldfd, int newfd);

static void *mmap(void *addr, size_t length, int writable, int fd, off_t offset);
static void munmap(void *addr);

/*              system call need func by inkyu            */
int add_file_to_fdt(struct file *file);
static struct file *find_file_by_fd(int fd);
void check_address(const uint64_t *uaddr);
static void check_writable_addr(void* ptr);
void remove_file_from_fdt(int fd);

const int STDIN = 1;
const int STDOUT = 2;
struct lock file_lock;

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
	lock_init(&file_lock);
	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	switch (f->R.rax)
	{
	case SYS_HALT:
		halt();
		break;
	case SYS_EXIT:
		exit(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
	case SYS_OPEN:
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_CLOSE:
		close(f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_EXEC:
		if (exec(f->R.rdi) == -1)
			exit(-1);
		break;
	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_DUP2:
		f->R.rax = dup2(f->R.rdi, f->R.rsi);
		break;
	//project 3
	case SYS_MMAP:
		f->R.rax = (uint64_t)mmap((void *)f->R.rdi, (size_t)f->R.rsi, (int)f->R.rdx, (int)f->R.r10, (off_t)f->R.r8);
		break;
	case SYS_MUNMAP:
		munmap((void *)f->R.rdi);
		break;
	default:
		break;
	}
}

void halt(void)
{
	power_off();
}

void exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;

	printf("%s: exit(%d)\n", thread_name(), status); // Process Termination Message
	thread_exit();
}

bool create(const char *file, unsigned initial_size)
{
	check_address(file);
	return filesys_create(file, initial_size);
}

bool remove(const char *file)
{
	check_address(file);
	return filesys_remove(file);
}

int
open (const char *file) {
	check_address(file);
	struct file *fileobj = filesys_open(file);
	if (fileobj == NULL)
		return -1;
	
	/*   아직 file이 막 open되었는데 다른 process들이 접근하여 파일을 수정해주어서는 안된다     */
	/* 때문에 현재 thread에 실행예정인 file의 주소를 넣어주고 접근을 못하게 막아준다. */
	if(strcmp(thread_name(), file) == 0){
		//file_deny_write(fileobj);
		}

	int fd = add_file_to_fdt(fileobj);
	lock_acquire(&file_lock);
	if(fd == -1){
		file_close(fileobj);
	}
	lock_release(&file_lock);
	return fd;
}

void close(int fd)
{
	/*       fd를이용하여 file 받음 by inkyu           */
	struct file *objfile = find_file_by_fd(fd);
	struct thread *cur = thread_current();

	if(objfile == NULL)
		return;

	if (fd == 0 || objfile == STDIN)
	{
		cur->stdin_count--;
	}
	else if (fd == 1 || objfile == STDOUT)
	{
		cur->stdout_count--;
	}

	if (fd <= 1 || objfile <= 2)
		return;

/*      table 에서 삭제           */
	remove_file_from_fdt(fd);
	lock_acquire(&file_lock);
	if(objfile->dupCount == 0)
		file_close(objfile);
	else if(objfile->dupCount > 0)
		objfile->dupCount--;
	lock_release(&file_lock);
	return;
}

int add_file_to_fdt(struct file *file)
{
	struct thread *cur = thread_current();
	struct file **fdt = cur->fdTable;
	//파일 개수 및 새로 열 파일의 fd값 raise
	while(cur->fdIdx<FDCOUNT_LIMIT && fdt[cur->fdIdx])
	{
		//printf("\n%d\n", cur->fdIdx);
		cur->fdIdx++;
	}

	// fdt full limit 512개
	if(cur->fdIdx >= FDCOUNT_LIMIT)
		return -1;
	
	fdt[cur->fdIdx] = file;
	return cur->fdIdx;
}

int filesize(int fd)
{
	struct file *fileobj = find_file_by_fd(fd);
	if(fileobj == NULL)
		return -1;
	return file_length(fileobj);
}

static struct file *find_file_by_fd(int fd)
{
	struct thread *cur = thread_current();
	if(fd<0 || fd>=FDCOUNT_LIMIT)
		return NULL;

	return cur->fdTable[fd];
}

void check_address(const uint64_t *uaddr)
{
	struct thread *cur = thread_current();
	
	if (uaddr == NULL) exit(-1);
	if (!(is_user_vaddr(uaddr))) exit(-1);
	uint64_t *pte = pml4e_walk(cur->pml4, (const uint64_t) uaddr, 0);
	if (pte == NULL) exit(-1);
	struct page *page = spt_find_page (&thread_current() -> spt, uaddr);
	if (page == NULL) exit(-1);
}

int exec(const *cmd_line)
{
	check_address(cmd_line);
	char *file_name[30];
	memcpy(file_name, cmd_line, strlen(cmd_line) + 1);
	if(process_exec(file_name) == -1)
		return -1;

	NOT_REACHED();
	return 0;
}

void remove_file_from_fdt(int fd)
{
	struct thread *cur = thread_current();

	// Error - invalid fd
	if (fd < 0 || fd >= FDCOUNT_LIMIT)
		return;

	cur->fdTable[fd] = NULL;
}

int read (int fd , void *buffer, unsigned size)
{
	
	check_address(buffer);
	check_writable_addr(buffer);
	int length;
	struct thread *cur = thread_current();
	struct file *fileobj = find_file_by_fd(fd);
	if(fileobj == NULL)
		return -1;
	
/*     extra 문제에서 더이상 stdin이 연결된애가 없으면 읽기 금지      */
	if(fd == 0 && cur->stdin_count!=0)
	{
		int i;
			unsigned char *buf = buffer;
			for (i = 0; i < size; i++)
			{
				char c = input_getc();
				*buf++ = c;
				if (c == '\0')
					break;
			}
			length = i;
	}
	else if(fd == 1){
		length = -1;
	}
	else if(fd>1){
		lock_acquire(&file_lock);
		length = file_read(fileobj, buffer, size);
		lock_release(&file_lock);
	}
	return length;
}

int write(int fd, const void *buffer, unsigned size)
{
	check_address(buffer);
	int length;
	struct thread *cur = thread_current();
	struct file *fileobj = find_file_by_fd(fd);
	
	if (fileobj == NULL)
		return -1;

/*        fd가 stdout인경우 putbuf를 이용하여 화면에 출력          */
/*     extra 문제에서 더이상 stdout이 연결된애가 없으면 쓰기 금지      */
	if(fileobj == STDOUT && cur->stdout_count != 0){
		//printf("\n%d : %p\n", fd, fileobj);
		putbuf(buffer, size);
		length = size;
	}
	else if(fileobj == STDIN){
		length = -1;
	}
	else if(fileobj>2){
		lock_acquire(&file_lock);
		length = file_write(fileobj, buffer, size);
		lock_release(&file_lock);
	}
	
	return length;
}

void seek(int fd, unsigned position)
{
	struct file *fileobj = find_file_by_fd(fd);
	if(fileobj <= 2)
		return;
	lock_acquire(&file_lock);
	fileobj->pos = position;
	lock_release(&file_lock);
	return;
}

unsigned tell(int fd)
{
	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj <= 2)
		return;
	return fileobj->pos;
}

int wait (tid_t tid)
{
	process_wait(tid);
}

tid_t fork(const char *thread_name, struct intr_frame *f)
{
	return process_fork(thread_name, f);
}

int dup2(int oldfd, int newfd)
{
	struct thread *cur = thread_current();
	struct file *objfile = find_file_by_fd(oldfd);
	if (objfile == NULL)
		return -1;
	if (oldfd == newfd)
		return newfd;

	if (objfile == STDIN)
		cur->stdin_count++;
	else if (objfile == STDOUT)
		cur->stdout_count++;
	else
		objfile->dupCount++;
	close(newfd);
	cur->fdTable[newfd] = objfile;

	return newfd;
}

static void
check_writable_addr(void* ptr){
	struct page *page = spt_find_page (&thread_current() -> spt, ptr);
	if (page == NULL || !page->writable) exit(-1);
}

static void *mmap(void *addr, size_t length, int writable, int fd, off_t offset){
	if(addr == 0 || (!is_user_vaddr(addr)) || (uint64_t)addr % PGSIZE != 0 || offset % PGSIZE != 0
	|| (uint64_t)addr + length == 0 || !is_user_vaddr((uint64_t)addr + length))
		return NULL;
	for(uint64_t i = (uint64_t)addr; i < (uint64_t)addr + length; i += PGSIZE){
		if(spt_find_page(&thread_current()->spt, (void *)i) != NULL)
			return NULL;
	}
	struct file *file = find_file_by_fd(fd);
	if(file == NULL)
		return;
	return do_mmap(addr, length, writable, file, offset);
}

static void munmap(void *addr){
	do_munmap(addr);
}