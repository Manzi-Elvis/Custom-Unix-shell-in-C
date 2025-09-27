#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TOKENS 256
#define MAX_COMMANDS 64
#define READ_END 0
#define WRITE_END 1

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("Out of memory\n");
    return p;
}

static char *xstrdup(const char *s) {
    char *d = strdup(s);
    if (!d) die("Out of memory\n");
    return d;
}

static volatile pid_t foreground_pgid = 0;

static void sigint_handler(int signo) {
    if (foreground_pgid > 0) {
        kill(-foreground_pgid, SIGINT);
    }
}

static void sigchld_handler(int signo) {
    int saved_errno = errno;
    while (true) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
    }
    errno = saved_errno;
}

typedef struct {
    char **tokens;
    int count;
} toklist_t;

static void free_toklist(toklist_t *tl) {
    if (!tl) return;
    for (int i = 0; i < tl->count; ++i) free(tl->tokens[i]);
    free(tl->tokens);
    tl->tokens = NULL;
    tl->count = 0;
}

static void toklist_append(toklist_t *tl, char *s) {
    tl->tokens = realloc(tl->tokens, sizeof(char*) * (tl->count + 1));
    tl->tokens[tl->count++] = s;
}

static toklist_t tokenize(const char *line) {
    toklist_t tl = {NULL, 0};
    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        if (*p == '|' || *p == '<' || *p == '>' || *p == '&' || *p == ';') {
            if (*p == '>' && *(p+1) == '>') {
                toklist_append(&tl, xstrdup(">>"));
                p += 2;
            } else {
                char op[2] = {*p, '\0'};
                toklist_append(&tl, xstrdup(op));
                ++p;
            }
            continue;
        }

        if (*p == '\'' || *p == '"') {
            char quote = *p++;
            const char *start = p;
            char *buf = xmalloc(strlen(p) + 1); 
            char *b = buf;
            while (*p && *p != quote) {
                if (quote == '"' && *p == '\\' && (*(p+1) == '"' || *(p+1)=='\\' || *(p+1)=='$')) {
                    ++p;
                    *b++ = *p++;
                } else {
                    *b++ = *p++;
                }
            }
            *b = '\0';
            if (*p == quote) ++p;
            toklist_append(&tl, buf);
            continue;
        }
        const char *start = p;
        char *buf = xmalloc(strlen(p) + 1);
        char *b = buf;
        while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '<' && *p != '>' && *p != '&' && *p != ';') {
            if (*p == '\\' && *(p+1) != '\0') {
                ++p;
                *b++ = *p++;
            } else {
                *b++ = *p++;
            }
        }
        *b = '\0';
        toklist_append(&tl, buf);
    }
    tl.tokens = realloc(tl.tokens, sizeof(char*) * (tl.count + 1));
    tl.tokens[tl.count] = NULL;
    return tl;
}
static char *expand_vars(const char *s) {
    size_t len = strlen(s);
    char *out = xmalloc(len * 2 + 1);
    char *o = out;
    const char *p = s;
    while (*p) {
        if (*p == '$') {
            ++p;
            if (*p == '{') {
                ++p;
                const char *start = p;
                while (*p && *p != '}') ++p;
                size_t vn = p - start;
                char *name = xmalloc(vn + 1);
                memcpy(name, start, vn);
                name[vn] = '\0';
                char *val = getenv(name);
                if (val) { strcpy(o, val); o += strlen(val); }
                free(name);
                if (*p == '}') ++p;
            } else if (isalpha((unsigned char)*p) || *p == '_') {
                const char *start = p;
                while (isalnum((unsigned char)*p) || *p == '_') ++p;
                size_t vn = p - start;
                char *name = xmalloc(vn + 1);
                memcpy(name, start, vn);
                name[vn] = '\0';
                char *val = getenv(name);
                if (val) { strcpy(o, val); o += strlen(val); }
                free(name);
            } else {
                *o++ = '$';
            }
        } else {
            *o++ = *p++;
        }
    }
    *o = '\0';
    char *res = xstrdup(out);
    free(out);
    return res;
}

typedef struct {
    char **argv;
    char *infile;
    char *outfile;
    bool append;
    bool background;
} command_t;

static void free_command(command_t *c) {
    if (!c) return;
    if (c->argv) {
        for (int i = 0; c->argv[i]; ++i) free(c->argv[i]);
        free(c->argv);
    }
    free(c->infile);
    free(c->outfile);
}

