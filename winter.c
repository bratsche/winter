#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>

#include <errno.h>

static const char* SPRING_SOCKET  = "spring";
static const char* SPRING_PIDFILE = "spring.pid";
static const char* SPRING_VERSION = "0.0.8";

typedef struct _Winter  Winter;

static char*   winter_get_socket_path ();
static char*   winter_get_pidfile_path ();
static void    winter_destroy (Winter* winter);
static void    error(Winter *winter, const char *);
static int     winter_read_pid (const Winter* winter);
static Winter* create_winter ();
static void    winter_init (Winter* winter);
static int     winter_verify_server_version (Winter *winter);

struct _Winter {
  int   fd;
  int   application;
  int   client;
  char* socket_path;
  char* pidfile;
};

static int
winter_read_pid (const Winter* winter)
{
  FILE* file;
  int   pid;

  if (!(file = fopen (winter->pidfile, "r")))
    return 0;

  fscanf (file, "%d", &pid);
  fclose (file);

  return pid;
}

static int
winter_is_server_running (const Winter* winter)
{
  int pid = winter_read_pid (winter);

  if (kill (pid, 0) && errno == ESRCH)
    return 0;

  return pid;
}

static void
print_help_screen ()
{
  printf ("Usage: winter COMMAND [ARGS]\n\n");
  printf ("Commands for winter itself:\n\n");

  printf ("  help            Print available commands.\n");
  printf ("  status          Show current status.\n\n");

  printf ("Commands for your application:\n\n");
  printf ("  cucumber        Execute a Cucumber feature.\n");
  printf ("  rails           Run a rails command. The following sub commands will use spring: console, runner, generate.\n");
  printf ("  rake            Run a rake task.\n");
  printf ("  rspec           Execute an RSpec spec.\n");
  printf ("  testunit        Execute a Test::Unit test.\n");
}

static void
winter_run_status (const Winter* winter)
{
  int pid = winter_is_server_running (winter);

  if (pid == 0) {
    printf ("Spring server is not running.\n");
  } else {
    printf ("Spring server is running, process id is %d.\n", pid);
  }
}

static int
send_fd (int sock, int fd)
{
  char buf[1];
  struct iovec iov;
  struct msghdr msg;
  struct cmsghdr *cmsg;
  int n;
  char cms[CMSG_SPACE(sizeof (int))];

  buf[0] = 0;
  iov.iov_base = buf;
  iov.iov_len = 1;

  memset (&msg, 0, sizeof (msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = (caddr_t)cms;
  msg.msg_controllen = CMSG_LEN (sizeof (int));

  cmsg = CMSG_FIRSTHDR (&msg);
  cmsg->cmsg_len = CMSG_LEN (sizeof (int));
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  memmove (CMSG_DATA (cmsg), &fd, sizeof (int));

  if ((n = sendmsg (sock, &msg, 0)) != iov.iov_len) {
    fprintf (stderr, "sendmsg() failed: %s (socket fd = %d)\n", strerror (errno), sock);
    return -1;
  }

  return 0;
}

static void
connect_to_application (Winter* winter)
{
  static const int APPLICATION = 0;
  static const int CLIENT = 1;
  int fd[2];

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) < 0) {
    perror ("Failed to open socketpair, existing.");
    exit (0);
  }

  winter->application = fd[APPLICATION];
  winter->client = fd[CLIENT];

  send_fd (winter->fd, winter->client);
}

static int
get_pid_from_server (Winter* winter)
{
  char buf[80];
  int n;
  char* start;
  char* end;

  bzero (buf, sizeof (buf));

  n = read (winter->fd, buf, 80);

  start = buf;
  end = start + strlen (start) - 1;
  while (end > start && isspace (*end)) end--;
  *(end + 1) = 0;

  return 0;
}

static void
forward_signals (Winter* winter, int pid)
{
  // INT QUIT USR1 USR2 INFO
  pid_t pgid = getpgid (pid);
}

static void
send_server_args (Winter* winter, const char* command, const char* args)
{
  char args_json[8192];
  char length[8192];
  char command_json_array[8192];

  connect_to_application (winter);

  bzero (args_json, sizeof (args_json));
  bzero (length, sizeof (length));
  bzero (command_json_array, sizeof (command_json_array));

  if (args == NULL || strlen (args) == 0) {
    sprintf (command_json_array, "[\"%s\"]", command);
  } else {
    sprintf (command_json_array, "[\"%s\", %s]", command, args);
  }

  sprintf (args_json, "{\"args\":%s,\"env\":\"ENV\"}", command_json_array);
  sprintf (length, "%d", (int)strlen (args_json));

  write (winter->fd, length, strlen (length));
  write (winter->fd, "\n", 1);
  write (winter->fd, args_json, strlen (args_json));

  send_fd (winter->application, STDOUT_FILENO);
  send_fd (winter->application, STDERR_FILENO);
  send_fd (winter->application, STDIN_FILENO);

  sprintf (length, "%d", (int)strlen (command_json_array));
  write (winter->application, length, strlen (length));
  write (winter->application, "\n", 1);
  write (winter->application, command_json_array, (int)strlen (command_json_array));
}

