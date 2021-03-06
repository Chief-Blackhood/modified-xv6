#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  // For MLFQ
  for(int i=0;i<5;i++)
  {
    queues[i] = 0;
  }
  for(int i=0;i<NPROC; i++)
  {
    free_nodes[i].use = 0;
  }
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  // initialize time variables for waitx
  acquire(&tickslock);
  p->ctime = ticks;
  release(&tickslock);
  p->rtime = 0;
  p->etime = 0;
  p->iotime = 0;

  #if SCHEDULER == SCHED_PBS
  p->priority = 60;
  p->chance = 0;
  #else
  p->priority = -1;
  #endif

  // For MLFQ
  p->cur_ticks = 0;
  p->enter_time = ticks;
  #if SCHEDULER == SCHED_MLFQ
  p->queue_no = 0;
  #else
  p->queue_no = -1;
  #endif
  p->change_queue = 0;

  // For PS
  p->cur_waiting_time = 0;
  p->n_run = 0;
  for (int i = 0; i < 5; i++)
  {
    #if SCHEDULER == SCHED_MLFQ
    p->ticks[i] = 0;
    #else
    p->ticks[i] = -1;
    #endif
  }
  

  return p;
}

struct node*
palloc()
{
  for(int i=0;i<NPROC;i++)
  {
    if(free_nodes[i].use == 0)
    {
      free_nodes[i].use = 1;
      return &free_nodes[i];
    }
  }
  return 0;
}

void
pfree(struct node *p)
{
  p->use = 0;
}

void
inc_time(void)
{
  acquire(&ptable.lock);

    for (struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state == RUNNING)
        p->rtime++;
      else if(p->state == SLEEPING)
        p->iotime++;
      else if(p->state == RUNNABLE)
        p->cur_waiting_time++;

      // if(p->pid > 3 && (p->state == SLEEPING ||  p->state == RUNNABLE || p->state == RUNNING))
      // {
      //   cprintf("%d %d %d\n", p->pid, p->queue_no, ticks);
      // }
    }

  release(&ptable.lock);
}

struct node*
push(struct node* head, struct proc* data)
{
  struct node* new = (struct node*)palloc();
  new->data = data;
  new->next = 0;
  if(head == 0)
  {
    return new;
  }
  struct node *last = head;
  while(last->next != 0)
  {
    last = last->next;
  }
  last->next = new;
  return head;
}

struct node*
pop(struct node* head)
{
  if(head == 0)
    return 0;
  struct node* temp = head->next;
  pfree(head);
  return temp;
}

