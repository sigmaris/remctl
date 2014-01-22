/*
 * Running commands.
 *
 * These are the functions for running external commands under remctld and
 * calling the appropriate protocol functions to deal with the output.
 *
 * Written by Russ Allbery <eagle@eyrie.org>
 * Based on work by Anton Ushakov
 * Copyright 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2012, 2013,
 *     2014 The Board of Trustees of the Leland Stanford Junior University
 *
 * See LICENSE for licensing terms.
 */

#include <config.h>
#include <portable/event.h>
#include <portable/system.h>
#include <portable/uio.h>

#include <fcntl.h>
#include <grp.h>
#include <sys/wait.h>

#include <server/internal.h>
#include <util/fdflag.h>
#include <util/macros.h>
#include <util/messages.h>
#include <util/protocol.h>
#include <util/xmalloc.h>

/*
 * Data structure used to hold details about a running process.  The events we
 * hook into the event loop are also stored here so that the event handlers
 * can use this as their data and have their pointers so that they can remove
 * themselves when needed.
 */
struct process {
    struct event_base *loop;    /* Event base for the process event loop. */
    struct bufferevent *inout;  /* Input and output from process. */
    struct bufferevent *err;    /* Standard error from process. */
    struct event *sigchld;      /* Handle the SIGCHLD signal for exit. */
    struct evbuffer *output;    /* Buffer of output from process. */
    struct client *client;      /* Pointer to corresponding remctl client. */
    socket_type stdinout_fd;    /* File descriptor for input and output. */
    socket_type stderr_fd;      /* File descriptor for standard error. */
    struct iovec *input;        /* Data to pass on standard input. */
    pid_t pid;                  /* Process ID of child. */
    int status;                 /* Exit status. */
    bool reaped;                /* Whether we've reaped the process. */
    bool saw_output;            /* Whether we saw process output. */
};


/*
 * Callback when all stdin data has been sent.  We only have a callback to
 * shut down our end of the socketpair so that the process gets EOF on its
 * next read.
 */
static void
handle_input_end(struct bufferevent *bev, void *data)
{
    struct process *process = data;

    bufferevent_disable(bev, EV_WRITE);
    if (shutdown(process->stdinout_fd, SHUT_WR) < 0)
        sysdie("cannot shut down input side of process socket pair");
}


/*
 * Callback used to handle output from a process (protocol version two or
 * later).  We use the same handler for both standard output and standard
 * error and check the bufferevent to determine which stream we're seeing.
 *
 * When called, note that we saw some output, which is a flag to continue
 * processing when running the event loop after the child has exited.
 */
static void
handle_output(struct bufferevent *bev, void *data)
{
    int stream;
    struct evbuffer *buf;
    struct process *process = data;

    process->saw_output = true;
    stream = (bev == process->inout) ? 1 : 2;
    buf = bufferevent_get_input(bev);
    if (!server_v2_send_output(process->client, stream, buf))
        event_base_loopbreak(process->loop);
}


/*
 * Discard all data in the evbuffer.  This handler is used with protocol
 * version one when we've already read as much data as we can return to the
 * remctl client.
 */
static void
handle_output_discard(struct bufferevent *bev, void *data UNUSED)
{
    size_t length;
    struct evbuffer *buf;

    buf = bufferevent_get_input(bev);
    length = evbuffer_get_length(buf);
    if (evbuffer_drain(buf, length) < 0)
        sysdie("internal error: cannot discard extra output");
}


/*
 * Callback used to handle filling the output buffer with protocol version
 * one.  When this happens, we pull all of the data out into a separate
 * evbuffer and then change our read callback to handle_output_discard, which
 * just drains (discards) all subsequent data from the process.
 */
static void
handle_output_full(struct bufferevent *bev, void *data)
{
    struct process *process = data;
    bufferevent_data_cb writecb;

    process->output = evbuffer_new();
    if (process->output == NULL)
        sysdie("internal error: cannot create discard evbuffer");
    if (bufferevent_read_buffer(bev, process->output) < 0)
        die("internal error: cannot read data from output buffer");

    /*
     * Change the output callback.  We need to be sure not to dump our input
     * callback if it exists.
     */
    writecb = (process->input == NULL) ? NULL : handle_input_end;
    bufferevent_setcb(bev, handle_output_discard, writecb, NULL, data);
}


