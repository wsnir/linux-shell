#include <linux/limits.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <ctype.h>
#include "LineParser.h"

int debug = 0;

#define TERMINATED -1
#define RUNNING 1
#define SUSPENDED 0

#define HISTLEN 10

typedef struct process{
    cmdLine* cmd;                         /* the parsed command line*/
    pid_t pid; 		                  /* the process id that is running the command*/
    int status;                           /* status of the process: RUNNING/SUSPENDED/TERMINATED */
    struct process *next;	                  /* next process in chain */
} process;

typedef struct history_entry {
    char *command;
    struct history_entry *next;
} history_entry;

process *processList = NULL;

history_entry *history_head = NULL;
history_entry *history_tail = NULL;
int history_count = 0;

void addHistoryEntry(const char *command) {
    if (HISTLEN <= 0) {
        return;
    }

    history_entry *new_entry = (history_entry *)malloc(sizeof(history_entry));
    new_entry->command = strdup(command);
    new_entry->next = NULL;

    if (history_count == HISTLEN) {
        history_entry *temp = history_head;
        history_head = history_head->next;
        free(temp->command);
        free(temp);
        history_count--;
    }

    if (history_count) {
        history_tail->next = new_entry;
    } else {
        history_head = new_entry;
    }

    history_tail = new_entry;
    history_count++;
}

void printHistory() {
    history_entry *current = history_head;
    int index = 1;
    while (current) {
        printf("%d %s\n", index, current->command);
        current = current->next;
        index++;
    }
}

char *getHistoryEntry(int index) {
    if (index < 1 || index > history_count) {
        perror("No such entry in history");
        return NULL;
    }

    history_entry *current = history_head;
    for (int i = 1; i < index; i++) {
        current = current->next;
    }
    return current->command;
}

void addProcess(process** process_list, cmdLine* cmd, pid_t pid){
    if (debug) {
        fprintf(stderr, "Adding process cmd = %p\n", cmd);
    }
    process* newProcess = (process*)malloc(sizeof(process));
    newProcess->next = NULL;
    newProcess->pid = pid;
    newProcess->cmd = cmd;
    newProcess->status = RUNNING;
    if (*process_list == NULL) { 
        *process_list = newProcess;
    } else {
        newProcess->next = *process_list;
        *process_list = newProcess;
    }
}

void updateProcessStatus(process* process_list, int pid, int status) {
    process* current = process_list;
    while (current) {
        if (current->pid == pid) {
            current->status = status;
            break;
        }
        current = current->next;
    }
}

void updateProcessList(process **process_list) {
    process* current = *process_list;
    int status;
    while (current) {
        pid_t result = waitpid(current->pid, &status, WNOHANG | WUNTRACED);
        if (result == -1) {
            updateProcessStatus(current, current->pid, TERMINATED);
        } else if (result > 0) {
            if (result == current->pid) {
                if (WIFEXITED(status)) {
                    updateProcessStatus(current, current->pid, TERMINATED);
                } else if (WIFSIGNALED(status)) {
                    updateProcessStatus(current, current->pid, TERMINATED);
                } else if (WIFSTOPPED(status)) {
                    updateProcessStatus(current, current->pid, SUSPENDED);
                } else if (status == __W_CONTINUED) {
                    updateProcessStatus(current, current->pid, RUNNING);
                }
            }
        }
        current = current->next;
    }
}

void printProcessList(process** process_list) {
    updateProcessList(process_list);
    process* current = *process_list;
    process* prev = NULL;

    printf("PID\t\tCommand\t\tSTATUS\n");
    while (current) {
        char* status;
        if (current->status == RUNNING) {
            status = "Running";
        } else if (current->status == SUSPENDED) {
            status = "Suspended";
        } else if (current->status == TERMINATED) {
            status = "Terminated";
        }

        printf("%d\t\t%s\t\t%s\n", current->pid, current->cmd->arguments[0], status);

        if (current->status == TERMINATED) {
            if (prev) {
                prev->next = current->next;
                freeCmdLines(current->cmd);
                free(current);
                current = prev->next;
            } else {
                *process_list = current->next;
                freeCmdLines(current->cmd);
                free(current);
                current = *process_list;
            }
        } else {
            prev = current;
            current = current->next;
        }
    }
}

