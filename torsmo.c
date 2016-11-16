/* torsmo, a system monitor
 *
 * This program is licensed under BSD license, read COPYING
 */

#include "torsmo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#if HAVE_DIRENT_H
#include <dirent.h>
#endif
#include <sys/time.h>
#include <X11/Xutil.h>

#define CONFIG_FILE "$HOME/.torsmorc"
#define MAIL_FILE "$MAIL"

/* alignments */
enum alignment {
  TOP_LEFT = 1,
  TOP_RIGHT,
  BOTTOM_LEFT,
  BOTTOM_RIGHT,
};

/* default config file */
static char *current_config;

/* set to 1 if you want all text to be in uppercase */
static unsigned int stuff_in_upper_case;

/* Position on the screen */
static int text_alignment;
static int gap_x, gap_y;

/* Font used */
static char *font_name;

/* Update interval */
static double update_interval;

/* fork? */
static int fork_to_background;

/* border */
static int draw_borders;
static int stippled_borders;

static int draw_shades, draw_outline;

static int border_margin, border_width;

static long default_fg_color, default_bg_color, default_out_color;

#ifdef OWN_WINDOW
/* create own window or draw stuff to root? */
static int own_window;

/* fixed size/pos is set if wm/user changes them */
static int fixed_size = 0, fixed_pos = 0;
#endif

static int minimum_width, minimum_height;

/* no buffers in used memory? */
int no_buffers;

/* pad percentages to decimals? */
static int pad_percents = 0;

/* Text that is shown */
static char original_text[] =
"$nodename - $sysname $kernel on $machine\n"
"$hr\n"
"${color grey}Uptime:$color $uptime\n"
"${color grey}Frequency (in MHz):$color $freq\n"
"${color grey}RAM Usage:$color $mem/$memmax - $memperc% ${membar 4}\n"
"${color grey}Swap Usage:$color $swap/$swapmax - $swapperc% ${swapbar 4}\n"
"${color grey}CPU Usage:$color $cpu% ${cpubar 4}\n"
"${color grey}Processes:$color $processes  ${color grey}Running:$color $running_processes\n"
"$hr\n"
"${color grey}File systems:\n"
" / $color${fs_free /}/${fs_size /} ${fs_bar 6 /}\n"
"${color grey}Networking:\n"
" Up:$color ${upspeed eth0} k/s${color grey} - Down:$color ${downspeed eth0} k/s\n"
"${color grey}Temperatures:\n"
" CPU:$color ${i2c temp 1}°C${color grey} - MB:$color ${i2c temp 2}°C\n"
"$hr\n"
#ifdef SETI
"${color grey}SETI@Home Statistics:\n"
"${color grey}Seti Unit Number:$color $seti_credit\n"
"${color grey}Seti Progress:$color $seti_prog% $seti_progbar\n"
#endif
;

static char *text = original_text;

static int total_updates;

/* font stuff */

static XFontStruct *font;

#ifdef XFT
static XftFont *xftfont;
static int font_alpha = 65535;
#endif

static inline int calc_text_width(const char *s, unsigned int l) {
#ifdef XFT
  if(use_xft) {
    XGlyphInfo gi;
    XftTextExtents8(display, xftfont, s, l, &gi);
    return gi.xOff;
  }
  else
#endif
  {
    return XTextWidth(font, s, l);
  }
}

#ifdef XFT

#define font_height() use_xft ? (xftfont->ascent + xftfont->descent) : \
    (font->max_bounds.ascent + font->max_bounds.descent)
#define font_ascent() use_xft ? xftfont->ascent : font->max_bounds.ascent
#define font_descent() use_xft ? xftfont->descent : font->max_bounds.descent

#else

#define font_height() (font->max_bounds.ascent + font->max_bounds.descent)
#define font_ascent() font->max_bounds.ascent
#define font_descent() font->max_bounds.descent

#endif

/* formatted text to render on screen, generated in generate_text(),
 * drawn in draw_stuff() */

#define TEXT_BUFFER_SIZE (1024*4)

static char text_buffer[TEXT_BUFFER_SIZE];

/* special stuff in text_buffer */

#define SPECIAL_CHAR '\x01'

enum {
  HORIZONTAL_LINE,
  STIPPLED_HR,
  BAR,
  FG,
  BG,
  OUTLINE,
};

static struct special_t {
  int type;
  short height;
  short width;
  long arg;
} specials[128];

static int special_count;
static int special_index; /* used when drawing */

static struct special_t *new_special(char *buf, int t) {
  if (special_count >= 128)
    CRIT_ERR("too much special things in text");

  buf[0] = SPECIAL_CHAR;
  buf[1] = '\0';
  specials[special_count].type = t;
  return &specials[special_count++];
}

static void new_bar(char *buf, int w, int h, int usage) {
  struct special_t *s = new_special(buf, BAR);
  s->arg = (usage > 255) ? 255 : ((usage < 0) ? 0 : usage);
  s->width = w;
  s->height = h;
}

static const char *scan_bar(const char *args, int *w, int *h) {
  *w = 0; /* zero width means all space that is available */
  *h = 4;
  /* bar's argument is either height or height,width */
  if (args) {
    int n=0;
    if (sscanf(args, "%d,%d %n", h, w, &n) <= 1)
      sscanf(args, "%d %n", h, &n);
    args += n;
  }

  return args;
}

static inline void new_hr(char *buf, int a) {
  new_special(buf, HORIZONTAL_LINE)->height = a;
}

static inline void new_stippled_hr(char *buf, int a, int b) {
  struct special_t *s = new_special(buf, STIPPLED_HR);
  s->height = b;
  s->arg = a;
}

static inline void new_fg(char *buf, long c) {
  new_special(buf, FG)->arg = c;
}

static inline void new_bg(char *buf, long c) {
  new_special(buf, BG)->arg = c;
}

static inline void new_outline(char *buf, long c) {
  new_special(buf, OUTLINE)->arg = c;
}

/* quite boring functions */

static inline void for_each_line(char *b, void (*f)(char *)) {
  char *ps, *pe;

  for (ps=b, pe=b; *pe; pe++) {
    if (*pe == '\n') {
      *pe = '\0';
      f(ps);
      *pe = '\n';
      ps = pe+1;
    }
  }

  if (ps < pe) f(ps);
}

static void convert_escapes(char *buf) {
  char *p = buf, *s = buf;

  while (*s) {
    if(*s == '\\') {
      s++;
      if(*s == 'n') *p++ = '\n';
      else if(*s == '\\') *p++ = '\\';
      s++;
    }
    else
      *p++ = *s++;
  }
  *p = '\0';
}

/* converts from bytes to human readable format (k, M, G) */
static void human_readable(long long a, char *buf) {
  if (a >= 1024*1024*1024)
    snprintf(buf, 255, "%.2fG", (a/1024/1024)/1024.0);
  else if (a >= 1024*1024) {
    double m = (a/1024)/1024.0;
    if(m >= 100.0)
      snprintf(buf, 255, "%.0fM", m);
    else
      snprintf(buf, 255, "%.1fM", m);
  }
  else if (a >= 1024)
    snprintf(buf, 255, "%Ldk", a/1024L);
  else
    snprintf(buf, 255, "%Ld", a);
}

/* text handling */

