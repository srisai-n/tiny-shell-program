/* 
 * tsh - A tiny shell program with job control
 * 
 * N.Srisai Karthik - 201401072@daiict.ac.in
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
	char c;
	char cmdline[MAXLINE];
	int emit_prompt = 1; /* emit prompt (default) */

	/* Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout) */
	dup2(1, 2);

	/* Parse the command line */
	while ((c = getopt(argc, argv, "hvp")) != EOF) {
		switch (c) {
			case 'h':             /* print help message */
				usage();
				break;
			case 'v':             /* emit additional diagnostic info */
				verbose = 1;
				break;
			case 'p':             /* don't print a prompt */
				emit_prompt = 0;  /* handy for automatic testing */
				break;
			default:
				usage();
		}
	}

	/* Install the signal handlers */

	/* These are the ones you will need to implement */
	Signal(SIGINT,  sigint_handler);   /* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

	/* This one provides a clean way to kill the shell */
	Signal(SIGQUIT, sigquit_handler); 

	/* Initialize the job list */
	initjobs(jobs);

	/* Execute the shell's read/eval loop */
	while (1) {

		/* Read command line */
		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}
		if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
			app_error("fgets error");
		if (feof(stdin)) { /* End of file (ctrl-d) */
			fflush(stdout);
			exit(0);
		}

		/* Evaluate the command line */
		eval(cmdline);
		fflush(stdout);
		fflush(stdout);
	} 

	exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void eval(char *cmdline) 
{
	char *argv[MAXARGS];
	int bg=parseline(cmdline,argv);		// argv is built with parts of cmdline
	if(argv[0]==NULL)			// returns to main if command line is empty i.e. ignores empty lines
		return;
	sigset_t set;				// sigset_t variable is declared and initialized to empty set
	sigemptyset(&set);
	sigaddset(&set,SIGCHLD);		// adds SIGCHLD to the signal set

	if(builtin_cmd(argv))			// executes the built-in commands and shell prompts again
		return;
	sigprocmask(SIG_BLOCK,&set,NULL);	// sigprocmask blocks the signal set using SIG_BLOCK before it is added to the jobs table
	pid_t pid;
	if((pid=fork())==0){
		setpgrp();			// this is equivalent to setpgid(0,0), creates a separate process group
		if(execvp(argv[0],argv)<0){
			printf("%s: Command not found\n",argv[0]);
			exit(0);		// prints an error for the not found commands and exits
		}
	}
	if(!bg){
		/* the fg process is added to the job table
		 * sigprocmask unblocks the signal set after the addition of the job (SIG_UNBLOCK)
		 * the process waits for the fg process to be terminated (maximum one fg process at a time)
		 */
		addjob(jobs,pid,FG,cmdline);
		sigprocmask(SIG_UNBLOCK,&set,NULL);
		waitfg(pid);
	}
	else{
		/* the bg process is added to the job table
		 * sigprocmask unblocks the signal set after the addition of the job (SIG_UNBLOCK)
		 */
		addjob(jobs,pid,BG,cmdline);
		printf("[%d] (%d) %s",pid2jid(pid),pid,cmdline);
		sigprocmask(SIG_UNBLOCK,&set,NULL);
	}
	return;
}


