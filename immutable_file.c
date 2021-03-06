
#define pr_fmt(fmt) "immutable_file: " fmt

#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/kprobes.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <asm/signal.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>

MODULE_DESCRIPTION("Protect write to a file and kill the responsible process");
MODULE_AUTHOR("Shubham <shubham0d@protonmail.com>");
MODULE_LICENSE("GPL 3");

unsigned int target_fd = 0;
unsigned int target_pid = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,7,0)
static unsigned long lookup_name(const char *name)
{
	struct kprobe kp = {
		.symbol_name = name
	};
	unsigned long retval;

	if (register_kprobe(&kp) < 0) return 0;
	retval = (unsigned long) kp.addr;
	unregister_kprobe(&kp);
	return retval;
}
#else
static unsigned long lookup_name(const char *name)
{
	return kallsyms_lookup_name(name);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,11,0)
#define FTRACE_OPS_FL_RECURSION FTRACE_OPS_FL_RECURSION_SAFE
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,11,0)
#define ftrace_regs pt_regs

static __always_inline struct pt_regs *ftrace_get_regs(struct ftrace_regs *fregs)
{
	return fregs;
}
#endif

/*
 * There are two ways of preventing vicious recursive loops when hooking:
 * - detect recusion using function return address (USE_FENTRY_OFFSET = 0)
 * - avoid recusion by jumping over the ftrace call (USE_FENTRY_OFFSET = 1)
 */
#define USE_FENTRY_OFFSET 0

/**
 * struct ftrace_hook - describes a single hook to install
 *
 * @name:     name of the function to hook
 *
 * @function: pointer to the function to execute instead
 *
 * @original: pointer to the location where to save a pointer
 *            to the original function
 *
 * @address:  kernel address of the function entry
 *
 * @ops:      ftrace_ops state for this function hook
 *
 * The user should fill in only &name, &hook, &orig fields.
 * Other fields are considered implementation details.
 */
struct ftrace_hook {
	const char *name;
	void *function;
	void *original;

	unsigned long address;
	struct ftrace_ops ops;
};

static int fh_resolve_hook_address(struct ftrace_hook *hook)
{
	hook->address = lookup_name(hook->name);

	if (!hook->address) {
		pr_debug("unresolved symbol: %s\n", hook->name);
		return -ENOENT;
	}

#if USE_FENTRY_OFFSET
	*((unsigned long*) hook->original) = hook->address + MCOUNT_INSN_SIZE;
#else
	*((unsigned long*) hook->original) = hook->address;
#endif

	return 0;
}

static void notrace fh_ftrace_thunk(unsigned long ip, unsigned long parent_ip,
		struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);

#if USE_FENTRY_OFFSET
	regs->ip = (unsigned long)hook->function;
#else
	if (!within_module(parent_ip, THIS_MODULE))
		regs->ip = (unsigned long)hook->function;
#endif
}

/**
 * fh_install_hooks() - register and enable a single hook
 * @hook: a hook to install
 *
 * Returns: zero on success, negative error code otherwise.
 */
int fh_install_hook(struct ftrace_hook *hook)
{
	int err;

	err = fh_resolve_hook_address(hook);
	if (err)
		return err;

	/*
	 * We're going to modify %rip register so we'll need IPMODIFY flag
	 * and SAVE_REGS as its prerequisite. ftrace's anti-recursion guard
	 * is useless if we change %rip so disable it with RECURSION.
	 * We'll perform our own checks for trace function reentry.
	 */
	hook->ops.func = fh_ftrace_thunk;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS
	                | FTRACE_OPS_FL_RECURSION
	                | FTRACE_OPS_FL_IPMODIFY;

	err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
	if (err) {
		pr_debug("ftrace_set_filter_ip() failed: %d\n", err);
		return err;
	}

	err = register_ftrace_function(&hook->ops);
	if (err) {
		pr_debug("register_ftrace_function() failed: %d\n", err);
		ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
		return err;
	}

	return 0;
}

/**
 * fh_remove_hooks() - disable and unregister a single hook
 * @hook: a hook to remove
 */
void fh_remove_hook(struct ftrace_hook *hook)
{
	int err;

	err = unregister_ftrace_function(&hook->ops);
	if (err) {
		pr_debug("unregister_ftrace_function() failed: %d\n", err);
	}

	err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
	if (err) {
		pr_debug("ftrace_set_filter_ip() failed: %d\n", err);
	}
}

/**
 * fh_install_hooks() - register and enable multiple hooks
 * @hooks: array of hooks to install
 * @count: number of hooks to install
 *
 * If some hooks fail to install then all hooks will be removed.
 *
 * Returns: zero on success, negative error code otherwise.
 */
int fh_install_hooks(struct ftrace_hook *hooks, size_t count)
{
	int err;
	size_t i;

	for (i = 0; i < count; i++) {
		err = fh_install_hook(&hooks[i]);
		if (err)
			goto error;
	}

	return 0;

error:
	while (i != 0) {
		fh_remove_hook(&hooks[--i]);
	}

	return err;
}

/**
 * fh_remove_hooks() - disable and unregister multiple hooks
 * @hooks: array of hooks to remove
 * @count: number of hooks to remove
 */
void fh_remove_hooks(struct ftrace_hook *hooks, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		fh_remove_hook(&hooks[i]);
}

#ifndef CONFIG_X86_64
#error Currently only x86_64 architecture is supported
#endif

#if defined(CONFIG_X86_64) && (LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0))
#define PTREGS_SYSCALL_STUBS 1
#endif

