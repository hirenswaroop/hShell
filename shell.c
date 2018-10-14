#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#define HIST 256

void parse(char *);
int process_builtin();
void initialize_history(void);
void save_cmd(char *);
void ifhistory(char *);
int redirectio(int, char *);
void ctrlCHandler();
void zombieHandler();
int count = 0;
int numPipes = 0;
int numcmds = 0;
char infile[64];
char outfile[64];
char *ex_argv[6][64];
int ex_argc[6];
int sigIntReceived;
int zombieSig;
int background;
int bgcount;

int
main(int argc, char **argv)
{
	int i, j, status;
	pid_t pid;
	char input[HIST];
	char cwd[128];
	char *ptr;
	int pipefd[2*5]; // Allows for 6 commands

	struct sigaction action;
	struct sigaction action2;

	action.sa_handler = ctrlCHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);

	action2.sa_handler = zombieHandler;
	sigemptyset(&action2.sa_mask);
	action2.sa_flags = 0;
	sigaction(SIGCHLD, &action2, NULL);

	for (i = 0 ; i < 6; i++) {
		for (j = 0 ; j < 64; j++) {
			ex_argv[i][j] = NULL;
		}
	}
	initialize_history();
	for (;;) {

		if (getcwd(cwd, sizeof(cwd)) != NULL) {
			if (isatty(0)) {
				printf("hShell:%s $ ", cwd);
			}
		}
		fflush(stdout);

		char *line = fgets(input, sizeof(input), stdin);
		if (line == NULL) {
			if (zombieSig) {
				zombieSig = 0;
				printf("[%d]+ Done\n", bgcount);
				bgcount--;
				continue;
			}
			if (!sigIntReceived) {
				printf("\n");
				exit(1);
			}
		}
		zombieSig = 0;
		input[strlen(input) - 1] = '\0';

		if (sigIntReceived) {
			sigIntReceived = 0;
			continue;
		}

		// Skip spaces
		for (i = 0; i < strlen(input); i++) {
			if (input[i] == ' ') {
				continue;
			} else {
				break;
			}
		}

		if (input[0] == '\0' || strlen(input) == i) {
			continue;
		}

		if (sigIntReceived) {
			sigIntReceived = 0;
			continue;
		}

		ifhistory(input);
		save_cmd(input);
		parse(input);
		if (!process_builtin()) {
			int i;
			int fdin, fdout, fderr;
			int In = dup(0);
			int Out = dup(1);
			int Err = dup(2);
			int pipefd[2];

			if (infile[0] != '\0') {
				fdin = redirectio(0, infile);
			} else  {
				fdin = dup(In);
			}

			fderr = dup(Err);
			dup2(fderr, 2);
			close(fderr);

			for (i = 0; i < numcmds; i++) {
				/* Setup input */
				dup2(fdin,  0);
				close(fdin);

				/* Last command? */
				if (i == numcmds - 1) {
					if (outfile[0] != '\0') {
						fdout = redirectio(1, outfile);
					} else  {
						fdout = dup(Out);	
					}
				} else {
					if (numPipes) {
						pipe(pipefd);

						fdin = pipefd[0];
						fdout = pipefd[1];
					}
				}

				dup2(fdout, 1);
				close(fdout);

				pid = fork();
				switch (pid) {
					/*
					 * Fork child and exec program
					 */
					case 0:
						if (execvp(ex_argv[i][0], &ex_argv[i][0]) < 0) {
							printf("Failed to execute program: %s errno: %d\n", ex_argv[i][0], errno);
							exit(1);
						}
						break;

					/* 
					 * Fork failed
					 */
					case -1:
						printf("Error: failed to fork child %d\n", errno);
						continue;
						break;

					/*
					 * Wait for child to run and exit
					 */
					default:
						if (!background) {
							waitpid(pid, NULL, 0);
						} else {
							printf("[%d] %d\n", bgcount, pid);
						}
						break;
				}
			}

			/* Restore default file descriptors */
			dup2(In, 0);
			dup2(Out, 1);
			dup2(Err, 2);

			close(In);
			close(Out);
			close(Err);
		}

		/*
		 * Now, Re-initialize argc and argv
		 */
		for (i = 0; i < 6; i++) {
			ex_argc[i] = 0;
			for (j = 0; j < 64; j++) {
				ex_argv[i][j] = NULL;
			}
		}
	}
	return 0;
}

/*
 * Parses the user input
 * Initializes argc and argv
 */
