#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_ARGS 5
#define MAX_COMMAND_LEN 1024
#define MAX_PIPES 5 // Maximum number of pipes for piping operations

int prev_status = 0; // Global variable to store the previous command's exit status
pid_t last_background_pid = -1; // Global variable to store the last background PID

// Function declarations
void execute_command(char *args[], int background, char *input_file, char *output_file, int append_mode);
void parse_command(char *command);
void create_new_shell();
void redirect_output(char *filename);
void redirect_output_append(char *filename);
void pipe_commands(char *args[], int arg_count, int pipe_pos[], int pipe_count, char *input_file, char *output_file, int append_mode);
void execute_conditional_commands(char *args[], int arg_count);
void execute_background_command(char *args[], int arg_count);
void execute_command_with_redirection(char *args[], char *filename, char *heredoc_delimiter, int background);
void concatenate_files(char *files[], int file_count);
void redirect_input(char *filename);
int validate_arg_count(int arg_count);
void bring_background_process_to_foreground(void);

// Validate the argument count
int validate_arg_count(int arg_count) {
    return arg_count >= 1 && arg_count <= MAX_ARGS;
}

// Function to redirect input from a file
void redirect_input(char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Error: Failed to open input file '%s'\n", filename);
        return;
    }
    if (dup2(fd, STDIN_FILENO) == -1) {
        perror("Failed to redirect input");
        return;
    }
    close(fd);
}

// Function to redirect output to a file
void redirect_output(char *filename) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        fprintf(stderr, "Error: Failed to open output file '%s'\n", filename);
        return;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) {
        perror("Failed to redirect output");
        return;
    }
    close(fd);
}

// Function to redirect output to a file in append mode
void redirect_output_append(char *filename) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        fprintf(stderr, "Error: Failed to open output file '%s' for append\n", filename);
        return;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) {
        perror("Failed to redirect output for append");
        return;
    }
    close(fd);
}

