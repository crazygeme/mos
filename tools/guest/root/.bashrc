# .bashrc

# User specific aliases and functions

alias rm='rm -i'
alias cp='cp -i'
alias mv='mv -i'

# Source global definitions
if [ -f /etc/bashrc ]; then
	. /etc/bashrc
fi

function __promp_command(){
	local EXIT="$?"
	local RCol='\[\e[0m\]'
    	local Red='\[\e[0;31m\]'
    	local Gre='\[\e[0;32m\]'

	PS1="\[\e[32m\]\u@\h\[\e[m\] \[\e[33m\]$ \w \[\e[m\]\n"
	#PS1="$(powerline-shell $?)\n"

	if [ $EXIT != 0 ]; then
		PS1="${PS1}${Red}\$${RCol} "
	else
		PS1="${PS1}${Gre}\$${RCol} "
	fi
}

PROMPT_COMMAND=__promp_command