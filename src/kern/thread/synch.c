/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
        struct semaphore *sem;

        sem = kmalloc(sizeof(*sem));
        if (sem == NULL) {
                return NULL;
        }

        sem->sem_name = kstrdup(name);
        if (sem->sem_name == NULL) {
                kfree(sem);
                return NULL;
        }

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
        sem->sem_count = initial_count;

        return sem;
}

void
sem_destroy(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
        kfree(sem->sem_name);
        kfree(sem);
}

void
P(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
        KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
        while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
        }
        KASSERT(sem->sem_count > 0);
        sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

        sem->sem_count++;
        KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
        struct lock *lock;

        lock = kmalloc(sizeof(*lock));
        if (lock == NULL) {
                return NULL;
        }

        lock->lk_name = kstrdup(name);                  // salva il nome per il debug
        if (lock->lk_name == NULL) {
                kfree(lock);
                return NULL;
        }

	HANGMAN_LOCKABLEINIT(&lock->lk_hangman, lock->lk_name);

        // add stuff here as needed

#if USE_SEMAPHORE_FOR_LOCK
        lock->lk_sem = sem_create(lock->lk_name,1);     // crea semaforo con nome e valore a 1
        if (lock->lk_sem == NULL) {
                kfree(lock->lk_name);
                kfree(lock);
                return NULL;
        }
#else   
        lock->lk_wchan = wchan_create(lock->lk_name);   // crea wchan con nome
        if (lock->lk_wchan == NULL){
                kfree(lock->lk_name);
                kfree(lock);
                return NULL;
        }

#endif
        lock->lk_owner = NULL;                          // setta ownership a zero, perchè nessuno l'ha ancora acquisito
        spinlock_init(&lock->lk_lock);
        return lock;
        
}

// --------------------------------------------

void
lock_destroy(struct lock *lock)
{
        KASSERT(lock != NULL);

        // add stuff here as needed

        spinlock_cleanup(&lock->lk_lock);                       // pulisce lo spinlock interno
#if USE_SEMAPHORE_FOR_LOCK
        sem_destroy(lock->lk_sem);                      // distrugge il semaforo
#else   
        wchan_destroy(lock->lk_wchan);                  // distrugge il wchan
#endif
        kfree(lock->lk_name);                           // dealloca la memoria
        kfree(lock);                                    
}

// --------------------------------------------

void
lock_acquire(struct lock *lock)
{

        // Write this

        KASSERT(lock != NULL);
        if (lock_do_i_hold(lock)){                      // verifica che questo thread non abbia già acquisito questo lock
                kprintf("AAACKK!\n");
        }
        KASSERT(!(lock_do_i_hold(lock)));

        KASSERT(curthread->t_in_interrupt == false);    // verifica che non ci siano interrupt, perché non si può acquisire un lock durante un interrupt

#if USE_SEMAPHORE_FOR_LOCK
        P(lock->lk_sem);                                // probe su lock. Se occupato, va in sleep (wchan_sleep) finchè non si libera. SEMAFORO BINARIO GARANTISCE L'OWNERSHIP UNICA!
        spinlock_acquire(&lock->lk_lock);
#else   
        spinlock_acquire(&lock->lk_lock);                       // acquire su spinlock interno
        while (lock->lk_owner != NULL){                 // aspetto finchè esiste già un Owner. Stiamo quindi verificando attivamente noi l'Ownership!
                wchan_sleep(lock->lk_wchan, &lock->lk_lock);
        }
#endif
        KASSERT(lock->lk_owner == NULL);
        lock->lk_owner=curthread;                       // ora settiamo che siamo noi il thread che ha (l'unica) ownership!
        spinlock_release(&lock->lk_lock);                       // release su spinlock interno

}

// --------------------------------------------