/*
 * Tail call optimization can interfere with recursion detection based on
 * return address on the stack. Disable it to avoid machine hangups.
 */
#if !USE_FENTRY_OFFSET
#pragma GCC optimize("-fno-optimize-sibling-calls")
#endif

static char *duplicate_filename(const char __user *filename)
{
	char *kernel_filename;

	kernel_filename = kmalloc(4096, GFP_KERNEL);
	if (!kernel_filename)
		return NULL;

	if (strncpy_from_user(kernel_filename, filename, 4096) < 0) {
		kfree(kernel_filename);
		return NULL;
	}

	return kernel_filename;
}


#ifdef PTREGS_SYSCALL_STUBS
static asmlinkage long (*real_sys_write)(struct pt_regs *regs);

static asmlinkage long fh_sys_write(struct pt_regs *regs)
{
	long ret;
	struct task_struct *task;
	task = current;
	int signum = SIGKILL;
	if (task->pid == target_pid)
	{
		if (regs->di == target_fd)
		{
			pr_info("write done by process %d to target file.\n", task->pid);
			struct kernel_siginfo info;
			memset(&info, 0, sizeof(struct kernel_siginfo));
			info.si_signo = signum;
			int ret = send_sig_info(signum, &info, task);
					if (ret < 0)
					{
					  printk(KERN_INFO "error sending signal\n");
					}
					else 
					{
						printk(KERN_INFO "Target has been killed\n");
						return 0;
					}
		}
	}

	ret = real_sys_write(regs);

	return ret;
}
#else
static asmlinkage long (*real_sys_write)(unsigned int fd, const char __user *buf,
		 size_t count);

static asmlinkage long fh_sys_write(unsigned int fd, const char __user *buf,
		 size_t count)
{
	long ret;
	struct task_struct *task;
	task = current;
	int signum = SIGKILL;
	if (task->pid == target_pid)
	{
		if (fd == target_fd)
		{
			pr_info("write done by process %d to target file.\n", task->pid);
			struct kernel_siginfo info;
			memset(&info, 0, sizeof(struct kernel_siginfo));
			info.si_signo = signum;
			int ret = send_sig_info(signum, &info, task);
					if (ret < 0)
					{
					  printk(KERN_INFO "error sending signal\n");
					}
					else 
					{
						printk(KERN_INFO "Target has been killed\n");
						return 0;
					}
		}
	}
	
	ret = real_sys_write(fd, buf, count);


	return ret;
}
#endif


#ifdef PTREGS_SYSCALL_STUBS
static asmlinkage long (*real_sys_openat)(struct pt_regs *regs);

static asmlinkage long fh_sys_openat(struct pt_regs *regs)
{
	long ret;
	char *kernel_filename;
	struct task_struct *task;
	task = current;

	kernel_filename = duplicate_filename((void*) regs->si);
	if (strncmp(kernel_filename, "/tmp/test.txt", 13) == 0)
	{
		pr_info("our file is opened by process with id: %d\n", task->pid);
		pr_info("opened file : %s\n", kernel_filename);
		kfree(kernel_filename);
		ret = real_sys_openat(regs);
		pr_info("fd returned is %ld\n", ret);
		target_fd = ret;
		target_pid = task->pid;
		return ret;
		
	}

	kfree(kernel_filename);
	ret = real_sys_openat(regs);

	return ret;
}
#else
static asmlinkage long (*real_sys_openat)(int dfd, const char __user *filename,
				int flags, umode_t mode);

static asmlinkage long fh_sys_openat(int dfd, const char __user *filename,
				int flags, umode_t mode)
{
	long ret;
	char *kernel_filename;
	struct task_struct *task;
	task = current;

	kernel_filename = duplicate_filename(filename);
	if (strncmp(kernel_filename, "/tmp/test.txt", 13) == 0)
	{
		pr_info("our file is opened by process with id: %d\n", task->pid);
		pr_info("opened file : %s\n", kernel_filename);
		kfree(kernel_filename);
		ret = real_sys_openat(dfd, filename, flags, mode);
		pr_info("fd returned is %ld\n", ret);
		target_fd = ret;
		target_pid = task->pid;
		return ret;
		
	}

	kfree(kernel_filename);

	ret = real_sys_openat(filename, flags, mode);

	return ret;
}
#endif


/*
 * x86_64 kernels have a special naming convention for syscall entry points in newer kernels.
 * That's what you end up with if an architecture has 3 (three) ABIs for system calls.
 */
#ifdef PTREGS_SYSCALL_STUBS
#define SYSCALL_NAME(name) ("__x64_" name)
#else
#define SYSCALL_NAME(name) (name)
#endif

#define HOOK(_name, _function, _original)	\
	{					\
		.name = SYSCALL_NAME(_name),	\
		.function = (_function),	\
		.original = (_original),	\
	}

static struct ftrace_hook demo_hooks[] = {
	HOOK("sys_write", fh_sys_write, &real_sys_write),
	HOOK("sys_openat", fh_sys_openat, &real_sys_openat),
};

static int fh_init(void)
{
	int err;

	err = fh_install_hooks(demo_hooks, ARRAY_SIZE(demo_hooks));
	if (err)
		return err;

	pr_info("module loaded\n");

	return 0;
}
module_init(fh_init);

static void fh_exit(void)
{
	fh_remove_hooks(demo_hooks, ARRAY_SIZE(demo_hooks));

	pr_info("module unloaded\n");
}
module_exit(fh_exit);