/*
 * Callback for events in input or output handling.  This means either an
 * error or EOF.  On EOF or an EPIPE error on write, just deactivate the
 * bufferevent.  On error, send an error message to the client and then break
 * out of the event loop.
 */
static void
handle_io_event(struct bufferevent *bev, short events, void *data)
{
    struct process *process = data;
    struct client *client = process->client;

    /* Check for EOF, after which we should stop trying to listen. */
    if (events & BEV_EVENT_EOF) {
        bufferevent_disable(bev, EV_READ);
        return;
    }

    /*
     * If we get ECONNRESET or EPIPE, the client went away without bothering
     * to read our data.  This is the same as EOF except that we should also
     * stop trying to write data.
     */
    if ((events & BEV_EVENT_ERROR))
        if (socket_errno == ECONNRESET || socket_errno == EPIPE) {
            bufferevent_disable(bev, EV_READ | EV_WRITE);
            return;
        }

    /* Everything else is some sort of error. */
    if (events & BEV_EVENT_READING)
        syswarn("read from process failed");
    else
        syswarn("write to standard input failed");
    server_send_error(client, ERROR_INTERNAL, "Internal failure");
    event_base_loopbreak(process->loop);
}


/*
 * Called when the process has exited.  Here we reap the status and then tell
 * the event loop to complete.  Ignore SIGCHLD if our child process wasn't the
 * one that exited.
 */
static void
handle_child_exit(evutil_socket_t sig UNUSED, short what UNUSED, void *data)
{
    struct process *process = data;

    if (waitpid(process->pid, &process->status, WNOHANG) > 0) {
        process->reaped = true;
        event_del(process->sigchld);
        event_base_loopexit(process->loop, NULL);
    }
}


/*
 * Processes the input to and output from an external program.  Takes the
 * client struct and a struct representing the running process.  Feeds input
 * data to the process on standard input and reads from all the streams as
 * output is available, stopping when they all reach EOF.
 *
 * For protocol v2 and higher, we can send the output immediately as we get
 * it.  For protocol v1, we instead accumulate the output in the buffer stored
 * in our client struct, and will send it out later in conjunction with the
 * exit status.
 *
 * Returns true on success, false on failure.
 */
static int
server_process_output(struct client *client, struct process *process)
{
    struct event_base *loop;
    bufferevent_data_cb writecb = NULL;
    bool success;

    /* Create the event base that we use for the event loop. */
    loop = event_base_new();
    process->loop = loop;

    /*
     * Set up a bufferevent to consume output from the process.
     *
     * There are two possibilities here.  For protocol version two, we use two
     * bufferevents, one for standard input and output and one for standard
     * error, that turn each chunk of data into a MESSAGE_OUTPUT token to the
     * client.  For protocol version one, we use a single bufferevent, which
     * sends standard intput and collects both standard output and standard
     * error, queuing it to send on process exit.  In this case, stdinout_fd
     * gets both streams (set up by server_exec).
     */
    process->inout = bufferevent_socket_new(loop, process->stdinout_fd, 0);
    if (process->inout == NULL)
        sysdie("internal error: cannot create stdin/stdout bufferevent");
    if (process->input == NULL)
        bufferevent_enable(process->inout, EV_READ);
    else {
        writecb = handle_input_end;
        bufferevent_enable(process->inout, EV_READ | EV_WRITE);
        if (bufferevent_write(process->inout, process->input->iov_base,
                              process->input->iov_len) < 0)
            sysdie("cannot queue input for process");
    }
    if (client->protocol == 1) {
        bufferevent_setcb(process->inout, handle_output_full, writecb,
                          handle_io_event, process);
        bufferevent_setwatermark(process->inout, EV_READ, TOKEN_MAX_OUTPUT_V1,
                                 TOKEN_MAX_OUTPUT_V1);
    } else {
        bufferevent_setcb(process->inout, handle_output, writecb,
                          handle_io_event, process);
        bufferevent_setwatermark(process->inout, EV_READ, 0, TOKEN_MAX_OUTPUT);
        process->err = bufferevent_socket_new(loop, process->stderr_fd, 0);
        if (process->err == NULL)
            sysdie("internal error: cannot create stderr bufferevent");
        bufferevent_enable(process->err, EV_READ);
        bufferevent_setcb(process->err, handle_output, NULL,
                          handle_io_event, process);
        bufferevent_setwatermark(process->err, EV_READ, 0, TOKEN_MAX_OUTPUT);
    }

    /* Create the event to handle SIGCHLD when the child process exits. */
    process->sigchld = evsignal_new(loop, SIGCHLD, handle_child_exit,
                                    process);
    if (event_add(process->sigchld, NULL) < 0)
        die("internal error: cannot add SIGCHLD processing event");

    /*
     * Run the event loop.  This will continue until handle_child_exit is
     * called, unless we encounter some fatal error.  If handle_child_exit was
     * successfully called, process->reaped will be set to true.
     */
    if (event_base_dispatch(loop) < 0)
        die("internal error: process event loop failed");
    if (event_base_got_break(loop))
        return false;

    /*
     * We cannot simply decide the child is done as soon as we get an exit
     * status since we may still have buffered output from the child sitting
     * in system buffers.  Therefore, we now repeatedly run the event loop in
     * EVLOOP_NONBLOCK mode, only continuing if process->saw_output remains
     * true, indicating we processed some output from the process.
     */
    do {
        process->saw_output = false;
        if (event_base_loop(loop, EVLOOP_NONBLOCK) < 0)
            die("internal error: process event loop failed");
    } while (process->saw_output && !event_base_got_break(loop));

    /*
     * For protocol version one, if the process sent more than the max output,
     * we already pulled out the output we care about into process->output.
     * Otherwise, we need to pull the output from the bufferevent before we
     * free it.
     */
    if (client->protocol == 1 && process->output == NULL) {
        process->output = evbuffer_new();
        if (process->output == NULL)
            die("internal error: cannot create output buffer");
        if (bufferevent_read_buffer(process->inout, process->output) < 0)
            die("internal error: cannot read data from output buffer");
    }

    /* Free resources and return. */
    success = !event_base_got_break(loop);
    bufferevent_free(process->inout);
    if (process->err != NULL)
        bufferevent_free(process->err);
    event_free(process->sigchld);
    event_base_free(loop);
    return success;
}