enum text_object_type {
  OBJ_acpiacadapter,
  OBJ_adt746xcpu,
  OBJ_adt746xfan,
  OBJ_acpifan,
  OBJ_acpitemp,
  OBJ_battery,
  OBJ_buffers,
  OBJ_cached,
  OBJ_color,
  OBJ_cpu,
  OBJ_cpubar,
  OBJ_downspeed,
  OBJ_downspeedf,
  OBJ_exec,
  OBJ_execi,
  OBJ_freq,
  OBJ_fs_bar,
  OBJ_fs_bar_free,
  OBJ_fs_free,
  OBJ_fs_free_perc,
  OBJ_fs_size,
  OBJ_fs_used,
  OBJ_fs_used_perc,
  OBJ_hr,
  OBJ_i2c,
  OBJ_kernel,
  OBJ_loadavg,
  OBJ_machine,
  OBJ_mails,
  OBJ_mem,
  OBJ_membar,
  OBJ_memmax,
  OBJ_memperc,
  OBJ_mixer,
  OBJ_mixerl,
  OBJ_mixerr,
  OBJ_mixerbar,
  OBJ_mixerlbar,
  OBJ_mixerrbar,
  OBJ_new_mails,
  OBJ_nodename,
#ifdef NVCTRL
  OBJ_nvctrl,
#endif
  OBJ_pre_exec,
  OBJ_processes,
  OBJ_running_processes,
  OBJ_shadecolor,
  OBJ_outlinecolor,
  OBJ_stippled_hr,
  OBJ_swap,
  OBJ_swapbar,
  OBJ_swapmax,
  OBJ_swapperc,
  OBJ_sysname,
  OBJ_temp1, /* i2c is used instead in these */
  OBJ_temp2,
  OBJ_text,
  OBJ_time,
  OBJ_utime,
  OBJ_totaldown,
  OBJ_totalup,
  OBJ_updates,
  OBJ_upspeed,
  OBJ_upspeedf,
  OBJ_uptime,
  OBJ_uptime_short,
#ifdef SETI
  OBJ_seti_prog,
  OBJ_seti_progbar,
  OBJ_seti_credit
#endif
};

struct text_object {
  int type;
  union {
    char *s; /* some string */
    int i;   /* some integer */
    long l;  /* some other integer */
    struct net_stat *net;
    struct fs_stat *fs;
    unsigned char loadavg[3];

    struct {
      struct fs_stat *fs;
      int w, h;
    } fsbar; /* 3 */

    struct {
      int l;
      int w, h;
    } mixerbar; /* 3 */

    struct {
      int fd;
      int arg;
    } i2c; /* 2 */

    struct {
      double last_update;
      float interval;
      char *cmd;
      char *buffer;
    } execi; /* 5 */

    struct {
      int a, b;
    } pair; /* 2 */

#ifdef NVCTRL
    struct {
      unsigned int arg;
    } nvctrl; /* 1 */
#endif

  } data;
};

static unsigned int text_object_count;
static struct text_object *text_objects;

/* new_text_object() allocates a new zeroed text_object */
static struct text_object *new_text_object() {
  text_object_count++;
  text_objects = (struct text_object *) realloc(text_objects,
      sizeof(struct text_object) * text_object_count);
  memset(&text_objects[text_object_count-1], 0, sizeof(struct text_object));

  return &text_objects[text_object_count-1];
}

static void free_text_objects() {
  unsigned int i;

  for (i=0; i<text_object_count; i++) {
    switch (text_objects[i].type) {
    case OBJ_acpitemp:
      close(text_objects[i].data.i);
      break;

    case OBJ_i2c:
      close(text_objects[i].data.i2c.fd);
      break;

    case OBJ_time:
    case OBJ_utime:
    case OBJ_text:
    case OBJ_exec:
    case OBJ_pre_exec:
    case OBJ_battery:
      free(text_objects[i].data.s);
      break;

    case OBJ_execi:
      free(text_objects[i].data.execi.cmd);
      free(text_objects[i].data.execi.buffer);
      break;
    }
  }

  free(text_objects);
  text_objects = NULL;
  text_object_count = 0;
}

void scan_mixer_bar(const char *arg, int *a, int *w, int *h) {
  char buf1[64];
  int n;

  if(arg && sscanf(arg, "%63s %n", buf1, &n) >= 1) {
    *a = mixer_init(buf1);
    (void) scan_bar(arg + n, w, h);
  }
  else {
    *a = mixer_init(0);
    (void) scan_bar(arg, w, h);
  }
}

