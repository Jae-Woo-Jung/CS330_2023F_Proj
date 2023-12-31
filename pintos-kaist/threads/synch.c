/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "list.h"
#include "threads/interrupt.h"
#include "threads/thread.h"

static bool more_cond_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

static bool
more_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return get_thread_priority(list_entry (a, struct thread, elem)) >
	get_thread_priority(list_entry (b, struct thread, elem));
}

static bool
more_lock_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return list_entry (a, struct lock, elem)->priority >
	list_entry (b, struct lock, elem)->priority;
}

static void
add_to_waiting_list (struct semaphore *sema, struct thread *t) {
	list_insert_ordered (&sema->waiters, &t->elem, more_priority, NULL);
}

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	thread_current()->waiting_sema = sema;
	while (sema->value == 0) {
		add_to_waiting_list (sema, thread_current());
		thread_block ();
	}
	thread_current()->waiting_sema = NULL;
	sema->value--;
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)) {
		if (!thread_mlfqs) {
			thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
		} else {
			struct list_elem *elem =
			list_max (&sema->waiters, cmp_thread_priority_mlfqs, NULL);
			// cmp_priority_mlfqs same as more_priority
			struct thread *th = list_entry(elem, struct thread, elem);
			list_remove (elem);
			thread_unblock (th);
		}
	}

	sema->value++;
	thread_yield_with_priority();
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	if (!thread_mlfqs) 
		lock->priority = PRI_MIN;
	sema_init (&lock->semaphore, 1);
}

static void update_holder_priority(struct thread *holder, struct lock *);
static void lock_donate_priority(struct lock *lock, struct thread *t);

static void update_holder_priority(struct thread *holder, struct lock *lock) {
	ASSERT(!thread_mlfqs);
	list_remove(&lock->elem);
	list_insert_ordered(&holder->holding_locks, &lock->elem, more_lock_priority, NULL);
	if (lock->priority > get_thread_priority(holder)) {
		holder->donated_priority = lock->priority;

		if (holder->status == THREAD_READY) {
			list_remove(&holder->elem);
			add_to_ready_list(holder);
		} else if (holder->status == THREAD_BLOCKED) {
			if (holder->waiting_cond) {
				list_remove(holder->cond_elem);
				list_insert_ordered (&holder->waiting_cond->waiters, holder->cond_elem, more_cond_priority, NULL);
			} else {
				// semaphore
				ASSERT(holder->waiting_sema != NULL);
				list_remove(&holder->elem);
				add_to_waiting_list(holder->waiting_sema, holder);
			}
		}
		struct lock *waiting_lock = holder->waiting_lock;
		if (waiting_lock) {
			lock_donate_priority(waiting_lock, holder);
		}
	}
}

static void lock_donate_priority(struct lock *lock, struct thread *t) {
	ASSERT(!thread_mlfqs);
	if (get_thread_priority(t) > lock->priority) {
		lock->priority = get_thread_priority(t);

		struct thread *holder = lock->holder;
		ASSERT(holder != NULL);
		update_holder_priority(holder, lock);
		
	}
}
static void lock_retrieve_priority(struct lock *lock, struct thread *t) {
	ASSERT(!thread_mlfqs);
	struct list_elem *elem = list_front(&t->holding_locks);
	list_remove(&lock->elem); // remove from thread->holding_locks
	if (elem == &lock->elem) {
		if (list_empty(&t->holding_locks)) {
			t->donated_priority = PRI_MIN;
		} else {
			elem = list_front(&t->holding_locks);
			t->donated_priority = list_entry(elem, struct lock, elem)->priority;
		}
	}
}

static void update_lock_priority(struct lock *lock) {
	if (list_empty(&lock->semaphore.waiters))
		lock->priority = PRI_MIN;
	else
		lock->priority = get_thread_priority(list_entry(list_front(&lock->semaphore.waiters), struct thread, elem));
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	enum intr_level old_level;
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));
	struct thread *t = thread_current();

	old_level = intr_disable();
	if (thread_mlfqs){
		sema_down(&lock->semaphore);
		lock->holder = t;
	} else {
		if (lock->holder != NULL) {
			lock_donate_priority(lock, t);
			t->waiting_lock = lock;
		} 
		sema_down (&lock->semaphore);
		lock->holder = t;
		t->waiting_lock = NULL;
		update_lock_priority(lock);
		list_insert_ordered(&t->holding_locks, &lock->elem, more_lock_priority, NULL);
		if (lock->priority > t->donated_priority)
			t->donated_priority = lock->priority;
	}
	intr_set_level (old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;
	enum intr_level old_level;
	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));
	struct thread *t = thread_current();

	old_level = intr_disable();

	success = sema_try_down (&lock->semaphore);
	if (success) {
		if (!thread_mlfqs) {
			update_lock_priority(lock);
			list_insert_ordered(&t->holding_locks, &lock->elem, more_lock_priority, NULL);
			if (lock->priority > t->donated_priority)
				t->donated_priority = lock->priority;
		}
		lock->holder = thread_current ();
	}
	intr_set_level (old_level);
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	enum intr_level old_level;
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	old_level = intr_disable();
	if (thread_mlfqs){
		lock->holder = NULL;
		sema_up (&lock->semaphore);
	} else {
		lock->holder = NULL;
		lock_retrieve_priority(lock, thread_current());
		update_lock_priority(lock);
		sema_up (&lock->semaphore);
	}
	intr_set_level (old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};


static bool
more_cond_priority (const struct list_elem *a, const struct list_elem *b, void *aux) {
	struct thread *t1, *t2;
	if (aux)
		t1 = (struct thread *) aux;
	else
		t1 = list_entry(list_begin(&list_entry (a, struct semaphore_elem, elem)->semaphore.waiters), struct thread, elem);
	t2 = list_entry(list_begin(&list_entry (b, struct semaphore_elem, elem)->semaphore.waiters), struct thread, elem);
	return get_thread_priority(t1) > get_thread_priority(t2);
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	thread_current()->waiting_cond = cond;
	thread_current()->cond_elem = &waiter.elem;
	list_insert_ordered (&cond->waiters, &waiter.elem, more_cond_priority, thread_current());
	lock_release (lock);
	sema_down (&waiter.semaphore);
	thread_current()->waiting_cond = NULL;
	thread_current()->cond_elem = NULL;
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));
	struct list_elem *elem;
	struct semaphore *sema;

	if (!list_empty (&cond->waiters)) {
		if (!thread_mlfqs) {
			sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
		} else {
			elem = list_max (&cond->waiters, more_cond_priority, NULL);
			sema = &list_entry (elem, struct semaphore_elem, elem)->semaphore;

			list_remove (elem);
			sema_up (sema);
		}
	}

}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