/*
 * Given a configuration line, a command, and a subcommand, return true if
 * that command and subcommand match that configuration line and false
 * otherwise.  Handles the ALL and NULL special cases.
 *
 * Empty commands are not yet supported by the rest of the code, but this
 * function copes in case that changes later.
 */
static bool
line_matches(struct confline *cline, const char *command,
             const char *subcommand)
{
    bool okay = false;

    if (strcmp(cline->command, "ALL") == 0)
        okay = true;
    if (command != NULL && strcmp(cline->command, command) == 0)
        okay = true;
    if (command == NULL && strcmp(cline->command, "EMPTY") == 0)
        okay = true;
    if (okay) {
        if (strcmp(cline->subcommand, "ALL") == 0)
            return true;
        if (subcommand != NULL && strcmp(cline->subcommand, subcommand) == 0)
            return true;
        if (subcommand == NULL && strcmp(cline->subcommand, "EMPTY") == 0)
            return true;
    }
    return false;
}


/*
 * Look up the matching configuration line for a command and subcommand.
 * Takes the configuration, a command, and a subcommand to match against
 * Returns the matching config line or NULL if none match.
 */
static struct confline *
find_config_line(struct config *config, char *command, char *subcommand)
{
    size_t i;

    for (i = 0; i < config->count; i++)
        if (line_matches(config->rules[i], command, subcommand))
            return config->rules[i];
    return NULL;
}


/*
 * Called on fatal errors in the child process before exec.  This callback
 * exists only to change the exit status for fatal internal errors to -1
 * instead of the default of 1.
 */
static int
child_die_handler(void)
{
    return -1;
}


/*
 * Runs a given command via exec.  This forks a child process, sets
 * environment and changes ownership if needed, then runs the command and
 * sends the output back to the remctl client.
 *
 * Takes the client, the short name for the command, an argument list, the
 * configuration line for that command, and the process.  Returns true on
 * success and false on failure.
 */
static bool
server_exec(struct client *client, char *command, char **req_argv,
            struct confline *cline, struct process *process)
{
    socket_type stdinout_fds[2] = { INVALID_SOCKET, INVALID_SOCKET };
    socket_type stderr_fds[2]   = { INVALID_SOCKET, INVALID_SOCKET };
    socket_type fd;
    bool ok = false;

