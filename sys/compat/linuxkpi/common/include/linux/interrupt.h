/*-
 * Copyright (c) 2010 Isilon Systems, Inc.
 * Copyright (c) 2010 iX Systems, Inc.
 * Copyright (c) 2010 Panasas, Inc.
 * Copyright (c) 2013-2015 Mellanox Technologies, Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */
#ifndef	_LINUX_INTERRUPT_H_
#define	_LINUX_INTERRUPT_H_

#include <linux/device.h>
#include <linux/pci.h>

#include <linux/hardirq.h>

#undef resource
typedef	irqreturn_t	(*irq_handler_t)(int, void *);

#define	IRQ_RETVAL(x)	((x) != IRQ_NONE)

#define	IRQF_SHARED	RF_SHAREABLE

struct irq_ent {
	struct list_head	links;
	struct device	*dev;
	struct resource	*res;
	void		*arg;
	irqreturn_t	(*handler)(int, void *);
	void		*tag;
	unsigned int	irq;
};

struct irqaction {
	irq_handler_t		handler;
	void			*dev_id;
	struct irqaction	*next;
	irq_handler_t		thread_fn;
	struct task_struct	*thread;
	struct irqaction	*secondary;
	unsigned int		irq;
	unsigned int		flags;
	unsigned long		thread_flags;
	unsigned long		thread_mask;
	const char		*name;
};


static inline int
linux_irq_rid(struct device *dev, unsigned int irq)
{
	if (irq == dev->irq)
		return (0);
	return irq - dev->msix + 1;
}

extern int linux_irq_handler(void *);

static inline struct irq_ent *
linux_irq_ent(struct device *dev, unsigned int irq)
{
	struct irq_ent *irqe;

	list_for_each_entry(irqe, &dev->irqents, links)
		if (irqe->irq == irq)
			return (irqe);

	return (NULL);
}

static inline int
request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
    const char *name, void *arg)
{
	struct resource *res;
	struct irq_ent *irqe;
	struct device *dev;
	int error;
	int rid;

	dev = linux_pci_find_irq_dev(irq);
	if (dev == NULL)
		return -ENXIO;
	rid = linux_irq_rid(dev, irq);
	res = bus_alloc_resource_any(dev->bsddev, SYS_RES_IRQ, &rid,
	    flags | RF_ACTIVE);
	if (res == NULL)
		return (-ENXIO);
	irqe = kmalloc(sizeof(*irqe), GFP_KERNEL);
	irqe->dev = dev;
	irqe->res = res;
	irqe->arg = arg;
	irqe->handler = handler;
	irqe->irq = irq;
	error = bus_setup_intr(dev->bsddev, res, INTR_TYPE_NET | INTR_MPSAFE,
	    linux_irq_handler, NULL, irqe, &irqe->tag);
	if (error) {
		bus_release_resource(dev->bsddev, SYS_RES_IRQ, rid, irqe->res);
		kfree(irqe);
		return (-error);
	}
	list_add(&irqe->links, &dev->irqents);

	return 0;
}

static inline int
bind_irq_to_cpu(unsigned int irq, int cpu_id)
{
	struct irq_ent *irqe;
	struct device *dev;

	dev = linux_pci_find_irq_dev(irq);
	if (dev == NULL)
		return (-ENOENT);

	irqe = linux_irq_ent(dev, irq);
	if (irqe == NULL)
		return (-ENOENT);

	return (-bus_bind_intr(dev->bsddev, irqe->res, cpu_id));
}

static inline void
free_irq(unsigned int irq, void *device)
{
	struct irq_ent *irqe;
	struct device *dev;
	int rid;

	dev = linux_pci_find_irq_dev(irq);
	if (dev == NULL)
		return;
	rid = linux_irq_rid(dev, irq);
	irqe = linux_irq_ent(dev, irq);
	if (irqe == NULL)
		return;
	bus_teardown_intr(dev->bsddev, irqe->res, irqe->tag);
	bus_release_resource(dev->bsddev, SYS_RES_IRQ, rid, irqe->res);
	list_del(&irqe->links);
	kfree(irqe);
}

#define resource linux_resource

/* Tasklets --- multithreaded analogue of BHs.

   Main feature differing them of generic softirqs: tasklet
   is running only on one CPU simultaneously.

   Main feature differing them of BHs: different tasklets
   may be run simultaneously on different CPUs.

   Properties:
   * If tasklet_schedule() is called, then tasklet is guaranteed
     to be executed on some cpu at least once after this.
   * If the tasklet is already scheduled, but its execution is still not
     started, it will be executed only once.
   * If this tasklet is already running on another CPU (or schedule is called
     from tasklet itself), it is rescheduled for later.
   * Tasklet is strictly serialized wrt itself, but not
     wrt another tasklets. If client needs some intertask synchronization,
     he makes it with spinlocks.
 */

struct tasklet_struct
{
	struct tasklet_struct *next;
	unsigned long state;
	atomic_t count;
	void (*func)(unsigned long);
	unsigned long data;
};

#define DECLARE_TASKLET(name, func, data) \
struct tasklet_struct name = { NULL, 0, ATOMIC_INIT(0), func, data }

#define DECLARE_TASKLET_DISABLED(name, func, data) \
struct tasklet_struct name = { NULL, 0, ATOMIC_INIT(1), func, data }


enum
{
	TASKLET_STATE_SCHED,	/* Tasklet is scheduled for execution */
	TASKLET_STATE_RUN	/* Tasklet is running (SMP only) */
};

#ifdef CONFIG_SMP
static inline int tasklet_trylock(struct tasklet_struct *t)
{
	return !test_and_set_bit(TASKLET_STATE_RUN, &(t)->state);
}

static inline void tasklet_unlock(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	clear_bit(TASKLET_STATE_RUN, &(t)->state);
}

static inline void tasklet_unlock_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &(t)->state)) { barrier(); }
}
#else
#define tasklet_trylock(t) 1
#define tasklet_unlock_wait(t) do { } while (0)
#define tasklet_unlock(t) do { } while (0)
#endif

extern void __tasklet_schedule(struct tasklet_struct *t);

static inline void tasklet_schedule(struct tasklet_struct *t)
{
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		__tasklet_schedule(t);
}

extern void tasklet_kill(struct tasklet_struct *t);
extern void tasklet_init(struct tasklet_struct *t,
			 void (*func)(unsigned long), unsigned long data);

#endif	/* _LINUX_INTERRUPT_H_ */