static void
build_comma_delimited_args (int argc, char* argv[], char* args_buffer, int modifier)
{
  int offset;
  int ac;
  char **av;

  args_buffer[0] = 0;
  ac = argc - modifier;
  av = argv + modifier;
  offset = 0;

  while (av++, --ac) {
    int to_write = 8192 - offset;
    int written;

    written = snprintf (args_buffer + offset, to_write, ac == 1 ? "\"%s\"" : "\"%s\",", *av);

    if (to_write < written)
      break;

    offset += written;
  }
}

static void
call_rails_command (Winter* winter, int argc, char* argv[])
{
  char command[8192];
  char args[8192];

  command[0] = 0;
  args[0] = 0;

  if (argc >= 3 && strcmp (argv[1], "rails") == 0) {
    // For example, "winter rails generate model user"
    //              "winter rails console"
    sprintf (command, "rails_%s", argv[2]);

    build_comma_delimited_args (argc, argv, args, 2);
  } else {
    sprintf (command, "%s", argv[1]);

    build_comma_delimited_args (argc, argv, args, 1);
  }

  send_server_args (winter, command, args);
}

static void
get_application_output (Winter* winter)
{
  char buffer[8192];
  int n;

  bzero (buffer, sizeof (buffer));

  n = read (winter->application, buffer, sizeof (buffer));
}

int main (int argc, char* argv[])
{
  Winter* winter;

  if (argc == 1) {
    print_help_screen ();
    return 0;
  }

  winter = create_winter ();

  if (!winter_verify_server_version (winter))
    error (winter, "Ruh roh, server version mismatch!");

  if (strcmp (argv[1], "status") == 0) {
    winter_run_status (winter);
  } else if (strcmp (argv[1], "help") == 0) {
    print_help_screen ();
  } else {
    if (strcmp (argv[1], "cucumber") != 0 &&
      strcmp (argv[1], "rails") != 0 &&
      strcmp (argv[1], "rake") != 0 &&
      strcmp (argv[1], "rspec") != 0 &&
      strcmp (argv[1], "testunit") != 0) {
      print_help_screen ();
    } else {
      call_rails_command (winter, argc, argv);

      get_application_output (winter);
    }
  }

  winter_destroy (winter);

  return 0;
}

static char*
string_chomp (char* str)
{
  char* end = str + strlen (str) - 1;
  while (end > str && isspace (*end)) end--;
  *(end + 1) = 0;

  return str;
}

static int
winter_verify_server_version (Winter *winter)
{
  char buf[80];
  int n;
  char* start;
  char* end;

  n = read (winter->fd, buf, 80);

  start = buf;
  end = start + strlen (buf) - 1;
  while (end > start && !isalnum (*end) && !ispunct (*end))
    end--;
  *(end + 1) = 0;

  return strcmp (buf, SPRING_VERSION) == 0;
}

static void
error (Winter* winter, const char* msg) {
  winter_destroy (winter);
  perror(msg);
  exit (0);
}

static char*
winter_get_pidfile_path ()
{
  const char* path = (const char*)getenv("SPRING_TMP_PATH");
  if (path == NULL) {
    path = "./tmp/spring";
  }

  size_t path_len = strlen (path);
  size_t pidfile_len = strlen (SPRING_PIDFILE);
  char* ret = malloc(sizeof(char) * (path_len + strlen(SPRING_PIDFILE) + 2));

  memcpy (ret, path, path_len);
  ret[path_len] = '/';
  memcpy (ret + path_len + 1, SPRING_PIDFILE, pidfile_len + 1);

  return ret;
}

static char*
winter_get_socket_path ()
{
  const char* path = (const char*)getenv("SPRING_TMP_PATH");
  if (path == NULL) {
    path = "./tmp/spring";
  }

  size_t path_len = strlen (path);
  size_t pidfile_len = strlen (SPRING_SOCKET);
  char* ret = malloc((path_len + strlen(SPRING_SOCKET) + 2) * sizeof(char));

  memcpy (ret, path, path_len);
  ret[path_len] = '/';
  memcpy (ret + path_len + 1, SPRING_SOCKET, pidfile_len + 1);

  return ret;
}

static int
winter_setup_socket (Winter *winter)
{
  struct sockaddr_un serv_addr;
  int fd, len, n;

  bzero ((char*)&serv_addr, sizeof(serv_addr));
  serv_addr.sun_family = AF_UNIX;
  strcpy (serv_addr.sun_path, winter->socket_path);
  len = strlen (serv_addr.sun_path) + sizeof (serv_addr.sun_family);

  if ((fd = socket (AF_UNIX, SOCK_STREAM, 0)) < 0)
    error (winter, "Creating socket");

  if (connect (fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    error (winter, "Connecting");

  return fd;
}

static Winter*
create_winter ()
{
  Winter* winter = calloc (1, sizeof (Winter));
  winter_init (winter);

  return winter;
}

static void
winter_init (Winter* winter)
{
  winter->socket_path = winter_get_socket_path ();
  winter->pidfile = winter_get_pidfile_path ();
  winter->fd = winter_setup_socket (winter);
}

static void
winter_destroy (Winter* winter)
{
  if (winter->socket_path != NULL) {
    free (winter->socket_path);
  }

  if (winter->fd > 0) {
    close (winter->fd);
  }

  free (winter);
}