// Function to parse and execute the shell command
void parse_command(char *command) {
    command[strcspn(command, "\n")] = '\0'; // Remove trailing newline character
    if (command[0] == '$') command++; // Skip the prompt if it starts with $

    // Check for the 'fg' command to bring the last background process to the foreground
    if (strcmp(command, "fg") == 0) {
        bring_background_process_to_foreground();
        return; // Skip further processing
    }

    char *segments[5]; // Up to 5 commands as per your requirement
    int numSegments = 0;
    char *segment = strtok(command, ";");
    while (segment != NULL && numSegments < 5) {
        segments[numSegments++] = segment;
        segment = strtok(NULL, ";");
    }

    // Execute each command segment
    for (int i = 0; i < numSegments; i++) {
        char *tempCommand = segments[i];
        while (*tempCommand == ' ') tempCommand++; // Trim leading space

        char *args[MAX_ARGS + 2] = {NULL};
        char *input_file = NULL, *output_file = NULL;
        int append_mode = 0, background = 0;
        int arg_count = 0, file_count = 0;
        char *file_list[MAX_ARGS] = {NULL};
        int pipe_pos[MAX_PIPES], pipe_count = 0;
        int isConditionalCommand = 0;

        // Tokenize the command segment
        for (char *token = strtok(tempCommand, " "); token != NULL; token = strtok(NULL, " ")) {
            if (strcmp(token, "&") == 0) {
                background = 1; // Set background execution flag
            } else if (strcmp(token, "|") == 0) {
                if (pipe_count < MAX_PIPES) {
                    pipe_pos[pipe_count++] = arg_count; // Record pipe position
                    args[arg_count++] = NULL; // Null-terminate command segment
                } else {
                    fprintf(stderr, "Error: Exceeds maximum piping operations\n");
                    return;
                }
            } else if (strcmp(token, ">") == 0) {
                output_file = strtok(NULL, " "); // Next token is the output file
                append_mode = 0; // Overwrite mode
            } else if (strcmp(token, ">>") == 0) {
                output_file = strtok(NULL, " "); // Next token is the output file
                append_mode = 1; // Append mode
            } else if (strcmp(token, "<") == 0) {
                input_file = strtok(NULL, " "); // Next token is the input file
            } else if (token[0] == '#' && strlen(token) > 1) {
                // Handle file concatenation
                file_list[file_count++] = token + 1; // Skip '#' and add to file list
            } else if (strcmp(token, "newt") == 0) {
                create_new_shell();
                return; // Return early since we don't need to process further
            } else if (strcmp(token, "&&") == 0 || strcmp(token, "||") == 0) {
                isConditionalCommand = 1; // Flag indicating a conditional command is present
                args[arg_count++] = token; // Include the conditional operator in the arguments
            } else {
                args[arg_count++] = token; // Regular command argument
            }
        }

        args[arg_count] = NULL; // Null-terminate the args array

        if (isConditionalCommand) {
            // Process conditional commands without immediate arg count validation
            char *conditional_args[MAX_ARGS + 2];
            int conditional_arg_count = 0, start = 0;

            // Execute conditional commands based on previous command's exit status
            for (int i = 0; i <= arg_count; i++) {
                if (args[i] == NULL || strcmp(args[i], "&&") == 0 || strcmp(args[i], "||") == 0) {
                    if (!validate_arg_count(i - start)) {
                        fprintf(stderr, "Error: Argument count for segment must be between 1 and %d\n", MAX_ARGS);
                        return; // Skip execution of this segment
                                            }

                    // Copy arguments for the current segment
                    memcpy(conditional_args, args + start, (i - start) * sizeof(char *));
                    conditional_arg_count = i - start;
                    conditional_args[conditional_arg_count] = NULL; // Null-terminate the segment

                    // Execute the command segment
                    execute_command(conditional_args, background, input_file, output_file, append_mode);
                    start = i + 1; // Update for the next segment
                }
            }
        } else if (file_count > 0) {
            // Concatenate files if '#' token is found
            concatenate_files(file_list, file_count);
        } else if (pipe_count > 0) {
            // Execute commands with piping
            pipe_commands(args, arg_count, pipe_pos, pipe_count, input_file, output_file, append_mode);
        } else {
            // For non-conditional commands, validate arg count here
            if (!validate_arg_count(arg_count)) {
                fprintf(stderr, "Error: Argument count must be between 1 and %d\n", MAX_ARGS);
                return;
            }
            // Execute single command without piping or conditional statements
            execute_command(args, background, input_file, output_file, append_mode);
        }
    }
}

// Function to execute command with input/output redirection or heredoc
void execute_command_with_redirection(char *args[], char *filename, char *heredoc_delimiter, int background) {
    if (heredoc_delimiter != NULL) {
        // Handle heredoc mode
        char *line = NULL;
        size_t len = 0;
        FILE *heredoc_file = fopen(filename, "w");
        if (heredoc_file == NULL) {
            fprintf(stderr, "Error: Failed to open heredoc file '%s'\n", filename);
            return;
        }
        printf("Heredoc mode. Enter '%s' on a line by itself to end.\n", heredoc_delimiter);
        while (getline(&line, &len, stdin) != -1) {
            if (strcmp(line, heredoc_delimiter) == 0) {
                break;
            }
            fprintf(heredoc_file, "%s", line);
        }
        fclose(heredoc_file);
        free(line);
        redirect_input(filename); // Redirect input to the heredoc file
    }
    execute_command(args, background, NULL, NULL, 0); // Execute the command
}

// Function to create a new shell instance
void create_new_shell() {
    pid_t pid = fork();
    if (pid == -1) {
        perror("Fork failed");
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        // Child process
        execlp("gnome-terminal", "gnome-terminal", "-e", "/home/meghna/Documents/ASP-Assignment-3/shell_a3", NULL);
        perror("Exec failed");
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        int status;
        waitpid(pid, &status, 0);
    }
}