/* construct_text_object() creates a new text_object */
static void construct_text_object(const char *s, const char *arg) {
  struct text_object *obj = new_text_object();

#define OBJ(a, n) if (strcmp(s, #a) == 0) { obj->type = OBJ_##a; need_mask |= (1 << n); {
#define END ; } } else

  if (s[0] == '#') {
    obj->type = OBJ_color;
    obj->data.l = get_x11_color(s);
  }
  else
  OBJ(acpitemp, 0)
    obj->data.i = open_acpi_temperature(arg);
  END
  OBJ(acpiacadapter, 0)
  END
  OBJ(freq, 0)
  END
  OBJ(acpifan, 0)
  END
  OBJ(battery, 0)
    char bat[64];
    if (arg)
      sscanf(arg, "%63s", bat);
    else
      strcpy(bat, "BAT0");
    obj->data.s = strdup(bat);
  END
  OBJ(buffers, INFO_BUFFERS)
  END
  OBJ(cached, INFO_BUFFERS)
  END
  OBJ(cpu, INFO_CPU)
  END
  OBJ(cpubar, INFO_CPU)
    (void) scan_bar(arg, &obj->data.pair.a, &obj->data.pair.b);
  END
  OBJ(color, 0)
    obj->data.l = arg ? get_x11_color(arg) : default_fg_color;
  END
  OBJ(downspeed, INFO_NET)
    obj->data.net = get_net_stat(arg);
  END
  OBJ(downspeedf, INFO_NET)
    obj->data.net = get_net_stat(arg);
  END
#ifdef HAVE_POPEN
  OBJ(exec, 0)
    obj->data.s = strdup(arg ? arg : "");
  END
  OBJ(execi, 0)
    unsigned int n;

    if (!arg || sscanf(arg, "%f %n", &obj->data.execi.interval, &n) <= 0) {
      char buf[256];
      ERR("${execi <interval> command}");
      obj->type = OBJ_text;
      snprintf(buf, 256, "${%s}", s);
      obj->data.s = strdup(buf);
    }
    else {
      obj->data.execi.cmd = strdup(arg + n);
      obj->data.execi.buffer = (char *) calloc(1, TEXT_BUFFER_SIZE);
    }
  END
  OBJ(pre_exec, 0)
    obj->type = OBJ_text;
    if (arg) {
      FILE *fp = popen(arg, "r");
      unsigned int n;
      char buf[2048];

      n = fread(buf, 1, 2048, fp);
      buf[n] = '\0';

      if(n && buf[n-1] == '\n') buf[n-1] = '\0';

      (void) pclose(fp);

      obj->data.s = strdup(buf);
    }
    else
      obj->data.s = strdup("");
  END
#endif
  OBJ(fs_bar, INFO_FS)
    obj->data.fsbar.h = 4;
    arg = scan_bar(arg, &obj->data.fsbar.w, &obj->data.fsbar.h);
    if (arg) {
      while (isspace(*arg)) arg++;
      if(*arg == '\0') arg = "/";
    }
    else
      arg = "/";
    obj->data.fsbar.fs = prepare_fs_stat(arg);
  END
  OBJ(fs_bar_free, INFO_FS)
    obj->data.fsbar.h = 4;
	if (arg) {
	  unsigned int n;
	  if (sscanf(arg, "%d %n", &obj->data.fsbar.h, &n) >= 1)
	    arg += n;
	}
	else
	  arg = "/";
	obj->data.fsbar.fs = prepare_fs_stat(arg);
  END
  OBJ(fs_free, INFO_FS)
    if (!arg) arg = "/";
    obj->data.fs = prepare_fs_stat(arg);
  END
  OBJ(fs_used_perc, INFO_FS)
	if (!arg) arg = "/";
		obj->data.fs = prepare_fs_stat(arg);
  END
  OBJ(fs_free_perc, INFO_FS)
    if (!arg) arg = "/";
    obj->data.fs = prepare_fs_stat(arg);
  END
  OBJ(fs_size, INFO_FS)
    if (!arg) arg = "/";
    obj->data.fs = prepare_fs_stat(arg);
  END
  OBJ(fs_used, INFO_FS)
    if (!arg) arg = "/";
    obj->data.fs = prepare_fs_stat(arg);
  END
  OBJ(hr, 0)
    obj->data.i = arg ? atoi(arg) : 1;
  END
  OBJ(i2c, INFO_I2C)
    char buf1[64], buf2[64];
    int n;

    if(!arg) {
      ERR("i2c needs arguments");
      obj->type = OBJ_text;
      obj->data.s = strdup("${i2c}");
      return;
    }

    if(sscanf(arg, "%63s %63s %d", buf1, buf2, &n) != 3) {
      /* if scanf couldn't read three values, read type and num and use
       * default device */
      sscanf(arg, "%63s %d", buf2, &n);
      obj->data.i2c.fd = open_i2c_sensor(0, buf2, n, &obj->data.i2c.arg);
    }
    else {
      obj->data.i2c.fd = open_i2c_sensor(buf1, buf2, n, &obj->data.i2c.arg);
    }
  END
  OBJ(loadavg, INFO_LOADAVG)
    int a = 1, b = 2, c = 3, r = 3;
    if (arg) {
      r = sscanf(arg, "%d %d %d", &a, &b, &c);
      if (r >= 3 && (c < 1 || c > 3))
        r--;
      if (r >= 2 && (b < 1 || b > 3))
        r--, b = c;
      if (r >= 1 && (a < 1 || a > 3))
        r--, a = b, b = c;
    }
    obj->data.loadavg[0] = (r >= 1) ? (unsigned char) a : 0;
    obj->data.loadavg[1] = (r >= 2) ? (unsigned char) b : 0;
    obj->data.loadavg[2] = (r >= 3) ? (unsigned char) c : 0;
  END
  OBJ(kernel, 0)
  END
  OBJ(machine, 0)
  END
  OBJ(mails, INFO_MAIL)
  END
  OBJ(mem, INFO_MEM)
  END
  OBJ(memmax, INFO_MEM)
  END
  OBJ(memperc, INFO_MEM)
  END
  OBJ(membar, INFO_MEM)
    (void) scan_bar(arg, &obj->data.pair.a, &obj->data.pair.b);
  END
  OBJ(mixer, INFO_MIXER)
    obj->data.l = mixer_init(arg);
  END
  OBJ(mixerl, INFO_MIXER)
    obj->data.l = mixer_init(arg);
  END
  OBJ(mixerr, INFO_MIXER)
    obj->data.l = mixer_init(arg);
  END
  OBJ(mixerbar, INFO_MIXER)
    scan_mixer_bar(arg, &obj->data.mixerbar.l, &obj->data.mixerbar.w,
        &obj->data.mixerbar.h);
  END
  OBJ(mixerlbar, INFO_MIXER)
    scan_mixer_bar(arg, &obj->data.mixerbar.l, &obj->data.mixerbar.w,
        &obj->data.mixerbar.h);
  END
  OBJ(mixerrbar, INFO_MIXER)
    scan_mixer_bar(arg, &obj->data.mixerbar.l, &obj->data.mixerbar.w,
        &obj->data.mixerbar.h);
  END
  OBJ(new_mails, INFO_MAIL)
  END
  OBJ(nodename, 0)
  END
#ifdef NVCTRL
  OBJ(nvctrl, 0)
    obj->data.nvctrl.arg = init_nvctrl(arg);
  END
#endif
  OBJ(processes, INFO_PROCS)
  END
  OBJ(running_processes, INFO_RUN_PROCS)
  END
  OBJ(shadecolor, 0)
    obj->data.l = arg ? get_x11_color(arg) : default_bg_color;
  END
  OBJ(outlinecolor, 0)
    obj->data.l = arg ? get_x11_color(arg) : default_out_color;
  END
  OBJ(stippled_hr, 0)
    int a = stippled_borders, b = 1;
    if(arg) {
      if(sscanf(arg, "%d %d", &a, &b) != 2)
        sscanf(arg, "%d", &b);
    }
    if (a <= 0) a = 1;
    obj->data.pair.a = a;
    obj->data.pair.b = b;
  END
  OBJ(swap, INFO_MEM)
  END
  OBJ(swapmax, INFO_MEM)
  END
  OBJ(swapperc, INFO_MEM)
  END
  OBJ(swapbar, INFO_MEM)
    (void) scan_bar(arg, &obj->data.pair.a, &obj->data.pair.b);
  END
  OBJ(sysname, 0)
  END
  OBJ(temp1, INFO_I2C)
    obj->type = OBJ_i2c;
    obj->data.i2c.fd = open_i2c_sensor(0, "temp", 1, &obj->data.i2c.arg);
  END
  OBJ(temp2, INFO_I2C)
    obj->type = OBJ_i2c;
    obj->data.i2c.fd = open_i2c_sensor(0, "temp", 2, &obj->data.i2c.arg);
  END
  OBJ(time, 0)
    obj->data.s = strdup(arg ? arg : "%F %T");
  END
  OBJ(utime, 0)
    obj->data.s = strdup(arg ? arg : "%F %T");
  END
  OBJ(totaldown, INFO_NET)
    obj->data.net = get_net_stat(arg);
  END
  OBJ(totalup, INFO_NET)
    obj->data.net = get_net_stat(arg);
  END
  OBJ(updates, 0)
  END
  OBJ(upspeed, INFO_NET)
    obj->data.net = get_net_stat(arg);
  END
  OBJ(upspeedf, INFO_NET)
    obj->data.net = get_net_stat(arg);
  END
  OBJ(uptime_short, INFO_UPTIME)
  END
  OBJ(uptime, INFO_UPTIME)
  END
#ifdef SETI
  OBJ(seti_prog, INFO_SETI)
  END
  OBJ(seti_progbar, INFO_SETI)
    (void) scan_bar(arg, &obj->data.pair.a, &obj->data.pair.b);
  END
  OBJ(seti_credit, INFO_SETI)
  END
#endif
  {
    char buf[256];
    ERR("unknown variable %s", s);
    obj->type = OBJ_text;
    snprintf(buf, 256, "${%s}", s);
    obj->data.s = strdup(buf);
  }
#undef OBJ
}

/* append_text() appends text to last text_object if it's text, if it isn't
 * it creates a new text_object */
static void append_text(const char *s) {
  struct text_object *obj;

  if (s == NULL || *s == '\0')
    return;

  obj = text_object_count ? &text_objects[text_object_count-1] : 0;

  /* create a new text object? */
  if (!obj || obj->type != OBJ_text) {
    obj = new_text_object();
    obj->type = OBJ_text;
    obj->data.s = strdup(s);
  }
  else {
    /* append */
    obj->data.s = (char *) realloc(obj->data.s,
        strlen(obj->data.s) + strlen(s) + 1);
    strcat(obj->data.s, s);
  }
}

static void extract_variable_text(const char *p) {
  const char *s = p;

  free_text_objects();

  while (*p) {
    if (*p == '$') {
      * (char *) p = '\0';
      append_text(s);
      * (char *) p = '$';
      p++;
      s = p;

      if (*p != '$') {
        char buf[256];
        const char *var;
        unsigned int len;

        /* variable is either $foo or ${foo} */
        if (*p == '{') {
          p++;
          s = p;
          while (*p && *p != '}') p++;
        }
        else {
          s = p;
          if (*p == '#') p++;
          while (*p && (isalnum((int) *p) || *p=='_')) p++;
        }

        /* copy variable to buffer */
        len = (p - s > 255) ? 255 : (p - s);
        strncpy(buf, s, len);
        buf[len] = '\0';

        if (*p == '}') p++;
        s = p;

        var = getenv(buf);

        /* if variable wasn't found from environment, use some special */
        if (!var) {
          char *p;
          char *arg = 0;

          /* split arg */
          if (strchr(buf, ' ')) {
            arg = strchr(buf, ' ');
            *arg = '\0';
            arg++;
            while (isspace((int) *arg)) arg++;
            if (!*arg) arg = 0;
          }

          /* lowercase variable name */
          p = buf;
          while (*p) {
            *p = tolower(*p);
            p++;
          }

          construct_text_object(buf, arg);
        }
        continue;
      }
      else
        append_text("$");
    }

    p++;
  }
  append_text(s);
}

