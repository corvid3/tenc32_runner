#include <assert.h>
#include <bits/time.h>
#include <getopt.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include <crow.10c32_serial/serial.h>
#include <crow.crowcpu/crowcpu.h>
#include <crow.vtparse/vtparse.h>
#include <stdlib.h>

#include "crow.10c32_floppy/floppy.h"
#include "crow.crowcpu_arch/crowcpu_arch.h"

_Atomic bool quit = false;
thrd_t quit_thread;
thrd_t input_thread;
int quit_shutdown_pipe_write;
struct tenc32_motherboard_t* mobo;

void
shutdown()
{
  printf("SHUTTING DOWN\n");
  quit = true;

  char c = 0;
  assert(write(quit_shutdown_pipe_write, &c, 1) == 1);

  thrd_join(quit_thread, 0);
  thrd_join(input_thread, 0);

  tenc32_motherboard_destroy(mobo);
  exit(0);
}

void
exception_callback(tenc32_motherboard_t* mobo, unsigned i)
{
  fflush(stdout);
  fflush(stderr);

  fprintf(stderr, "EXCEPTION %i\n", i);
  tenc32_dump_registers(mobo);

  shutdown();
}

void
onsig(int m)
{
  (void)m;
  quit = true;
  tenc32_awake_mobo(mobo);
}

struct quit_handler
{
  jmp_buf leave;
  int pipeval;
  int shutdown_read;
};

static unsigned
quit_more(struct quit_handler* handler)
{
  struct pollfd pfds[2];
  pfds[0].fd = handler->pipeval;
  pfds[0].events = POLLIN;
  pfds[1].fd = handler->shutdown_read;
  pfds[1].events = POLLIN;

  char c = 0;

  if (poll(pfds, 2, -1) == -1)
    abort();

  if (pfds[1].revents)
    longjmp(handler->leave, 0);

  /* slamming reads is suuuper slow but this is for keyboard input,
   * i can buffer it later if i REALLY need to. */
  if (read(handler->pipeval, &c, 1) == 0)
    abort();
  return c;
}

static int
quit_handler(struct quit_handler* handler)
{
  if (setjmp(handler->leave)) {
    return 0;
  }

  do {
    unsigned const what = vtparse((vtparse_more)quit_more, handler);
    if (what == VT_F1)
      break;
  } while (!quit);

  quit = 1;
  signal(SIGINT, 0);

  return 0;
}

struct input_manager
{
  int pipeout;
  int stdcpyout;
};

static int
input_manager(struct input_manager* man)
{
  enum
  {
    WAIT_MS = 20,
  };

  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;

  do {
    if (poll(&pfd, 1, WAIT_MS) == -1)
      break;

    if ((unsigned)pfd.revents & POLLIN) {
      char c = 0;
      assert(read(STDIN_FILENO, &c, 1) == 1);
      assert(write(man->pipeout, &c, 1) == 1);
      assert(write(man->stdcpyout, &c, 1) == 1);
    } else if (pfd.revents & POLLERR || pfd.revents & POLLHUP) {
      quit = true;
    }
  } while (!quit);

  return 0;
}

static int
init_input_handlers()
{
  int p[2];
  int stdcpy[2];
  int shutp[2];
  assert(pipe(p) == 0);
  assert(pipe(stdcpy) == 0);
  assert(pipe(shutp) == 0);

  struct quit_handler* handler = malloc(sizeof *handler);
  struct input_manager* manager = malloc(sizeof *manager);

  handler->pipeval = p[0];
  handler->shutdown_read = shutp[0];
  manager->pipeout = p[1];
  manager->stdcpyout = stdcpy[1];

  quit_shutdown_pipe_write = shutp[1];

  thrd_create(&quit_thread, (thrd_start_t)quit_handler, handler);
  thrd_create(&input_thread, (thrd_start_t)input_manager, manager);

  struct termios t = { 0 };
  tcgetattr(STDIN_FILENO, &t);
  t.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
  tcsetattr(STDIN_FILENO, 0, &t);

  return stdcpy[0];
}

