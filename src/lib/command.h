#ifndef MOS_COMMAND_H
#define MOS_COMMAND_H

/* Linux ioctl commands are indexed by their type and number bytes. */
typedef int (*command_fn)(void *context, unsigned command, void *argument);
typedef struct {
	unsigned command;
	command_fn invoke;
} command_operation;

static inline command_fn
command_lookup(const command_operation *const groups[256], unsigned command)
{
	const command_operation *group = groups[(command >> 8) & 255];
	if (!group)
		return 0;
	const command_operation *operation = &group[command & 255];
	if (operation->command != command)
		return 0;
	return operation->invoke;
}

static inline int command_dispatch(const command_operation *const groups[256],
				   void *context, unsigned command,
				   void *argument, int unsupported)
{
	command_fn invoke = command_lookup(groups, command);
	return invoke ? invoke(context, command, argument) : unsupported;
}

#endif
