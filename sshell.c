#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdbool.h>

#define CMDLINE_MAX 512
#define PATH_MAX_LEN 1024  // manually defined path max length

typedef struct {
    pid_t p_id;
    int exit_status; // Exit status after process termination.
} child_process;

// Phase 4 globals: track up to 4 background processes and their command
#define MAX_BG 4
static pid_t bg_pids[MAX_BG];
static int  bg_nprocs = 0;
static char *bg_command = NULL;

int input_tokenizer(char *cmd, char *ar_cg[]){
    int ar_count = 0;
    char *token = strtok(cmd, " \t");  // to handle input

    while (token != NULL) {
        if (ar_count >= 16) {
            // it detected too many so return
            return -1;
        }
        // stores token and increments for next 
        ar_cg[ar_count] = token;
        ar_count = ar_count + 1;
        token = strtok(NULL, " \t");
    }
    return ar_count;
}

static int argv_end_loc(int carg, int num_pipes, int pipe_pos[], int out_i, int arg_count) {
    int loc;
    if (carg < num_pipes) {
      loc = pipe_pos[carg];
    } else {
      if (out_i != -1)
        loc = out_i;
      else
        loc = arg_count;
    }
    return loc;
}

// fork + redirect in to STDIN and out to STDOUT then exec
static pid_t redir_exc(char *const argv[], int fdin, int fdout) {
    pid_t pid = fork();
    // error check 
    if (pid < 0) {
         perror("fork");
         return -1;
    }
    if (pid != 0) {
         return pid;
    }

    if (fdin != STDIN_FILENO) {
        dup2(fdin, STDIN_FILENO);
        close(fdin);
    }
    if (fdout != STDOUT_FILENO) {
        dup2(fdout, STDOUT_FILENO);
        close(fdout);
    }
    execvp(argv[0], argv);
    fprintf(stderr, "Error: command not found\n");
    _exit(1);
}

