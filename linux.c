#include "torsmo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

static struct sysinfo s_info;

void prepare_update() {
}

static void update_sysinfo() {
  sysinfo(&s_info);

  info.uptime = (double) s_info.uptime;

  /* there was some problem with these */
#if 0
  info.loadavg[0] = s_info.loads[0] / 100000.0f;
  info.loadavg[1] = s_info.loads[1] / 100000.0f;
  info.loadavg[2] = s_info.loads[2] / 100000.0f;
  info.mask |= 1 << INFO_LOADAVG;
#endif

  info.procs = s_info.procs;

  /* these aren't nice, no cache and should check kernel version for mem_unit */
#if 0
  info.memmax = s_info.totalram;
  info.mem = s_info.totalram - s_info.freeram;
  info.swapmax = s_info.totalswap;
  info.swap = s_info.totalswap - s_info.swap;
  info.mask |= 1 << INFO_MEM;
#endif

  info.mask |= (1 << INFO_UPTIME) | (1 << INFO_PROCS);
}

void update_uptime() {
  /* prefers sysinfo() for uptime, I don't really know which one is better
   * (=faster?) */
#ifdef USE_PROC_UPTIME
  static int rep;
  FILE *fp = open_file("/proc/uptime", &rep);
  if(!fp) return 0;
  fscanf(fp, "%lf", &info.uptime);
  fclose(fp);

  info.mask |= (1 << INFO_UPTIME);
#else
  update_sysinfo();
#endif
}

/* these things are also in sysinfo except Buffers:, that's why I'm reading
 * them from proc */

static FILE *meminfo_fp;

void update_meminfo() {
  static int rep;
/*  unsigned int a; */
  char buf[256];

  info.mem = info.memmax = info.swap = info.swapmax = info.bufmem =
    info.buffers = info.cached = 0;

  if (meminfo_fp == NULL)
    meminfo_fp = open_file("/proc/meminfo", &rep);
  else
    fseek(meminfo_fp, 0, SEEK_SET);
  if (meminfo_fp == NULL) return;

  while (!feof(meminfo_fp)) {
    if (fgets(buf, 255, meminfo_fp) == NULL) break;

    if (strncmp(buf, "MemTotal:", 9) == 0) {
      sscanf(buf, "%*s %u", &info.memmax);
    }
    else if (strncmp(buf, "MemFree:", 8) == 0) {
      sscanf(buf, "%*s %u", &info.mem);
    }
    else if (strncmp(buf, "SwapTotal:", 10) == 0) {
      sscanf(buf, "%*s %u", &info.swapmax);
    }
    else if (strncmp(buf, "SwapFree:", 9) == 0) {
      sscanf(buf, "%*s %u", &info.swap);
    }
    else if (strncmp(buf, "Buffers:", 8) == 0) {
      sscanf(buf, "%*s %u", &info.buffers);
    }
    else if (strncmp(buf, "Cached:", 7) == 0) {
      sscanf(buf, "%*s %u", &info.cached);
    }
  }

  info.mem = info.memmax - info.mem;
  info.swap = info.swapmax - info.swap;

  info.bufmem = info.cached + info.buffers;

  info.mask |= (1 << INFO_MEM) | (1 << INFO_BUFFERS);
}

static FILE *net_dev_fp;

void update_net_stats() {
  static int rep;
  unsigned int i;
  char buf[256];
  double delta;

  /* get delta */
  delta = current_update_time - last_update_time;
  if (delta <= 0.0001) return;

  /* open file and ignore first two lines */
  if (net_dev_fp == NULL)
    net_dev_fp = open_file("/proc/net/dev", &rep);
  else
    fseek(net_dev_fp, 0, SEEK_SET);
  if (!net_dev_fp) return;

  fgets(buf, 255, net_dev_fp); /* garbage */
  fgets(buf, 255, net_dev_fp); /* garbage (field names) */

  /* read each interface */
  for (i=0; i<16; i++) {
    struct net_stat *ns;
    char *s, *p;
    long long r, t, last_recv, last_trans;

    if (fgets(buf, 255, net_dev_fp) == NULL) break;
    p = buf;
    while (isspace((int) *p)) p++;

    s = p;

    while (*p && *p != ':') p++;
    if (*p == '\0') continue;
    *p = '\0';
    p++;

    ns = get_net_stat(s);
    ns->up = 1;
    last_recv = ns->recv;
    last_trans = ns->trans;

    sscanf(p,
    /* bytes packets errs drop fifo frame compressed multicast|bytes ... */
       "%Ld  %*d     %*d  %*d  %*d  %*d   %*d        %*d       %Ld",
        &r,                                                    &t);

    /* if recv or trans is less than last time, an overflow happened */

    if (r < ns->last_read_recv)
      ns->recv += ((long long) 4294967295U - ns->last_read_recv) + r;
    else
      ns->recv += (r - ns->last_read_recv);
    ns->last_read_recv = r;

    if (t < ns->last_read_trans)
      ns->trans += ((long long) 4294967295U - ns->last_read_trans) + t;
    else
      ns->trans += (t - ns->last_read_trans);
    ns->last_read_trans = t;

    /* calculate speeds */
    ns->recv_speed = (ns->recv - last_recv) / delta;
    ns->trans_speed = (ns->trans - last_trans) / delta;
  }

  /* fclose(net_dev_fp); net_dev_fp = NULL; */
}

