#include <assert.h>
#include <bits/time.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <crow.10c32_serial/serial.h>
#include <crow.crowcpu/crowcpu.h>
#include <stdlib.h>

#include "crow.10c32_floppy/floppy.h"
#include "crow.crowcpu_arch/crowcpu_arch.h"

void
exception_callback(tenc32_motherboard_t* mobo, unsigned i)
{
  fflush(stdout);
  fflush(stderr);

  fprintf(stderr, "EXCEPTION %i\n", i);
  tenc32_dump_registers(mobo);
  tenc32_motherboard_destroy(mobo);
  exit(0);
}

bool quit = false;
struct tenc32_motherboard_t* mobo;

void
onsig(int m)
{
  (void)m;
  quit = true;
  tenc32_awake_mobo(mobo);
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

  /* initialize the serial monitor */
  tenc32_add_io_space(
    mobo,
    tenc32_serial_create(
      mobo, SERIAL_HARDWARE_ID, SERIAL_IRQ, STDOUT_FILENO, STDIN_FILENO));

  /* initialize the floppy disk controller peripheral
   * we assume theres already a disk in (/dev/sdb)
   */

#ifdef READY_FLOPPY
  struct cfdc* fdc = tenc32_fdc_create(mobo, 0x00);
  tenc32_add_io_space(mobo, tenc32_fdc_create_io(fdc, 0x11));
  if (!tenc32_fdc_insert_diskette(fdc, "/dev/sdb", 0x00))
    return 0;
#endif

  // tenc32_fdc_debug(fdc, STDERR_FILENO);

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

    // if (state != TENC32_STEP_OK)
    //   fprintf(stderr, "sHITTT!!! %i\n", state);
    if (state == TENC32_STEP_HALT)
      tenc32_halt_sleep(mobo);
  } while ((state = tenc32_step(mobo)),
           state != TENC32_STEP_POWEROFF && state != TENC32_STEP_CRASH &&
             !quit);

#ifdef READY_FLOPPY
  tenc32_fdc_remove_diskette(fdc, 0);
  tenc32_fdc_destroy(fdc);
#endif
  tenc32_motherboard_destroy(mobo);
  fflush(stdout);
  fflush(stderr);
}
