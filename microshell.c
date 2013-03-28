/*
 microshell.c
 ---
 Implements a basic UNIX shell. Reads a command, sequences of commands, or command pipelines.
 Allows the user to redirect I/O to and from files and execute conditional command chains.
 
 Author: Michael Falcone
 */

#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>


#define MAX_INPUT_LEN 256


#define ISWHITESPACE(c) (c == ' ' || c == '\t' || c == '\n')
#define ISCONTROLCHAR(c) (c == ';' || c == '&' || c == '|' || c == '<' || c == '>')



// define reasons for stopping during argument processing
#define SR_DONE 0
#define SR_ERROR 1
#define SR_SEQ_CHAIN 2
#define SR_SEQ_AND 3
#define SR_SEQ_OR 4
#define SR_PIPE 5
#define SR_REDIRECT_IN 6
#define SR_REDIRECT_IN_HERE 7
#define SR_REDIRECT_OUT 8
#define SR_REDIRECT_OUT_APPEND 9
#define SR_BACKGROUND 10


typedef char* arg_t;


// represents a command in a command chain
typedef struct _command{
    arg_t *argList;
    int fdIn;
    int fdOut;
    int stopOnFailure;
    int stopOnSuccess;
    int piped;
    int background;
    struct _command *next;
} command_t;




int processArgs(const char *input, int *argChars, char *argBuffer, int *argCount, arg_t *argList, int *stopReason);
int buildCommandChains(const char *input, char *argBuffer, arg_t *argList, command_t *chains);
int executeCommandChain(const command_t *chain, int *commandCount);
int executeSingleCommand(const command_t *command);
int executePipedCommands(const command_t *left, const command_t *right);



/*
 Main function. Displays a prompt and executes chains of commands entered by the user.
 */
int main(void){
    
    char input[MAX_INPUT_LEN];
    arg_t argList[MAX_INPUT_LEN];
    char argBuffer[MAX_INPUT_LEN];
    command_t commands[MAX_INPUT_LEN];
    int numCommandChains, chainSkip, chainCount, commandCount;
    int exitStatus;
    
    while(1){
        memset(input, 0, MAX_INPUT_LEN);
        memset(argList, 0, MAX_INPUT_LEN * sizeof(arg_t));
        memset(argBuffer, 0, MAX_INPUT_LEN);
        memset(commands, 0, MAX_INPUT_LEN * sizeof(command_t));
        
        
        printf(">> ");
        fgets(input, MAX_INPUT_LEN, stdin);
        
        if(strlen(input) > 0){
            
            numCommandChains = buildCommandChains(input, argBuffer, argList, commands);
            commandCount = 0;
            for (chainCount=0; chainCount < numCommandChains; ++chainCount){
                exitStatus = executeCommandChain(commands+commandCount, &chainSkip);
                commandCount += chainSkip;
            }
        }
    }
    return 0;
}




/*
 Processes the specified input as command line arguments. Returns the number of input characters
 processed in a single call to this function.
 
 Return parameters:
  *argChars - the number of charactersused by the function in the argument buffer
  *argBuffer - the buffer for storing all arguments
  *argList - the actual list representing arguments
  *stopReason - the reason for stopping processing and returning
 */