// Function to concatenate files and print to stdout
void concatenate_files(char *files[], int file_count) {
    for (int i = 0; i < file_count; ++i) {
        FILE *src = fopen(files[i], "r");
        if (src == NULL) {
            fprintf(stderr, "Error: Failed to open file '%s'\n", files[i]);
            return;
        }
        char buffer[1024];
        size_t bytes_read;
        while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
            fwrite(buffer, 1, bytes_read, stdout); // Write directly to stdout instead of appending to a file
        }
        fclose(src);
    }
}

// Function to handle piping between commands
void pipe_commands(char *args[], int arg_count, int pipe_pos[], int pipe_count, char *input_file, char *output_file, int append_mode) {
    int pipes[2 * pipe_count];
    pid_t pids[pipe_count + 1];

    // Create pipes
    for (int i = 0; i < pipe_count; ++i) {
        if (pipe(pipes + i * 2) != 0) {
            perror("Creating pipe");
            exit(EXIT_FAILURE);
        }
    }

    // Create child processes for each segment of the pipe
    for (int i = 0; i <= pipe_count; ++i) {
        pids[i] = fork();
        if (pids[i] == 0) { // Child process
            // Redirect input from the previous segment
            if (i != 0) {
                if (dup2(pipes[(i - 1) * 2], STDIN_FILENO) < 0) {
                    perror("dup2 input");
                    exit(EXIT_FAILURE);
                }
            } else if (input_file != NULL) {
                // Handle input redirection for the first command
                int in_fd = open(input_file, O_RDONLY);
                if (in_fd < 0) {
                    perror("Opening input file");
                    exit(EXIT_FAILURE);
                }
                if (dup2(in_fd, STDIN_FILENO) < 0) {
                    perror("dup2 input file");
                    exit(EXIT_FAILURE);
                }
                close(in_fd);
            }

            // Redirect output to the next segment
            if (i != pipe_count) {
                if (dup2(pipes[i * 2 + 1], STDOUT_FILENO) < 0) {
                    perror("dup2 output");
                    exit(EXIT_FAILURE);
                }
            } else if (output_file != NULL) {
                // Handle output redirection for the last command
                int out_fd = open(output_file, O_WRONLY | O_CREAT | (append_mode ? O_APPEND : O_TRUNC), 0644);
                if (out_fd < 0) {
                    perror("Opening output file");
                    exit(EXIT_FAILURE);
                }
                if (dup2(out_fd, STDOUT_FILENO) < 0) {
                    perror("dup2 output file");
                    exit(EXIT_FAILURE);
                }
                close(out_fd);
            }

            // Close all pipes in the child process
            for (int j = 0; j < 2 * pipe_count; ++j) {
                close(pipes[j]);
            }

            // Calculate the command segment to execute
            int start = 0;
            if (i > 0) {
                start = pipe_pos[i - 1] + 1;
            }
            char **cmd_segment = &args[start];

            // Execute the command segment
            if (execvp(cmd_segment[0], cmd_segment) == -1) {
                perror("execvp failed");
                exit(EXIT_FAILURE);
            }
        } else if (pids[i] < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
    }

    // Close all pipes in the parent
    for (int i = 0; i < 2 * pipe_count; ++i) {
        close(pipes[i]);
    }

    // Wait for all child processes to complete
    for (int i = 0; i <= pipe_count; ++i) {
        waitpid(pids[i], NULL, 0);

    }
}

// Function to bring a background process to the foreground
void bring_background_process_to_foreground() {
    if (last_background_pid == -1) {
        printf("No background process to bring to foreground.\n");
        return;
    }

    // Send SIGCONT signal to the process to continue if stopped
    kill(last_background_pid, SIGCONT);

    int status;
    // Wait for the background process to finish
    waitpid(last_background_pid, &status, WUNTRACED);
    prev_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    // Indicate that the process has been brought to the foreground and is now complete
    printf("Process  with PID %d is in foreground now.\n", last_background_pid);

    // Reset last_background_pid as it's now been handled
    last_background_pid = -1;
}

// Function to execute a single command with or without input/output redirection
void execute_command(char *args[], int background, char *input_file, char *output_file, int append_mode) {
    if (args[0] == NULL) {
        return; // Empty command, do nothing
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("Fork failed");
        return;
    } else if (pid == 0) { // Child process
        // Redirection handling
        if (input_file != NULL) {
            int in_fd = open(input_file, O_RDONLY);
            if (in_fd == -1) {
                perror("Error opening input file");
                exit(EXIT_FAILURE);
            }
            dup2(in_fd, STDIN_FILENO); // Redirect stdin
            close(in_fd);
        }
        if (output_file != NULL) {
            int flags = O_WRONLY | O_CREAT | (append_mode ? O_APPEND : O_TRUNC);
            int out_fd = open(output_file, flags, 0644);
            if (out_fd == -1) {
                perror("Error opening output file");
                exit(EXIT_FAILURE);
            }
            dup2(out_fd, STDOUT_FILENO); // Redirect stdout
            close(out_fd);
        }

        // Execute the command
        if (execvp(args[0], args) == -1) {
            perror("Execvp failed");
            exit(EXIT_FAILURE);
        }
    } else { // Parent process
        if (!background) {
            int status;
            waitpid(pid, &status, 0); // Wait for the command to complete
            prev_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1; // Store exit status
        } else {
            printf("Command executed in background with PID %d\n", pid);
            last_background_pid = pid; // Update the last background PID
        }
    }
}

// Function to execute a command in the background
void execute_background_command(char *args[], int arg_count) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("Fork failed");
        return;
    } else if (pid == 0) { // Child process
        setpgid(0, 0); // Ensure the child runs in a new process group
        execute_command(args, 1, NULL, NULL, 0); // Execute the command in the background without redirection
        exit(EXIT_SUCCESS);
    } else { // Parent process
        printf("Background process with PID %d started\n", pid);
        // No wait here for background process
    }
}