double current_update_time, last_update_time;

static void generate_text() {
  unsigned int i, n;
  struct information *cur = &info;
  char *p;

  special_count = 0;

  /* update info */

  current_update_time = get_time();

  update_stuff();

  /* generate text */

  n = TEXT_BUFFER_SIZE - 2;
  p = text_buffer;

  for (i=0; i<text_object_count; i++) {
    struct text_object *obj = &text_objects[i];

#define OBJ(a) break; case OBJ_##a:

    switch (obj->type) {
    default: {
      ERR("not implemented obj type %d", obj->type);
    }
    OBJ(acpitemp) {
      /* does anyone have decimals in acpi temperature? */
      snprintf(p, n, "%d", (int) get_acpi_temperature(obj->data.i));
    }
    OBJ(freq) {
      snprintf(p, n, "%s", get_freq());
    }
    OBJ(adt746xcpu) {
      snprintf(p, n, "%s", get_adt746x_cpu());
    }
    OBJ(adt746xfan) {
      snprintf(p, n, "%s", get_adt746x_fan());
    }
    OBJ(acpifan) {
      snprintf(p, n, "%s", get_acpi_fan());
    }
    OBJ(acpiacadapter) {
      snprintf(p, n, "%s", get_acpi_ac_adapter());
    }
    OBJ(battery) {
      get_battery_stuff(p, n, obj->data.s);
    }
    OBJ(buffers) {
      human_readable(cur->buffers*1024, p);
    }
    OBJ(cached) {
      human_readable(cur->cached*1024, p);
    }
    OBJ(cpu) {
      snprintf(p, n, "%*d", pad_percents, (int) (cur->cpu_usage*100.0));
    }
    OBJ(cpubar) {
      new_bar(p, obj->data.pair.a, obj->data.pair.b, (int) (cur->cpu_usage*255.0));
    }
    OBJ(color) {
      new_fg(p, obj->data.l);
    }
    OBJ(downspeed) {
      snprintf(p, n, "%d", (int) (obj->data.net->recv_speed/1024));
    }
    OBJ(downspeedf) {
      snprintf(p, n, "%.1f", obj->data.net->recv_speed/1024.0);
    }
#ifdef HAVE_POPEN
    OBJ(exec) {
      char *p2 = p;
      FILE *fp = popen(obj->data.s, "r");
      int n2 = fread(p, 1, n, fp);
      (void) pclose(fp);

      p[n2] = '\0';
      if(n2 && p[n2-1] == '\n') p[n2-1] = '\0';

      while (*p2) {
        if (*p2 == '\001')
          *p2 = ' ';
        p2++;
      }
    }
    OBJ(execi) {
      if (current_update_time - obj->data.execi.last_update <
          obj->data.execi.interval) {
        snprintf(p, n, "%s", obj->data.execi.buffer);
      }
      else {
        char *p2 = obj->data.execi.buffer;
        FILE *fp = popen(obj->data.execi.cmd, "r");
        int n2 = fread(p2, 1, TEXT_BUFFER_SIZE, fp);
        (void) pclose(fp);

        p2[n2] = '\0';
        if(n2 && p2[n2-1] == '\n') p2[n2-1] = '\0';

        while (*p2) {
          if (*p2 == '\001')
            *p2 = ' ';
          p2++;
        }

        snprintf(p, n, "%s", obj->data.execi.buffer);

        obj->data.execi.last_update = current_update_time;
      }
    }
#endif
    OBJ(fs_bar) {
      if (obj->data.fs != NULL) {
        if (obj->data.fs->size == 0)
          new_bar(p, obj->data.fsbar.w, obj->data.fsbar.h, 255);
        else
          new_bar(p, obj->data.fsbar.w, obj->data.fsbar.h,
              (int) (255 - obj->data.fsbar.fs->avail*255/obj->data.fs->size));
      }
    }
    OBJ(fs_free) {
      if (obj->data.fs != NULL)
        human_readable(obj->data.fs->avail, p);
    }
    OBJ(fs_free_perc) {
      if (obj->data.fs != NULL) {
        if (obj->data.fs->size)
          snprintf(p, n, "%*d", pad_percents,
              (int) ((obj->data.fs->avail*100) / obj->data.fs->size));
        else
          snprintf(p, n, "0");
      }
    }
    OBJ(fs_size) {
      if (obj->data.fs != NULL)
        human_readable(obj->data.fs->size, p);
    }
    OBJ(fs_used) {
      if (obj->data.fs != NULL)
        human_readable(obj->data.fs->size - obj->data.fs->avail, p);
    }
    OBJ(fs_bar_free) {
	  if (obj->data.fs != NULL) {
	    if (obj->data.fs->size == 0)
		  new_bar(p, obj->data.fsbar.w, obj->data.fsbar.h, 255);
		else
		  new_bar(p, obj->data.fsbar.w, obj->data.fsbar.h,
		      (int) (obj->data.fsbar.fs->avail*255/obj->data.fs->size));
      }
    }
    OBJ(fs_used_perc) {
      if (obj->data.fs != NULL) {
        if (obj->data.fs->size)
          snprintf(p, n, "%d",
          100 - ((int) ((obj->data.fs->avail*100) / obj->data.fs->size)));
      else
        snprintf(p, n, "0");
      }
    }
    OBJ(loadavg) {
      float *v = info.loadavg;

      if (obj->data.loadavg[2])
        snprintf(p, n, "%.2f %.2f %.2f", v[obj->data.loadavg[0] - 1],
            v[obj->data.loadavg[1] - 1], v[obj->data.loadavg[2] - 1]);
      else if (obj->data.loadavg[1])
        snprintf(p, n, "%.2f %.2f", v[obj->data.loadavg[0] - 1],
            v[obj->data.loadavg[1] - 1]);
      else if (obj->data.loadavg[0])
        snprintf(p, n, "%.2f", v[obj->data.loadavg[0] - 1]);
    }
    OBJ(hr) {
      new_hr(p, obj->data.i);
    }
    OBJ(i2c) {
      double r;

      r = get_i2c_info(obj->data.i2c.fd, obj->data.i2c.arg);

      if (r >= 100.0 || r == 0)
        snprintf(p, n, "%d", (int) r);
      else
        snprintf(p, n, "%.1f", r);
    }
    OBJ(kernel) {
      snprintf(p, n, "%s", cur->uname_s.release);
    }
    OBJ(machine) {
      snprintf(p, n, "%s", cur->uname_s.machine);
    }

    /* memory stuff */
    OBJ(mem) {
      human_readable(cur->mem*1024, p);
    }
    OBJ(memmax) {
      human_readable(cur->memmax*1024, p);
    }
    OBJ(memperc) {
      if (cur->memmax)
        snprintf(p, n, "%*d", pad_percents, (cur->mem*100) / (cur->memmax));
    }
    OBJ(membar) {
      new_bar(p, obj->data.pair.a, obj->data.pair.b,
          cur->memmax ? (cur->mem*255) / (cur->memmax) : 0);
    }

    /* mixer stuff */
    OBJ(mixer) {
      snprintf(p, n, "%d", mixer_get_avg(obj->data.l));
    }
    OBJ(mixerl) {
      snprintf(p, n, "%d", mixer_get_left(obj->data.l));
    }
    OBJ(mixerr) {
      snprintf(p, n, "%d", mixer_get_right(obj->data.l));
    }
    OBJ(mixerbar) {
      new_bar(p, obj->data.mixerbar.w, obj->data.mixerbar.h,
          mixer_get_avg(obj->data.mixerbar.l)*255/100);
    }
    OBJ(mixerlbar) {
      new_bar(p, obj->data.mixerbar.w, obj->data.mixerbar.h,
          mixer_get_left(obj->data.mixerbar.l)*255/100);
    }
    OBJ(mixerrbar) {
      new_bar(p, obj->data.mixerbar.w, obj->data.mixerbar.h,
          mixer_get_right(obj->data.mixerbar.l)*255/100);
    }

    /* mail stuff */
    OBJ(mails) {
      snprintf(p, n, "%d", cur->mail_count);
    }
    OBJ(new_mails) {
      snprintf(p, n, "%d", cur->new_mail_count);
    }

    OBJ(nodename) {
      snprintf(p, n, "%s", cur->uname_s.nodename);
    }
#ifdef NVCTRL
    OBJ(nvctrl) {
      snprintf(p, n, "%d", get_nvctrl_info(obj->data.nvctrl.arg));
    }
#endif
    OBJ(outlinecolor) {
      new_outline(p, obj->data.l);
    }
    OBJ(processes) {
      snprintf(p, n, "%d", cur->procs);
    }
    OBJ(running_processes) {
      snprintf(p, n, "%d", cur->run_procs);
    }
    OBJ(text) {
      snprintf(p, n, "%s", obj->data.s);
    }
    OBJ(shadecolor) {
      new_bg(p, obj->data.l);
    }
    OBJ(stippled_hr) {
      new_stippled_hr(p, obj->data.pair.a, obj->data.pair.b);
    }
    OBJ(swap) {
      human_readable(cur->swap*1024, p);
    }
    OBJ(swapmax) {
      human_readable(cur->swapmax*1024, p);
    }
    OBJ(swapperc) {
      snprintf(p, 255, "%*u", pad_percents, (cur->swap*100) / cur->swapmax);
    }
    OBJ(swapbar) {
      new_bar(p, obj->data.pair.a, obj->data.pair.b,
          cur->swapmax ? (cur->swap*255) / (cur->swapmax) : 0);
    }
    OBJ(sysname) {
      snprintf(p, n, "%s", cur->uname_s.sysname);
    }
    OBJ(time) {
      time_t t = time(NULL);
      struct tm *tm = localtime(&t);
      setlocale(LC_TIME, "");
      strftime(p, n, obj->data.s, tm);
    }
    OBJ(utime) {
      time_t t = time(NULL);
      struct tm *tm = gmtime(&t);
      strftime(p, n, obj->data.s, tm);
    }
    OBJ(totaldown) {
      human_readable(obj->data.net->recv, p);
    }
    OBJ(totalup) {
      human_readable(obj->data.net->trans, p);
    }
    OBJ(updates) {
      snprintf(p, n, "%d", total_updates);
    }
    OBJ(upspeed) {
      snprintf(p, n, "%d", (int) (obj->data.net->trans_speed/1024));
    }
    OBJ(upspeedf) {
      snprintf(p, n, "%.1f", obj->data.net->trans_speed/1024.0);
    }
    OBJ(uptime_short) {
      format_seconds_short(p, n, (int) cur->uptime);
    }
    OBJ(uptime) {
      format_seconds(p, n, (int) cur->uptime);
    }

#ifdef SETI
    OBJ(seti_prog) {
      snprintf(p, n, "%.2f", cur->seti_prog * 100.0f);
    }
    OBJ(seti_progbar) {
      new_bar(p, obj->data.pair.a, obj->data.pair.b, (int)(cur->seti_prog * 255.0f));
    }
    OBJ(seti_credit) {
      snprintf(p, n, "%.0f", cur->seti_credit);
    }
#endif
    break;
    }

    {
      unsigned int a = strlen(p);
      p += a;
      n -= a;
    }
  }

  if (stuff_in_upper_case) {
    char *p;

    p = text_buffer;
    while (*p) {
      *p = toupper(*p);
      p++;
    }
  }

  last_update_time = current_update_time;
  total_updates++;
}

