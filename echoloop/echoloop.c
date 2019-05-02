#include "sopbuf.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum echoloop_semnum {
	SEM_SINGLE,
	SEM_MAIN,
	SEM_SENDER,
	SEM_TR_ACTIVE,
	SEM_TR_DONE,
	SEM_MAX
};

#define MAX_SOPS 8
#define ECHO_INTERVAL 1

struct strelem {
	char *str;
	size_t str_s;
	struct strelem *next;
};

typedef struct strlist {
	struct strelem *first;
	struct strelem *last;
} strlist_t;

strlist_t *echo_strlist = NULL;

struct strlist *strlist_new()
{
	struct strlist *ptr = malloc(sizeof(*ptr));
	if (!ptr)
		return NULL;
	ptr->first = NULL;
	ptr->last = NULL;
	return ptr;
}

void strlist_delete(struct strlist *list)
{
	for (struct strelem *ptr = list->first; ptr != NULL; ptr = ptr->next)
		free(ptr->str);
	free(list);
}

/* All strings here must be dynamically allocated */
/* List may be safely printed while interrupt */
int strlist_append(struct strlist *list, char *str, size_t str_s)
{
	struct strelem *elem = malloc(sizeof(*elem));
	if (!elem)
		return -1;
	elem->str = str;
	elem->str_s = str_s;

	if (!list->first)
		list->first = elem;
	else
		list->last->next = elem;

	list->last = elem;
	return 0;
}

int strlist_print(struct strlist *list, int fd)
{
	for (struct strelem *ptr = list->first; ptr != NULL; ptr = ptr->next) {
		char *str = ptr->str;
		size_t str_s = ptr->str_s;
		while (str_s) {
			ssize_t ret = write(fd, str, str_s);
			if (ret < 0) {
				perror("Error: write");
				return -1;
			}
			str_s -= ret;
		}
		write(fd, "\n", 1);
	}
	return 0;
}

jmp_buf sighandler_exit_buf;

void sighandler_echo_strlist(int sig)
{
	if (strlist_print(echo_strlist, STDOUT_FILENO) < 0)
		longjmp(sighandler_exit_buf, -1);
}

void sighandler_quit(int sig)
{
	fprintf(stderr, "%s caught, exiting...\n", strsignal(sig));
	longjmp(sighandler_exit_buf, 1);
}

int echoloop_main_ready(sopbuf_t *sops)
{
	/* Do not start while previous transfer is running */
	sopbuf_add(sops, SEM_TR_ACTIVE, 0, 0);
	sopbuf_add(sops, SEM_MAIN,      1, SEM_UNDO);
	if (sopbuf_semop(sops) < 0) {
		perror("Error: semop");
		return -1;
	}
	return 0;
}

int echoloop_main_enter_section(sopbuf_t *sops)
{
	/* Wait for sender */
	/* Up active */
	sopbuf_add(sops, SEM_SENDER,    -1, 0);
	sopbuf_add(sops, SEM_SENDER,     1, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE,  1, SEM_UNDO);
	if (sopbuf_semop(sops) < 0) {
		perror("Error: semop");
		return -1;
	}

	/* Check that sender process is running */
	/* Wait for sender process to be active */
	sopbuf_add(sops, SEM_SENDER,    -1, IPC_NOWAIT);
	sopbuf_add(sops, SEM_SENDER,     1, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE, -2, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE,  2, 0);
	if (sopbuf_semop(sops) < 0) {
		if (errno == EAGAIN)
			fprintf(stderr, "Error: sender process is dead\n");
		else
			perror("Error: semop");
		return -1;
	}
	return 0;
}

int echoloop_main_quit_section(sopbuf_t *sops)
{
	/* Transfer done */
	sopbuf_add(sops, SEM_TR_DONE, 1, SEM_UNDO);
	if (sopbuf_semop(sops) < 0) {
		perror("Error: semop");
		return -1;
	}

	/* Check that sender process is active */
	/* Wait for sender */
	/* Release main lock */
	sopbuf_add(sops, SEM_TR_ACTIVE, -2, IPC_NOWAIT);
	sopbuf_add(sops, SEM_TR_ACTIVE,  2, 0);
	sopbuf_add(sops, SEM_TR_DONE,   -2, 0);
	sopbuf_add(sops, SEM_TR_DONE,    2, 0);
	sopbuf_add(sops, SEM_MAIN,      -1, SEM_UNDO);
	if (sopbuf_semop(sops) == -1) {
		if (errno == EAGAIN)
			fprintf(stderr, "Error: main process failed\n");
		else
			perror("Error: semop");
		return -1;
	}

	/* Success, wait for sender */
	/* Disable transfer */
	/* Restore SEM_TR_DONE */
	sopbuf_add(sops, SEM_SENDER,     0, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE, -1, SEM_UNDO);
	sopbuf_add(sops, SEM_TR_DONE,   -1, SEM_UNDO);
	if (sopbuf_semop(sops) == -1) {
		perror("Error: semop");
		return -1;
	}
	return 0;
}