    /*
     * Socket pairs are used for communication with the child process that
     * actually runs the command.  We have to use sockets rather than pipes
     * because libevent's buffevents require sockets.
     *
     * For protocol version one, we can use one socket pair for eerything,
     * since we don't distinguish between streams.  For protocol version two,
     * we use one socket pair for standard intput and standard output, and a
     * separate read-only one for standard error so that we can keep the
     * stream separate.
     */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stdinout_fds) < 0) {
        syswarn("cannot create stdin and stdout socket pair");
        server_send_error(client, ERROR_INTERNAL, "Internal failure");
        goto done;
    }
    if (client->protocol > 1)
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, stderr_fds) < 0) {
            syswarn("cannot create stderr socket pair");
            server_send_error(client, ERROR_INTERNAL, "Internal failure");
            goto done;
        }

    /*
     * Flush output before forking, mostly in case -S was given and we've
     * therefore been writing log messages to standard output that may not
     * have been flushed yet.
     */
    fflush(stdout);
    process->pid = fork();
    switch (process->pid) {
    case -1:
        syswarn("cannot fork");
        server_send_error(client, ERROR_INTERNAL, "Internal failure");
        goto done;

    /* In the child. */
    case 0:
        message_fatal_cleanup = child_die_handler;

        /* Close the server sides of the sockets. */
        close(stdinout_fds[0]);
        stdinout_fds[0] = INVALID_SOCKET;
        if (stderr_fds[0] != INVALID_SOCKET) {
            close(stderr_fds[0]);
            stderr_fds[0] = INVALID_SOCKET;
        }

        /*
         * Set up stdin if we have input data.  If we don't have input data,
         * reopen on /dev/null instead so that the process gets immediate EOF.
         * Ignore failure here, since it probably won't matter and worst case
         * is that we leave stdin closed.
         */
        if (process->input != NULL)
            dup2(stdinout_fds[1], 0);
        else {
            close(0);
            fd = open("/dev/null", O_RDONLY);
            if (fd > 0) {
                dup2(fd, 0);
                close(fd);
            }
        }

        /* Set up stdout and stderr. */
        dup2(stdinout_fds[1], 1);
        if (client->protocol == 1)
            dup2(stdinout_fds[1], 2);
        else {
            dup2(stderr_fds[1], 2);
            close(stderr_fds[1]);
        }
        close(stdinout_fds[1]);

        /*
         * Older versions of MIT Kerberos left the replay cache file open
         * across exec.  Newer versions correctly set it close-on-exec, but
         * close our low-numbered file descriptors anyway for older versions.
         * We're just trying to get the replay cache, so we don't have to go
         * very high.
         */
        for (fd = 3; fd < 16; fd++)
            close(fd);

        /*
         * Put the authenticated principal and other connection and command
         * information in the environment.  REMUSER is for backwards
         * compatibility with earlier versions of remctl.
         */
        if (setenv("REMUSER", client->user, 1) < 0)
            sysdie("cannot set REMUSER in environment");
        if (setenv("REMOTE_USER", client->user, 1) < 0)
            sysdie("cannot set REMOTE_USER in environment");
        if (setenv("REMOTE_ADDR", client->ipaddress, 1) < 0)
            sysdie("cannot set REMOTE_ADDR in environment");
        if (client->hostname != NULL)
            if (setenv("REMOTE_HOST", client->hostname, 1) < 0)
                sysdie("cannot set REMOTE_HOST in environment");
        if (setenv("REMCTL_COMMAND", command, 1) < 0)
            sysdie("cannot set REMCTL_COMMAND in environment");

        /* Drop privileges if requested. */
        if (cline->user != NULL && cline->uid > 0) {
            if (initgroups(cline->user, cline->gid) != 0)
                sysdie("cannot initgroups for %s\n", cline->user);
            if (setgid(cline->gid) != 0)
                sysdie("cannot setgid to %d\n", cline->gid);
            if (setuid(cline->uid) != 0)
                sysdie("cannot setuid to %d\n", cline->uid);
        }

        /*
         * Run the command.  On error, we intentionally don't reveal
         * information about the command we ran.
         */
        if (execv(cline->program, req_argv) < 0)
            sysdie("cannot execute command");

    /* In the parent. */
    default:
        close(stdinout_fds[1]);
        stdinout_fds[1] = INVALID_SOCKET;
        if (client->protocol > 1) {
            close(stderr_fds[1]);
            stderr_fds[1] = INVALID_SOCKET;
        }

        /*
         * Unblock the read ends of the output pipes, to enable us to read
         * from both iteratively, and unblock the write end of the input pipe
         * if we have one so that we don't block when feeding data to our
         * child.
         */
        fdflag_nonblocking(stdinout_fds[0], true);
        if (client->protocol > 1)
            fdflag_nonblocking(stderr_fds[0], true);

        /*
         * This collects output from both pipes iteratively, while the child
         * is executing, and processes it.  It also sends input data if we
         * have any.
         */
        process->stdinout_fd = stdinout_fds[0];
        if (client->protocol > 1)
            process->stderr_fd = stderr_fds[0];
        ok = server_process_output(client, process);
        close(process->stdinout_fd);
        if (client->protocol > 1)
            close(process->stderr_fd);
        if (!process->reaped)
            waitpid(process->pid, &process->status, 0);
        if (WIFEXITED(process->status))
            process->status = (signed int) WEXITSTATUS(process->status);
        else
            process->status = -1;
    }

 done:
    if (stdinout_fds[0] != INVALID_SOCKET)
        close(stdinout_fds[0]);
    if (stdinout_fds[1] != INVALID_SOCKET)
        close(stdinout_fds[1]);
    if (stderr_fds[0] != INVALID_SOCKET)
        close(stderr_fds[0]);
    if (stderr_fds[1] != INVALID_SOCKET)
        close(stderr_fds[1]);

    return ok;
}