/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
	static char array[MAXLINE]; /* holds local copy of command line */
	char *buf = array;          /* ptr that traverses command line */
	char *delim;                /* points to first space delimiter */
	int argc;                   /* number of args */
	int bg;                     /* background job? */

	strcpy(buf, cmdline);
	buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
	while (*buf && (*buf == ' ')) /* ignore leading spaces */
		buf++;

	/* Build the argv list */
	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	}
	else {
		delim = strchr(buf, ' ');
	}

	while (delim) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf && (*buf == ' ')) /* ignore spaces */
			buf++;

		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		}
		else {
			delim = strchr(buf, ' ');
		}
	}
	argv[argc] = NULL;

	if (argc == 0)  /* ignore blank line */
		return 1;

	/* should the job run in the background? */
	if ((bg = (*argv[argc-1] == '&')) != 0) {
		argv[--argc] = NULL;
	}
	return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
	if(strcmp(argv[0],"quit")==0){
		/* if the first argument is "quit", it exits only if there are no running jobs in the background
		 * else prints a statement and returns to eval
		 */
		int i;
		for(i=0;i<MAXJOBS;i++){
			if(jobs[i].state==BG){
				printf("Running background jobs, can't quit\n");
				return 1;
			}
		}
		exit(0);
	}
	else if(strcmp(argv[0],"jobs")==0){
		/* if the argument is "jobs", it prints all the jobs - both running, foreground and stopped
		 */
		listjobs(jobs);
		return 1;
	}
	else if(strcmp(argv[0],"bg")==0 || strcmp(argv[0],"fg")==0){
		/* if the argument is either "fg" or "bg", it calls the method do_bgfg()
		 * do_bgfg() executes these fg and bg commands
		 */
		do_bgfg(argv);
		return 1;
	}
	return 0;				// command is not inbuilt
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
	pid_t jobpid;
	int i,temp;
	if(argv[1]==NULL){
		/* if argument is missing after fg or bg, prints an error message
		 */
		printf("%s command requires PID or %%jobid argument\n",argv[0]);
	}
	else{
		if(*argv[1]=='%'){
			/* if first char of argument is '%', checks whether the string after % is an integer
			 * if it's not an integer, prints an error
			 */
			for(i=1;i<strlen(argv[1]);i++){
				if(!isdigit(*(argv[1]+i))){
					printf("%s: argument must be a PID or %%jobid\n",argv[0]);
					return;
				}
			}
			/* once the string after % is a number, checks whether there is an existing job with the given jobid
			 * if there is no such process with the given jid, prints a message
			 */
			temp=atoi(argv[1]+1);
			if(getjobjid(jobs,temp)==NULL){
				printf("%s %%%d: No such job\n",argv[0],temp);
				return;
			}
			jobpid=getjobjid(jobs,temp)->pid;
		}
		else{
			/* checks if the input string is a number
			 * if it's not an integer, shows an error
			 */
			for(i=0;i<strlen(argv[1]);i++){
				if(!isdigit(*(argv[1]+i))){
					printf("%s: argument must be a PID or %%jobid\n",argv[0]);
					return;
				}
			}
			/* once the string is a number, checks whether there is an existing job with the given pid
			 * if there is no such process with the given pid, prints a message
			 */
			temp=atoi(argv[1]);
			if(getjobpid(jobs,temp)==NULL){
				printf("(%s): No such process\n",argv[1]);
				return;
			}
			jobpid=getjobpid(jobs,temp)->pid;
		}
		/* if the argument is "fg"("bg"), then the state of the job is changed to FG(BG)
		 * if it is "fg", then the process waits until current fg process completes its execution
		 * prints a message of the job details when it is a background job
		 * the stopped processes are continued executing using SIGCONT signal
		 */
		struct job_t *job=getjobpid(jobs,jobpid);
		kill(jobpid,SIGCONT);
		if(strcmp(argv[0],"fg")==0){
			job->state=FG;
			waitfg(jobpid);
		}
		else{
			job->state=BG;
			printf("[%d] (%d) %s",job->jid,job->pid,job->cmdline);
		}
	}
	return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
	struct job_t *job=getjobpid(jobs,pid);
	while(job!=NULL && job->state==FG){
		sleep(1);			// sleeps until process with pid is not the fg process
	}
	return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
	int stat;
	pid_t pid;
	struct job_t *job;
	while((pid=waitpid(-1,&stat,WNOHANG|WUNTRACED))>0){
		job=getjobpid(jobs,pid);
		if(WIFEXITED(stat)){
			deletejob(jobs,pid);	// normal exit, so the job is deleted
		}
		else if(WIFSIGNALED(stat)){
			/* job is terminated by a signal
			 * deletes the job from the jobs table
			 * prints a message having jid, pid of the job and the signal number that caused termination
			 */
			printf("Job [%d] (%d) terminated by signal %d\n",job->jid,job->pid,WTERMSIG(stat));
			deletejob(jobs,pid);
		}
		else if(WIFSTOPPED(stat)){
			/* job is stopped by a signal
			 * prints a message having jid, pid of the job and the signal number that caused stopping the process
			 * it's state is then changed to ST i.e. stopped
			 */
			printf("Job [%d] (%d) stopped by signal %d\n",job->jid,job->pid,WSTOPSIG(stat));
			job->state=ST;
		}
	}
	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
	pid_t pid=fgpid(jobs);
	if(pid!=0){
		/* terminates the fg job on the receipt of SIGINT signal
		 */
		kill(pid,SIGINT);
	}
	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
	pid_t pid=fgpid(jobs);
	if(pid!=0){
		/* stops the fg job on the receipt of SIGTSTP signal
		 */
		kill(pid,SIGTSTP);
	}
	return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
	int i, max=0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(jobs[i].cmdline, cmdline);
			if(verbose){
				printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
			}
			return 1;
		}
	}
	printf("Tried to create too many jobs\n");
	return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs)+1;
			return 1;
		}
	}
	return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return jobs[i].pid;
	return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
	int i;

	if (pid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return &jobs[i];
	return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
	int i;

	if (jid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return &jobs[i];
	return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid) {
			return jobs[i].jid;
		}
	return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
			switch (jobs[i].state) {
				case BG: 
					printf("Running ");
					break;
				case FG: 
					printf("Foreground ");
					break;
				case ST: 
					printf("Stopped ");
					break;
				default:
					printf("listjobs: Internal error: job[%d].state=%d ", 
							i, jobs[i].state);
			}
			printf("%s", jobs[i].cmdline);
		}
	}
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
	struct sigaction action, old_action;

	action.sa_handler = handler;  
	sigemptyset(&action.sa_mask); /* block sigs of type being handled */
	action.sa_flags = SA_RESTART; /* restart syscalls if possible */

	if (sigaction(signum, &action, &old_action) < 0)
		unix_error("Signal error");
	return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}