int
main(int argc, char** argv)
{
  char* bios_path = NULL;
  unsigned tps = 10;

  signal(SIGINT, onsig);

  char c;
  while ((c = getopt(argc, argv, "b:t:")) != -1) {
    switch (c) {
      case 'b':
        bios_path = optarg;
        break;

      case 't':
        tps = atoi(optarg);
        break;

      case '?':
        fprintf(stderr, "unknown option %c\n", optopt);
        exit(1);
        break;
    }
  }

  unsigned interonset_msec = 1000. / tps;
  [[maybe_unused]] unsigned interonset_nsec = 1000 * 1000 * interonset_msec;
  [[maybe_unused]] unsigned interonset_multiplicity = 0;

  // struct timespec res;
  // clock_getres(CLOCK_MONOTONIC, &res);

  // interonset_multiplicity = res.tv_nsec / interonset_nsec;

  if (bios_path == NULL) {
    fprintf(stderr, "path to bios must be provided\n");
    exit(1);
  }

  struct stat s = {};
  assert(stat(bios_path, &s) == 0);

  if (s.st_size != TENC32_ROM_SIZE) {
    fprintf(stderr,
            "provided bios file <%s> is not precisely %#x bytes {%#lx}\n",
            bios_path,
            TENC32_ROM_SIZE,
            s.st_size);
    exit(1);
  }

  char bios[TENC32_ROM_SIZE];

  {
    FILE* bios_file = fopen(bios_path, "r");
    [[maybe_unused]] unsigned x = fread(bios, 1, sizeof bios, bios_file);
    fclose(bios_file);
  }

  tenc32_configuration_t conf = {
    .cci_con_ms = 1000,
    .cci_update_ms = 1000,
  };

  mobo = tenc32_motherboard_create(&conf, TENC32_MIN_RAM_SIZE);

  enum
  {
    SERIAL_HARDWARE_ID = 0x10,
    SERIAL_IRQ = 0x01,
  };

  int const stdcpy = init_input_handlers();
  /* initialize the serial monitor */
  tenc32_add_io_space(
    mobo,
    tenc32_serial_create(
      mobo, SERIAL_HARDWARE_ID, SERIAL_IRQ, STDOUT_FILENO, stdcpy));

  /* initialize the floppy disk controller peripheral
   * we assume theres already a disk in (/dev/sdb)
   */

#define READY_FLOPPY
#ifdef READY_FLOPPY
  struct cfdc* fdc = tenc32_fdc_create(mobo, 0x00);
  tenc32_fdc_debug(fdc, STDERR_FILENO);
  tenc32_add_io_space(mobo, tenc32_fdc_create_io(fdc, 0x11));
  if (!tenc32_fdc_insert_diskette(fdc, "./images/flop.fat", 0x00))
    return fprintf(stderr, "failed to open disk\n"), 0;
#endif

  printf("motherboard created\n");
  tenc32_restart(mobo, &bios);

  tenc32_insert_exception_callback(mobo, exception_callback);

  int state = 0;
  [[maybe_unused]] int incrementer = 0;

  printf("=== VM META INFORMATION END ===\n\n");

  do {
    // tenc32_dump_registers(mobo);
    // if (incrementer++ == 1) {
    //   struct timespec ts;
    //   ts.tv_sec = 0;
    //   ts.tv_nsec = 20000000;
    //   nanosleep(&ts, NULL);
    //   incrementer = 0;
    // }
    if (state == TENC32_STEP_HALT)
      tenc32_halt_sleep(mobo);
  } while ((state = tenc32_step(mobo)),
           state != TENC32_STEP_POWEROFF && state != TENC32_STEP_CRASH &&
             !quit);

#ifdef READY_FLOPPY
  tenc32_fdc_remove_diskette(fdc, 0);
  tenc32_fdc_destroy(fdc);
#endif
  fflush(stdout);
  fflush(stderr);
  shutdown();
}