void
lock_release(struct lock *lock)
{
        // Write this

        KASSERT(lock != NULL);
        KASSERT(lock_do_i_hold(lock));                  // verifica che questo thread SIA in possesso di questo lock
        spinlock_acquire(&lock->lk_lock);                       // acquire su spinlock interno
        lock->lk_owner = NULL;                          // resetta ownership
#if USE_SEMAPHORE_FOR_LOCK
        V(lock->lk_sem);                                // segnala liberazione di lock
#else   
        wchan_wakeone(lock->lk_wchan, &lock->lk_lock);  // notify_one del wchan
#endif
        spinlock_release(&lock->lk_lock);                       // release su spinlock interno

}

// --------------------------------------------

bool
lock_do_i_hold(struct lock *lock)                       // serve a verificare se il thread corrente possiede o meno il lock
{
        // Write this
#if OPT_SYNCH
        bool res;
	spinlock_acquire(&lock->lk_lock);
	res = lock->lk_owner == curthread;              // se siamo i possessori di questo thread ritorna true, altrimenti false 
	spinlock_release(&lock->lk_lock);
	return res;
#endif

        (void)lock;  // suppress warning until code gets written

        return true; // dummy until code gets written
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)                             // identico a lock_create (no semaforo ovviamente)
{
        struct cv *cv;

        cv = kmalloc(sizeof(*cv));
        if (cv == NULL) {
                return NULL;
        }

        cv->cv_name = kstrdup(name);
        if (cv->cv_name==NULL) {
                kfree(cv);
                return NULL;
        }

        // add stuff here as needed
#if OPT_SYNCH
	cv->cv_wchan = wchan_create(cv->cv_name);
	if (cv->cv_wchan == NULL) {
	        kfree(cv->cv_name);
		kfree(cv);
		return NULL;
	}
        spinlock_init(&cv->cv_lock);
#endif
        return cv;
}

void
cv_destroy(struct cv *cv)                               // identico a lock_destroy (no semaforo ovviamente)
{
        KASSERT(cv != NULL);

        // add stuff here as needed
#if OPT_SYNCH
	spinlock_cleanup(&cv->cv_lock);
	wchan_destroy(cv->cv_wchan);
#endif
        kfree(cv->cv_name);
        kfree(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)               // assumi che il lock sia già stato preso!
{
        // Write this
#if OPT_SYNCH
        KASSERT(lock != NULL);
	KASSERT(cv != NULL);
	KASSERT(lock_do_i_hold(lock));                          // deve possedere il lock!

	spinlock_acquire(&cv->cv_lock);                         //prendo lock interno per poter operare su cv
	lock_release(lock);                                             //lascio il lock esterno fino alla notifica
	wchan_sleep(cv->cv_wchan,&cv->cv_lock);                 // aspetta un wakeone dal cv_signal
	spinlock_release(&cv->cv_lock);                         // non serve più questo cv, quindi rilascio il lock
	lock_acquire(lock);                                             // riprendo il lock esterno dopo segnale
#endif

        (void)cv;    // suppress warning until code gets written
        (void)lock;  // suppress warning until code gets written
}

void
cv_signal(struct cv *cv, struct lock *lock)             // assumi che il lock sia già stato preso!
{
        // Write this
#if OPT_SYNCH
        KASSERT(lock != NULL);
	KASSERT(cv != NULL);
	KASSERT(lock_do_i_hold(lock));                          // verifica che sono effettivamente il possessore del lock
	spinlock_acquire(&cv->cv_lock);
	wchan_wakeone(cv->cv_wchan,&cv->cv_lock);               // sveglia un thread dalla coda di wait
	spinlock_release(&cv->cv_lock);
#endif
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
}

void
cv_broadcast(struct cv *cv, struct lock *lock)          // identico a signal, cambia solo il wakeall
{
	// Write this
#if OPT_SYNCH
        KASSERT(lock != NULL);
	KASSERT(cv != NULL);
	KASSERT(lock_do_i_hold(lock));
	spinlock_acquire(&cv->cv_lock);
	wchan_wakeall(cv->cv_wchan,&cv->cv_lock);
	spinlock_release(&cv->cv_lock);
#endif
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
}