int
length(struct node* head)
{
  int count = 0;
  struct node* last = head;
  while(last!=0)
  {
    last=last->next;
    count++;
  }
  return count;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  // For MLFQ
  #if SCHEDULER == SCHED_MLFQ
  p->enter_time = ticks;
  queues[0] = push(queues[0], p);
  #endif

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  // For MLFQ
  #if SCHEDULER == SCHED_MLFQ
  np->enter_time = ticks;
  queues[0] = push(queues[0], np);
  #endif

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // setting etime to current time as now the process is completed and is waiting to be reaped
  curproc->etime = ticks;

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int
waitx(int *wtime, int *rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        *rtime = p->rtime;
        *wtime = p->etime - p->rtime - p->iotime - p->ctime;
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int
my_ps()
{
  struct proc* p;
  cprintf("PID\tPriority\tState\t\tr_time\tw_time\tn_run\tcur_q\tq0\tq1\tq2\tq3\tq4\n");
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    char *states[] = { "UNUSED\t", "EMBRYO\t", "SLEEPING", "RUNNABLE", "RUNNING\t", "ZOMBIE\t" };
    if(p->state != UNUSED)
      cprintf("%d\t%d\t\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", p->pid, p->priority, states[p->state], p->rtime, p->cur_waiting_time, p->n_run, p->queue_no, p->ticks[0], p->ticks[1], p->ticks[2], p->ticks[3], p->ticks[4]);
  }
  return 0;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  #if SCHEDULER != SCHED_MLFQ
  struct proc *p;
  #endif
  struct cpu *c = mycpu();
  c->proc = 0;
  
  #if SCHEDULER == SCHED_RR
    for(;;){
      // Enable interrupts on this processor.
      sti();

      // Loop over process table looking for process to run.
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;

        // #ifdef DEBUG
        //   cprintf("On core: %d\nScheduling\nProcess name: %s with pid: %d and creation time: %d\n", c->apicid, p->name, p->pid, p->ctime);
        // #endif

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        p->cur_waiting_time = 0;
        p->n_run++;
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);

    }
    #elif SCHEDULER == SCHED_FCFS
    for(;;){
      // Enable interrupts on this processor.
      sti();
      int min_time = ticks + 50; // infinity for creation time
      struct proc *proc_selected = 0;

      // Loop over process table looking for process to run.
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        
        if(min_time > p->ctime)
        {
          min_time = p->ctime;
          proc_selected = p;
        }
      }

      if(proc_selected == 0)
      {
        release(&ptable.lock);
        continue;
      }

      #ifdef DEBUG
        cprintf("On core: %d\nScheduling\nProcess name: %s with pid: %d and creation time: %d\n", c->apicid, proc_selected->name, proc_selected->pid, proc_selected->ctime);
      #endif
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc_selected->cur_waiting_time = 0;
      proc_selected->n_run++;
      c->proc = proc_selected;
      switchuvm(proc_selected);
      proc_selected->state = RUNNING;

      swtch(&(c->scheduler), proc_selected->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      release(&ptable.lock);

    }
  #elif SCHEDULER == SCHED_PBS
    for(;;){
      // Enable interrupts on this processor.
      sti();
      struct proc *proc_selected = 0;
      int reset_chance = 1;

      // Loop over process table looking for process to run.
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        if(proc_selected == 0)
        {
          proc_selected = p;
        }
        else if(proc_selected->priority > p->priority || (proc_selected->priority == p->priority && proc_selected->chance > p->chance))
        {
          proc_selected = p;
        }
      }

      if(proc_selected == 0)
      {
        release(&ptable.lock);
        continue;
      }

      #ifdef DEBUG
        cprintf("On core: %d\nScheduling\nProcess name: %s with pid: %d and priority: %d\n", c->apicid, proc_selected->name, proc_selected->pid, proc_selected->priority);
      #endif
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire itcprintf("On core: %d\nScheduling\nProcess name: %s with pid: %d and priority: %d\n", c->apicid, proc_selected->name, proc_selected->pid, proc_selected->priority);
      // before jumping back to us.
      proc_selected->chance++;
      proc_selected->cur_waiting_time = 0;
      proc_selected->n_run++;
      c->proc = proc_selected;
      switchuvm(proc_selected);
      proc_selected->state = RUNNING;

      swtch(&(c->scheduler), proc_selected->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state!=RUNNABLE)
          continue;
        if(p->priority == proc_selected->priority && proc_selected->chance != p->chance)
        {
          reset_chance = 0;
          break;
        }
      }
      for(p = ptable.proc; p < &ptable.proc[NPROC] && reset_chance; p++){
        if(p->state!=RUNNABLE)
          continue;
        if(p->priority == proc_selected->priority)
        {
          p->chance = 0;
        }
      }
      release(&ptable.lock);

    }

  #elif SCHEDULER == SCHED_MLFQ
  for(;;)
  {
    sti();

    acquire(&ptable.lock);
    for(int i = 0; i < 5; i++)
    {
      if(length(queues[i]) > 0)
      {
        if(queues[i]->data->state == SLEEPING || queues[i]->data->state == ZOMBIE)
        {
          queues[i] = pop(queues[i]);
        }
      }
    }

    for(int i = 1;i < 5; i++)
    {
      while((length(queues[i]) > 0) && (ticks - queues[i]->data->enter_time > 30))
      {
        #ifdef DEBUG
        cprintf("Pid: %d is promoted from queue number: %d\n", queues[i]->data->pid, queues[i]->data->queue_no);
        #endif
        struct proc* temp = queues[i]->data;
        queues[i] = pop(queues[i]);
        temp->cur_waiting_time = 0;
        temp->cur_ticks = 0;
        temp->queue_no--;
        temp->enter_time = ticks;
        temp->change_queue = 0;
        queues[i-1] = push(queues[i-1], temp);
      }
    }
    struct proc *selected_proc = 0;
    for(int i = 0; i < 5;i++)
    {
      if((length(queues[i]) > 0) && (queues[i]->data->state == RUNNABLE))
      {
        selected_proc = queues[i]->data;
        queues[i] = pop(queues[i]);
        break;
      }
    }
    if(selected_proc == 0)
    {
      release(&ptable.lock);
      continue;
    }

    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    selected_proc->cur_waiting_time = 0;
    selected_proc->n_run++;
    c->proc = selected_proc;
    switchuvm(selected_proc);
    selected_proc->state = RUNNING;

    swtch(&(c->scheduler), selected_proc->context);
    switchkvm();
    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    if((selected_proc != 0) && (selected_proc->change_queue == 0) && (selected_proc->state == RUNNABLE))
    {
      selected_proc->cur_ticks = 0;
      selected_proc->enter_time = ticks;
      queues[selected_proc->queue_no] = push(queues[selected_proc->queue_no], selected_proc);
    }
    else if((selected_proc != 0) && (selected_proc->change_queue == 1) && (selected_proc->state == RUNNABLE))
    {
      // cprintf("queue change\n");
      selected_proc->cur_ticks = 0;
      selected_proc->enter_time = ticks;
      selected_proc->change_queue = 0;
      if(selected_proc->queue_no != 4)
      {
        #ifdef DEBUG
        cprintf("Pid: %d is demoted from queue number: %d\n", selected_proc->pid, selected_proc->queue_no);
        #endif
        selected_proc->queue_no++;
      }
      queues[selected_proc->queue_no] = push(queues[selected_proc->queue_no], selected_proc);
    }
    // cprintf("%s\n", selected_proc->name);
    release(&ptable.lock);
  }

  #endif
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