// Function to execute conditional commands (commands separated by '&&' or '||')
void execute_conditional_commands(char *args[], int arg_count) {
    int execute_next = 1; // Flag to indicate whether to execute the next command
    char *segment_args[MAX_ARGS + 1]; // Temporary storage for command arguments within a segment
    int segment_arg_count = 0; // Counter for the number of arguments in the current segment

    for (int i = 0; i <= arg_count; i++) {
        // When reaching the end of a segment or the command list
        if (args[i] == NULL || strcmp(args[i], "&&") == 0 || strcmp(args[i], "||") == 0) {
            if (segment_arg_count > 0) { // Ensure there's at least one command to execute
                if (execute_next) {
                    // Null-terminate the segment arguments
                    segment_args[segment_arg_count] = NULL;

                    // Execute the current command segment
                    execute_command(segment_args, 0, NULL, NULL, 0);

                    // Update execution decision for next segment based on previous command's result
                    execute_next = (prev_status == 0); // If previous command succeeded, execute the next one
                }
                segment_arg_count = 0; // Reset for the next segment
            }
        } else {
            // Collect arguments for the current segment
            if (segment_arg_count < MAX_ARGS) {
                segment_args[segment_arg_count++] = args[i];
            } else {
                fprintf(stderr, "Error: Argument count for segment must be between 1 and %d\n", MAX_ARGS);
                break; // Exit on argument count violation
            }
        }
    }
}

// Main function
int main() {
    char command[MAX_COMMAND_LEN];
    while (1) {
        printf("$ "); // Print shell prompt
        fgets(command, sizeof(command), stdin); // Read user input
        if (strcmp(command, "exit\n") == 0) {
            break; // Exit loop if user inputs 'exit'
        }
        command[strcspn(command, "\n")] = 0; // Remove newline character
        parse_command(command); // Parse and execute the command
    }
    return 0;
}