/*
 * Find the summary of all commands the user can run against this remctl
 * server.  We do so by checking all configuration lines for any that
 * provide a summary setup that the user can access, then running that
 * line's command with the given summary sub-command.
 *
 * Takes a client object, the user requesting access, and the list of all
 * valid configurations.
 */
static void
server_send_summary(struct client *client, const char *user,
                    struct config *config)
{
    char *path = NULL;
    char *program;
    struct confline *cline = NULL;
    size_t i;
    char **req_argv = NULL;
    bool ok;
    bool ok_any = false;
    int status_all = 0;
    struct process process;
    struct evbuffer *output = NULL;

    /* Create a buffer to hold all the output for protocol version one. */
    if (client->protocol == 1) {
        output = evbuffer_new();
        if (output == NULL)
            die("internal error: cannot create output buffer");
    }

    /*
     * Check each line in the config to find any that are "<command> ALL"
     * lines, the user is authorized to run, and which have a summary field
     * given.
     */
    for (i = 0; i < config->count; i++) {
        memset(&process, 0, sizeof(process));
        process.client = client;
        cline = config->rules[i];
        if (strcmp(cline->subcommand, "ALL") != 0)
            continue;
        if (!server_config_acl_permit(cline, user))
            continue;
        if (cline->summary == NULL)
            continue;
        ok_any = true;

        /*
         * Get the real program name, and use it as the first argument in
         * argv passed to the command.  Then add the summary command to the
         * argv and pass off to be executed.
         */
        path = cline->program;
        req_argv = xmalloc(3 * sizeof(char *));
        program = strrchr(path, '/');
        if (program == NULL)
            program = path;
        else
            program++;
        req_argv[0] = program;
        req_argv[1] = cline->summary;
        req_argv[2] = NULL;
        ok = server_exec(client, cline->summary, req_argv, cline, &process);
        if (ok) {
            if (client->protocol == 1)
                if (evbuffer_add_buffer(output, process.output) < 0)
                    die("internal error: cannot copy data from output buffer");
            if (process.status != 0)
                status_all = process.status;
        }
        free(req_argv);
    }

    /*
     * Sets the last process status to 0 if all succeeded, or the last
     * failed exit status if any commands gave non-zero.  Return that
     * we had output successfully if any command gave it.
     */
    if (ok_any) {
        if (client->protocol == 1)
            server_v1_send_output(client, output, status_all);
        else
            server_v2_send_status(client, status_all);
    } else {
        notice("summary request from user %s, but no defined summaries",
               user);
        server_send_error(client, ERROR_UNKNOWN_COMMAND, "Unknown command");
    }
    if (output != NULL)
        evbuffer_free(output);
}

/*
 * Create the argv we will pass along to a program at a full command
 * request.  This will be created from the full command and arguments given
 * via the remctl client.
 *
 * Takes the command and optional sub-command to run, the config line for this
 * command, the process, and the existing argv from remctl client.  Returns
 * a newly-allocated argv array that the caller is responsible for freeing.
 */
