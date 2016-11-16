#include "torsmo.h"
#include "NVCtrl.h"
#include "NVCtrlLib.h"

/* nvctrl */
unsigned int init_nvctrl(const char *feat) {
  if (strcmp(feat, "temp") == 0) 
    return NV_CTRL_GPU_CORE_TEMPERATURE;
  else if (strcmp(feat, "ambient") == 0)
    return NV_CTRL_AMBIENT_TEMPERATURE;
  else
    ERR("Unknown parameter %s for nvctrl", feat);

  return 0;
}

int get_nvctrl_info(unsigned int arg) {
    if(arg == 0) return -1;
    
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        ERR("Cannot open display '%s'.", XDisplayName(NULL));
        return -1;
    }
    
    int screen = DefaultScreen(dpy);

    if (!XNVCTRLIsNvScreen(dpy, screen)) {
        ERR("The NV-CONTROL X not available on screen %d of '%s'.", screen, XDisplayName(NULL));
        return -1;
    }

    int display_devices;
    Bool ret = XNVCTRLQueryAttribute(dpy, screen, 0, NV_CTRL_ENABLED_DISPLAYS, &display_devices);
    if (!ret) {
        ERR("Unable to determine enabled display devices for screen %d of '%s'\n", screen, XDisplayName(NULL));
        return -1;
    }

    // get temp for first display found
    int mask;
    for (mask = 1; mask < (1<<24); mask <<= 1) {
        if (!(mask & display_devices)) continue;
        
        int retval;
        ret = XNVCTRLQueryAttribute(dpy, screen, mask, arg, &retval);

        return (ret ? retval : -1);
    }
    
    return -1;
}