int processArgs(const char *input, int *argChars, char *argBuffer, int *argCount, arg_t *argList, int *stopReason){
    
    int escaped = 0;
    int quoted = 0;
    char quoteChar = 0;
    int processed = 0;
    const char *c = input;
    char curArg[MAX_INPUT_LEN];
    int curArgLen = 0;
    int argsOffset = 0;
    
    memset(curArg, 0, MAX_INPUT_LEN);
    *argCount = 0;
    
    while(1){
        if(*c == 0){
            *stopReason = SR_DONE;
            break;
        }
        
        if(*c == '\\' && !escaped){
            escaped = 1;
        }
        
        else if((*c == '\'' || *c == '"') && !escaped){
            if(!quoted){
                quoteChar = *c;
                quoted = 1;
            }
            else if(*c == quoteChar){
                quoted = 0;
                quoteChar = 0;
            }
        }
        
        else if((ISWHITESPACE(*c) || ISCONTROLCHAR(*c)) && !(escaped || quoted)){
                    
            if(curArgLen > 0){
                memcpy((argBuffer+argsOffset), curArg, curArgLen);
                argList[(*argCount)++] = (argBuffer+argsOffset);
                argsOffset += (curArgLen + 1);
                memset(curArg, 0, curArgLen);
                curArgLen = 0;
                
                while(ISWHITESPACE(*c)){
                    ++c;
                    ++processed;
                }
                
                if(*c == ';'){
                    *stopReason = SR_SEQ_CHAIN;
                    break;
                }
                else if(*c == '|' && *(c+1) == '|'){
                    *stopReason = SR_SEQ_OR;
                    ++processed;
                    break;
                }
                else if(*c == '|'){
                    *stopReason = SR_PIPE;
                    break;
                }
                else if(*c == '&' && *(c+1) == '&'){
                    *stopReason = SR_SEQ_AND;
                    ++processed;
                    break;
                }
                else if(*c == '&'){
                    *stopReason = SR_BACKGROUND;
                    break;
                }
                else if(*c == '>' && *(c+1) == '>'){
                    *stopReason = SR_REDIRECT_OUT_APPEND;
                    ++processed;
                    break;
                }
                else if(*c == '>'){
                    *stopReason = SR_REDIRECT_OUT;
                    break;
                }
                else if(*c == '<' && *(c+1) == '<'){
                    *stopReason = SR_REDIRECT_IN_HERE;
                    ++processed;
                    break;
                }
                else if(*c == '<'){
                    *stopReason = SR_REDIRECT_IN;
                    break;
                }
                else{
                    --c;
                    --processed;
                }
            }
            else if(ISCONTROLCHAR(*c)){
                *stopReason = SR_ERROR;
                break;
            }
            
        } 
        else{
            curArg[curArgLen++] = *c;
            escaped = 0;
        }
        
        ++c;
        ++processed;
    }
    
    ++processed; // count character that broke the loop
    *argChars = argsOffset;
    
    return processed;
}



/*
 Builds chains of commands from the given input and returns the number of chains.
 
 Return parameters:
 *argBuffer - the buffer for storing all arguments
 *argList - the array for storing the beginning of each argument
 *chains - the array of command chains
 */
int buildCommandChains(const char *input, char *argBuffer, arg_t *argList, command_t *chains){
    
    int argCount = 0, argTotal = 0, argChars = 0, argCharsTotal = 0;
    int inputPos = 0;
    int stopReason;
    int chainCount = 0;
    int commandCount = 0;
    int continueLast = 0;
    int *getFd = 0;
    int oflags = 0;
    arg_t filename;
    command_t *com, *lastCom = 0;
    
    
    do{
        if(getFd){ // the user wants to open a file for redirection
            inputPos += processArgs(input+inputPos, &argChars, argBuffer+argCharsTotal,
                                    &argCount, argList+argTotal, &stopReason);
            
            if(stopReason == SR_ERROR || argCount != 1){
                fprintf(stderr, "Error reading filename for redirect.\n");
                continue;
            }
            
            filename = *(argList+argTotal);
            argCharsTotal += argChars;
            argTotal += argCount+1;
            
            *getFd = open(filename, oflags, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IROTH);
            if(*getFd < 0){
                fprintf(stderr, "Error opening file '%s' for redirect.\n", filename);
                continue;
            }
            lseek(*getFd, 0, SEEK_SET);
            getFd = 0;
        }
        
        else{ // the user wants to execute the next command
            inputPos += processArgs(input+inputPos, &argChars, argBuffer+argCharsTotal,
                                    &argCount, argList+argTotal, &stopReason);
            
            if(stopReason == SR_ERROR && !continueLast){
                fprintf(stderr, "Unrecognized command input.\n");
                continue;
            }
            
            if(!continueLast){
                if(argCount == 0){
                    continue;
                }
                
                com = chains+commandCount++;
                com->argList = argList+argTotal;
                com->stopOnFailure = 0;
                com->stopOnSuccess = 0;
                com->background = 0;
                com->piped = 0;
                com->next = 0;
                com->fdIn = fileno(stdin);
                com->fdOut = fileno(stdout);
                
                if(lastCom){
                    lastCom->next = com;
                }
            }
            
            argCharsTotal += argChars;
            argTotal += argCount+1;
            continueLast = 0;
        }
        
        switch(stopReason){
            case SR_SEQ_AND:
                com->stopOnFailure = 1;
                lastCom = com;
                break;
                
            case SR_SEQ_OR:
                com->stopOnSuccess = 1;
                lastCom = com;
                break;
                
            case SR_PIPE:
                com->piped = 1;
                com->stopOnFailure = 1;
                lastCom = com;
                break;
                
            case SR_REDIRECT_IN:
                getFd = &(com->fdIn);
                oflags = O_RDONLY;
                break;
                
            case SR_REDIRECT_OUT:
                getFd = &(com->fdOut);
                oflags = O_WRONLY | O_CREAT | O_TRUNC;
                break;
                
            case SR_REDIRECT_OUT_APPEND:
                getFd = &(com->fdOut);
                oflags = O_WRONLY | O_CREAT | O_APPEND;
                break;
                
            case SR_BACKGROUND:
                continueLast = 1;
                com->background = 1;
                break;
                
            case SR_SEQ_CHAIN:
            default:
                ++chainCount;
                lastCom = 0;
        }
        
    } while(stopReason != SR_DONE);
    
    
    return chainCount;
}




