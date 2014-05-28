%%%---------------------------------------------------------------------------
%%% @doc
%%%   Module that imitates multiple connections.
%%%
%%%   Active socket sends:
%%%   <ul>
%%%     <li>`{unix, Port, Data}'</li>
%%%     <li>`{unix_closed, Port}'</li>
%%%     <li>`{unix_error, Port, Reason}'</li>
%%%   </ul>
%%% @end
%%%---------------------------------------------------------------------------

-module(af_unix).

%% server socket API
-export([listen/1, accept/1, accept/2]).
%% connection socket API
-export([connect/2, send/2, recv/2, recv/3]).
-export([controlling_process/2, setopts/2]).
%% common to server and connection sockets
-export([close/1]).

-export_type([socket/0]).

%%%---------------------------------------------------------------------------

-define(PORT_DRIVER_NAME, "af_unix_drv").
-define(APP_NAME, af_unix).

-define(ACCEPT_LOOP_INTERVAL, 100). % 100ms

-define(PORT_COMMAND_ACCEPT, 133).

%%%---------------------------------------------------------------------------

%% @type socket() = port().

-type socket() :: port().

%% @type connection_socket() = socket().

-type connection_socket() :: socket().

%% @type server_socket() = socket().

-type server_socket() :: socket().

%%%---------------------------------------------------------------------------
%%% server socket API
%%%---------------------------------------------------------------------------

%% @doc Setup a socket listening on specified address.
%%
%% @spec listen(string()) ->
%%   {ok, server_socket()} | {error, Reason}

-spec listen(string()) ->
  {ok, server_socket()} | {error, term()}.

listen(Address) ->
  ensure_driver_loaded(),
  try
    Port = open_port({spawn_driver, "af_unix_drv " ++ Address}, [binary]),
    {ok, Port}
  catch
    error:Reason ->
      {error, Reason}
  end.

%% @doc Accept a client connection.
%%   The function waits infinitely for a client.
%%
%% @spec accept(server_socket()) ->
%%   {ok, connection_socket()} | {error, Reason}

-spec accept(server_socket()) ->
  {ok, connection_socket()} | {error, term()}.

accept(Socket) ->
  case try_accept(Socket) of
    {ok, Client} ->
      {ok, Client};
    nothing ->
      timer:sleep(?ACCEPT_LOOP_INTERVAL),
      accept(Socket)
  end.

%% @doc Accept a client connection.
%%
%%   To work with returned socket, see {@link af_unix_connection}.
%%
%% @spec accept(server_socket(), non_neg_integer()) ->
%%   {ok, connection_socket()} | {error, timeout} | {error, Reason}

-spec accept(server_socket(), non_neg_integer()) ->
  {ok, connection_socket()} | {error, timeout} | {error, term()}.

accept(Socket, Timeout) when Timeout =< 0 ->
  case try_accept(Socket) of
    {ok, Client} -> {ok, Client};
    nothing -> {error, timeout}
  end;

accept(Socket, Timeout) when Timeout =< ?ACCEPT_LOOP_INTERVAL ->
  case try_accept(Socket) of
    {ok, Client} ->
      {ok, Client};
    nothing ->
      timer:sleep(Timeout),
      accept(Socket, 0)
  end;

accept(Socket, Timeout) when Timeout > ?ACCEPT_LOOP_INTERVAL ->
  case try_accept(Socket) of
    {ok, Client} ->
      {ok, Client};
    nothing ->
      timer:sleep(?ACCEPT_LOOP_INTERVAL),
      accept(Socket, Timeout - ?ACCEPT_LOOP_INTERVAL)
  end.

%%%---------------------------------------------------------------------------
%%% connection socket
%%%---------------------------------------------------------------------------

%% @doc Connect to specified socket.
%%
%% @spec connect(string(), list()) ->
%%   {ok, connection_socket()} | {error, Reason}

connect(_Address, _Opts) ->
  {error, not_implemented}.

%% @doc Send data to the socket.
%%
%% @spec send(connection_socket(), iolist() | binary()) ->
%%   ok | {error, Reason}

send(Socket, Data) ->
  try
    port_command(Socket, Data),
    ok
  catch
    error:badarg ->
      {error, badarg}
  end.

%% @doc Read `Length' bytes from socket.
%%
%% @spec recv(connection_socket(), non_neg_integer()) ->
%%   {ok, Packet} | {error, Reason}

recv(_Socket, _Length) ->
  {error, not_implemented}.

%% @doc Read `Length' bytes from socket (with timeout).
%%
%% @spec recv(connection_socket(), non_neg_integer(),
%%            non_neg_integer() | infinity) ->
%%   {ok, Packet} | {error, Reason}

recv(_Socket, _Length, _Timeout) ->
  {error, not_implemented}.

%% @doc Set the socket owner.
%%
%% @spec controlling_process(socket(), pid()) ->
%%   ok | {error, Reason}

controlling_process(_Socket, _Pid) ->
  % remember to unlink from old socket
  {error, not_implemented}.

%% @doc Set socket options.
%%
%% @spec setopts(socket(), list()) ->
%%   ok | {error, Reason}

setopts(_Socket, _Opts) ->
  {error, not_implemented}.

%%%---------------------------------------------------------------------------
%%% common to server and connection
%%%---------------------------------------------------------------------------

%% @doc Close server connection.
%%
%% @spec close(socket()) ->
%%   ok

-spec close(socket()) ->
  ok.

close(Socket) ->
  true = port_close(Socket),
  ok.

%%%---------------------------------------------------------------------------
%%% private helpers
%%%---------------------------------------------------------------------------

%% @doc Try accepting connection.
%%
%% @spec try_accept(server_socket()) ->
%%   {ok, connection_socket()} | nothing

try_accept(Socket) ->
  case erlang:port_call(Socket, ?PORT_COMMAND_ACCEPT, ignore) of
    {ok, nothing} ->
      nothing;
    {ok, client} ->
      receive
        {Socket, {client, Client}} ->
          {ok, Client}
      end
  end.

%% @doc Ensure the port driver library is loaded.
%%
%% @spec ensure_driver_loaded() ->
%%   ok

ensure_driver_loaded() ->
  PrivDir = code:lib_dir(?APP_NAME, priv),
  ok = erl_ddll:load_driver(PrivDir, ?PORT_DRIVER_NAME),
  ok.

%%%---------------------------------------------------------------------------
%%% vim:ft=erlang:foldmethod=marker