/*
 * text size
 */

static int text_start_x, text_start_y; /* text start position in window */
static int text_width, text_height;

static inline int get_string_width(const char *s) {
  return *s ? calc_text_width(s, strlen(s)) : 0;
}

static void text_size_updater(char *s) {
  int w = 0;
  char *p;

  /* get string widths and skip specials */
  p = s;
  while (*p) {
    if (*p == SPECIAL_CHAR) {
      *p = '\0';
      w += get_string_width(s);
      *p = SPECIAL_CHAR;

      if(specials[special_index].type == BAR) {
        w += specials[special_index].width;
      }

      special_index++;
      s = p+1;
    }
    p++;
  }

  w += get_string_width(s);

  if (w > text_width) text_width = w;

  text_height += font_height();
}

static void update_text_area() {
  int x, y;

  /* update text size if it isn't fixed */
#ifdef OWN_WINDOW
  if (!fixed_size)
#endif
  {
    text_width = minimum_width;
    text_height = 0;
    special_index = 0;
    for_each_line(text_buffer, text_size_updater);
    text_width += 1;
    if (text_height < minimum_height)
      text_height = minimum_height;
  }

  /* get text position on workarea */
  switch (text_alignment) {
  case TOP_LEFT:
    x = gap_x;
    y = gap_y;
    break;

  case TOP_RIGHT:
    x = workarea[2] - text_width - gap_x;
    y = gap_y;
    break;

  default:
  case BOTTOM_LEFT:
    x = gap_x;
    y = workarea[3] - text_height - gap_y;
    break;

  case BOTTOM_RIGHT:
    x = workarea[2] - text_width - gap_x;
    y = workarea[3] - text_height - gap_y;
    break;
  }

#ifdef OWN_WINDOW
  if (own_window) {
    x += workarea[0];
    y += workarea[1];
    text_start_x = border_margin + 1;
    text_start_y = border_margin + 1;
    window.x = x - border_margin - 1;
    window.y = y - border_margin - 1;
  }
  else
#endif
  {
    /* If window size doesn't match to workarea's size, then window
     * probably includes panels (gnome).
     * Blah, doesn't work on KDE. */
    if (workarea[2] != window.width || workarea[3] != window.height) {
      y += workarea[1];
      x += workarea[0];
    }

    text_start_x = x;
    text_start_y = y;
  }
}

/*
 * drawing stuff
 */

static int cur_x, cur_y; /* current x and y for drawing */
static int draw_mode; /* FG, BG or OUTLINE */
static long current_color;

static inline void set_foreground_color(long c) {
  current_color = c;
  XSetForeground(display, window.gc, c);
}

static void draw_string(const char *s) {
  if (s[0] == '\0') return;

#ifdef XFT
  if(use_xft) {
    XColor c;
    XftColor c2;
    c.pixel = current_color;
    XQueryColor(display, DefaultColormap(display, screen), &c);

    c2.pixel = c.pixel;
    c2.color.red = c.red;
    c2.color.green = c.green;
    c2.color.blue = c.blue;
    c2.color.alpha = font_alpha;

    XftDrawString8(window.xftdraw, &c2, xftfont,
        cur_x, cur_y, (XftChar8 *) s, strlen(s));
  }
  else
#endif
  {
    XDrawString(display, window.drawable, window.gc,
        cur_x, cur_y, s, strlen(s));
  }

  cur_x += get_string_width(s);
}

