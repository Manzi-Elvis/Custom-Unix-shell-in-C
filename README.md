# Custom Shell (C / Unix)

A lightweight custom shell implementation written in **C** for Linux/Unix systems.  
This project mimics the behavior of popular shells (like `bash` and `zsh`) by handling command execution, pipes, redirection, and environment variables.  

It’s a classic systems programming project that demonstrates strong knowledge of **processes, signals, memory management, and low-level OS concepts**.  

---

## Features

- Execute built-in and external commands  
- Support for **pipes** (`|`)  
- Input/output **redirection** (`>`, `<`, `>>`)  
- Environment variable expansion (`$PATH`, `$HOME`, etc.)  
- Command history (basic implementation)  
- Signal handling (`Ctrl+C`, `Ctrl+Z`)  
- Error handling and memory management  

---
## Example Usage: 
 myshell> ls -l
 myshell> echo $HOME
 myshell> cat input.txt | grep "hello" > output.txt
 myshell> ./a.out < input.txt >> log.txt