void
parse(char *input)
{
	char *ptr;
	int fileIn = 0;
	int fileOut = 0;
	int i, j;

	numPipes = 0;
	numcmds = 1;
	background = 0;
	infile[0] = '\0';
	outfile[0] = '\0';

	i = j = 0;
	ptr = strtok(input, " ");
	while (ptr != NULL) {
		if (*ptr == '<') {
			fileIn = 1;
		} else if (*ptr == '>') {
			fileOut = 1;
		} else if (*ptr == '|') {
			numPipes++;
			i++;
			j = 0;
		} else if (*ptr == '&') {
			bgcount++;
			background = 1;
		} else {
			if (fileIn) {
				strcpy(infile, ptr);
				fileIn = 0;
			} else if (fileOut) {
				strcpy(outfile, ptr);
				fileOut = 0;
			} else {
				ex_argc[i] += 1;
				ex_argv[i][j] = ptr;
				ex_argv[i][j+1] = NULL;
				j++;
			}
		}
		ptr = strtok(NULL, " ");

	}

	if (numPipes > 0) {
		numcmds += numPipes;
	}
}

int
redirectio(int inputfile, char *file)
{
	int fd;

	if (inputfile == 0) {
		fd = open(file, O_RDONLY);
		if (fd < 0) {
			printf("Error %d\n", errno);
			return -1;
		}
	} else {
		fd = open(file, O_CREAT | O_WRONLY, 0666);
		if (fd < 0) {
			printf("Error %d\n", errno);
			return -1;
		}
	}
	return fd;
}

struct history {
	int index;
	char cmd_line[HIST / 2];
	struct history *next;
	struct history *prev;
};

struct history *histptr, *currhist;

/*
 * Allocates memory into each history node
 */
void 
initialize_history(void)
{
	int i;
	struct history *ptr, *hprev;

	ptr = hprev = NULL;
	for (i = 0; i < HIST; i++) {
		ptr = malloc(sizeof(struct history));
		ptr->index = i + 1;
		if (histptr == NULL) {
			histptr = currhist = ptr;
			ptr->prev = NULL;
		} else {
			hprev->next = ptr;
			ptr->prev = hprev;
		}
		hprev = ptr;
	}
	hprev->next = NULL;
}

/*
 * Saves user command into history 
 */
void
save_cmd(char *input)
{
	strcpy(currhist->cmd_line, input);
	currhist = currhist->next;
	count++;
}

/*
 * Runs commands in history
 */
void
ifhistory(char *input)
{
	int index = 0;
	struct history *curr = NULL;

	if (input[0] == '!') {
		if (input[1] >= 'A') {
			for(curr = currhist->prev; curr->prev != NULL; curr = curr->prev) {
				if (input[1] == curr->cmd_line[0]) {
					printf("%s\n", curr->cmd_line);
					strcpy(input, curr->cmd_line);
					break;
				}
			}
		} else {
			index = atoi(&input[1]);
			printf("%d\n", index);
			for (curr = histptr; curr->next != NULL; curr = curr->next) {
				if (curr->index == index) {
					break;
				}
			}
			printf("%s\n", curr->cmd_line);
			strcpy(input, curr->cmd_line);
		}
	} else if (input[0] == 0x1b && input[1] == 0x5b) {
		if (input[2] == 0x41) {
			curr = currhist->prev;
			currhist = curr;
			strcpy(input, curr->cmd_line);
		} else if (input[2] == 0x42) {
			printf("pagedown\n");
		}
	}
}

/*
 * Checks if input is a builtin processes and runs the process
 */
int
process_builtin()
{
	int i;
	int ret = 1;
	char path[128];
	struct history *ptr;

	if (strcmp(ex_argv[0][0], "cd") == 0) {
		if (ex_argv[0][1] != NULL) {
			strcpy(path, ex_argv[0][1]);
			if (chdir(path) == -1) {
				printf("Directory %s not found\n", path);
			}
		} else {
			chdir(getenv("HOME"));
		}
	} else if (strcmp(ex_argv[0][0], "exit") == 0) {
		exit(0);
	} else if (strcmp(ex_argv[0][0], "history") == 0) {
		ptr = histptr;
		for (i = 0; i < count; i++) {
			printf("%d: %s\n", ptr->index, ptr->cmd_line);
			ptr = ptr->next;
		}
	} else {
		ret = 0;
	}

	return ret;
}

void
ctrlCHandler()
{
	printf("\n");
	sigIntReceived = 1;
}

void
zombieHandler()
{
	while(waitpid(-1, 0, WNOHANG) > 0);
	zombieSig = 1;
}