static command_t *parse_commands(toklist_t *tl, int *ncmds_out) {
    command_t *cmds = xmalloc(sizeof(command_t) * MAX_COMMANDS);
    int cmd_i = 0;
    int pos = 0;
    int tcount = tl->count;
    while (pos < tcount) {
        command_t c = {NULL, NULL, NULL, false, false};
        char **argv = NULL;
        int argc = 0;

        while (pos < tcount) {
            char *tok = tl->tokens[pos];
            if (strcmp(tok, "|") == 0) {
                pos++;
                break; 
            } else if (strcmp(tok, "<") == 0) {
                pos++;
                if (pos >= tcount) { fprintf(stderr, "Syntax error: expected infile\n"); goto parse_err; }
                c.infile = xstrdup(tl->tokens[pos++]);
            } else if (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
                bool is_append = (strcmp(tok, ">>") == 0);
                pos++;
                if (pos >= tcount) { fprintf(stderr, "Syntax error: expected outfile\n"); goto parse_err; }
                c.outfile = xstrdup(tl->tokens[pos++]);
                c.append = is_append;
            } else if (strcmp(tok, "&") == 0) {
                pos++;
                c.background = true;
            } else if (strcmp(tok, ";") == 0) {
                pos++;
                break;
            } else {

                char *expanded = expand_vars(tok);
                argv = realloc(argv, sizeof(char*) * (argc + 1 + 1));
                argv[argc++] = expanded;
                argv[argc] = NULL;
                pos++;
            }
        }

        c.argv = argv ? argv : (char**)malloc(sizeof(char*));
        c.argv[argc] = NULL;
        cmds[cmd_i++] = c;
        if (cmd_i >= MAX_COMMANDS) break;
    }

    *ncmds_out = cmd_i;
    return cmds;

parse_err:
    for (int i = 0; i < cmd_i; ++i) free_command(&cmds[i]);
    free(cmds);
    *ncmds_out = 0;
    return NULL;
}

static bool is_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0 || strcmp(cmd, "export") == 0 ||
           strcmp(cmd, "unset") == 0 || strcmp(cmd, "pwd") == 0 || strcmp(cmd, "env") == 0;
}

static int builtin_exec(char **argv) {
    if (!argv || !argv[0]) return -1;
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argv[1];
        if (!dir) dir = getenv("HOME");
        if (!dir) dir = "/";
        if (chdir(dir) != 0) {
            perror("cd");
            return -1;
        }
        return 0;
    } else if (strcmp(argv[0], "exit") == 0) {
        int code = 0;
        if (argv[1]) code = atoi(argv[1]);
        exit(code);
    } else if (strcmp(argv[0], "export") == 0) {
        if (!argv[1]) {
            fprintf(stderr, "export: usage: export NAME=VALUE\n");
            return -1;
        }
        char *pair = argv[1];
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            setenv(pair, eq+1, 1);
            *eq = '=';
        } else {
            if (argv[2]) {
                setenv(pair, argv[2], 1);
            } else {
                setenv(pair, "", 1);
            }
        }
        return 0;
    } else if (strcmp(argv[0], "unset") == 0) {
        if (!argv[1]) {
            fprintf(stderr, "unset: usage: unset NAME\n");
            return -1;
        }
        unsetenv(argv[1]);
        return 0;
    } else if (strcmp(argv[0], "pwd") == 0) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            printf("%s\n", cwd);
            return 0;
        } else {
            perror("pwd");
            return -1;
        }
    } else if (strcmp(argv[0], "env") == 0) {
        extern char **environ;
        for (char **e = environ; *e; ++e) puts(*e);
        return 0;
    }
    return -1;
}

static void child_set_signals_default(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);

}

