#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <current.h>
#include <synch.h>

int sys_write(int fd, userptr_t buf_ptr, size_t size){
    int i;
    char *p = (char *)buf_ptr;

    if(fd!=STDOUT_FILENO && fd!=STDERR_FILENO){
        kprintf("sys_write supported only to sdtdout\n");
        return -1;
    }

    for(i=0; i<(int)size; i++){
        putch(p[i]);
    }

    return (int)size;
}

// --------------------------------------------------

int sys_read(int fd, userptr_t buf_ptr, size_t size){
    int i;
    char *p = (char *)buf_ptr;

    if(fd!=STDIN_FILENO){
        kprintf("sys_read supported only to stdin\n");
        return -1;
    }

    for(i=0; i<(int)size; i++){
        p[i] = getch();

        if(p[i] < 0){
            return i;
        }
    }

    return (int)size;
}

// --------------------------------------------------

void sys__exit(int status){

#if OPT_WAITPID

    struct proc *p = curproc;
    p->p_status = status & 0xff;                        // salva solo i primi 8 bit dello status (perchè in UNIX l'exit status deve essere di 8 bit)
    proc_remthread(curthread);                          // decrementa il numero di thread del processo corrente (numthread)

    #if USE_SEMAPHORE_FOR_WAITPID
        V(p->p_sem);                                    // risveglia eventuali processi in attesa sul semaforo
    #else
        lock_acquire(p->p_lock);
        cv_signal(p->p_cv);                             // risveglia eventuali processi in attesa sul condvar
        lock_release(p->p_lock);
    #endif


#else
    struct addrspace *as = proc_getas();                // ottiene l'address space dal processo corrente

    as_destroy(as);                                     // distrugge l'address space, liberando memoria

#endif

    thread_exit();                                      // termina il thread corrente

    panic("thread exit returned (should not happen)\n");
    (void) status;
}

// -----------------------------------------------

int
sys_waitpid(pid_t pid, userptr_t statusp, int options)
{
#if OPT_WAITPID
  struct proc *p = proc_search_pid(pid);                // (4) cerca il processo figlio con il PID specificato
  int s;
  (void)options; /* not handled */
  if (p==NULL) return -1;                               // se non trova il provesso, ritorna errore
  s = proc_wait(p);                                     // attende che il professo figlio termini e ottiene il suo exit status
  if (statusp!=NULL) 
    *(int*)statusp = s;                                 // se il puntatore statusp non è nullo, scrive lo status di uscita del figlio 
  return pid;                                           // ritorna il PID del processo atteso
#else
  (void)options; /* not handled */
  (void)pid;
  (void)statusp;
  return -1;
#endif
}