static void draw_line(char *s) {
  char *p;

  cur_x = text_start_x;
  cur_y += font_ascent();

  /* find specials and draw stuff */
  p = s;
  while (*p) {
    if (*p == SPECIAL_CHAR) {
      int w = 0;

      /* draw string before special */
      *p = '\0';
      draw_string(s);
      *p = SPECIAL_CHAR;
      s = p+1;

      /* draw special */
      switch (specials[special_index].type) {
      case HORIZONTAL_LINE:
        {
          int h = specials[special_index].height;
          int mid = font_ascent() / 2;
          w = text_start_x + text_width - cur_x;

          XSetLineAttributes(display, window.gc, h,
              LineSolid, CapButt, JoinMiter);
          XDrawLine(display, window.drawable, window.gc,
              cur_x, cur_y-mid, cur_x+w, cur_y-mid);
        }
        break;

      case STIPPLED_HR:
        {
          int h = specials[special_index].height;
          int s = specials[special_index].arg;
          int mid = font_ascent() / 2;
          char ss[2] = { s, s };
          w = text_start_x + text_width - cur_x - 1;

          XSetLineAttributes(display, window.gc, h, LineOnOffDash, CapButt,
              JoinMiter);
          XSetDashes(display, window.gc, 0, ss, 2);
          XDrawLine(display, window.drawable, window.gc,
              cur_x, cur_y-mid, cur_x+w, cur_y-mid);
        }
        break;

      case BAR:
        {
          int h = specials[special_index].height;
          int bar_usage = specials[special_index].arg;
          int by = cur_y - (font_ascent() + h)/2 + 1;
          w = specials[special_index].width;
          if(w == 0)
            w = text_start_x + text_width - cur_x - 1;
          if(w < 0) w = 0;

          XSetLineAttributes(display, window.gc, 1, LineSolid, CapButt, JoinMiter);

          XDrawRectangle(display, window.drawable, window.gc,
              cur_x, by, w, h);
          XFillRectangle(display, window.drawable, window.gc,
              cur_x, by, w * bar_usage / 255, h);
        }
        break;

      case FG:
        if (draw_mode == FG)
          set_foreground_color(specials[special_index].arg);
        break;

      case BG:
        if (draw_mode == BG)
          set_foreground_color(specials[special_index].arg);
        break;

      case OUTLINE:
        if (draw_mode == OUTLINE)
          set_foreground_color(specials[special_index].arg);
        break;
      }

      cur_x += w;

      special_index++;
    }

    p++;
  }

  draw_string(s);

  cur_y += font_descent();
}

static void draw_text() {
  cur_y = text_start_y;

  /* draw borders */
  if (draw_borders && border_width > 0) {
    unsigned int b = (border_width+1)/2;

    if(stippled_borders) {
      char ss[2] = { stippled_borders, stippled_borders };
      XSetLineAttributes(display, window.gc, border_width,
          LineOnOffDash, CapButt, JoinMiter);
      XSetDashes(display, window.gc, 0, ss, 2);
    }
    else {
      XSetLineAttributes(display, window.gc, border_width,
          LineSolid, CapButt, JoinMiter);
    }

    XDrawRectangle(display, window.drawable, window.gc,
        text_start_x - border_margin + b,
        text_start_y - border_margin + b,
        text_width + border_margin*2 - 1 - b*2,
        text_height + border_margin*2 - 1 - b*2);
  }

  /* draw text */
  special_index = 0;
  for_each_line(text_buffer, draw_line);
}

static void draw_stuff() {
  if (draw_shades && !draw_outline) {
    text_start_x++;
    text_start_y++;
    set_foreground_color(default_bg_color);
    draw_mode = BG;
    draw_text();
    text_start_x--;
    text_start_y--;
  }

  if (draw_outline) {
    int i, j;
    for (i=-1;i<2;i++)
      for (j=-1;j<2;j++) {
        if (i==0 && j==0)
          continue;
        text_start_x+=i;
        text_start_y+=j;
        set_foreground_color(default_out_color);
        draw_mode = OUTLINE;
        draw_text();
        text_start_x-=i;
        text_start_y-=j;
      }
  }

  set_foreground_color(default_fg_color);
  draw_mode = FG;
  draw_text();

#ifdef XDBE
  if (use_xdbe) {
    XdbeSwapInfo swap;
    swap.swap_window = window.window;
    swap.swap_action = XdbeBackground;
    XdbeSwapBuffers(display, &swap, 1);
  }
#endif
}

static void clear_text(int exposures) {
#ifdef XDBE
  if (use_xdbe) return; /* The swap action is XdbeBackground, which clears */
#endif
  /* there is some extra space for borders and outlines */
  XClearArea(display, window.drawable,
      text_start_x-border_margin-1, text_start_y-border_margin-1,
      text_width+border_margin*2+2, text_height+border_margin*2+2,
      exposures ? True : 0);
}

static int need_to_update;

/* update_text() generates new text and clears old text area */
static void update_text() {
  generate_text();
  clear_text(1);
  need_to_update = 1;
}

static void main_loop() {
  Region region = XCreateRegion();

  while (1) {
    XFlush(display);

    /* wait for X event or timeout */

    if (!XPending(display)) {
      fd_set fdsr;
      struct timeval tv;
      int s;
      double t = update_interval - (get_time() - last_update_time);

      if (t < 0) t = 0;

      tv.tv_sec = (long) t;
      tv.tv_usec = (long) (t * 1000000) % 1000000;

      FD_ZERO(&fdsr);
      FD_SET(ConnectionNumber(display), &fdsr);

      s = select(ConnectionNumber(display) + 1, &fdsr, 0, 0, &tv);
      if (s == -1) {
        if (errno != EINTR)
          ERR("can't select(): %s", strerror(errno));
      }
      else {
        /* timeout */
        if (s == 0)
          update_text();
      }
    }

    if (need_to_update) {
#ifdef OWN_WINDOW
      int wx = window.x, wy = window.y;
#endif

      need_to_update = 0;

      update_text_area();

#ifdef OWN_WINDOW
      if (own_window) {
        /* resize window if it isn't right size */
        if (!fixed_size &&
            (text_width+border_margin*2 != window.width ||
             text_height+border_margin*2 != window.height)) {
          window.width = text_width + border_margin*2 + 1;
          window.height = text_height + border_margin*2 + 1;
          XResizeWindow(display, window.window, window.width, window.height);
        }

        /* move window if it isn't in right position */
        if (!fixed_pos &&
            (window.x != wx || window.y != wy)) {
          XMoveWindow(display, window.window, window.x, window.y);
        }
      }
#endif

      clear_text(1);

#ifdef XDBE
      if (use_xdbe) {
        XRectangle r;
        r.x = text_start_x - border_margin;
        r.y = text_start_y - border_margin;
        r.width = text_width + border_margin*2;
        r.height = text_height + border_margin*2;
        XUnionRectWithRegion(&r, region, region);
      }
#endif
    }

    /* handle X events */

    while (XPending(display)) {
      XEvent ev;
      XNextEvent(display, &ev);

      switch (ev.type) {
      case Expose: {
          XRectangle r;
          r.x = ev.xexpose.x;
          r.y = ev.xexpose.y;
          r.width = ev.xexpose.width;
          r.height = ev.xexpose.height;
          XUnionRectWithRegion(&r, region, region);
        }
        break;

#ifdef OWN_WINDOW
      case ReparentNotify:
        /* set background to ParentRelative for all parents */
        if (own_window)
          set_transparent_background(window.window);
        break;

      case ConfigureNotify:
        if (own_window) {
          /* if window size isn't what expected, set fixed size */
          if (ev.xconfigure.width != window.width ||
              ev.xconfigure.height != window.height) {
            if (window.width != 0 && window.height != 0)
              fixed_size = 1;

            /* clear old stuff before screwing up size and pos */
            clear_text(1);

            {
              XWindowAttributes attrs;
              if (XGetWindowAttributes(display, window.window, &attrs)) {
                window.width = attrs.width;
                window.height = attrs.height;
              }
            }

            text_width = window.width - border_margin*2 - 1;
            text_height = window.height - border_margin*2 - 1;
          }

          /* if position isn't what expected, set fixed pos, total_updates
           * avoids setting fixed_pos when window is set to weird locations
           * when started */
          if (total_updates >= 2 && !fixed_pos &&
              (window.x != ev.xconfigure.x || window.y != ev.xconfigure.y) &&
              (ev.xconfigure.x != 0 || ev.xconfigure.y != 0)) {
            fixed_pos = 1;
          }
        }
        break;
#endif

      default:
        break;
      }
    }

    /* XDBE doesn't seem to provide a way to clear the back buffer without
     * interfering with the front buffer, other than passing XdbeBackground
     * to XdbeSwapBuffers. That means that if we're using XDBE, we need to
     * redraw the text even if it wasn't part of the exposed area. OTOH,
     * if we're not going to call draw_stuff at all, then no swap happens
     * and we can safely do nothing.
     */

    if (!XEmptyRegion(region)) {
#ifdef XDBE
      if (use_xdbe) {
        XRectangle r;
        r.x = text_start_x - border_margin;
        r.y = text_start_y - border_margin;
        r.width = text_width + border_margin*2;
        r.height = text_height + border_margin*2;
        XUnionRectWithRegion(&r, region, region);
      }
#endif
      XSetRegion(display, window.gc, region);
#ifdef XFT
      if (use_xft)
        XftDrawSetClip(window.xftdraw, region);
#endif
      draw_stuff();
      XDestroyRegion(region);
      region = XCreateRegion();
    }
  }
}