/*
 Executes a single command chain starting with the first command in *chain. The
 parameter *commandCount returns the total number of commands in the chain, regardless
 of whether all commands were successfully executed.
 */
int executeCommandChain(const command_t *chain, int *commandCount){
    
    int status, allStatus;
    int stopped = 0;
    
    *commandCount = 0;
    
    while(chain){
        
        if(!stopped){
            
            if(chain->piped){
                status = executePipedCommands(chain, chain->next);
                if(!commandCount){
                    allStatus = status;
                }
                
                while(chain->piped){
                    ++(*commandCount);
                    chain = chain->next;
                }
            }
            else{
                status = executeSingleCommand(chain);
                if(!commandCount){
                    allStatus = status;
                }
            }
            
            if(chain->stopOnFailure){
                allStatus += abs(status); // if any has nonzero exit status, total status is nonzero
                stopped = status;
            }
            else if(chain->stopOnSuccess){
                allStatus *= status; // if any has zero exit status, total is zero
                stopped = !status;
            }
            else{
                allStatus += status;
            }
        }
        
        ++(*commandCount);
        chain = chain->next;
    }
    
    return allStatus;
}




/*
 Forks the current process to execute the specified command. Returns the exit status
 of the command, or 1 if it does not finish executing.
 */
int executeSingleCommand(const command_t *command){
    
    int exitStatus;
    pid_t pid;
    
    
    // if exit command just exit the current process without forking
    if(strcmp(command->argList[0], "exit") == 0){
        exit(0);
        return 0;
    }
    
    pid = fork();
    
    if(pid < 0){
        fprintf(stderr, "Error! Could not fork process for command '%s'.\n", command->argList[0]);
        exit(1);
    }
    else if(pid == 0){
        
        dup2(command->fdIn, fileno(stdin));
        dup2(command->fdOut, fileno(stdout));
        
        execvp(command->argList[0], command->argList);
        
        // show an error if the command was not successfully exec'd
        fprintf(stderr, "Error! The command '%s' could not be found.\n", command->argList[0]);
        exit(1);
        
    }
    else{
        waitpid(pid, &exitStatus, 0);
        
        if(command->fdIn != fileno(stdin)){
            close(command->fdIn);
        }
        if(command->fdOut != fileno(stdout)){
            close(command->fdOut);
        }
        
        exitStatus = WEXITSTATUS(exitStatus);
    }
    
    return exitStatus;
}




/*
 Recursively executes a pipeline (commands connected by a pipe), piping output from
 the left command to the right command's input. Returns a sum of the exit status
 of each command in the pipeline.
 */
int executePipedCommands(const command_t *left, const command_t *right){
    
    int exitStatus, childExitStatus;
    int commandPipe[2];
    pid_t pidRight, pidLeft;
    
    
    if(!(left->piped && right)){
        // if this is the last command in the pipeline, run it and return
        return executeSingleCommand(left);
    }
    
    
    pipe(commandPipe);
    pidRight = fork(); // fork once to execute pipeline on right
    
    if(pidRight < 0){
        fprintf(stderr, "Error! Could not fork process for command '%s'.\n", right->argList[0]);
        exit(1);
    }
    else if(pidRight == 0){
        close(commandPipe[1]);
        dup2(commandPipe[0], right->fdIn);
        exit(executePipedCommands(right, right->next));
    }
    else{
        pidLeft = fork(); // fork again to execute left command
        
        if(pidLeft < 0){
            fprintf(stderr, "Error! Could not fork process for command '%s'.\n", left->argList[0]);
            exit(1);
        }
        else if(pidLeft == 0){
            close(commandPipe[0]);
            dup2(commandPipe[1], left->fdOut);
            exit(executeSingleCommand(left));
        }
        else{
            close(commandPipe[0]);
            close(commandPipe[1]);
            
            waitpid(pidRight, &childExitStatus, 0);
            childExitStatus = WEXITSTATUS(childExitStatus);
            
            waitpid(pidLeft, &exitStatus, 0);
            exitStatus = WEXITSTATUS(exitStatus);
            
            exitStatus += WEXITSTATUS(childExitStatus);
        }
    }
    return exitStatus;
}














