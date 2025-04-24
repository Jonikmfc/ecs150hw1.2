# The makefile
# default compiliation in gcc using the 3 core parts required for 1.2. -o sshell so its binary and output is ./sshell
all:
	gcc -Wall -Wextra -Werror sshell.c -o sshell
# deletes the shell program for a rebuild if needed 
# -f hides errors
clean:
	rm -f sshell

