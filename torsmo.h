#ifndef _torsmo_h_
#define _torsmo_h_

#include "config.h"
#include <sys/utsname.h>
#include <stdio.h>
#include <stdlib.h>

#define ERR(s, varargs...) \
  fprintf(stderr, "torsmo: " s "\n", ##varargs)

/* critical error */
#define CRIT_ERR(s, varargs...) \
  { fprintf(stderr, "torsmo: " s "\n", ##varargs); exit(EXIT_FAILURE); }

struct net_stat {
  const char *dev;
  int up;
  long long last_read_recv, last_read_trans;
  long long recv, trans;
  double recv_speed, trans_speed;
};

struct fs_stat {
  int fd;
  char *path;
  long long size;
  long long avail;
};

struct cpu_stat {
  unsigned int user, nice, system, idle, iowait, irq, softirq;
};

enum {
  INFO_CPU       = 0,
  INFO_MAIL      = 1,
  INFO_MEM       = 2,
  INFO_NET       = 3,
#ifdef SETI
  INFO_SETI      = 4,
#endif
  INFO_PROCS     = 5,
  INFO_RUN_PROCS = 6,
  INFO_UPTIME    = 7,
  INFO_BUFFERS   = 8,
  INFO_FS        = 9,
  INFO_I2C       = 10,
  INFO_MIXER     = 11,
  INFO_LOADAVG   = 12,
  INFO_UNAME     = 13,
  INFO_FREQ      = 14,
};

struct information {
  unsigned int mask;

  struct utsname uname_s;

  char freq[10];
  
  double uptime;

  /* memory information in kilobytes */
  unsigned int mem, memmax, swap, swapmax;
  unsigned int bufmem, buffers, cached;

  unsigned int procs;
  unsigned int run_procs;

  float cpu_usage;
  struct cpu_stat cpu_summed;
  unsigned int cpu_count;

  float loadavg[3];

  int new_mail_count, mail_count;

  float seti_prog;
  float seti_credit;
};

/* in x11.c */

#include <X11/Xlib.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#ifdef XFT
#include <X11/Xft/Xft.h>
#endif

#if defined(HAVE_X11_EXTENSIONS_XDBE_H) && defined(HAVE_LIBXEXT) && defined(DOUBLE_BUFFER)
# define XDBE
# include <X11/extensions/Xdbe.h>
#endif

#define ATOM(a) XInternAtom(display, #a, False)

struct torsmo_window {
  Window window;
  Drawable drawable;
  GC gc;
#ifdef XDBE
  XdbeBackBuffer back_buffer;
#endif
#ifdef XFT
  XftDraw *xftdraw;
#endif

  int width;
  int height;
#ifdef OWN_WINDOW
  int x;
  int y;
#endif
};

#ifdef XDBE
extern int use_xdbe;
#endif

#ifdef XFT
extern int use_xft;
#endif

extern Display *display;
extern int display_width;
extern int display_height;
extern int screen;

extern int workarea[4];

extern struct torsmo_window window;

void init_X11();
void init_window(int use_own_window, int width, int height);
void create_gc();
void set_transparent_background(Window win);
long get_x11_color(const char *);

/* in common.c */

/* struct that has all info */
struct information info;

void update_uname();
double get_time(void);
FILE *open_file(const char *file, int *reported);
void variable_substitute(const char *s, char *dest, unsigned int n);
void format_seconds(char *buf, unsigned int n, long t);
void format_seconds_short(char *buf, unsigned int n, long t);
struct net_stat *get_net_stat(const char *dev);

void update_stuff();

#define SET_NEED(a) need_mask |= 1 << (a)
extern unsigned int need_mask;

extern double current_update_time, last_update_time;

extern int no_buffers;

/* system dependant (in linux.c) */

void prepare_update(void);
void update_uptime(void);
void update_meminfo(void);
void update_net_stats(void);
void update_cpu_usage(void);
void update_total_processes(void);
void update_running_processes(void);
char* get_freq();
void update_load_average();
int open_i2c_sensor(const char *dev, const char *type, int n, int *div);
double get_i2c_info(int fd, int arg);

char* get_adt746x_cpu(void);
char* get_adt746x_fan(void);

int open_acpi_temperature(const char *name);
double get_acpi_temperature(int fd);
char* get_acpi_ac_adapter(void);
char* get_acpi_fan(void);
void get_battery_stuff(char *buf, unsigned int n, const char *bat);

#ifdef NVCTRL
/* in nvctrl.c */
unsigned int init_nvctrl(const char *feat);
int get_nvctrl_info(unsigned int arg);
#endif

/* fs-stuff is possibly system dependant (in fs.c) */

void update_fs_stats(void);
struct fs_stat *prepare_fs_stat(const char *path);
void clear_fs_stats(void);

/* in mixer.c */

int mixer_init(const char *);
int mixer_get_avg(int);
int mixer_get_left(int);
int mixer_get_right(int);

/* in mail.c */

extern char *current_mail_spool;

void update_mail_count();

/* in seti.c */

#ifdef SETI
extern char *seti_dir;

void update_seti();
#endif

#endif