static char **
create_argv_command(struct confline *cline, struct process *process,
                    struct iovec **argv)
{
    size_t count, i, j, stdin_arg;
    char **req_argv = NULL;
    const char *program;

    /* Get ready to assemble the argv of the command. */
    for (count = 0; argv[count] != NULL; count++)
        ;
    req_argv = xmalloc((count + 1) * sizeof(char *));

    /*
     * Get the real program name, and use it as the first argument in argv
     * passed to the command.  Then build the rest of the argv for the
     * command, splicing out the argument we're passing on stdin (if any).
     */
    program = strrchr(cline->program, '/');
    if (program == NULL)
        program = cline->program;
    else
        program++;
    req_argv[0] = xstrdup(program);
    if (cline->stdin_arg == -1)
        stdin_arg = count - 1;
    else
        stdin_arg = (size_t) cline->stdin_arg;
    for (i = 1, j = 1; i < count; i++) {
        if (i == stdin_arg) {
            process->input = argv[i];
            continue;
        }
        if (argv[i]->iov_len == 0)
            req_argv[j] = xstrdup("");
        else
            req_argv[j] = xstrndup(argv[i]->iov_base, argv[i]->iov_len);
        j++;
    }
    req_argv[j] = NULL;
    return req_argv;
}


/*
 * Create the argv we will pass along to a program in response to a help
 * request.  This is fairly simple, created off of the specific command
 * we want help with, along with any sub-command given for specific help.
 *
 * Takes the path of the program to run and the command and optional
 * sub-command to run.  Returns a newly allocated argv array that the caller
 * is responsible for freeing.
 */
static char **
create_argv_help(const char *path, const char *command, const char *subcommand)
{
    char **req_argv = NULL;
    const char *program;

    if (subcommand == NULL)
        req_argv = xmalloc(3 * sizeof(char *));
    else
        req_argv = xmalloc(4 * sizeof(char *));

    /* The argv to pass along for a help command is very simple. */
    program = strrchr(path, '/');
    if (program == NULL)
        program = path;
    else
        program++;
    req_argv[0] = xstrdup(program);
    req_argv[1] = xstrdup(command);
    if (subcommand == NULL)
        req_argv[2] = NULL;
    else {
        req_argv[2] = xstrdup(subcommand);
        req_argv[3] = NULL;
    }
    return req_argv;
}


/*
 * Process an incoming command.  Check the configuration files and the ACL
 * file, and if appropriate, forks off the command.  Takes the argument vector
 * and the user principal, and a buffer into which to put the output from the
 * executable or any error message.  Returns 0 on success and a negative
 * integer on failure.
 *
 * Using the command and the subcommand, the following argument, a lookup in
 * the conf data structure is done to find the command executable and acl
 * file.  If the conf file, and subsequently the conf data structure contains
 * an entry for this command with subcommand equal to "ALL", that is a
 * wildcard match for any given subcommand.  The first argument is then
 * replaced with the actual program name to be executed.
 *
 * After checking the acl permissions, the process forks and the child execv's
 * the command with pipes arranged to gather output. The parent waits for the
 * return code and gathers stdout and stderr pipes.
 */
void
server_run_command(struct client *client, struct config *config,
                   struct iovec **argv)
{
    char *command = NULL;
    char *subcommand = NULL;
    char *helpsubcommand = NULL;
    struct confline *cline = NULL;
    char **req_argv = NULL;
    size_t i;
    bool ok = false;
    bool help = false;
    const char *user = client->user;
    struct process process;

    /* Start with an empty process. */
    memset(&process, 0, sizeof(process));
    process.client = client;

    /*
     * We need at least one argument.  This is also rejected earlier when
     * parsing the command and checking argc, but may as well be sure.
     */
    if (argv[0] == NULL) {
        notice("empty command from user %s", user);
        server_send_error(client, ERROR_BAD_COMMAND, "Invalid command token");
        goto done;
    }

    /* Neither the command nor the subcommand may ever contain nuls. */
    for (i = 0; argv[i] != NULL && i < 2; i++) {
        if (memchr(argv[i]->iov_base, '\0', argv[i]->iov_len)) {
            notice("%s from user %s contains nul octet",
                   (i == 0) ? "command" : "subcommand", user);
            server_send_error(client, ERROR_BAD_COMMAND,
                              "Invalid command token");
            goto done;
        }
    }

