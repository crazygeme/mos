#ifndef _PS0_H_
#define _PS0_H_

/* EFLAGS Register. */
#define FLAG_MBS  0x00000002    /* Must be set. */
#define FLAG_IF   0x00000200    /* Interrupt Flag. */



int sys_execve(const char* file, char** argv, char** envp);

#endif
