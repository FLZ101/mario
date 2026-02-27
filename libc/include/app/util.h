#ifndef _APP_UTIL_H
#define _APP_UTIL_H

void cat(char *filename);

void run(char *filename);
void run_arg(char *filename, char **argv);
void run_arg_env(char *filename, char **argv, char **envp);

void ls(char *pathname);

#endif /* _APP_UTIL_H */
