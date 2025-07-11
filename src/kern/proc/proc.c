/*
 * Copyright (c) 2013
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
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>

#if OPT_WAITPID
#include <synch.h>

// ---------------------------------------------------------------------------------------------------------

#define MAX_PROC 100
static struct _processTable {								// PROCESS TABLE
  int active;           /* initial value 0 */
  struct proc *proc[MAX_PROC+1]; /* [0] not used. pids are >= 1 */
  int last_i;           /* index of last allocated pid */
  struct spinlock lk;	/* Lock for this table */
} processTable;

#endif
/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

// ---------------------------------------------------------------------------------------------------------

/*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
struct proc *
proc_search_pid(pid_t pid) {								// (X.) Trova processo nella tabella tramite PID -> restituisce puntatore
#if OPT_WAITPID
  struct proc *p;
  KASSERT(pid>=0&&pid<MAX_PROC);								// verifica che il pid di input sia nell'intervallo di validità
  p = processTable.proc[pid];									// ottiene il processo corrispondente al pid di input dalla tabella
  KASSERT(p->p_pid==pid);
  return p;														// restituisce il puntatore al processo trovato
#else
  (void)pid;
  return NULL;
#endif
}

// ---------------------------------------------------------------------------------------------------------

static void
proc_init_waitpid(struct proc *proc, const char *name) {	// 3. Assegna PID in tabella e crea strumenti di sincronizzazione
#if OPT_WAITPID
  /* search a free index in table using a circular strategy */
  int i;
  spinlock_acquire(&processTable.lk);							// acquisisce lock sulla tabella dei processi
  i = processTable.last_i+1;									// parte da posizione successiva a ultimo PID
  proc->p_pid = 0;												// inizializza il pid a 0
  if (i>MAX_PROC) i=1;											// strategia circolare: se arriva alla fine, ricomincia
  while (i!=processTable.last_i) {
    if (processTable.proc[i] == NULL) {							// non appena trova un buco
      processTable.proc[i] = proc;									// inserisce il processo nello slot
      processTable.last_i = i;										// setta che l'ultimo pid inserito è i
      proc->p_pid = i;												// ed assegna questo pid al processo
      break;
    }
    i++;
    if (i>MAX_PROC) i=1;
  }
  spinlock_release(&processTable.lk);							// rilascia il lock
  if (proc->p_pid==0) {
    panic("too many processes. proc table is full\n");			// se pid è rimasto 0 => panic
  }
  proc->p_status = 0;											// se tutto ok (quindi non è stato lanciato panic) assegna status = 0 (ok)
#if USE_SEMAPHORE_FOR_WAITPID
  proc->p_sem = sem_create(name, 0);							// se uso semaforo, lo creo
#else
  proc->p_cv = cv_create(name);									// se uso condvar, la creo + creo lock da utilizzare
  proc->p_lock = lock_create(name);
#endif
#else
  (void)proc;
  (void)name;
#endif
}

// ---------------------------------------------------------------------------------------------------------

static void
proc_end_waitpid(struct proc *proc) {						// 6. Rimuove da tabella e distrugge sincronizzazione
#if OPT_WAITPID
  /* remove the process from the table */
  int i;
  spinlock_acquire(&processTable.lk);								// acquisisce lock sulla tabella dei processi
  i = proc->p_pid;												// ottiene il PID del processo da rimuovere
  KASSERT(i>0 && i<=MAX_PROC);									// verifica che il PID sia nell'intervallo di validità
  processTable.proc[i] = NULL;									// rimuove il processo dalla tabella
  spinlock_release(&processTable.lk);								// rilascia il lock sulla tabella dei processi

#if USE_SEMAPHORE_FOR_WAITPID
  sem_destroy(proc->p_sem);										// distrugge il semaforo del processo
#else
  cv_destroy(proc->p_cv);										// distrugge il condvar del processo + distrugge il lock usato col condvar
  lock_destroy(proc->p_lock);	
#endif
#else
  (void)proc;
#endif
}