void update_total_processes() {
  update_sysinfo();
}

static unsigned int cpu_user, cpu_system, cpu_nice;
static double last_cpu_sum;
static int clock_ticks;

static FILE *stat_fp;

static void update_stat() {
  static int rep;
  char buf[256];

  if (stat_fp == NULL)
    stat_fp = open_file("/proc/stat", &rep);
  else
    fseek(stat_fp, 0, SEEK_SET);
  if (stat_fp == NULL) return;

  info.cpu_count = 0;

  while (!feof(stat_fp)) {
    if (fgets(buf, 255, stat_fp) == NULL)
      break;

    if (strncmp(buf, "procs_running ", 14) == 0) {
      sscanf(buf, "%*s %d", &info.run_procs);
      info.mask |= (1 << INFO_RUN_PROCS);
    }
    else if (strncmp(buf, "cpu ", 4) == 0) {
      sscanf(buf, "%*s %u %u %u", &cpu_user, &cpu_nice, &cpu_system);
      info.mask |= (1 << INFO_CPU);
    }
    else if (strncmp(buf, "cpu", 3) == 0 && isdigit(buf[3])) {
      info.cpu_count++;
    }
  }

  {
    double delta;
    delta = current_update_time - last_update_time;
    if (delta <= 0.001) return;

    if (clock_ticks == 0)
      clock_ticks = sysconf(_SC_CLK_TCK);

    info.cpu_usage = (cpu_user+cpu_nice+cpu_system - last_cpu_sum) / delta
      / (double) clock_ticks / info.cpu_count;
    last_cpu_sum = cpu_user+cpu_nice+cpu_system;
  }
}

void update_running_processes() {
  update_stat();
}

void update_cpu_usage() {
  update_stat();
}

void update_load_average() {
#ifdef HAVE_GETLOADAVG
  double v[3];
  getloadavg(v, 3);
  info.loadavg[0] = (float) v[0];
  info.loadavg[1] = (float) v[1];
  info.loadavg[2] = (float) v[2];
#else
  static int rep;
  FILE *fp;

  fp = open_file("/proc/loadavg", &rep);
  if (!fp) {
    v[0] = v[1] = v[2] = 0.0;
    return;
  }

  fscanf(fp, "%f %f %f", &info.loadavg[0], &info.loadavg[1], &info.loadavg[2]);

  fclose(fp);
#endif
}


static int no_dots(const struct dirent *d) {
  if(d->d_name[0] == '.') return 0;
  return 1;
}

static int get_first_file_in_a_directory(const char *dir, char *s, int *rep) {
  struct dirent **namelist;
  int i, n;

  n = scandir(dir, &namelist, no_dots, alphasort);
  if (n < 0) {
    if (!rep || !*rep) {
      ERR("scandir for %s: %s", dir, strerror(errno));
      if (rep) *rep = 1;
    }
    return 0;
  }
  else {
    if (n == 0) return 0;

    strncpy(s, namelist[0]->d_name, 255);
    s[255] = '\0';

    for (i=0; i<n; i++)
      free(namelist[i]);
    free(namelist);

    return 1;
  }
}

#define I2C_DIR "/sys/bus/i2c/devices/"