int main(void)
{
    char cmd[CMDLINE_MAX];
    char orig_cmd[CMDLINE_MAX];
    char *eof;
    char *arg_inp[17];
    char  isov[CMDLINE_MAX * 2];

    while (1) {
        char *nl;

        // --- Phase 4: reap any completed background job(s) ---
        int status_array[MAX_BG];
        if (bg_nprocs > 0) {
            int still = 0;
            for (int i = 0; i < bg_nprocs; ++i) {
                pid_t w = waitpid(bg_pids[i], &status_array[i], WNOHANG);
                if (w <= 0) still++;
            }
            if (still == 0) {
                // all done—print one line with every exit code
                fprintf(stderr, "+ completed '%s' ", bg_command);
                for (int i = 0; i < bg_nprocs; ++i) {
                    fprintf(stderr, "[%d]", WEXITSTATUS(status_array[i]));
                }
                fprintf(stderr, "\n");
                free(bg_command);
                bg_command = NULL;
                bg_nprocs = 0;
            }
        }

        /* Print prompt */
        printf("sshell@ucd$ ");
        fflush(stdout);

        /* Get command line */
        eof = fgets(cmd, CMDLINE_MAX, stdin);
        if (!eof)
            /* Make EOF equate to exit */
            strncpy(cmd, "exit\n", CMDLINE_MAX);

        /* Print command line if stdin is not provided by terminal */
        if (!isatty(STDIN_FILENO)) {
            printf("%s", cmd);
            fflush(stdout);
        }

        /* Remove trailing newline from command line */
        nl = strchr(cmd, '\n');
        if (nl)
            *nl = '\0';
        strcpy(orig_cmd, cmd);

        // if enter inputted
        if (cmd[0] == '\0') {
            continue;
        }

        /* Builtin command: exit */
        if (!strcmp(cmd, "exit")) {
            // Phase 4: block exit if a background job is still running
            if (bg_nprocs > 0) {
                fprintf(stderr, "Error: active job still running\n");
                continue;
            }
            fprintf(stderr, "Bye...\n");
            fprintf(stderr, "+ completed 'exit' [0]\n");
            break;
        }

        // --- Phase 4: meta-character spacing for > | < & ---
        int si = 0;
        int ci = 0;
        while (cmd[ci++]) {
            int c1 = ci - 1;
            if (cmd[c1] == '>' || cmd[c1] == '|' || cmd[c1] == '<' || cmd[c1] == '&') {
                isov[si++] = ' ';
                isov[si++] = cmd[c1];
                isov[si++] = ' ';
            } else {
                isov[si++] = cmd[c1];
            }
        }
        isov[si] = '\0';

        // call tokenization
        int arg_count = input_tokenizer(isov, arg_inp);
        if (arg_count == -1) { // too many args
            fprintf(stderr, "Error: too many process arguments\n");
            continue;
        }
        arg_inp[arg_count] = NULL;

        // pwd builtin function
        if (!strcmp(arg_inp[0], "pwd")) {
            char cwd[PATH_MAX_LEN]; // uses defined max path length
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("%s\n", cwd);
                fprintf(stderr, "+ completed 'pwd' [0]\n");
            } else {
                perror("getcwd");
                fprintf(stderr, "+ completed 'pwd' [1]\n");
            }
            continue; // don't fork
        }

        // cd builtin
        if (!strcmp(arg_inp[0], "cd")) {
            if (arg_count < 2) {
                // if no directory is given
                fprintf(stderr, "Error: missing argument for cd\n");
                fprintf(stderr, "+ completed 'cd' [1]\n");
            } else if (chdir(arg_inp[1]) != 0) {
                // if the given directory doesn't exist or cannot cd
                fprintf(stderr, "Error: cannot cd into directory\n");
                fprintf(stderr, "+ completed 'cd %s' [1]\n", arg_inp[1]);
            } else {
                // if chdir succeeds
                fprintf(stderr, "+ completed 'cd %s' [0]\n", arg_inp[1]);
            }
            continue; // don't fork
        }

        // input redirection parsing
        int in_i = -1;
        int fd_in = -1;
        for (int i = 0; i < arg_count; ++i) {
            if (!strcmp(arg_inp[i], "<")) { in_i = i; break; }
        }
        if (in_i != -1) {
            if (in_i == arg_count - 1) {
                fprintf(stderr, "Error: no input file\n");
                continue;
            }
            int mis_error = 0;
            for (int i = in_i + 1; i < arg_count; ++i) {
                if (!strcmp(arg_inp[i], "|")) {
                    fprintf(stderr, "Error: mislocated input redirection\n");
                    mis_error = 1;
                    break;
                }
            }
            if (mis_error) continue;
            fd_in = open(arg_inp[in_i + 1], O_RDONLY);
            if (fd_in < 0) {
                fprintf(stderr, "Error: cannot open input file\n");
                continue;
            }
            // fix for testcase
            int i = in_i;
            while (i +2 < arg_count) {
                arg_inp[i] = arg_inp[i +2];
                i = i + 1;
            }

            arg_count = arg_count -2;
            arg_inp[arg_count] = NULL;
        }

        // background job detecting
        bool background = false;
        if (arg_count > 0 && !strcmp(arg_inp[arg_count - 1], "&")) {
            background = true;
            arg_inp[--arg_count] = NULL;
            // ensure single & at end, no stray '&'
            for (int i = 0; i < arg_count; ++i) {
                if (!strcmp(arg_inp[i], "&")) {
                    fprintf(stderr, "Error: mislocated background sign\n");
                    background = false;
                    break;
                }
            }
            if (!background) continue;
        }

        // locate | and >
        int pipe_pos[3];
        int num_pipes = 0;
        int out_i      = -1;
        for (int i = 0; i < arg_count; ++i) {
            if (num_pipes < 3 && !strcmp(arg_inp[i], "|")) {
                pipe_pos[num_pipes++] = i;
            }
            if (out_i == -1 && !strcmp(arg_inp[i], ">")) {
                out_i = i;
            }
        }

        // outputs for error stuff -------------
        int first_pipe = -1;
        int last_pipe  = -1;
        int arg_1      = arg_count - 1;
        if (num_pipes > 0) {
            first_pipe = pipe_pos[0];
            last_pipe  = pipe_pos[num_pipes - 1];
        }
        if ((first_pipe == 0) ||
            (last_pipe == arg_1) ||
            (num_pipes > 1 && pipe_pos[1] - first_pipe == 1) ||
            out_i == 0) {
            fprintf(stderr, "Error: missing command\n");
            continue;
        }
        if (out_i != -1 && out_i == arg_1) {
            fprintf(stderr, "Error: no output file\n");
            continue;
        }
        // Detects and blocks mislocated '>'
        int mis_error = 0;
        if (out_i != -1) {
            for (int i = out_i + 1; i < arg_count; ++i) {
                if (!strcmp(arg_inp[i], "|")) {
                    fprintf(stderr, "Error: mislocated output redirection\n");
                    mis_error = 1;
                    break;
                }
            }
            if (mis_error) continue;
        }

        // open output file
        int fd_out = -1;
        if (out_i != -1) {
            fd_out = open(arg_inp[out_i + 1],
                          O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd_out < 0) {
                fprintf(stderr, "Error: cannot open output file\n");
                continue;
            }
            arg_inp[out_i] = NULL; // remove '>'
        }

        // pipeline stuff
        if (num_pipes) {
            int rd_end = (fd_in != -1 ? fd_in : STDIN_FILENO), fds[2];
            pid_t pids[4];
            int   status[4];
            int seg_start = 0;

            for (int c = 0; c <= num_pipes; ++c) {
                int seg_end = argv_end_loc(c, num_pipes,
                                          pipe_pos, out_i, arg_count);
                arg_inp[seg_end] = NULL;
                char **sub_argv  = &arg_inp[seg_start];
                int fdout = (c == num_pipes && fd_out != -1)
                            ? fd_out : STDOUT_FILENO;
                if (c < num_pipes) {
                    pipe(fds);
                    fdout = fds[1];
                }
                // Fork the child
                pids[c] = redir_exc(sub_argv, rd_end, fdout);
                if (rd_end != STDIN_FILENO) close(rd_end);
                if (c < num_pipes) {
                    close(fds[1]);
                    rd_end = fds[0];
                }
                seg_start = seg_end + 1;
            }

            // Phase 4: background pipeline storage
            if (background) {
                for (int i = 0; i <= num_pipes; ++i) {
                    bg_pids[i] = pids[i];
                }
                bg_nprocs  = num_pipes + 1;
                bg_command = strdup(orig_cmd);
                if (fd_out != -1) close(fd_out);
                continue;
            }

            // waits for completion
            for (int c = 0; c <= num_pipes; ++c) {
                waitpid(pids[c], &status[c], 0);
            }
            // prints completion
            fprintf(stderr, "+ completed '%s' ", orig_cmd);
            for (int c = 0; c <= num_pipes; ++c) {
                fprintf(stderr, "[%d]", WEXITSTATUS(status[c]));
            }
            fputc('\n', stderr);
            if (fd_out != -1) close(fd_out);
            continue;
        }

        // single command
        child_process proc;
        proc.p_id = fork();
        if (proc.p_id < 0) {
            perror("fork"); exit(1);
        }

        if (!proc.p_id) {  /* child */
            // Phase 4: apply input redirection
            if (fd_in != -1) {
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            }
            if (fd_out != -1) {
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            }
            execvp(arg_inp[0], arg_inp);
            fprintf(stderr, "Error: command not found\n");
            _exit(1);
        }

        // Phase 4: background single command
        if (background) {
            bg_pids[0]  = proc.p_id;
            bg_nprocs   = 1;
            bg_command  = strdup(orig_cmd);
            if (fd_out != -1) close(fd_out);
            continue;
        }

        int retval;
        waitpid(proc.p_id, &retval, 0);
        proc.exit_status = WIFEXITED(retval)
                         ? WEXITSTATUS(retval)
                         : 1;

        // check for any unfinished background jobs before reporting
        if (bg_nprocs > 0) {
            int all_done = 1;
            int status_array[MAX_BG];
            for (int i = 0; i < bg_nprocs; i++) {
                pid_t w = waitpid(bg_pids[i], &status_array[i], WNOHANG);
                if (w <= 0) {
                    all_done = 0;
                }
            }
            if (all_done) {
                fprintf(stderr, "+ completed '%s' ", bg_command);
                for (int i = 0; i < bg_nprocs; i++) {
                    fprintf(stderr, "[%d]", WEXITSTATUS(status_array[i]));
                }
                fprintf(stderr, "\n");
                free(bg_command);
                bg_command = NULL;
                bg_nprocs = 0;
            }
        }


        fprintf(stderr, "+ completed '%s' [%d]\n",
                orig_cmd, proc.exit_status);
        if (fd_out != -1) close(fd_out);
    }
    return EXIT_SUCCESS;
}
