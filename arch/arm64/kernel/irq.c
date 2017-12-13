/*
 * Based on arch/arm/kernel/irq.c
 *
 * Copyright (C) 1992 Linus Torvalds
 * Modifications for ARM processor Copyright (C) 1995-2000 Russell King.
 * Support for Dynamic Tick Timer Copyright (C) 2004-2005 Nokia Corporation.
 * Dynamic Tick Timer written by Tony Lindgren <tony@atomide.com> and
 * Tuukka Tikkanen <tuukka.tikkanen@elektrobit.com>.
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel_stat.h>
#include <linux/irq.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#ifdef CONFIG_HTC_POWER_DEBUG
#include <linux/slab.h>
#include <soc/qcom/htc_util.h>
#endif

unsigned long irq_err_count;

/* irq stack only needs to be 16 byte aligned - not IRQ_STACK_SIZE aligned. */
DEFINE_PER_CPU(unsigned long [IRQ_STACK_SIZE/sizeof(long)], irq_stack) __aligned(16);

int arch_show_interrupts(struct seq_file *p, int prec)
{
	show_ipi_list(p, prec);
	seq_printf(p, "%*s: %10lu\n", prec, "Err", irq_err_count);
	return 0;
}

#ifdef CONFIG_HTC_POWER_DEBUG
#define IRQ_OUTPUT_BUFFER 1024
#define IRQ_PIECE_BUFFER 64
static unsigned int *previous_irqs = NULL;
static int pre_nr_irqs = 0;
static char irq_output[IRQ_OUTPUT_BUFFER];
static int debug = 0;
static void htc_show_interrupt(int i)
{
        struct irqaction *action;
        unsigned long flags;
        struct irq_desc *desc;
        char irq_piece[IRQ_PIECE_BUFFER];
        char name_resize[9];
        bool err = false;

        if (i < nr_irqs) {
                desc = irq_to_desc(i);
                if (!desc)
                        return;
                raw_spin_lock_irqsave(&desc->lock, flags);
                action = desc->action;
                if (!action)
                        goto unlock;
                if (!(kstat_irqs_cpu(i, 0)) || previous_irqs[i] == (kstat_irqs_cpu(i, 0)))
                        goto unlock;
                memset(irq_piece, 0, sizeof(irq_piece));
                memset(name_resize, 0, sizeof(name_resize));
				if(action->name) {
					strncat(name_resize, action->name, 8);
				} else {
					printk("[K] irq: [debug] irqaction name is null\n");
					strncat(name_resize, "(null)", 8);
				}
				err = kstat_irqs_cpu(i, 0) < previous_irqs[i];
                snprintf(irq_piece, sizeof(irq_piece), "%s(%d:%s,%u%s)",
						 strlen(irq_output) > 0 ? "," : "",
						 i,
						 name_resize,
						 err ? 0 : kstat_irqs_cpu(i, 0) - previous_irqs[i],
						 err ? "(prev > cur)" : "");
				if(strlen(irq_output) + strlen(irq_piece) <= IRQ_OUTPUT_BUFFER) {
					safe_strcat(irq_output, irq_piece);
				} else {
					printk("[K] irq: [debug] IRQ_OUTPUT_BUFFER isn't enough\n");
				}
                if (debug) {
                    k_pr_embedded("[K] irq: [debug] i=%d kstat_irqs_cpu=%u previous_irqs=%u\n",
                        i, kstat_irqs_cpu(i, 0), previous_irqs[i]);
                }
                previous_irqs[i] = kstat_irqs_cpu(i, 0);
unlock:
                raw_spin_unlock_irqrestore(&desc->lock, flags);
        } else if (i == nr_irqs) {
                if (previous_irqs[nr_irqs] == irq_err_count)
                        return;
                printk("[K] irq: [Err] irq_err_count %lu\n", irq_err_count - previous_irqs[nr_irqs]);
                previous_irqs[nr_irqs] = irq_err_count;
        }
}

void htc_show_interrupts(void)
{
        int i = 0;
               if(pre_nr_irqs != nr_irqs) {
                       if (pre_nr_irqs != 0) {
                           if (debug) k_pr_embedded("[K] irq: [debug] alloc again\n");
                       }
                       pre_nr_irqs = nr_irqs;
					   if(previous_irqs)
						   kfree(previous_irqs);
                       previous_irqs = (unsigned int *)kcalloc(nr_irqs + 1, sizeof(int),GFP_KERNEL);

					   if(!previous_irqs)
						   goto malloc_failed;
               }
               memset(irq_output, 0, sizeof(irq_output));
               if (debug) k_pr_embedded("[K] irq: [debug] nr_irqs=%d pre_nr_irqs=%d\n", nr_irqs, pre_nr_irqs);
               for (i = 0; i <= nr_irqs; i++) {
                       htc_show_interrupt(i);
               }
               k_pr_embedded("[K] irq: %s\n", irq_output);
			   return;

 malloc_failed:
			   pre_nr_irqs = -1;
			   pr_err("[K] irq: kcalloc failed\n");

}
#endif

void (*handle_arch_irq)(struct pt_regs *) = NULL;

void __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	if (handle_arch_irq)
		return;

	handle_arch_irq = handle_irq;
}

void __init init_IRQ(void)
{
	irqchip_init();
	if (!handle_arch_irq)
		panic("No interrupt controller found.");
}
