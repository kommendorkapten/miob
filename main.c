/*
 * Fredrik Skogman, skogman@gmail.com 2015.
 */

#ifndef __OpenBSD__
# define _POSIX_C_SOURCE 200112L
#endif

#if defined(__APPLE__) || defined(__OpenBSD__)
# define __EV_KQUEUE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#ifdef __EV_KQUEUE
# include <sys/event.h>
# include <sys/select.h>
#endif
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#ifdef __sun
# include <port.h>
#endif
#ifdef __linux
# include <sys/epoll.h>
#endif

/**
 * c99 -m64 -fast -o pollcc main.c
 * gcc -std=c99 -m64 -W -Wall -pedantic -O3 -o pollgcc main.c
 */

#define USEC(start, stop) (((stop.tv_sec - start.tv_sec) * 1000000) - \
                           (start.tv_usec - stop.tv_usec))

void get_args(int, char**);
void usage(const char*);
int  init_multiplex(const char*);
void child(int);
void parent(int);
void reap_chld();
void close_fds(int[], int);
int  drain(int);

int setup_select(void);
int wait_select(int);
int setup_poll(void);
int wait_poll(int);
#ifdef __sun
int setup_port(void);
int wait_port(int);
#endif
#ifdef __EV_KQUEUE
int setup_kqueue(void);
int wait_kqueue(int);
#endif
#ifdef __linux
int setup_epoll(void);
int wait_epoll(int);
#endif

pid_t pid;
int   NUM_FD;
int*  rfds;
int*  wfds;
int   nfds = 0;
int   verbose = 0;
char* METHOD = NULL;
struct pollfd* poll_fds;
#ifdef __sun
int evport;
#endif
#ifdef __EV_KQUEUE
int   kqfd;
struct kevent* kqevents;
#endif
#ifdef __linux
int epfd;
#endif
int  (*setup_multiplex)(void);
int (*wait4data)(int);

int main(int argc, char** argv)
{
	int cntl[2];
	int fds[2];
	int ulimit_n;
	struct rlimit rlp;
	struct sigaction sa;

	get_args(argc, argv);

	// We need to have 3 + 2 + 2*NUM_FD file descriptors open
	ulimit_n = 3 + 2 + NUM_FD * 2;
	printf("Need to open %d fds\n", ulimit_n);
	rlp.rlim_cur = ulimit_n;
	rlp.rlim_max = ulimit_n;
	if (setrlimit(RLIMIT_NOFILE, &rlp) < 0)
	{
		perror("main:setrlimit");
		return 1;
	}

	rfds = malloc(NUM_FD * sizeof(int));
	wfds = malloc(NUM_FD * sizeof(int));

	sa.sa_handler = &reap_chld;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGCHLD, &sa, NULL)) 
	{
		perror("main:sigaction");
		return 1;
	}

	// Set up control pipe
	if (pipe(cntl)) 
	{
		perror("main:pipe");
		return 1;
	}

	// Set up worker pipes
	for (int i = 0; i < NUM_FD; ++i)
	{
		if (pipe(fds)) 
		{
			fprintf(stderr, "pipe(2) failed at iteration %d\n", i);
			perror("main:pipe");
			return 1;
		}
		rfds[i] = fds[0];
		wfds[i] = fds[1];

		// Store the maximum fd for select.
		if (fds[1] > nfds) 
		{
			nfds = fds[1];
		}
		if (fds[0] > nfds) 
		{
			nfds = fds[0];
		}
	}

	// Let's begin
	pid = fork();
	if (pid < 0) 
	{
		perror("main:fork");
		return 1;
	}
	else if (pid == 0)
	{
		close(cntl[1]);
		close_fds(rfds, NUM_FD);

		child(cntl[0]);
		printf("Child done. Leaving.\n");
		_Exit(0);
	} 
	else 
	{
		close(cntl[0]);
		close_fds(wfds, NUM_FD);

		parent(cntl[1]);
		reap_chld();
	}

	return 0;
}

void usage(const char* program) 
{
	printf("%s [-v] -f NUM_FD -m METHOD -h\n", program);
	printf("METHOD can be poll, select, kqueue (BSD), epoll(Linux) and port (Solaris)\n");
	printf("-v enable verbose mode\n");

	return;
}

void get_args(int argc, char** argv) 
{
	int c;
	int m;
	int err = 0;
	extern char* optarg;
	extern int optopt;

	while ((c = getopt(argc, argv, ":vf:m:h:")) != -1)
	{
		switch (c)
		{
		case 'v':
			verbose = 1;
			break;
		case 'f':
			NUM_FD = atoi(optarg);
			break;
		case 'm':
			m = init_multiplex(optarg);
			if ( m < 0)
			{
				usage(argv[0]);
				exit(1);
			}
			if (m > 0)
			{
				exit(0);
			}
			break;
		case 'h':
			printf("Got h\n");
			usage(argv[0]);
			exit(0);
			break;
		case ':':
			usage(argv[0]);
			exit(optopt != 'h');
			break;
		case '?':
			fprintf(stderr, "Unknown argument %c\n", optopt);
			break;
		}
	}

	if (NUM_FD == 0)
	{
		fprintf(stderr, "NUM_FD must be provided\n");
		err = 1;
	}
	if (METHOD == NULL)
	{
		fprintf(stderr, "METHOD must be provided\n");
		err = 1;
	}

	if (err)
	{
		exit(1);
	}
}