static void load_font() {
#ifdef XFT
  /* load Xft font */
  if (use_xft) {
    if (xftfont != NULL) XftFontClose(display, xftfont);

    if ((xftfont = XftFontOpenName(display, screen, font_name)) != NULL)
      return;

    ERR("can't load Xft font '%s'", font_name);
    if ((xftfont = XftFontOpenName(display, screen, "courier-12")) != NULL)
      return;

    ERR("can't load Xft font '%s'", "courier-12");

    if ((font = XLoadQueryFont(display, "fixed")) == NULL) {
      CRIT_ERR("can't load font '%s'", "fixed");
    }
    use_xft = 0;

    return;
  }
#endif

  /* load normal font */
  if (font != NULL) XFreeFont(display, font);

  if ((font = XLoadQueryFont(display, font_name)) == NULL) {
    ERR("can't load font '%s'", font_name);
    if ((font = XLoadQueryFont(display, "fixed")) == NULL) {
      CRIT_ERR("can't load font '%s'", "fixed");
    }
  }
}

static void set_font() {
#ifdef XFT
  if (use_xft) {
    if (window.xftdraw != NULL) XftDrawDestroy(window.xftdraw);
    window.xftdraw = XftDrawCreate(display, window.drawable,
        DefaultVisual(display, screen), DefaultColormap(display, screen));
  }
  else
#endif
  {
    XSetFont(display, window.gc, font->fid);
  }
}

static void load_config_file(const char *);

/* signal handler that reloads config file */
static void reload_handler(int a) {
  fprintf(stderr, "torsmo: received signal %d, reloading config\n", a);

  if (current_config) {
    clear_fs_stats();
    load_config_file(current_config);
    load_font();
    set_font();
    extract_variable_text(text);
    free(text);
    text = NULL;
    update_text();
  }
}

static void clean_up() {
#ifdef XDBE
  if (use_xdbe)
    XdbeDeallocateBackBufferName(display, window.back_buffer);
#endif
#ifdef OWN_WINDOW
  if (own_window)
    XDestroyWindow(display, window.window);
  else
#endif
  {
    clear_text(1);
    XFlush(display);
  }

  XFreeGC(display, window.gc);

  /* it is really pointless to free() memory at the end of program but ak|ra
   * wants me to do this */

  free_text_objects();

  if (text != original_text)
    free(text);

  free(current_config);
  free(current_mail_spool);
#ifdef SETI
  free(seti_dir);
#endif
}

static void term_handler(int a) {
  a = a; /* to get rid of warning */
  clean_up();
  exit(0);
}

static int string_to_bool(const char *s) {
  if (!s) return 1;
  if (strcasecmp(s, "yes") == 0) return 1;
  if (strcasecmp(s, "true") == 0) return 1;
  if (strcasecmp(s, "1") == 0) return 1;
  return 0;
}

static enum alignment string_to_alignment(const char *s) {
  if (strcasecmp(s, "top_left") == 0) return TOP_LEFT;
  else if (strcasecmp(s, "top_right") == 0) return TOP_RIGHT;
  else if (strcasecmp(s, "bottom_left") == 0) return BOTTOM_LEFT;
  else if (strcasecmp(s, "bottom_right") == 0) return BOTTOM_RIGHT;
  else if (strcasecmp(s, "tl") == 0) return TOP_LEFT;
  else if (strcasecmp(s, "tr") == 0) return TOP_RIGHT;
  else if (strcasecmp(s, "bl") == 0) return BOTTOM_LEFT;
  else if (strcasecmp(s, "br") == 0) return BOTTOM_RIGHT;

  return TOP_LEFT;
}

static void set_default_configurations(void) {
  text_alignment = BOTTOM_LEFT;
  fork_to_background = 0;
  border_margin = 3;
  border_width = 1;
  default_fg_color = WhitePixel(display, screen);
  default_bg_color = BlackPixel(display, screen);
  default_out_color = BlackPixel(display, screen);
  draw_borders = 0;
  draw_shades = 1;
  draw_outline = 0;
  free(font_name);
#ifdef XFT
  use_xft = 1;
  font_name = strdup("courier-12");
#else
  font_name = strdup("6x10");
#endif
  gap_x = 5;
  gap_y = 5;

  free(current_mail_spool);
  {
    char buf[256];
      variable_substitute(MAIL_FILE, buf, 256);
    if (buf[0] != '\0')
      current_mail_spool = strdup(buf);
  }

  minimum_width = 5;
  minimum_height = 5;
  no_buffers = 1;
#ifdef OWN_WINDOW
  own_window = 0;
#endif
  stippled_borders = 0;
  update_interval = 10.0;
  stuff_in_upper_case = 0;
}