int open_i2c_sensor(const char *dev, const char *type, int n, int *div) {
  char path[256];
  char buf[64];
  int fd;
  int divfd;

  /* if i2c device is NULL or *, get first */
  if (dev == NULL || strcmp(dev, "*") == 0) {
    static int rep;
    if (!get_first_file_in_a_directory(I2C_DIR, buf, &rep))
      return -1;
    dev = buf;
  }

  /* change vol to in */
  if (strcmp(type, "vol") == 0)
    type = "in";

  snprintf(path, 255, I2C_DIR "%s/%s%d_input", dev, type, n);

  /* open file */
  fd = open(path, O_RDONLY);
  if (fd < 0) ERR("can't open '%s': %s", path, strerror(errno));

  if (strcmp(type, "in") == 0 || strcmp(type, "temp") == 0)
    *div = 1;
  else
    *div = 0;

  /* test if *_div file exist, open it and use it as divisor */
  snprintf(path, 255, I2C_DIR "%s/%s%d_div", dev, type, n);

  divfd = open(path, O_RDONLY);
  if (divfd > 0) {
    /* read integer */
    char divbuf[64];
    unsigned int divn;
    divn = read(divfd, divbuf, 63);
    /* should read until n == 0 but I doubt that kernel will give these
     * in multiple pieces. :) */
    divbuf[divn] = '\0';
    *div = atoi(divbuf);
  }

  close(divfd);

  return fd;
}

double get_i2c_info(int fd, int div) {
  int val = 0;

  if (fd <= 0) return 0;

  lseek(fd, 0, SEEK_SET);

  /* read integer */
  {
    char buf[64];
    unsigned int n;
    n = read(fd, buf, 63);
    /* should read until n == 0 but I doubt that kernel will give these
     * in multiple pieces. :) */
    buf[n] = '\0';
    val = atoi(buf);
  }

  /* divide voltage and temperature by 1000 */
  /* or if any other divisor is given, use that */ 
  if (div > 1)
    return val / div;
  else if (div)
    return val / 1000.0;
  else
    return val;
}

#define ADT746X_FAN "/sys/devices/temperatures/cpu_fan_speed"

static char *adt746x_fan_state;

char* get_adt746x_fan() {
  static int rep;
  FILE *fp;

  if (adt746x_fan_state == NULL) {
    adt746x_fan_state = (char*)malloc(100);
    assert(adt746x_fan_state!=NULL);
  }

  fp = open_file(ADT746X_FAN, &rep);
  if (!fp) {
    strcpy(adt746x_fan_state,"No fan found! Hey, you don't have one?");
    return adt746x_fan_state;
  }
  fscanf(fp, "%s", adt746x_fan_state);
  fclose(fp);

  return adt746x_fan_state;
}

#define ADT746X_CPU "/sys/devices/temperatures/cpu_temperature"

static char *adt746x_cpu_state;

char* get_adt746x_cpu() {
  static int rep;
  FILE *fp;

  if (adt746x_cpu_state == NULL) {
    adt746x_cpu_state = (char*)malloc(100);
    assert(adt746x_cpu_state!=NULL);
  }

  fp = open_file(ADT746X_CPU, &rep);
  fscanf(fp, "%2s", adt746x_cpu_state);
  fclose(fp);

  return adt746x_cpu_state;
}

static char *frequency;

char* get_freq()
{
    FILE *f;
    char s[1000];
    if (frequency == NULL) {
    	frequency = (char*)malloc(100);
    	assert(frequency!=NULL);
    }
    //char frequency[10];
    f=fopen("/proc/cpuinfo","r"); //open the CPU information file
    //if (!f)
    //    return;
    while (fgets(s,1000,f)!=NULL)  //read the file
        if ( strncmp(s, "cpu M", 5)==0 ) { //and search for the cpu mhz
                //printf("%s", strchr(s, ':')+2);
                strcpy(frequency,strchr(s, ':')+2); //copy just the number
                frequency[strlen(frequency)-1] = '\0'; // strip \n
                break;
        }
    fclose(f);
    return frequency;
}

#define ACPI_FAN_DIR "/proc/acpi/fan/"

static char *acpi_fan_state;

char* get_acpi_fan() {
  static int rep;
  char buf[256];
  char buf2[256];
  FILE *fp;

  if (acpi_fan_state == NULL) {
    acpi_fan_state = (char*)malloc(100);
    assert(acpi_fan_state!=NULL);
  }

  /* yeah, slow... :/ */
  if (!get_first_file_in_a_directory(ACPI_FAN_DIR, buf, &rep))
    return "no fans?";

  snprintf(buf2, 256, "%s%s/state", ACPI_FAN_DIR, buf);

  fp = open_file(buf2, &rep);
  if (!fp) {
    strcpy(acpi_fan_state, "can't open fan's state file");
    return acpi_fan_state;
  }
  fscanf(fp, "%*s %99s", acpi_fan_state);

  return acpi_fan_state;
}

#define ACPI_AC_ADAPTER_DIR "/proc/acpi/ac_adapter/"

static char *acpi_ac_adapter_state;