// ---------------------------------------------------------------------------------------------------------

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)								// 2. Alloca memoria e inizializza struttura base -> restituisce puntatore a nuovo processo
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));								// alloca memoria per la struct processo
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);								// duplica la stringa per il debug
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}
																// inizializza tutto il necessario:
	proc->p_numthreads = 0;											// num threads
	spinlock_init(&proc->p_lock);									// spinlock

	/* VM fields */
	proc->p_addrspace = NULL;										// address space

	/* VFS fields */
	proc->p_cwd = NULL;												// directory corrente

	proc_init_waitpid(proc,name);									// (3) avvia supporto per waitpid (assegna PID e crea strumenti per sincronizzazione)

	return proc;												// restituisce il puntatore al nuovo processo
}

// ---------------------------------------------------------------------------------------------------------

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)								// 5. Cleanup completo del processo
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {											// verifica se il processo ha una directory corrente
		VOP_DECREF(proc->p_cwd);									// usa il puntatore per arrivare alla struttura e decrementare il riferimento alla directory
		proc->p_cwd = NULL;											// imposta la directory corrente a NULL, quindi il puntatore non punta più a niente
	}

	/* VM fields */
	if (proc->p_addrspace) {									// verifica se il processo ha un address space
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {									// se è il processo corrente
			as = proc_setas(NULL);									// rimuove l'address space dal processo corrente e lo salva in as
			as_deactivate();										// disattiva l'address space dalla MMU
		}
		else {													// altrimenti, se non è il processo corrente
			as = proc->p_addrspace;									// ottiene l'address space e lo salva in as
			proc->p_addrspace = NULL;								// rimuove l'address space dal processo
		}
		as_destroy(as);											// distrugge l'address space salvato in as
	}

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);							// pulisce spinlock del processo

	proc_end_waitpid(proc);										// (7) rimuove processo dalla tabella e distrugge strutture per sincronizzazione

	kfree(proc->p_name);										// libera memoria prima dal p_name e poi dall'intero proc
	kfree(proc);					
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)										// Crea la struttura proc del kernel
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
#if OPT_WAITPID
	spinlock_init(&processTable.lk);
	/* kernel process is not registered in the table */
	processTable.active = 1;
#endif
}

// ---------------------------------------------------------------------------------------------------------

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)					// 1. Creazione del processo e eredita directory dal padre -> restituisce il puntatore al nuovo processo
{
	struct proc *newproc;

	newproc = proc_create(name);								// (2) crea la struttura base del processo
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;								// inizializza lo spazio di indirizzamento a NULL

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);							// lock su processo corrente (padre)
	if (curproc->p_cwd != NULL) {								
		VOP_INCREF(curproc->p_cwd);								// incrementa riferimento alla directory padre
		newproc->p_cwd = curproc->p_cwd;						// copia la directory corrente nel nuovo processo
	}
	spinlock_release(&curproc->p_lock);

	return newproc;												// restituisce il puntatore al nuovo processo
}

// ---------------------------------------------------------------------------------------------------------

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)							// Rimuove un thread dal processo corrente
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;										// decrementa il numero di thread 
	spinlock_release(&proc->p_lock);

	spl = splhigh();											// setta livello interrupt al massimo per non averne e restituisce old
	t->t_proc = NULL;											// il thread non appartiene più a nessun processo: rimuove il collegamento tra il thread e il processo a cui apparteneva
	splx(spl);													// ripristina il vecchio livello di interrupt
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}


// ---------------------------------------------------------------------------------------------------------

int 
proc_wait(struct proc *proc)									// 4. Attende terminazione -> restituisce status
{
#if OPT_WAITPID
        int return_status;
        /* NULL and kernel proc forbidden */
	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

        /* wait on semaphore or condition variable */ 
#if USE_SEMAPHORE_FOR_WAITPID
        P(proc->p_sem);												// attesa sul semaforo che processo figlio termini, segnalato con sys__exit
#else
        lock_acquire(proc->p_lock);
        cv_wait(proc->p_cv);										// attesa sul condvar che processo figlio termini, segnalato con sys__exit
        lock_release(proc->p_lock);
#endif
        return_status = proc->p_status;								// copia lo status di uscita dal processo terminato
        proc_destroy(proc);											// distrugge la struttura del processo (cleanup)
        return return_status;										// restituisce lo status di uscita
#else
        /* this doesn't synchronize */ 
        (void)proc;
        return 0;
#endif
}