int echoloop_main_receive(sopbuf_t *sops, int fifo_fd)
{
	if (echoloop_main_ready(sops) < 0)
		return -1;

	if (echoloop_main_enter_section(sops) < 0)
		return -1;

	ssize_t ret;
	size_t buf_s;

	ret = read(fifo_fd, &buf_s, sizeof(size_t));
	if (ret != sizeof(size_t)) {
		fprintf(stderr, "Error: read sizeof str");
		return -1;
	}

	char *buf = malloc(buf_s);
	if (!buf) {
		perror("Error: malloc");
		return -1;
	}

	char *ptr = buf;
	size_t rem = buf_s;

	while (rem) {
		ret = read(fifo_fd, ptr, rem);
		if (ret < 0) {
			perror("Error: read");
			return -1;
		}
		ptr += ret;
		rem -= ret;
		if (ret == 0 && rem != 0) {
			fprintf(stderr, "Error: can't receive full data\n");
			return -1;
		}
	}

	if (strlist_append(echo_strlist, buf, buf_s) < 0) {
		perror("Error: strlist_append");
		return -1;
	}

	if (echoloop_main_quit_section(sops) < 0)
		return -1;
	return 0;
}

int echoloop_main(sopbuf_t *sops, char *data)
{
	int fifo_fd = open("/tmp/echoloop.fifo", O_RDONLY | O_NONBLOCK);
	if (fifo_fd < 0) {
		perror("Error: open");
		return -1;
	}
	if (fcntl(fifo_fd, F_SETFL, 0) < 0) {
		perror("Error: fnctl");
		return -1;
	}

	char *data_copy = strdup(data);
	if (!data_copy) {
		perror("Error: strdup");
		return -1;
	}

	echo_strlist = strlist_new();
	if (!echo_strlist) {
		perror("Error: strlist_new");
		return -1;
	}

	if (strlist_append(echo_strlist, data_copy, strlen(data_copy)) < 0) {
		perror("Error: strlist_append");
		return -1;
	}

	struct sigaction echo_sa = {
		.sa_handler = sighandler_echo_strlist,
		.sa_flags   = SA_RESTART
	};

	struct itimerval echo_time = {
		.it_interval = { .tv_sec = ECHO_INTERVAL, .tv_usec = 0 },
		.it_value    = { .tv_sec = ECHO_INTERVAL, .tv_usec = 0 }
	};

	if (sigaction(SIGALRM, &echo_sa, NULL) < 0) {
		perror("Error: sigaction");
		return -1;
	}

	if (setitimer(ITIMER_REAL, &echo_time, NULL) < 0) {
		perror("Error: setitimer\n");
		return -1;
	}

	while (1) {
		if (echoloop_main_receive(sops, fifo_fd) < 0) {
			close(fifo_fd);
			return -1;
		}
	}

	close(fifo_fd);

	return 0;
}

int echoloop_sender_capture(sopbuf_t *sops)
{
	/* Capture sender "mutex" */
	/* Do not start while previous transfer is running */
	sopbuf_add(sops, SEM_TR_ACTIVE, 0, 0);
	sopbuf_add(sops, SEM_SENDER,    0, 0);
	sopbuf_add(sops, SEM_SENDER,    1, SEM_UNDO);
	if (sopbuf_semop(sops) < 0) {
		perror("Error: semop");
		return -1;
	}
	return 0;
}

int echoloop_sender_enter_section(sopbuf_t *sops)
{
	/* Check that main process is running */
	/* Wait main process to be redy for transfer */
	/* Up active */
	sopbuf_add(sops, SEM_SINGLE,    -1, IPC_NOWAIT);
	sopbuf_add(sops, SEM_SINGLE,     1, 0);
	sopbuf_add(sops, SEM_MAIN,      -1, 0);
	sopbuf_add(sops, SEM_MAIN,       1, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE,  1, SEM_UNDO);
	if (sopbuf_semop(sops) < 0) {
		if (errno == EAGAIN)
			fprintf(stderr, "Error: main process is dead\n");
		else
			perror("Error: semop");
		return -1;
	}

	/* Check that main process is ready/running */
	/* Wait for main process to be active */
	sopbuf_add(sops, SEM_MAIN,      -1, IPC_NOWAIT);
	sopbuf_add(sops, SEM_MAIN,       1, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE, -2, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE,  2, 0);
	if (sopbuf_semop(sops) < 0) {
		if (errno == EAGAIN)
			fprintf(stderr, "Error: main process is dead\n");
		else
			perror("Error: semop");
		return -1;
	}
	return 0;
}