static void load_config_file(const char *f) {
#define CONF_ERR ERR("%s: %d: config file error", f, line);
  int line = 0;
  FILE *fp;

  set_default_configurations();

  fp = open_file(f, 0);
  if (!fp) return;

  while (!feof(fp)) {
    char buf[256], *p, *p2, *name, *value;
    line++;
    if (fgets(buf, 256, fp) == NULL) break;

    p = buf;

    /* break at comment */
    p2 = strchr(p, '#');
    if (p2) *p2 = '\0';

    /* skip spaces */
    while (*p && isspace((int) *p)) p++;
    if (*p == '\0') continue; /* empty line */

    name = p;

    /* skip name */
    p2 = p;
    while (*p2 && !isspace((int) *p2)) p2++;
    if (*p2 != '\0') {
      *p2 = '\0'; /* break at name's end */
      p2++;
    }

    /* get value */
    if (*p2) {
      p = p2;
      while (*p && isspace((int) *p)) p++;

      value = p;

      p2 = value + strlen(value);
      while (isspace((int) *(p2-1))) *--p2 = '\0';
    }
    else {
      value = 0;
    }

#define CONF2(a) if (strcasecmp(name, a) == 0)
#define CONF(a) else CONF2(a)
#define CONF3(a,b) \
  else if (strcasecmp(name, a) == 0 || strcasecmp(name, a) == 0)


    CONF2("alignment") {
      if (value) {
        int a = string_to_alignment(value);
        if (a <= 0)
          CONF_ERR
        else
          text_alignment = a;
      }
      else
        CONF_ERR
    }
    CONF("background") {
      fork_to_background = string_to_bool(value);
    }
    CONF("border_margin") {
      if(value)
        border_margin = strtol(value, 0, 0);
      else
        CONF_ERR
    }
    CONF("border_width") {
      if(value)
        border_width = strtol(value, 0, 0);
      else
        CONF_ERR
    }
    CONF("default_color") {
      if (value)
        default_fg_color = get_x11_color(value);
      else
        CONF_ERR
    }
    CONF3("default_shade_color", "default_shadecolor") {
      if (value)
        default_bg_color = get_x11_color(value);
      else
        CONF_ERR
    }
    CONF3("default_outline_color", "default_outlinecolor") {
      if (value)
        default_out_color = get_x11_color(value);
      else
        CONF_ERR
    }
#ifdef XDBE
    CONF("double_buffer") {
      use_xdbe = string_to_bool(value);
    }
#endif
    CONF("draw_borders") {
      draw_borders = string_to_bool(value);
    }
    CONF("draw_shades") {
      draw_shades = string_to_bool(value);
    }
    CONF("draw_outline") {
      draw_outline = string_to_bool(value);
    }
#ifdef XFT
    CONF("use_xft") {
      use_xft = string_to_bool(value);
    }
    CONF("font") {
      /* font silently ignored when Xft */
    }
    CONF("xftalpha") {
      if (value)
        font_alpha = atof(value) * 65535.0;
      else
        CONF_ERR
    }
    CONF("xftfont") {
#else
    CONF("use_xft") {
      if(string_to_bool(value))
        ERR("Xft not enabled");
    }
    CONF("xftfont") {
      /* xftfont silently ignored when no Xft */
    }
    CONF("xftalpha") {
      /* xftalpha is silently ignored when no Xft */
    }
    CONF("font") {
#endif
      if (value) {
        free(font_name);
        font_name = strdup(value);
      }
      else
        CONF_ERR
    }
    CONF("gap_x") {
      if (value)
        gap_x = atoi(value);
      else
        CONF_ERR
    }
    CONF("gap_y") {
      if (value)
        gap_y = atoi(value);
      else
        CONF_ERR
    }
    CONF("mail_spool") {
      if (value) {
        char buf[256];
        variable_substitute(value, buf, 256);

        if (buf[0] != '\0') {
          if (current_mail_spool) free(current_mail_spool);
          current_mail_spool = strdup(buf);
        }
      }
      else
        CONF_ERR
    }
    CONF("minimum_size") {
      if (value) {
        if (sscanf(value, "%d %d", &minimum_width, &minimum_height) != 2)
          if (sscanf(value, "%d", &minimum_width) != 1)
            CONF_ERR
      }
      else
        CONF_ERR
    }
    CONF("no_buffers") {
      no_buffers = string_to_bool(value);
    }
#ifdef OWN_WINDOW
    CONF("own_window") {
      own_window = string_to_bool(value);
    }
#endif
    CONF("pad_percents") {
       pad_percents = atoi(value);
    }
    CONF("stippled_borders") {
      if(value)
        stippled_borders = strtol(value, 0, 0);
      else
        stippled_borders = 4;
    }
    CONF("temp1") {
      ERR("temp1 configuration is obsolete, use ${i2c <i2c device here> temp 1}");
    }
    CONF("temp1") {
      ERR("temp2 configuration is obsolete, use ${i2c <i2c device here> temp 2}");
    }
    CONF("update_interval") {
      if (value)
        update_interval = strtod(value, 0);
      else
        CONF_ERR
    }
    CONF("uppercase") {
      stuff_in_upper_case = string_to_bool(value);
    }
#ifdef SETI
    CONF("seti_dir") {
      seti_dir = (char *)malloc(strlen(value) + 1);
      strcpy(seti_dir, value);
    }
#endif
    CONF("text") {
      if (text != original_text)
        free(text);

      text = (char *) malloc(1);
      text[0] = '\0';

      while (!feof(fp)) {
        unsigned int l = strlen(text);
        if (fgets(buf, 256, fp) == NULL) break;
        text = (char *) realloc(text, l + strlen(buf) + 1);
        strcat(text, buf);

        if (strlen(text) > 1024*8) break;
      }
      fclose(fp);
      return;
    }
    else
      ERR("%s: %d: no such configuration: '%s'", f, line, name);

#undef CONF
#undef CONF2
  }

  fclose(fp);
#undef CONF_ERR
}

/* : means that character before that takes an argument */
static const char *getopt_string = "vVdt:f:u:hc:w:x:y:a:"
#ifdef OWN_WINDOW
"o"
#endif
#ifdef XDBE
"b"
#endif
;

int main(int argc, char **argv) {
  /* handle command line parameters that don't change configs */
  while (1) {
    int c = getopt(argc, argv, getopt_string);
    if (c == -1) break;

    switch (c) {
    case 'v':
    case 'V':
      printf("torsmo " VERSION " compiled " __DATE__ "\n");
      return 0;

    case 'c':
      /* if current_config is set to a strdup of CONFIG_FILE, free it (even
       * though free() does the NULL check itself;), then load optarg value */
      if (current_config) free(current_config);
      current_config = strdup(optarg);
      break;

    case 'h':
      printf(
"Usage: %s [OPTION]...\n"
"Torsmo is a system monitor that renders text on desktop or to own transparent\n"
"window. Command line options will override configurations defined in config\n"
"file.\n"
"   -V            version\n"
"   -a ALIGNMENT  text alignment on screen, {top,bottom}_{left,right}\n"
"   -c FILE       config file to load instead of " CONFIG_FILE "\n"
"   -d            daemonize, fork to background\n"
"   -f FONT       font to use\n"
"   -h            help\n"
#ifdef OWN_WINDOW
"   -o            create own window to draw\n"
#endif
#ifdef XDBE
"   -b            double buffer (prevents flickering)\n"
#endif
"   -t TEXT       text to render, remember single quotes, like -t '$uptime'\n"
"   -u SECS       update interval\n"
"   -w WIN_ID     window id to draw\n"
"   -x X          x position\n"
"   -y Y          y position\n"
, argv[0]);
      return 0;

    case 'w':
      window.window = strtol(optarg, 0, 0);
      break;

    case '?':
      exit(EXIT_FAILURE);
    }
  }

  /* initalize X BEFORE we load config. (we need to so that 'screen' is set) */
  init_X11();

  /* load current_config or CONFIG_FILE */

#ifdef CONFIG_FILE
  if (current_config == NULL)
  {
    /* load default config file */
    char buf[256];

    variable_substitute(CONFIG_FILE, buf, 256);

    if (buf[0] != '\0')
      current_config = strdup(buf);
  }
#endif

  if (current_config != NULL)
    load_config_file(current_config);
  else
    set_default_configurations();

#ifdef MAIL_FILE
  if (current_mail_spool == NULL) {
    char buf[256];
    variable_substitute(MAIL_FILE, buf, 256);

    if (buf[0] != '\0')
      current_mail_spool = strdup(buf);
  }
#endif

  /* handle other command line arguments */

  optind = 0;

  while (1) {
    int c = getopt(argc, argv, getopt_string);
    if(c == -1) break;

    switch (c) {
    case 'a':
      text_alignment = string_to_alignment(optarg);
      break;

    case 'd':
      fork_to_background = 1;
      break;

    case 'f':
      font_name = strdup(optarg);
      break;

#ifdef OWN_WINDOW
    case 'o':
      own_window = 1;
      break;
#endif
#ifdef XDBE
    case 'b':
      use_xdbe = 1;
      break;
#endif

    case 't':
      if (text != original_text) free(text);
      text = strdup(optarg);
      convert_escapes(text);
      break;

    case 'u':
      update_interval = strtod(optarg, 0);
      break;

    case 'x':
      gap_x = atoi( optarg );
      break;

    case 'y':
      gap_y = atoi( optarg );
      break;

    case '?':
      exit(EXIT_FAILURE);
    }
  }

  /* load font */
  load_font();

  /* generate text and get initial size */
  extract_variable_text(text);
  if (text != original_text) free(text);
  text = NULL;

  update_uname();

  generate_text();
  update_text_area(); /* to get initial size of the window */

  init_window(own_window,
      text_width + border_margin*2 + 1,
      text_height + border_margin*2 + 1);

  update_text_area(); /* to position text/window on screen */

#ifdef OWN_WINDOW
  if(own_window)
    XMoveWindow(display, window.window, window.x, window.y);
#endif

  create_gc();

  set_font();

  draw_stuff();

  /* fork */
  if (fork_to_background) {
    int ret = fork();
    switch (ret) {
    case -1:
      ERR("can't fork() to background: %s", strerror(errno));
      break;

    case 0:
      break;

    default:
      fprintf(stderr, "torsmo: forked to background, pid is %d\n", ret);
      exit(0);
      break;
    }
  }

  /* set SIGUSR1, SIGINT and SIGTERM handlers */
  {
    struct sigaction sa;

    sa.sa_handler = reload_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
      ERR("can't set signal handler for SIGUSR1: %s", strerror(errno));

    sa.sa_handler = term_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) != 0)
      ERR("can't set signal handler for SIGINT: %s", strerror(errno));

    sa.sa_handler = term_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGTERM, &sa, NULL) != 0)
      ERR("can't set signal handler for SIGTERM: %s", strerror(errno));
  }

  main_loop();

  return 0;
}