char* get_acpi_ac_adapter() {
  static int rep;
  char buf[256];
  char buf2[256];
  FILE *fp;

  if (acpi_ac_adapter_state == NULL) {
    acpi_ac_adapter_state = (char*)malloc(100);
    assert(acpi_ac_adapter_state!=NULL);
  }

  /* yeah, slow... :/ */
  if (!get_first_file_in_a_directory(ACPI_AC_ADAPTER_DIR, buf, &rep))
    return "no ac_adapters?";

  snprintf(buf2, 256, "%s%s/state", ACPI_AC_ADAPTER_DIR, buf);

  fp = open_file(buf2, &rep);
  if (!fp) {
    strcpy(acpi_ac_adapter_state,"No ac adapter found.... where is it?");
    return acpi_ac_adapter_state;
  }
  fscanf(fp, "%*s %99s", acpi_ac_adapter_state);
  fclose(fp);

  return acpi_ac_adapter_state;
}

/*
/proc/acpi/thermal_zone/THRM/cooling_mode
cooling mode:            active
/proc/acpi/thermal_zone/THRM/polling_frequency
<polling disabled>
/proc/acpi/thermal_zone/THRM/state
state:                   ok
/proc/acpi/thermal_zone/THRM/temperature
temperature:             45 C
/proc/acpi/thermal_zone/THRM/trip_points
critical (S5):           73 C
passive:                 73 C: tc1=4 tc2=3 tsp=40 devices=0xcdf6e6c0
*/

#define ACPI_THERMAL_DIR "/proc/acpi/thermal_zone/"
#define ACPI_THERMAL_FORMAT "/proc/acpi/thermal_zone/%s/temperature"

int open_acpi_temperature(const char *name) {
  char path[256];
  char buf[64];
  int fd;

  if (name == NULL || strcmp(name, "*") == 0) {
    static int rep;
    if (!get_first_file_in_a_directory(ACPI_THERMAL_DIR, buf, &rep))
      return -1;
    name = buf;
  }

  snprintf(path, 255, ACPI_THERMAL_FORMAT, name);

  fd = open(path, O_RDONLY);
  if (fd < 0) ERR("can't open '%s': %s", path, strerror(errno));

  return fd;
}

static double last_acpi_temp;
static double last_acpi_temp_time;

double get_acpi_temperature(int fd) {
  if (fd <= 0) return 0;

  /* don't update acpi temperature too often */
  if (current_update_time - last_acpi_temp_time < 11.32) {
    return last_acpi_temp;
  }
  last_acpi_temp_time = current_update_time;

  /* seek to beginning */
  lseek(fd, 0, SEEK_SET);

  /* read */
  {
    char buf[256];
    int n;
    n = read(fd, buf, 255);
    if (n < 0) ERR("can't read fd %d: %s", fd, strerror(errno));
    else {
      buf[n] = '\0';
      sscanf(buf, "temperature: %lf", &last_acpi_temp);
    }
  }

  return last_acpi_temp;
}

/*
hipo@lepakko hipo $ cat /proc/acpi/battery/BAT1/info 
present:                 yes
design capacity:         4400 mAh
last full capacity:      4064 mAh
battery technology:      rechargeable
design voltage:          14800 mV
design capacity warning: 300 mAh
design capacity low:     200 mAh
capacity granularity 1:  32 mAh
capacity granularity 2:  32 mAh
model number:            02KT
serial number:           16922
battery type:            LION
OEM info:                SANYO
*/

/*
hipo@lepakko torsmo $ cat /proc/acpi/battery/BAT1/state
present:                 yes
capacity state:          ok
charging state:          unknown
present rate:            0 mA
remaining capacity:      4064 mAh
present voltage:         16608 mV
*/

/*
2213<@jupet�kellari��> jupet@lagi-unstable:~$ cat /proc/apm 
2213<@jupet�kellari��> 1.16 1.2 0x03 0x01 0xff 0x10 -1% -1 ?
2213<@jupet�kellari��> (-1 ollee ei akkua kiinni, koska akku on p�yd�ll�)
2214<@jupet�kellari��> jupet@lagi-unstable:~$ cat /proc/apm 
2214<@jupet�kellari��> 1.16 1.2 0x03 0x01 0x03 0x09 98% -1 ?

2238<@jupet�kellari��> 1.16 1.2 0x03 0x00 0x00 0x01 100% -1 ? ilman verkkovirtaa
2239<@jupet�kellari��> 1.16 1.2 0x03 0x01 0x00 0x01 99% -1 ? verkkovirralla

2240<@jupet�kellari��> 1.16 1.2 0x03 0x01 0x03 0x09 100% -1 ? verkkovirralla ja monitori p��ll�
2241<@jupet�kellari��> 1.16 1.2 0x03 0x00 0x00 0x01 99% -1 ? monitori p��ll� mutta ilman verkkovirtaa
*/