int echoloop_sender_quit_section(sopbuf_t *sops)
{
	/* Transfer done */
	sopbuf_add(sops, SEM_TR_DONE, 1, SEM_UNDO);
	if (sopbuf_semop(sops) < 0) {
		perror("Error: semop");
		return -1;
	}

	/* Check that main process is active */
	/* Wait for main */
	/* Release sender lock */
	sopbuf_add(sops, SEM_TR_ACTIVE, -2, IPC_NOWAIT);
	sopbuf_add(sops, SEM_TR_ACTIVE,  2, 0);
	sopbuf_add(sops, SEM_TR_DONE,   -2, 0);
	sopbuf_add(sops, SEM_TR_DONE,    2, 0);
	sopbuf_add(sops, SEM_SENDER,    -1, SEM_UNDO);
	if (sopbuf_semop(sops) == -1) {
		if (errno == EAGAIN)
			fprintf(stderr, "Error: main process failed\n");
		else
			perror("Error: semop");
		return -1;
	}

	/* Success, wait for main */
	/* Disable transfer */
	/* Retsore SEM_TR_DONE */
	sopbuf_add(sops, SEM_MAIN,       0, 0);
	sopbuf_add(sops, SEM_TR_ACTIVE, -1, SEM_UNDO);
	sopbuf_add(sops, SEM_TR_DONE,   -1, SEM_UNDO);
	if (sopbuf_semop(sops) == -1) {
		perror("Error: semop");
		return -1;
	}
	return 0;
}

int echoloop_sender(sopbuf_t *sops, char *data)
{
	if (echoloop_sender_capture(sops) < 0)
		return -1;

	int fifo_fd = open("/tmp/echoloop.fifo", O_WRONLY | O_NONBLOCK);
	if (fifo_fd < 0) {
		if (errno == ENXIO)
			fprintf(stderr, "Error: main process failed\n");
		else
			perror("Error: open");
		return -1;
	}
	if (fcntl(fifo_fd, F_SETFL, 0) < 0) {
		perror("Error: fnctl");
		return -1;
	}

	if (echoloop_sender_enter_section(sops) < 0)
		return -1;

	size_t data_s = strlen(data);
	ssize_t ret = write(fifo_fd, &data_s, sizeof(size_t));
	if (ret != sizeof(size_t)) {
		fprintf(stderr, "Error: can't write data size\n");
		return -1;
	}

	char *ptr = data;

	while (data_s) {
		ret = write(fifo_fd, ptr, data_s);
		if (ret < 0) {
			perror("Error: write");
			return -1;
		}
		data_s -= ret;
		ptr += ret;
	}
	close(fifo_fd);

	if (echoloop_sender_quit_section(sops) < 0)
		return -1;

	fprintf(stdout, "echoloop for \"%s\" finished\n", data);
	return 0;
}

int echoloop_start(sopbuf_t *sops, char *str)
{
	/* Try to capture single "mutex" */
	/* Do not start while previous transfer is active */
	sopbuf_add(sops, SEM_TR_ACTIVE, 0, 0);
	sopbuf_add(sops, SEM_SINGLE,    0, IPC_NOWAIT);
	sopbuf_add(sops, SEM_SINGLE,    1, SEM_UNDO);
	if (sopbuf_semop(sops) < 0) {
		if (errno != EAGAIN) {
			perror("Error: semop");
			return -1;
		}
		if (echoloop_sender(sops, str) < 0)
			return -1;
	} else {
		if (echoloop_main(sops, str) < 0)
			return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Wrong argv\n");
		exit(EXIT_FAILURE);
	}

	int ret = mkfifo("/tmp/echoloop.fifo", 0666);
	if (ret < 0 && errno != EEXIST) {
		perror("Error: mkfifo");
		exit(EXIT_FAILURE);
	}

	key_t key = ftok("/tmp/echoloop.fifo", 1);
	if (key < 0) {
		perror("Error: ftok");
		exit(EXIT_FAILURE);
	}

	int semid = semget(key, SEM_MAX, IPC_CREAT | 0644);
	if (semid < 0) {
		perror("Error: semget");
		exit(EXIT_FAILURE);
	}

	sopbuf_t *sops = sopbuf_new(semid, MAX_SOPS);
	if (!sops) {
		perror("Error: sopbuf_new");
		exit(EXIT_FAILURE);
	}

	struct sigaction sa_quit = {
		.sa_handler = sighandler_quit
	};

	if (sigaction(SIGQUIT, &sa_quit, NULL) < 0) {
		perror("Error: sigaction");
		return -1;
	}
	if (sigaction(SIGINT, &sa_quit, NULL) < 0) {
		perror("Error: sigaction");
		return -1;
	}

	if (setjmp(sighandler_exit_buf)) {
		sopbuf_delete(sops);
		if (echo_strlist)
			strlist_delete(echo_strlist);
		exit(EXIT_FAILURE);
	}

	if (echoloop_start(sops, argv[1]) < 0) {
		fprintf(stderr, "Error: echoloop failed\n");
		sopbuf_delete(sops);
		if (echo_strlist)
			strlist_delete(echo_strlist);
		exit(EXIT_FAILURE);
	}

	sopbuf_delete(sops);
	if (echo_strlist)
		strlist_delete(echo_strlist);
	return 0;
}
