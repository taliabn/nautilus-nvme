obj-y += \
	init.o \
	fmtout.o \
	printk.o \
	doprnt.o \
	vc.o \
	queue.o \
	irqdev.o \
	gpio.o \
	gpiodev.o \
	interrupt.o \
	naut_string.o \
	crypto.o \
	spinlock.o \
	rwlock.o \
	condvar.o \
	semaphore.o \
	msg_queue.o \
	hashtable.o \
	rbtree.o \
	radix_tree.o \
	random.o \
	smp.o \
	idle.o \
	thread.o \
	task.o \
	future.o \
	waitqueue.o \
	group.o \
	timer.o \
	scheduler.o \
	group_sched.o \
	barrier.o \
	backtrace.o \
	env.o \
	setjmp.o \
	mm/ \
	dev.o \
	chardev.o \
	blkdev.o \
	netdev.o \
	gpudev.o \
	fs.o \
	shell.o \
	fprintk.o \
	dtoa.o \
	module.o \
	linker.o \
	getopt.o \
	cmdline.o \
	lru_cache.o \
	ipi.o \
	iomap.o \
	topo.o \
	argp.o \
	xcall.o \
	ringbuffer.o

obj-$(NAUT_CONFIG_ARCH_X86) += \
	mb_utils.o \
	ticketlock.o \
	acpi.o \
	numa.o \
	nemo.o \
	setjmp.o \
	prog.o \

obj-$(NAUT_CONFIG_USE_FDT) += \
	of/

obj-$(NAUT_CONFIG_PROFILE) += instrument.o
obj-$(NAUT_CONFIG_XEON_PHI) += sfi.o

obj-$(NAUT_CONFIG_PALACIOS) += vmm.o


obj-$(NAUT_CONFIG_ISOCORE) += isocore.o

obj-$(NAUT_CONFIG_ENABLE_REMOTE_DEBUGGING) += gdb-stub.o

obj-$(NAUT_CONFIG_ENABLE_MONITOR) += monitor.o

obj-$(NAUT_CONFIG_WATCHDOG) +=  watchdog.o

obj-$(NAUT_CONFIG_CACHEPART) +=	cachepart.o

obj-$(NAUT_CONFIG_FIBER_ENABLE) += fiber.o

obj-$(NAUT_CONFIG_ASPACES) +=  aspace.o 

obj-$(NAUT_CONFIG_ALLOCS) +=  alloc.o 

obj-$(NAUT_CONFIG_PARTITION_SUPPORT) += partition.o

obj-$(NAUT_CONFIG_PROVENANCE) += provenance.o

obj-$(NAUT_CONFIG_PROCESSES) += process.o \
				signal.o

obj-$(NAUT_CONFIG_LINUX_SYSCALLS) += syscalls/

obj-$(NAUT_CONFIG_USE_IST) += gdt.o

obj-$(NAUT_CONFIG_ENABLE_STACK_CHECK) += stack_prot.o

