//----------------------------------------------------------------------------
// preamble

//----------------------------------------------------------
// sockets and poll {{{

#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <limits.h>

// }}}
//----------------------------------------------------------
// Erlang port driver {{{

#include <erl_driver.h>
#include <ei.h> // Erlang term manipulation

// }}}
//----------------------------------------------------------
// definitions {{{

#define MAX_BUFFER (64 * 1024)

#define PORT_DRIVER_NAME      "af_unix_drv"
#define PORT_DRIVER_NAME_SYM   af_unix_drv

#define PORT_DRIVER_NAME_LEN (sizeof(PORT_DRIVER_NAME) - 1)

#if ERL_DRV_EXTENDED_MAJOR_VERSION == 2
// >=E15B
typedef ErlDrvSizeT  erl_size_t;
typedef ErlDrvSSizeT erl_ssize_t;
#else
// E14A
typedef int erl_size_t;
typedef int erl_ssize_t;
#endif

// }}}
//----------------------------------------------------------

//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
// basic socket operations {{{

void unix_srv_close(char *address, int lsock);

int unix_srv_listen(char *address, int type, uid_t uid, gid_t gid, mode_t mode)
{
  int lsock = socket(AF_UNIX, type, 0);
  struct sockaddr_un bind_addr;
  bind_addr.sun_family = AF_UNIX;
  strncpy(bind_addr.sun_path, address, sizeof(bind_addr.sun_path));

  int result = bind(lsock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
  if (result != 0)
    return -1;

  chmod(address, mode);
  // TODO: chown(address, uid, gid);

  result = listen(lsock, 1);
  if (result != 0) {
    int old_errno = errno;
    unix_srv_close(address, lsock);
    errno = old_errno;

    return -1;
  }

  return lsock;
}

void unix_srv_close(char *address, int lsock)
{
  close(lsock);
  unlink(address);
}

// }}}
//----------------------------------------------------------------------------
// Erlang port driver API {{{

struct client {
  char buffer[MAX_BUFFER]; // small optimization: don't allocate 64k every time
};

struct server {
  char address[PATH_MAX];
  struct pollfd poll[1];
};

struct unix_sock_context {
  enum { entry_server, entry_client } type;
  ErlDrvPort erl_port;
  int fd;
  union {
    struct client client;
    struct server server;
  };
};

//----------------------------------------------------------
// entry point definition {{{

ErlDrvData  unix_sock_driver_start(ErlDrvPort port, char *cmd);
void        unix_sock_driver_output(ErlDrvData drv_data, char *buf, erl_size_t len);
void        unix_sock_driver_stop(ErlDrvData drv_data);
void        unix_sock_driver_ready_input(ErlDrvData drv_data, ErlDrvEvent event);
erl_ssize_t unix_sock_driver_call(ErlDrvData drv_data, unsigned int command, char *buf, erl_size_t len, char **rbuf, erl_size_t rlen, unsigned int *flags);
void        unix_sock_driver_stop_select(ErlDrvEvent event, void *reserved);

ErlDrvEntry unix_sock_driver_entry = {
  NULL,       // int        init(void)
  unix_sock_driver_start,       // ErlDrvData start(ErlDrvPort port, char *cmd)
  unix_sock_driver_stop,        // void       stop(ErlDrvData drv_data)
  unix_sock_driver_output,      // void       output(ErlDrvData drv_data, char *buf, int len) // Erlang has data for driver
  unix_sock_driver_ready_input, // void       ready_input(ErlDrvData, ErlDrvEvent)
  NULL,                         // void       ready_output(ErlDrvData, ErlDrvEvent)
  PORT_DRIVER_NAME,             // <driver name>
  NULL,                         // void       finish(void)
  NULL,                         // <reserved>
  NULL,                         // int        control(...) // port_control/3 handler
  NULL,                         // void       timeout(ErlDrvData drv_data)
  NULL,                         // void       outputv(ErlDrvData drv_data, ErlIOVec *ev) // Erlang has data for driver
  NULL,                         // void       ready_async(ErlDrvData drv_data, ErlDrvThreadData thread_data)
  NULL,                         // void       flush(ErlDrvData drv_data)
  unix_sock_driver_call,        // int        call(ErlDrvData drv_data, unsigned int command, char *buf, int len, char **rbuf, int rlen, unsigned int *flags) // like control(), a synchronous call
  NULL,                         // void       event(ErlDrvData drv_data, ErlDrvEvent event, ErlDrvEventData event_data)
  ERL_DRV_EXTENDED_MARKER,
  ERL_DRV_EXTENDED_MAJOR_VERSION,
  ERL_DRV_EXTENDED_MINOR_VERSION,
  ERL_DRV_FLAG_USE_PORT_LOCKING,  // XXX: driver flags
  NULL,                         // <reserved>
  NULL,                         // called when process monitor dies
  unix_sock_driver_stop_select  // called to close an event object
};

// the same as <driver name> in structure above, but as identifer instead of
// string
DRIVER_INIT(PORT_DRIVER_NAME_SYM)
{
  return &unix_sock_driver_entry;
}

// }}}
//----------------------------------------------------------
// Erlang port start {{{

int setup_socket(struct unix_sock_context *context, char *addr, int len);
char* find_address(char *cmd);

ErlDrvData unix_sock_driver_start(ErlDrvPort port, char *cmd)
{
  fprintf(stderr, "@@ driver start: %s\r\n", cmd);
  fprintf(stderr, "@@ types: server=%d client=%d\r\n",
          entry_server, entry_client);

  struct unix_sock_context *context =
    driver_alloc(sizeof(struct unix_sock_context));
  memset(context, 0, sizeof(*context));

  context->erl_port = port;
  context->type = entry_server;

  char *address = find_address(cmd);

  // TODO: setup_socket() < 0 => error
  setup_socket(context, address, strlen(address));

  return (ErlDrvData)context;
}

char* find_address(char *cmd)
{
  cmd += strcspn(cmd, " ");
  cmd += strspn(cmd, " ");
  return cmd;
}

int setup_socket(struct unix_sock_context *context, char *addr, int len)
{
  fprintf(stderr, "@@ setup socket: %.*s\r\n", len, addr);

  size_t addr_len = sizeof(context->server.address);
  addr_len = (addr_len <= len) ? addr_len - 1 : len; // trim the address
  memmove(context->server.address, addr, addr_len);
  context->server.address[addr_len] = 0; // terminate with NIL byte

  int type = SOCK_STREAM;
  uid_t uid = 0;      // TODO: read from command buffer
  gid_t gid = 0;      // TODO: read from command buffer
  mode_t mode = 0660; // TODO: read from command buffer

  int lsock = unix_srv_listen(context->server.address, type, uid, gid, mode);
  if (lsock < 0)
    return -1;

  fprintf(stderr, "@@ listen() -> fd=%d\r\n", lsock);

  context->fd = lsock;
  context->server.poll[0].fd = context->fd;
  context->server.poll[0].events = POLLIN;

  return 0;
}

// }}}
//----------------------------------------------------------
// Erlang event close (after port stop) {{{

void unix_sock_driver_stop_select(ErlDrvEvent event, void *reserved)
{
  int fd = (int)event;
  fprintf(stderr, "@@ stop select(%d)\r\n", fd);

  close(fd);
}

// }}}
//----------------------------------------------------------
// Erlang port stop {{{

void unix_sock_driver_stop(ErlDrvData drv_data)
{
  struct unix_sock_context *context = (struct unix_sock_context *)drv_data;

  fprintf(stderr, "@@ driver stop(type=%d, fd=%d)\r\n",
          context->type, context->fd);

  ErlDrvEvent event = (ErlDrvEvent)(context->fd);
  if (context->type == entry_client) {
    driver_select(context->erl_port, event, ERL_DRV_USE | ERL_DRV_READ, 0);
  } else { // context->type == entry_server
    // XXX: server socket is not under Erlang's select mechanism, so it can be
    // safely close here
    unix_srv_close(context->server.address, context->fd);
  }

  driver_free(context);
}

// }}}
//----------------------------------------------------------
// Erlang port output (data written to port) {{{

void unix_sock_driver_output(ErlDrvData drv_data, char *buf, erl_size_t len)
{
  struct unix_sock_context *context = (struct unix_sock_context *)drv_data;

  fprintf(stderr, "@@ driver output(type=%d, fd=%d)\r\n",
          context->type, context->fd);

  if (context->type == entry_client) {
    // just assume this won't block
    // TODO: driver_enq(), driver_deq()
    write(context->fd, buf, len);
  } else { // context->type == entry_server
    // TODO: error
  }
}

// }}}
//----------------------------------------------------------
// Erlang port call {{{

erl_ssize_t unix_sock_driver_call(ErlDrvData drv_data, unsigned int command,
                                  char *buf, erl_size_t len,
                                  char **rbuf, erl_size_t rlen,
                                  unsigned int *flags)
{
  struct unix_sock_context *context = (struct unix_sock_context *)drv_data;

  fprintf(stderr, "@@ driver call(type=%d, fd=%d, command=%d, rlen=%d)\r\n",
          context->type, context->fd, command, rlen);

  if (context->type == entry_client)
    // client socket doesn't support port_call()
    return -1;

  switch (command) {
    case 133:
      fprintf(stderr, "@@ poll()\r\n");
      if (poll(context->server.poll, 1, 0) > 0) {
        // context->server.poll[0].fd == context->fd
        fprintf(stderr, "@@ context->server.poll[0].fd=%d context.fd=%d\r\n",
                context->server.poll[0].fd, context->fd);

        // TODO: implement me

        int client = accept(context->server.poll[0].fd, NULL, NULL);
        char msg[] = "Hello. This is a TODO.\n";
        write(client, msg, sizeof(msg) - 1);
        close(client);
      }
      fprintf(stderr, "@@ poll() finished\r\n");
    break;

    // TODO: default: error
  }

  // result: {ok,everything}
  char result_atom_1[] = "ok";
  char result_atom_2[] = "everything";

  int result_len = 0;
  ei_encode_version(NULL, &result_len);
  ei_encode_tuple_header(NULL, &result_len, 2);     // assume successful
  ei_encode_atom(NULL, &result_len, result_atom_1); // assume successful
  ei_encode_atom(NULL, &result_len, result_atom_2); // assume successful

  if (result_len > rlen) {
    *rbuf = driver_alloc(result_len);
  }

  result_len = 0;
  ei_encode_version(*rbuf, &result_len);
  ei_encode_tuple_header(*rbuf, &result_len, 2);
  ei_encode_atom(*rbuf, &result_len, result_atom_1);
  ei_encode_atom(*rbuf, &result_len, result_atom_2);

  fprintf(stderr, "@@ data =");
  int i;
  for (i = 0; i < result_len; ++i)
    fprintf(stderr, " %02x", (unsigned char)rbuf[0][i]);
  fprintf(stderr, "\r\n");

  fprintf(stderr, "@@ returning %d\r\n", result_len);
  return result_len;

  //ErlDrvEvent event = (ErlDrvEvent)(context->fd);
  //driver_select(context->erl_port, event, ERL_DRV_USE | ERL_DRV_READ, 1);
}

// }}}
//----------------------------------------------------------
// Erlang input on select socket {{{

void read_data(ErlDrvPort port, int fd, char *buffer, int buflen);

void unix_sock_driver_ready_input(ErlDrvData drv_data, ErlDrvEvent event)
{
  struct unix_sock_context *context = (struct unix_sock_context *)drv_data;
  // event == context->fd

  fprintf(stderr, "@@ driver input ready(type = %d, fd = %d)\r\n",
          context->type, context->fd);

  // XXX: context->type == entry_client, because entry_server is handled by
  // poll() without Erlang
  read_data(context->erl_port, context->fd,
            context->client.buffer, sizeof(context->client.buffer));
}

void read_data(ErlDrvPort port, int fd, char *buffer, int buflen)
{
  // TODO: implement me: read() + driver_output()
}

// }}}
//----------------------------------------------------------

// }}}
//----------------------------------------------------------------------------
// vim:ft=c:foldmethod=marker:nowrap