int
set_priority(int new_priority, int pid)
{
  struct proc *p;
  struct proc *curr_proc = 0;
  int old_priority;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid)
    {
      curr_proc = p;
      break;
    }
  }
  if(curr_proc == 0)
  {
    release(&ptable.lock);
    return -1;
  }
  if(new_priority > 100 || new_priority < 0)
  {
    release(&ptable.lock);
    return -1;
  }
  old_priority = curr_proc->priority;
  curr_proc->priority = new_priority;

  #ifdef DEBUG
    cprintf("Process with id %d and name %s changed its priority from %d to %d\n",curr_proc->pid, curr_proc->name, old_priority, new_priority);
  #endif

  curr_proc->chance = 0;
  release(&ptable.lock);
  if(curr_proc->priority < old_priority)
  {
    yield();
  }
  return old_priority;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
    {
      p->state = RUNNABLE;
      // cprintf("Inside wakeup1, pid: %d name: %s", p->pid,p->name);
      #if SCHEDULER == SCHED_MLFQ
      p->cur_ticks = 0;
      p->enter_time = ticks;
      p->change_queue = 0;
      queues[p->queue_no] = push(queues[p->queue_no], p);
      #endif
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
      {
        p->state = RUNNABLE;
        #if SCHEDULER == SCHED_MLFQ
        p->cur_ticks = 0;
        p->enter_time = ticks;
        p->change_queue = 0;
        queues[p->queue_no] = push(queues[p->queue_no], p);
        #endif
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