static void execute_pipeline(command_t *cmds, int ncmds) {
    if (ncmds <= 0) return;

    if (ncmds == 1 && cmds[0].argv && cmds[0].argv[0] && is_builtin(cmds[0].argv[0]) &&
        !cmds[0].infile && !cmds[0].outfile && !cmds[0].background) {
        builtin_exec(cmds[0].argv);
        return;
    }

    int pipes[MAX_COMMANDS-1][2];
    for (int i = 0; i < ncmds-1; ++i) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            return;
        }
    }

    pid_t pids[MAX_COMMANDS];
    pid_t pgid = 0;

    for (int i = 0; i < ncmds; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            for (int j = 0; j < i; ++j) kill(pids[j], SIGKILL);
            return;
        }
        if (pid == 0) {
            if (i == 0) {
                pgid = getpid();
                if (setpgid(0, pgid) == -1) perror("setpgid");
            } else {
                if (setpgid(0, pgid) == -1) perror("setpgid");
            }
            child_set_signals_default();
          
            if (i == 0 && cmds[i].infile) {
                int fd = open(cmds[i].infile, O_RDONLY);
                if (fd < 0) { perror("open infile"); exit(EXIT_FAILURE); }
                if (dup2(fd, STDIN_FILENO) == -1) { perror("dup2 infile"); exit(EXIT_FAILURE); }
                close(fd);
            }
            if (i == ncmds-1 && cmds[i].outfile) {
                int flags = O_CREAT | O_WRONLY | (cmds[i].append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].outfile, flags, 0644);
                if (fd < 0) { perror("open outfile"); exit(EXIT_FAILURE); }
                if (dup2(fd, STDOUT_FILENO) == -1) { perror("dup2 outfile"); exit(EXIT_FAILURE); }
                close(fd);
            }

            if (i > 0) {
                if (dup2(pipes[i-1][READ_END], STDIN_FILENO) == -1) { perror("dup2 pipe in"); exit(EXIT_FAILURE); }
            }
            if (i < ncmds-1) {
                if (dup2(pipes[i][WRITE_END], STDOUT_FILENO) == -1) { perror("dup2 pipe out"); exit(EXIT_FAILURE); }
            }
            for (int j = 0; j < ncmds-1; ++j) {
                close(pipes[j][READ_END]);
                close(pipes[j][WRITE_END]);
            }
            if (cmds[i].argv && cmds[i].argv[0] && is_builtin(cmds[i].argv[0])) {
                builtin_exec(cmds[i].argv);
                exit(EXIT_SUCCESS);
            }
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "mini_shell: command not found: %s\n", cmds[i].argv[0]);
            exit(127);
        } else {
            pids[i] = pid;
            if (i == 0) {
                pgid = pid;
                if (setpgid(pid, pgid) == -1) perror("setpgid parent");
            } else {
                if (setpgid(pid, pgid) == -1) perror("setpgid parent");
            }
        }
    }

    for (int j = 0; j < ncmds-1; ++j) {
        close(pipes[j][READ_END]);
        close(pipes[j][WRITE_END]);
    }

    bool any_background = false;
    for (int i = 0; i < ncmds; ++i) if (cmds[i].background) any_background = true;

    if (!any_background) {
        foreground_pgid = pgid;
        eline
        int status;
        for (int i = 0; i < ncmds; ++i) {
            pid_t w;
            do {
                w = waitpid(pids[i], &status, 0);
            } while (w == -1 && errno == EINTR);
        }
        foreground_pgid = 0;
    } else {
        printf("[bg] pid %d\n", pgid);
    }
}

static char *prompt = "mini-shell$ ";

int main(int argc, char **argv) {
    struct sigaction sa_int, sa_chld;
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa_int, NULL) == -1) perror("sigaction SIGINT");

    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) perror("sigaction SIGCHLD");
    char *line = NULL;
    size_t n = 0;
    while (true) {
        if (isatty(STDIN_FILENO)) {
            printf("%s", prompt);
            fflush(stdout);
        }

        ssize_t r = getline(&line, &n, stdin);
        if (r == -1) {
            if (feof(stdin)) {
                printf("\n");
                break;
            } else if (errno == EINTR) {
                clearerr(stdin);
                continue;
            } else {
                perror("getline");
                break;
            }
        }
        if (r > 0 && line[r-1] == '\n') line[r-1] = '\0';
        char *p = line;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) continue;
        toklist_t tl = tokenize(line);
        if (tl.count == 0) { free_toklist(&tl); continue; }
        int ncmds = 0;
        command_t *cmds = parse_commands(&tl, &ncmds);
        if (!cmds) { free_toklist(&tl); continue; }
        execute_pipeline(cmds, ncmds);
      
        for (int i = 0; i < ncmds; ++i) free_command(&cmds[i]);
        free(cmds);
        free_toklist(&tl);
    }

    free(line);
    return 0;
}