int init_multiplex(const char* m)
{
	int err = 0;

	if (strcmp(m, "select") == 0) 
	{
		METHOD = "select";
		setup_multiplex = &setup_select;
		wait4data = &wait_select;
	}
	else if (strcmp(m, "poll") == 0)
	{
		METHOD = "poll";
		setup_multiplex = &setup_poll;
		wait4data = &wait_poll;
	}
#ifdef __EV_KQUEUE
	else if (strcmp(m, "kqueue") == 0)
	{
		METHOD = "kqueue";
		setup_multiplex = &setup_kqueue;
		wait4data = &wait_kqueue;
	}
#endif
#ifdef __linux
	else if (strcmp(m, "epoll") == 0)
	{
		METHOD = "epoll";
		setup_multiplex = &setup_epoll;
		wait4data = &wait_epoll;
	}
#endif
#ifdef __sun
	else if (strcmp(m, "port") == 0)
	{
		METHOD = "port";
		setup_multiplex = &setup_port;
		wait4data = &wait_port;
	}
#endif
	else
	{
		printf("Unknown method '%s'\n", m);
		err = -1;
	}

	return err;
}

void reap_chld(void) 
{
	int stat;
	pid_t child = wait(&stat);

	if (child < 0) 
	{
		if (errno == EINTR) 
		{
			return;
		}
		perror("reap_chld:wait");
		return;
	}

	if (WIFEXITED(stat))
	{
		stat = WEXITSTATUS(stat);
		printf("Child [%d] has died with exit status %d\n", child, stat);
	}
	else if (WIFSIGNALED(stat)) 
	{
		stat = WTERMSIG(stat);
		printf("Child [%d] was killed by signal %d\n", child, stat);
	}
	else 
	{
		stat = WSTOPSIG(stat);
		printf("Child [%d] was stopped by signal %d\n", child, stat);
	}
	

	return;
}

void child(int cntl) 
{
	ssize_t nb;
	int run = 1;

	while (run)
	{
		nb = read(cntl, &run, sizeof(run));
		if (nb != sizeof (run))
		{
			perror("child:read");
			_Exit(1);
		}
		nb = write(wfds[run], &run, sizeof(int));
		if (verbose)
		{
			printf("[child] write %d to fd %d\n", 
			       wfds[run], 
			       wfds[run]);
		}
		if (nb != sizeof (run))
		{
			perror("child:write");
			_Exit(1);
		}		
	}
	
	return;
}

void parent(int cntl)
{
	int op = NUM_FD;
	ssize_t nb;
	struct timeval start;
	struct timeval stop;
	long duration = 0;

	if (setup_multiplex())
	{
		// Off you go
		kill(pid, SIGKILL);
		return;
	}

	while (op) {
		--op;

		nb = write(cntl, &op, sizeof (op));
		if (nb != sizeof (op)) 
		{
			perror("parent:write");
		}

		if (gettimeofday(&start, NULL)) 
		{
			perror("parent:gettimeofday");
			exit(1);
		}

		wait4data(op);
		
		if (gettimeofday(&stop, NULL)) 
		{
			perror("parent:gettimeofday");
			exit(1);
		}

		duration += USEC(start, stop);
	}

	duration /= NUM_FD;
	printf("Average service time for %s was %ldus\n", METHOD, duration);

	return;
}

void close_fds(int fds[], int num)
{
	for (int i = 0; i < num; ++i)
	{
		close(fds[i]);
	}
	
	return;
}

int drain(int fd)
{
	int nb;
	int data;
	
	nb = read(fd, &data, sizeof(data));
	
	// nb = 0 is EOF, as fd is a blocking pipe.
	if (nb == 0) 
	{
		if (verbose)
		{
			printf("Reached EOF at %d\n", fd);
		}
	}
	else if (nb != sizeof(data))
	{
		printf("Could not drain pipe [%d]\n", fd);
		printf("Read %d, expected %lu\n", nb, sizeof(data));
	}

	return data;
}

int setup_select(void)
{
	printf("FD_SETSIZE: %d\n", FD_SETSIZE);
	return 0;
}

int wait_select(int d)
{
	fd_set fds;
	int nr;
	
	FD_ZERO(&fds);
	for (int i = 0; i < NUM_FD; ++i) 
	{
		FD_SET(rfds[i], &fds);
	}
	nr = select(nfds, &fds, NULL, NULL, NULL);
	if (nr < 0)
	{
		perror("wait_select:select");
		return -1;
	}
	if (nr != 1) 
	{
		printf("%d fds ready!\n", nr);
	}
	for (int i = 0; i < NUM_FD; ++i) 
	{
		if (FD_ISSET(rfds[i], &fds))
		{
			int rd;

			if (verbose) 
			{
				printf("fd %d is ready for reading\n", rfds[i]);
			}
			rd = drain(rfds[i]);
			if (rd != d) 
			{
				printf("Unexpected data %d at fd %d\n", 
				       rd, 
				       rfds[i]);
			}
		}
	}

	return 0;
}