#define ACPI_BATTERY_BASE_PATH "/proc/acpi/battery"
#define APM_PATH "/proc/apm"

static FILE *acpi_bat_fp;
static FILE *apm_bat_fp;

static int acpi_last_full;

static char last_battery_str[64];

static double last_battery_time;

void get_battery_stuff(char *buf, unsigned int n, const char *bat) {
  static int rep, rep2;
  char acpi_path[128];
  snprintf(acpi_path, 127, ACPI_BATTERY_BASE_PATH "/%s/state", bat);

  /* don't update battery too often */
  if (current_update_time - last_battery_time < 29.5) {
    snprintf(buf, n, "%s", last_battery_str);
    return;
  }
  last_battery_time = current_update_time;

  /* first try ACPI */

  if (acpi_bat_fp == NULL && apm_bat_fp == NULL)
    acpi_bat_fp = open_file(acpi_path, &rep);

  if (acpi_bat_fp != NULL) {
    int present_rate = -1;
    int remaining_capacity = -1;
    char charging_state[64];

    /* read last full capacity if it's zero */
    if (acpi_last_full == 0) {
      static int rep;
      char path[128];
      FILE *fp;
      snprintf(path, 127, ACPI_BATTERY_BASE_PATH "/%s/info", bat);
      fp = open_file(path, &rep);
      if (fp != NULL) {
        while (!feof(fp)) {
          char b[256];
          if (fgets(b, 256, fp) == NULL) break;

          if (sscanf(b, "last full capacity: %d", &acpi_last_full) != 0)
            break;
        }

        fclose(fp);
      }
    }

    fseek(acpi_bat_fp, 0, SEEK_SET);

    strcpy(charging_state, "unknown");

    while (!feof(acpi_bat_fp)) {
      char buf[256];
      if (fgets(buf, 256, acpi_bat_fp) == NULL) break;

      /* let's just hope units are ok */
      if(buf[0] == 'c')
        sscanf(buf, "charging state: %63s", charging_state);
      else if(buf[0] == 'p')
        sscanf(buf, "present rate: %d", &present_rate);
      else if(buf[0] == 'r')
        sscanf(buf, "remaining capacity: %d", &remaining_capacity);
    }

    /* charging */
    if (strcmp(charging_state, "charging") == 0) {
      if (acpi_last_full != 0 && present_rate > 0) {
        strcpy(last_battery_str, "charging ");
        format_seconds(last_battery_str+9, 63-9,
            (acpi_last_full - remaining_capacity) * 60 * 60 / present_rate);
      }
      else if (acpi_last_full != 0 && present_rate <= 0) {
        sprintf(last_battery_str, "charging %d%%",
            remaining_capacity * 100 / acpi_last_full);
      }
      else {
        strcpy(last_battery_str, "charging");
      }
    }
    /* discharging */
    else if (strcmp(charging_state, "discharging") == 0) {
      if (present_rate > 0)
        format_seconds(last_battery_str, 63,
            (remaining_capacity * 60 * 60) / present_rate);
      else
        sprintf(last_battery_str, "discharging %d%%",
            remaining_capacity * 100 / acpi_last_full);
    }
    /* unknown, probably full / AC */
    else {
      if (acpi_last_full != 0 && remaining_capacity != acpi_last_full)
        sprintf(last_battery_str, "unknown %d%%",
            remaining_capacity * 100 / acpi_last_full);
      else
        strcpy(last_battery_str, "AC");
    }
  }
  else {
    /* APM */
    if (apm_bat_fp == NULL)
      apm_bat_fp = open_file(APM_PATH, &rep2);

    if (apm_bat_fp != NULL) {
      int ac, status, flag, life;

      fscanf(apm_bat_fp, "%*s %*s %*x %x   %x       %x     %d%%",
                                      &ac, &status, &flag, &life);

      if (life == -1) {
        /* could check now that there is ac */
        snprintf(last_battery_str, 64, "AC");
      }
      else if (ac && life != 100) { /* could check that status==3 here? */
        snprintf(last_battery_str, 64, "charging %d%%", life);
      }
      else {
        snprintf(last_battery_str, 64, "%d%%", life);
      }

      /* it seemed to buffer it so file must be closed (or could use syscalls
       * directly but I don't feel like coding it now) */
      fclose(apm_bat_fp);
      apm_bat_fp = NULL;
    }
  }

  snprintf(buf, n, "%s", last_battery_str);
}
