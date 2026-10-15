/*
    NINJAM Server - ninjamsrv_embedded.h

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Runs the server inside another program, such as the JamTaba plugin, on a
  background thread. Build ninjamsrv.cpp with NINJAM_EMBEDDED defined (it then
  has no main()). The server reads the same config file as the command line
  server, but installs no signal handlers, writes no pid file and never exits
  the process. Only one embedded server can run at a time.

*/

#ifndef _NINJAMSRV_EMBEDDED_H_
#define _NINJAMSRV_EMBEDDED_H_

// called with each log line, from the server's threads
typedef void (*ninjamsrv_log_func)(void *ctx, const char *text);

enum
{
  NINJAMSRV_OK = 0,
  NINJAMSRV_ALREADY_RUNNING = -1,
  NINJAMSRV_CONFIG_ERROR = -2,
  NINJAMSRV_LISTEN_ERROR = -3, // port in use or not allowed; the log says which port
};

// Starts the server and returns once it is listening (NINJAMSRV_OK) or has failed.
int ninjamsrv_embedded_start(const char *configfile, ninjamsrv_log_func log, void *ctx);

// Stops the server and waits for it, including removing the router port mapping.
void ninjamsrv_embedded_stop();

bool ninjamsrv_embedded_running();

// Copies the names of the users who are logged in, one per line, into buf
// (truncated to bufsize). Returns how many users are logged in.
int ninjamsrv_embedded_users(char *buf, int bufsize);

#endif