void handle_redirection(cmdLine *pCmdLine) {
    // Handle input redirection
    if (pCmdLine->inputRedirect) {
        int input_fd = open(pCmdLine->inputRedirect, O_RDONLY);
        if (input_fd == -1) {
            perror("Failed to open input file");
            _exit(1);
        }
        if (dup2(input_fd, STDIN_FILENO) == -1) {
            perror("Failed to redirect input");
            _exit(1);
        }
        close(input_fd);
    }

    // Handle output redirection
    if (pCmdLine->outputRedirect) {
        int output_fd = open(pCmdLine->outputRedirect, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (output_fd == -1) {
            perror("Failed to open output file");
            _exit(1);
        }
        if (dup2(output_fd, STDOUT_FILENO) == -1) {
            perror("Failed to redirect output");
            _exit(1);
        }
        close(output_fd);
    }
}

int handleSignalExecution(cmdLine *pCmdLine){
    int signal = 0;
    int status;
    if (strcmp(pCmdLine->arguments[0], "stop") == 0) {
        signal = SIGTSTP ;
        status = SUSPENDED;
    } else if (strcmp(pCmdLine->arguments[0], "wake") == 0) {
        signal = SIGCONT;
        status = RUNNING;
    } else if (strcmp(pCmdLine->arguments[0], "term") == 0) {
        signal = SIGINT;
        status = TERMINATED;
    }
    
    if (signal){
        int pid = atoi(pCmdLine->arguments[1]);
        printf("Sending signal to %d\n", pid);
        if (kill(pid, signal) == -1) {
            perror("Failed to send signal");
        }
        else {
            updateProcessStatus(processList, pid, status);
        }
        freeCmdLines(pCmdLine);
    }
    return signal;
}

int handlePipeExecution(cmdLine *left, cmdLine *right){
    // detaching right from left
    left->next = NULL;
    right->idx = 0;

    if (left->outputRedirect || right->inputRedirect) {
        fprintf(stderr, "Error: Invalid redirection with pipes\n");
        freeCmdLines(left);
        freeCmdLines(right);
        return 1;
    }

    int pipefd[2];
    pid_t leftChildPid;
    pid_t rightChildPid;

    if (pipe(pipefd) == -1) {
        perror("pipe creation failed");
        _exit(1);
    }

    if ((leftChildPid = fork()) == -1) {
        perror("fork failed");
        _exit(1);
    }

    if (leftChildPid == 0) {
        handle_redirection(left);
        close(pipefd[0]); // Close unused read end
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe write-end
        close(pipefd[1]);
        if (execvp(left->arguments[0], left->arguments) == -1) {
            perror("Execution failed");
            _exit(1);
        }
    }

    if ((rightChildPid = fork()) == -1) {
        perror("fork failed");
        _exit(1);
    }

    if (rightChildPid == 0) {
        handle_redirection(right);
        close(pipefd[1]); // Close unused write end
        dup2(pipefd[0], STDIN_FILENO); // Redirect stdin to pipe read-end
        close(pipefd[0]);
        if (execvp(right->arguments[0], right->arguments) == -1) {
            perror("Execution failed");
            _exit(1);
        }
    }

    close(pipefd[0]);
    close(pipefd[1]);

    if (debug) {
        fprintf(stderr, "First child PID: %d\n", leftChildPid);
        fprintf(stderr, "Second child PID: %d\n", rightChildPid);
    }
    
    if (waitpid(leftChildPid, NULL, 0) == -1) {
        perror("Error waiting for left child");
    }

    if (waitpid(rightChildPid, NULL, 0) == -1) {
        perror("Error waiting for right child");
    }

    addProcess(&processList, left, leftChildPid);
    addProcess(&processList, right, rightChildPid);
    return 0;
}

int handle_cd(cmdLine *parsedLine) {
    if (strcmp(parsedLine->arguments[0], "cd") == 0) {
        if (parsedLine->argCount < 2) {
            if (debug) {
                fprintf(stderr, "cd: missing argument\n");
            }
        }
        else {
            if (chdir(parsedLine->arguments[1]) != 0) {
                if (debug) {
                    fprintf(stderr, "chdir failed\n");
                }
            }
        }
        freeCmdLines(parsedLine);
        return 1;
    }
    return 0;
}

void execute(cmdLine *pCmdLine){
    if (handle_cd(pCmdLine) || handleSignalExecution(pCmdLine)){
        return;
    }

    if (pCmdLine->next){
        handlePipeExecution(pCmdLine, pCmdLine->next);
        return;
    }

    if (strcmp(pCmdLine->arguments[0], "procs") == 0){
        printProcessList(&processList);
        freeCmdLines(pCmdLine);
        return;
    }

    if (strcmp(pCmdLine->arguments[0], "history") == 0) {
        printHistory();
        freeCmdLines(pCmdLine);
        return;
    }
    
    pid_t pid = fork();

    if (pid == -1){
        perror("fork creation failed");
        _exit(1);
    }
    else if (pid == 0){
        handle_redirection(pCmdLine);
        if (execvp(pCmdLine->arguments[0], pCmdLine->arguments) == -1) {
            perror("The execution failed");
            _exit(1);
        }
        _exit(0);
    } else {
        //means the pid is the parent's
        addProcess(&processList, pCmdLine, pid);
        if (debug){
            fprintf(stderr, "PID: %d\n", pid);
            fprintf(stderr, "Executing command: %s\n", pCmdLine->arguments[0]);
        }
        if (pCmdLine->blocking){
            if (waitpid(pid, NULL, 0) == -1) {
                perror("waitpid failed");
            }
        }
    }
}

void freeProcessList(process *process_list){
    if (process_list){
        freeCmdLines(process_list->cmd);
        freeProcessList(process_list->next);
        free(process_list);
    }
}

void delete_history(history_entry *history){
    if (history){
        free(history->command);
        delete_history(history->next);
        free(history);
    }
}

int main(int argc, char** argv){
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        debug = 1;
    }
    
    while (1){
        char path[PATH_MAX];
        getcwd(path, PATH_MAX);
        printf("%s>>",path);
        char input[2048];
        fgets(input, 2048, stdin);

        if (debug){
            fprintf(stderr, "%s\n", input);
        }

        input[strcspn(input, "\n")] = '\0';

        if (!input[0]){
            continue;
        }
        
        if (strcmp(input, "quit") == 0){
            break;
        }

        cmdLine *line = parseCmdLines(input);

        if (line->arguments[0][0] == '!'){
            int index = isdigit(line->arguments[0][1]) ? atoi(&line->arguments[0][1]) : history_count;
            char *command = getHistoryEntry(index);
            if (command){
                cmdLine *redo = parseCmdLines(command);
                addHistoryEntry(command);
                execute(redo);
            }
            freeCmdLines(line);
            continue;
        }

        addHistoryEntry(input);
        execute(line);
    }
    freeProcessList(processList);
    delete_history(history_head);
}