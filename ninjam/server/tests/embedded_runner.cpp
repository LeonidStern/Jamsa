/*
  Runs the server through the embedded API (ninjamsrv_embedded.h) as if it were
  the command line server, so the whole test suite can run against it:

    NINJAMSRV=path/to/ninjamsrv_embedded_runner python3 -m unittest test_server

  SIGHUP stops the server and starts it again with the reloaded config, which
  also tests that the embedded server can be restarted in the same process.
  SIGPIPE keeps its default action, so a send to a closed socket that raised it
  would kill the runner and fail the tests.
*/

#include "../ninjamsrv_embedded.h"

#include <atomic>
#include <chrono>
#include <signal.h>
#include <stdio.h>
#include <thread>

static std::atomic<int> g_stop, g_restart;

static void onSignal(int sig)
{
#ifdef SIGHUP
  if (sig == SIGHUP)
  {
    g_restart=1;
    return;
  }
#endif
  g_stop=1;
}

static void onLog(void *, const char *text)
{
  fputs(text,stdout);
  fflush(stdout);
}

int main(int argc, char **argv)
{
  if (argc != 2)
  {
    fprintf(stderr,"usage: %s config.cfg\n",argv[0]);
    return 1;
  }

  signal(SIGINT,onSignal);
  signal(SIGTERM,onSignal);
#ifdef SIGHUP
  signal(SIGHUP,onSignal);
#endif

  int r=ninjamsrv_embedded_start(argv[1],onLog,NULL);
  if (r != NINJAMSRV_OK)
  {
    fprintf(stderr,"embedded server failed to start (%d)\n",r);
    return 1;
  }

  while (!g_stop)
  {
    if (g_restart)
    {
      g_restart=0;
      ninjamsrv_embedded_stop();
      r=ninjamsrv_embedded_start(argv[1],onLog,NULL);
      if (r != NINJAMSRV_OK)
      {
        fprintf(stderr,"embedded server failed to restart (%d)\n",r);
        return 1;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ninjamsrv_embedded_stop();
  return ninjamsrv_embedded_running() ? 1 : 0;
}