    /* We need the command and subcommand as nul-terminated strings. */
    command = xstrndup(argv[0]->iov_base, argv[0]->iov_len);
    if (argv[1] != NULL)
        subcommand = xstrndup(argv[1]->iov_base, argv[1]->iov_len);

    /*
     * Find the program path we need to run.  If we find no matching command
     * at first and the command is a help command, then we either dispatch
     * to the summary command if no specific help was requested, or if a
     * specific help command was listed, check for that in the configuration
     * instead.
     */
    cline = find_config_line(config, command, subcommand);
    if (cline == NULL && strcmp(command, "help") == 0) {

        /* Error if we have more than a command and possible subcommand. */
        if (argv[1] != NULL && argv[2] != NULL && argv[3] != NULL) {
            notice("help command from user %s has more than three arguments",
                   user);
            server_send_error(client, ERROR_TOOMANY_ARGS,
                              "Too many arguments for help command");
        }

        if (subcommand == NULL) {
            server_send_summary(client, user, config);
            goto done;
        } else {
            help = true;
            if (argv[2] != NULL)
                helpsubcommand = xstrndup(argv[2]->iov_base,
                                          argv[2]->iov_len);
            cline = find_config_line(config, subcommand, helpsubcommand);
        }
    }

    /*
     * Arguments may only contain nuls if they're the argument being passed on
     * standard input.
     */
    for (i = 1; argv[i] != NULL; i++) {
        if (cline != NULL) {
            if (help == false && (long) i == cline->stdin_arg)
                continue;
            if (argv[i + 1] == NULL && cline->stdin_arg == -1)
                continue;
        }
        if (memchr(argv[i]->iov_base, '\0', argv[i]->iov_len)) {
            notice("argument %lu from user %s contains nul octet",
                   (unsigned long) i, user);
            server_send_error(client, ERROR_BAD_COMMAND,
                              "Invalid command token");
            goto done;
        }
    }

    /* Log after we look for command so we can get potentially get logmask. */
    server_log_command(argv, cline, user);

    /*
     * Check the command, aclfile, and the authorization of this client to
     * run this command.
     */
    if (cline == NULL) {
        notice("unknown command %s%s%s from user %s", command,
               (subcommand == NULL) ? "" : " ",
               (subcommand == NULL) ? "" : subcommand, user);
        server_send_error(client, ERROR_UNKNOWN_COMMAND, "Unknown command");
        goto done;
    }
    if (!server_config_acl_permit(cline, user)) {
        notice("access denied: user %s, command %s%s%s", user, command,
               (subcommand == NULL) ? "" : " ",
               (subcommand == NULL) ? "" : subcommand);
        server_send_error(client, ERROR_ACCESS, "Access denied");
        goto done;
    }

    /*
     * Check for a specific command help request with the cline and do error
     * checking and arg massaging.
     */
    if (help) {
        if (cline->help == NULL) {
            notice("command %s from user %s has no defined help",
                   command, user);
            server_send_error(client, ERROR_NO_HELP,
                              "No help defined for command");
            goto done;
        } else {
            subcommand = xstrdup(cline->help);
        }
    }

    /* Assemble the argv for the command we're about to run. */
    if (help)
        req_argv = create_argv_help(cline->program, subcommand, helpsubcommand);
    else
        req_argv = create_argv_command(cline, &process, argv);

    /* Now actually execute the program. */
    ok = server_exec(client, command, req_argv, cline, &process);
    if (ok) {
        if (client->protocol == 1)
            server_v1_send_output(client, process.output, process.status);
        else
            server_v2_send_status(client, process.status);
    }

 done:
    if (command != NULL)
        free(command);
    if (subcommand != NULL)
        free(subcommand);
    if (helpsubcommand != NULL)
        free(helpsubcommand);
    if (req_argv != NULL) {
        for (i = 0; req_argv[i] != NULL; i++)
            free(req_argv[i]);
        free(req_argv);
    }
    if (process.output != NULL)
        evbuffer_free(process.output);
}


/*
 * Free a command, represented as a NULL-terminated array of pointers to iovec
 * structs.
 */
void
server_free_command(struct iovec **command)
{
    struct iovec **arg;

    for (arg = command; *arg != NULL; arg++) {
        if ((*arg)->iov_base != NULL)
            free((*arg)->iov_base);
        free(*arg);
    }
    free(command);
}