int setup_poll(void)
{
	poll_fds = malloc(NUM_FD * sizeof(struct pollfd));
	for (int i = 0; i < NUM_FD; ++i) 
	{
		poll_fds[i].fd = rfds[i];
		poll_fds[i].events = POLLIN;
	}
	return 0;
}

int wait_poll(int d)
{
	int nr;

	nr = poll(poll_fds, NUM_FD, -1);
	if (nr < 0)
	{
		perror("wait_poll:poll");
		return -1;
	}
	nr = 0;
	for (int i = 0; i < NUM_FD; ++i)
	{
		if (poll_fds[i].revents & POLLIN)
		{
			int rd;

			if (verbose)
			{
				printf("fd %d is ready for reading\n", rfds[i]);
			}
			rd = drain(rfds[i]);
			++nr;
			if (rd != d) 
			{
				printf("Unexpected data %d at fd %d\n", 
				       rd,
				       rfds[i]);
			}
		}
		else if (poll_fds[i].revents & POLLHUP)
		{
			if (verbose) 
			{
				printf("fd %d has been hung up on.\n", rfds[i]);
			}
		}
	}
	if (nr != 1) 
	{
		printf("%d fds ready!\n", nr);
	}

	return 0;
}

#ifdef __sun
int setup_port(void)
{
	evport = port_create();
	if (evport < 0)
	{
		perror("setup_port:port_create");
		return 1;
	}
	for (int i = 0; i < NUM_FD; ++i)
	{
		if (port_associate(evport, 
				   PORT_SOURCE_FD,
				   (uintptr_t) rfds[i],
				   POLLIN,
				   NULL))
		{
			perror("setup_port:port_associate");
			return 1;
		}
	}

	return 0;
}

int wait_port(int d)
{
	port_event_t pe;
	int fd;

	if (port_get(evport, &pe, NULL) != 0)
	{
		perror("wait_port:port_get");
		return -1;
	}

	fd = (int)pe.portev_object;
	int rd;
			
	if (verbose)
	{
		printf("fd %d is ready for reading\n", fd);
	}
	rd = drain(fd);
	if (rd != d )
	{
		printf("Unexpected data %d at fd %d\n", 
		       rd,
		       fd);
	}
	
	// Objects of type PORT_SOURCE_FD are removed from the 
	// port when an event occur.
	if(port_associate(evport,
			  PORT_SOURCE_FD,
			  (uintptr_t) fd,
			  POLLIN,
			  NULL))
	{
		perror("wait_port:port_associate");
	}
	
	return 0;
}
#endif

#ifdef __EV_KQUEUE
int setup_kqueue(void)
{
	kqfd = kqueue();
	if (kqfd < 0) 
	{
		perror("setup_kqueue:kqueue");
		return 1;
	}

	kqevents = malloc(NUM_FD * sizeof(struct kevent));
	for (int i = 0; i < NUM_FD; ++i) 
	{
		EV_SET(kqevents + i,
		       rfds[i],
		       EVFILT_READ,
		       EV_ADD | EV_ENABLE,
		       0,
		       0,
		       NULL);
	}
	if (kevent(kqfd, kqevents, NUM_FD, NULL, 0, NULL) < 0)
	{
		perror("setup_kqueue:kevent");
		return 1;
	}
	return 0;
}

int wait_kqueue(int d)
{
	struct kevent ke;
	int rd;
	int fd;

	if (kevent(kqfd, NULL, 0, &ke, 1, NULL) < 0) 
	{
		perror("wait_kqueue:kevent");
		return -1;
	}
	fd = (int) ke.ident;
	if (verbose)
	{
		printf("fd %d is read for reading (%dbytes)\n", 
		       fd,
		       (int)ke.data);
	}
	rd = drain(fd);
	if (rd != d) 
	{
		printf("Unexpected data %d at fd %d\n", 
		       rd,
		       fd);
	}

	return 0;
}
#endif

#ifdef __linux
int setup_epoll(void)
{
	struct epoll_event ev;

	epfd = epoll_create(NUM_FD);
	if (epfd < 0) 
	{
		perror("setup_epoll:epoll_create");
		return -1;
	}
	
	ev.events = EPOLLIN;
	for (int i = 0; i < NUM_FD; ++i) 
	{
		ev.data.fd = rfds[i];
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, rfds[i], &ev))
		{
			perror("setup_epoll:epoll_ctl");
		}
	}
	
	return 0;
}

int wait_epoll(int d)
{
	struct epoll_event ev;
	int ne;
	int fd;
	int rd;

	ne = epoll_wait(epfd, &ev, 1, -1);
	if (ne < 0 || ne == 0)
	{
		perror("wait_epoll:epoll_wait");
		return -1;
	}
	fd = ev.data.fd;
	if (verbose)
	{
		printf("fd %d is read for reading.\n", fd);
	}
	rd = drain(fd);
	if (rd != d)
	{
		printf("Unexpected data %d at fd %d\n", rd, fd);
	}

	return 0;
}
#endif
