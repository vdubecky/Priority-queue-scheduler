#include "scheduler.h"
#include "assert.h"

#ifdef CONFIG_ENABLE_DEBUG
#include <stdio.h>
#define DEBUG(STATEMENT) \
    do {                 \
        STATEMENT;       \
    } while (0)
#else
#define DEBUG(STATEMENT)
#endif /* CONFIG_ENABLE_DEBUG */

const uint16_t CPU_MASK_MAX = 65535;

/**
 * @note returns the number of processors on which the process can run.
 *
 * @param cpu mask
 */
static unsigned short cpu_count(uint16_t mask)
{
    unsigned short count = 0;
    while (mask > 0) {
        if (mask & 1) {
            count++;
        }
        mask >>= 1;
    }

    return count;
}

/**
 * @note returns push_success if the push process has a higher or equal priority than the queued process,
 * @note push_duplicate if input processes are same, push_inconsistent when processes has same callbacks
 * @note and contexts, otherwise push_error
 *
 * @param process to push and process from queue
 */
static bool process_evaluation(process_type push_process, process_type process)
{
    unsigned long push_process_priority = push_process.niceness * push_process.remaining_time;
    unsigned long process_priority = process.niceness * process.remaining_time;

    if (push_process_priority == process_priority) {
        if (cpu_count(push_process.cpu_mask) < cpu_count(process.cpu_mask)) {
            return true;
        }
        return false;
    }

    return push_process_priority < process_priority;
}

static enum push_result processes_are_valid(priority_queue *queue, process_type process)
{
    for (priority_queue_item *item = queue->top; item; item = item->next) {
        if (item->process.callback == process.callback && item->process.context == process.context) {
            if (item->process.remaining_time == process.remaining_time &&
                    item->process.niceness == process.niceness &&
                    item->process.cpu_mask == process.cpu_mask) {
                return push_duplicate;
            }

            return push_inconsistent;
        }
    }
    return push_success;
}

/**
 * @note returns pointer to new priority_queue_item or NULL if it fails
 *
 * @param process, pointer to next item, pointer to previous item
 */
static priority_queue_item *create_item(process_type process, priority_queue_item *next, priority_queue_item *prev)
{
    priority_queue_item *item = malloc(sizeof(struct priority_queue_item));
    if (!item) {
        return NULL;
    }

    item->process = process;
    item->next = next;
    item->prev = prev;

    return item;
}

static void pop_item(priority_queue *queue, priority_queue_item *item)
{
    assert(queue);
    assert(item);

    queue->size--;

    if (item == queue->top) {
        if (!queue->top->next) {
            clear_queue(queue);
            return;
        }
        queue->top = queue->top->next;
        queue->top->prev = NULL;
    } else if (item == queue->bottom) {
        queue->bottom = queue->bottom->prev;
        queue->bottom->next = NULL;
    } else {
        item->next->prev = item->prev;
        item->prev->next = item->next;
    }

    free(item);
}

priority_queue create_queue(void)
{
    priority_queue p = {
        .bottom = NULL,
        .top = NULL,
        .size = 0
    };

    return p;
}

enum push_result push_to_queue(priority_queue *queue, process_type process)
{
    assert(queue);
    assert(process.niceness >= 10 && process.niceness <= 49);

    priority_queue_item *new_item;
    if (!queue->top) {
        new_item = create_item(process, NULL, NULL);
        if (!new_item) {
            return push_error;
        }

        queue->top = new_item;
        queue->bottom = new_item;
        queue->size++;
        return push_success;
    }

    enum push_result valid_state = processes_are_valid(queue, process);
    if (valid_state != push_success) {
        return valid_state;
    }

    for (priority_queue_item *item = queue->top; item; item = item->next) {
        if (process_evaluation(process, item->process)) {
            new_item = create_item(process, item, item->prev);
            if (!new_item) {
                return push_error;
            }
            if (item == queue->top) {
                queue->top->prev = new_item;
                queue->top = new_item;
            } else {
                item->prev->next = new_item;
                item->prev = new_item;
            }

            queue->size++;
            return push_success;
        }
    }

    new_item = create_item(process, NULL, queue->bottom);

    if (!new_item) {
        return push_error;
    }

    queue->bottom->next = new_item;
    queue->bottom = new_item;
    queue->size++;

    return push_success;
}

bool renice(priority_queue *queue, cb_type callback, void *context, unsigned int niceness)
{
    assert(queue);
    assert(niceness >= 10 && niceness <= 49);

    for (priority_queue_item *item = queue->top; item; item = item->next) {
        if (item->process.callback == callback && item->process.context == context) {
            item->process.niceness = niceness;
            process_type p = item->process;
            pop_item(queue, item);
            push_to_queue(queue, p);
            return true;
        }
    }

    return false;
}

process_type *get_top(const priority_queue *queue, uint16_t cpu_mask)
{
    assert(queue);

    for (priority_queue_item *item = queue->top; item; item = item->next) {
        if (item->process.cpu_mask & cpu_mask) {
            return &item->process;
        }
    }

    return NULL;
}

bool pop_top(priority_queue *queue, uint16_t cpu_mask, process_type *out)
{
    assert(queue);
    for (priority_queue_item *item = queue->top; item; item = item->next) {
        if (item->process.cpu_mask & cpu_mask) {
            if (out) {
                *out = item->process;
            }
            pop_item(queue, item);
            return true;
        }
    }

    return false;
}

bool copy_queue(priority_queue *dest, const priority_queue *source)
{
    assert(dest);
    assert(source);

    priority_queue new_queue = create_queue();

    for (priority_queue_item *item = source->top; item; item = item->next) {
        priority_queue_item *new_item = create_item(item->process, NULL, NULL);
        if (!new_item) {
            clear_queue(&new_queue);
            return false;
        }
        if (!new_queue.top) {
            new_queue.top = new_item;
        }
        if (new_queue.bottom) {
            new_item->prev = new_queue.bottom;
            new_queue.bottom->next = new_item;
        }
        new_queue.bottom = new_item;
        new_queue.bottom->next = NULL;
        new_queue.size++;
    }

    dest->top = new_queue.top;
    dest->bottom = new_queue.bottom;
    dest->size = new_queue.size;
    return true;
}

void clear_queue(priority_queue *queue)
{
    assert(queue);
    priority_queue_item *item = queue->top;
    while (item) {
        priority_queue_item *item_to_del = item;
        item = item->next;
        free(item_to_del);
    }

    queue->bottom = NULL;
    queue->top = NULL;
    queue->size = 0;
}

unsigned int run_top(priority_queue *queue, uint16_t cpu_mask, unsigned int run_time)
{
    assert(queue);
    if (!queue->top) {
        return 0;
    }

    process_type *proc = get_top(queue, cpu_mask);

    if (!proc) {
        return 0;
    }

    unsigned int call_res = proc->callback(run_time, proc->context);
    if (call_res == 0) {
        pop_top(queue, cpu_mask, proc);
        return 0;
    }

    if (proc->remaining_time > run_time) {
        proc->remaining_time = proc->remaining_time - run_time + call_res;
    } else {
        proc->remaining_time = call_res;
    }

    process_type p;
    pop_top(queue, cpu_mask, &p);
    push_to_queue(queue, p);
    return p.remaining_time;
}
