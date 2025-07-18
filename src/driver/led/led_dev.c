#include <driver/led/led_dev.h>
#include <zephyr/drivers/led.h>

LOG_MODULE_REGISTER(led_dev, LOG_LEVEL_ERR);

#define MAX_LED_DEVICES 10 /* Max number of LED devices */

/* A global index variable used to iterate over entries */
static int led_index = 0;

struct led_file_data {
  const struct device *led_dev; // LED device handle
  uint32_t channel;             // LED channel number
  atomic_t opened;              // Open the status flag
  const char *name;             // LED name
  const char *current_attr;     // Properties of the current operation

  uint8_t brightness;     // Current brightness value (0-100)
  uint8_t max_brightness; // Maximum brightness value (100)

  uint32_t delay_on;   // Blink on time (ms)
  uint32_t delay_off;  // Blink off time (ms)
  bool trigger_active; // Triggers the mode activation flag

  struct k_timer blink_timer; // Blinking timer
  bool is_led_on;             // Current LED status
};

struct led_device_info {
  const struct device *dev; // LED device handle
  const char *label;        // LED labels in the device tree
  atomic_t opened;          // Open the status flag
};

typedef enum {
  LED_DIR_TYPE_ROOT, // Root directory type
  LED_DIR_TYPE_LED,  // LED subdirectory type
} led_dir_type_t;

typedef struct {
  led_dir_type_t type; // Directory type
  size_t led_index;    // Current LED index
  size_t attr_index;   // Current property index
} led_dir_ctx_t;
/* Extract LED information from the device tree */
#define DT_DRV_COMPAT gpio_leds
#define LED_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(LED_NUM <= MAX_LED_DEVICES, "Exceeded maximum LED device count");

#define LED_DEVICE_PROCESS(child)                                              \
  {                                                                            \
      .dev = DEVICE_DT_GET(DT_NODELABEL(leds)),                                \
      .label = DT_PROP(child, label),                                          \
      .opened = ATOMIC_INIT(0),                                                \
  },

/* List of LED devices (automatically generated from the device tree) */
struct led_device_info led_devices[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(leds), LED_DEVICE_PROCESS)};

static struct led_file_data led_instances[ARRAY_SIZE(led_devices)];

/* List of LED properties */
static const char *led_attrs[] = {"max_brightness", "brightness", "trigger",
                                  "delay_on",       "delay_off",  NULL};

/* Print all found LED devices */
void print_leds(void) {
  size_t count = ARRAY_SIZE(led_devices);
  printk("[LED_DEV] Found %zu LEDs:\n", count);
  for (size_t i = 0; i < count; i++) {
    if (led_devices[i].dev && device_is_ready(led_devices[i].dev)) {
      printk(" [LED_DEV] [%zu] %s\n", i, led_devices[i].label);
    }
  }
}

static struct fs_mount_t ledfs_mnt = {
    .type = FS_LED,
    .fs_data = &led_devices,
};

static int leddev_open(struct fs_file_t *zfp, const char *path,
                       fs_mode_t mode) {
  LOG_ERROR("[LED_DEV] Opening file: %s", path);

  static const size_t led_root_len = strlen("/sys/class/leds/");
  const char *led_root = "/sys/class/leds/";

  if (strncmp(path, led_root, led_root_len) != 0) {
    LOG_ERROR("[LED_DEV] Invalid path prefix");
    return -EINVAL;
  }

  /* parse path: /sys/class/leds/<led_name>/<attribute> */
  const char *after_leds = path + led_root_len;
  const char *slash = strchr(after_leds, '/');

  if (!slash || slash == after_leds || *(slash + 1) == '\0') {
    LOG_ERROR("[LED_DEV] Invalid path format");
    return -EINVAL;
  }

  const char *led_name = after_leds;
  const char *attr_name = slash + 1;
  size_t led_name_len = slash - after_leds;

  LOG_ERROR("[LED_DEV] Opening LED: %.*s, Attribute: %s", (int)led_name_len,
            led_name, attr_name);

  /* Find the matching LED device */
  for (size_t i = 0; i < ARRAY_SIZE(led_instances); i++) {
    struct led_file_data *led = &led_instances[i];

    if (strncmp(led->name, led_name, led_name_len) == 0 &&
        strlen(led->name) == led_name_len) {

      if (!led->led_dev || !device_is_ready(led->led_dev)) {
        LOG_ERROR("[LED_DEV] LED device not ready");
        return -ENODEV;
      }

      if (!attr_name || strlen(attr_name) == 0) {
        LOG_ERROR("[LED_DEV] Empty attribute name");
        return -EINVAL;
      }

      /* Checks if the property exists */
      for (int j = 0; led_attrs[j]; j++) {
        if (strcmp(attr_name, led_attrs[j]) == 0) {
          if (!atomic_cas(&led->opened, 0, 1)) {
            LOG_ERROR("[LED_DEV] Device already opened");
            return -EBUSY;
          }

          led->current_attr = attr_name;
          zfp->filep = led;
          LOG_ERROR("[LED_DEV] File opened successfully");
          return 0;
        }
      }

      LOG_ERROR("[LED_DEV] Attribute not found: %s", attr_name);
      return -ENOENT;
    }
  }

  LOG_ERROR("[LED_DEV] LED device not found: %.*s", (int)led_name_len,
            led_name);
  return -ENOENT;
}

static int leddev_close(struct fs_file_t *zfp) {
  if (!zfp) {
    LOG_ERROR("[LED_DEV] Invalid file pointer");
    return -EINVAL; // Assuming -EINVAL indicates invalid argument
  }

  LOG_ERROR("[LED_DEV] Closing file");

  if (zfp->filep) {
    struct led_file_data *led = zfp->filep;
    atomic_set(&led->opened, 0);
    LOG_ERROR("[LED_DEV] File closed successfully");
  }

  zfp->filep = NULL;
  return 0;
}

/* File Operations - Reading properties files */
static ssize_t leddev_read(struct fs_file_t *zfp, void *dest, size_t size) {
  LOG_ERROR("[LED_DEV] Reading attribute");

  if (!zfp->filep || !dest || size == 0) {
    LOG_ERROR("[LED_DEV] Invalid parameters");
    return -EINVAL;
  }

  struct led_file_data *led = zfp->filep;
  char *buf = (char *)dest;
  ssize_t len = 0;

  if (strcmp(led->current_attr, "brightness") == 0) {
    len = snprintf(buf, size, "%u\n", led->brightness);
  } else if (strcmp(led->current_attr, "max_brightness") == 0) {
    len = snprintf(buf, size, "%u\n", led->max_brightness);
  } else if (strcmp(led->current_attr, "trigger") == 0) {
    len = snprintf(buf, size, "%s\n", led->trigger_active ? "timer" : "none");
  } else if (strcmp(led->current_attr, "delay_on") == 0) {
    len = snprintf(buf, size, "%u\n", led->delay_on);
  } else if (strcmp(led->current_attr, "delay_off") == 0) {
    len = snprintf(buf, size, "%u\n", led->delay_off);
  } else {
    LOG_ERROR("[LED_DEV] Unsupported attribute");
    return -EINVAL;
  }

  if (len < 0 || len > size) {
    LOG_ERROR("[LED_DEV] Buffer too small");
    return -ENOSPC;
  }

  LOG_ERROR("[LED_DEV] Read %zd bytes", len);
  return len;
}

/* The flickering timer callback function */
static void blink_timer_handler(struct k_timer *timer) {
  LOG_ERROR("[LED_DEV] blink_timer_handler start");
  int ret = 0;
  if (!timer || !timer->user_data) {
    LOG_ERROR("[LED_DEV] Invalid timer data");
    return;
  }

  struct led_file_data *led = (struct led_file_data *)timer->user_data;

  /* Check device status, trigger mode, and brightness */
  if (!led->trigger_active || !led->led_dev || !device_is_ready(led->led_dev) ||
      led->brightness == 0) {
    LOG_ERROR("[LED_DEV] Blink stopped (device not ready/trigger "
              "inactive/brightness 0)");
    led->trigger_active = false;
    k_timer_stop(timer);
    /* Make sure the LED is off */
    led_off(led->led_dev, led->channel);
    led->is_led_on = false;
    return;
  }

  /* Switch the LED state and set the next trigger time */
  k_timer_stop(timer); // Ensure clean restart
  if (led->is_led_on) {
    /* Now on, switch to off */
    ret = led_off(led->led_dev, led->channel);
    if (ret != 0) {
      LOG_ERROR("[LED_DEV] brightness = 0, led on fail, ret=[%d]", ret);
    }
    led->is_led_on = false;
    /* Set the duration of the next firing (extinction) */
    k_timer_start(timer, K_MSEC(led->delay_off), K_NO_WAIT);
    LOG_ERROR("[LED_DEV] LED %s off, next trigger in %u ms", led->name,
              led->delay_off);
  } else {
    /* Now off, switch to on */
    ret = led_on(led->led_dev, led->channel);
    if (ret != 0) {
      LOG_ERROR("[LED_DEV] brightness = 0, led on fail, ret=[%d]", ret);
    }
    led->is_led_on = true;
    /* Set the next trigger (duration of light) */
    k_timer_start(timer, K_MSEC(led->delay_on), K_NO_WAIT);
    LOG_ERROR("[LED_DEV] LED %s on, next trigger in %u ms", led->name,
              led->delay_on);
  }
}

/* Set the LED trigger mode */
static int set_led_trigger(struct led_file_data *led, const char *trigger) {
  LOG_ERROR("[LED_DEV] set_led_trigger start");
  int ret = 0;
  if (!led->led_dev || !device_is_ready(led->led_dev)) {
    LOG_ERROR("[LED_DEV] LED device not ready");
    return -ENODEV;
  }

  if (strcmp(trigger, "timer") == 0) {
    /* Enable blinking mode (only if brightness is greater than 0) */
    if (led->brightness > 0) {
      led->trigger_active = true;
      /* The initial state is off */
      led->is_led_on = false;
      ret = led_off(led->led_dev, led->channel);
      if (ret != 0) {
        LOG_ERROR("[LED_DEV] brightness > 0, led off fail, ret=[%d]", ret);
      }
      k_timer_start(&led->blink_timer, K_MSEC(led->delay_on), K_NO_WAIT);
      LOG_ERROR("[LED_DEV] Blink started (on=%u ms, off=%u ms)", led->delay_on,
                led->delay_off);
    } else {
      LOG_ERROR("[LED_DEV] Blink not started: brightness is 0");
    }
    return 0;
  } else if (strcmp(trigger, "none") == 0) {
    /* Turn off the blink mode and set the state according to the brightness */
    led->trigger_active = false;
    k_timer_stop(&led->blink_timer);

    if (led->brightness > 0) {
      /* When the brightness is greater than 0, it is kept constant */
      ret = led_on(led->led_dev, led->channel);
      if (ret != 0) {
        LOG_ERROR("[LED_DEV] brightness > 0, led on fail, ret=[%d]", ret);
      }
      led->is_led_on = true;
      LOG_ERROR("[LED_DEV] Set to constant on (brightness=%u)",
                led->brightness);
    } else {
      /* When the brightness is equal to 0, it stays off */
      ret = led_off(led->led_dev, led->channel);
      if (ret != 0) {
        LOG_ERROR("[LED_DEV] brightness = 0, led off fail, ret=[%d]", ret);
      }
      led->is_led_on = false;
      LOG_ERROR("[LED_DEV] Set to constant off");
    }
    return 0;
  } else {
    LOG_ERROR("[LED_DEV] Unsupported trigger: %s", trigger);
    return -EINVAL;
  }
}

static ssize_t leddev_write(struct fs_file_t *zfp, const void *data,
                            size_t size) {
  LOG_ERROR("[LED_DEV] Writing attribute start");

  if (!zfp->filep || !data || size == 0) {
    LOG_ERROR("[LED_DEV] Writing Invalid parameters");
    return -EINVAL;
  }

  struct led_file_data *led = zfp->filep;
  char buf[32];

  if (size >= sizeof(buf)) {
    LOG_ERROR("[LED_DEV] Writing Data too large");
    return -EINVAL;
  }

  memcpy(buf, data, size);
  buf[size] = '\0';

  /* Remove newline characters */
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n') {
    buf[len - 1] = '\0';
  }

  int ret = 0;

  if (strcmp(led->current_attr, "brightness") == 0) {
    LOG_ERROR("[LED_DEV] Writing brightness start");
    char *endptr;
    unsigned long value = strtoul(buf, &endptr, 10);

    if (endptr == buf || *endptr != '\0' || value > led->max_brightness) {
      LOG_ERROR("[LED_DEV] Brightness value invalid or out of range");
      return -EINVAL;
    }

    led->brightness = (uint8_t)value;
    LOG_ERROR("[LED_DEV] Brightness value =[%lu]", value);
    if (led->led_dev == NULL) {
      LOG_ERROR("[LED_DEV] led->led_dev == NULL");
      return -EINVAL;
    }

    // Prevent integer overflow
    uint32_t scaled_value = (uint32_t)value * 100;
    if (scaled_value / 100 != value) { // Check for overflow
      LOG_ERROR("[LED_DEV] Value scaling caused overflow");
      return -EINVAL;
    }

    ret = led_set_brightness(led->led_dev, led->channel,
                             (uint8_t)(scaled_value / led->max_brightness));
    if (ret) {
      LOG_ERROR("[LED_DEV] Failed to set brightness: ret=[%d]", ret);
      return -EINVAL;
    }
    LOG_ERROR("[LED_DEV] Writing brightness end");
  } else if (strcmp(led->current_attr, "trigger") == 0) {
    LOG_ERROR("[LED_DEV] write set trigger: start");
    ret = set_led_trigger(led, buf);
    if (ret) {
      LOG_ERROR("[LED_DEV] Failed to set trigger: %d", ret);
      return -EINVAL;
    }
    LOG_ERROR("[LED_DEV] write set trigger end");
  } else if (strcmp(led->current_attr, "delay_on") == 0) {
    LOG_ERROR("[LED_DEV] write set delay_on start");
    char *endptr;
    led->delay_on = (uint32_t)strtoul(buf, &endptr, 10);
    if (endptr == buf || *endptr != '\0') {
      LOG_ERROR("[LED_DEV] Delay on value invalid");
      return -EINVAL;
    }
    LOG_ERROR("[LED_DEV] Set delay_on to %u ms", led->delay_on);
    if (led->trigger_active) {
      LOG_ERROR("[LED_DEV] write set delay_on timer");
      ret = set_led_trigger(led, "timer");
      if (ret) {
        LOG_ERROR("[LED_DEV] Failed to set trigger: %d", ret);
        return -EINVAL;
      }
    }
    LOG_ERROR("[LED_DEV] write set delay_on end");
  } else if (strcmp(led->current_attr, "delay_off") == 0) {
    LOG_ERROR("[LED_DEV] write set delay_off start");
    char *endptr;
    led->delay_off = (uint32_t)strtoul(buf, &endptr, 10);
    if (endptr == buf || *endptr != '\0') {
      LOG_ERROR("[LED_DEV] Delay off value invalid");
      return -EINVAL;
    }
    LOG_ERROR("[LED_DEV] Set delay_off to %u ms", led->delay_off);
    if (led->trigger_active) {
      LOG_ERROR("[LED_DEV] write set delay_off timer");
      ret = set_led_trigger(led, "timer");
      if (ret) {
        LOG_ERROR("[LED_DEV] Failed to set trigger: %d", ret);
        return -EINVAL;
      }
    }
    LOG_ERROR("[LED_DEV] write set delay_off end");
  } else {
    LOG_ERROR("[LED_DEV] write Unsupported attribute");
    return -EINVAL;
  }
  return size;
}

static int leddev_opendir(struct fs_dir_t *dp, const char *path) {
  size_t mnt_len = strlen(LED_MNT_POINT);

  /* Check if path matches LED_MNT_POINT or LED_MNT_POINT + '/' */
  if (strncmp(path, LED_MNT_POINT, mnt_len) != 0 ||
      (path[mnt_len] != '\0' && path[mnt_len] != '/')) {
    LOG_ERROR("[LED_DEV] Invalid directory path: %s", path);
    return -EINVAL;
  }

  dp->dirp = &led_index;
  led_index = 0;

  LOG_ERROR("[LED_DEV] Opened LED directory: %s", path);
  return 0;
}

static int leddev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry) {
  /* Get the current index pointer */
  int *index = dp->dirp;
  static int total_entries = -1;
  int current_entry = 0;

  /* Calculate total number of entries only once */
  if (total_entries == -1) {
    total_entries = 0;
    for (size_t i = 0; i < ARRAY_SIZE(led_instances); i++) {
      if (led_instances[i].led_dev &&
          device_is_ready(led_instances[i].led_dev)) {
        for (int j = 0; led_attrs[j]; j++) {
          total_entries++;
        }
      }
    }
  }

  /* End of traversal */
  if (*index >= total_entries) {
    memset(entry, 0, sizeof(struct fs_dirent));
    return 0;
  }

  /* Iterate over LED instances */
  for (size_t i = 0; i < ARRAY_SIZE(led_instances); i++) {
    struct led_file_data *led = &led_instances[i];

    if (!led->led_dev || !device_is_ready(led->led_dev)) {
      continue;
    }

    for (int j = 0; led_attrs[j]; j++) {
      if (current_entry == *index) {
        int len = snprintf(entry->name, sizeof(entry->name), "%s/%s", led->name,
                           led_attrs[j]);

        if (len >= (int)sizeof(entry->name)) {
          // Optional: log truncation warning
          LOG_ERROR("[LED_DEV] Entry name truncated: %s/%s", led->name,
                    led_attrs[j]);
        }

        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = 0;
        (*index)++;
        return 0;
      }
      current_entry++;
    }
  }

  /* Unexpected case: invalid index */
  memset(entry, 0, sizeof(struct fs_dirent));
  return 0;
}

static int leddev_closedir(struct fs_dir_t *dp) {
  LOG_ERROR("[LED_DEV] Closed LED directory");
  /* Resetting the index */
  led_index = 0;
  if (dp->dirp) {
    dp->dirp = NULL;
  }
  return 0;
}

#define LED_ATTR_FILE_SIZE 4

static int leddev_stat(struct fs_mount_t *mountp, const char *path,
                       struct fs_dirent *entry) {
  ARG_UNUSED(mountp);
  LOG_ERROR("[LED_DEV] Stat path: %s", path);

  /* Handle the directory itself (/sys/class/leds and /sys/class/leds/) */
  if (strcmp(path, LED_MNT_POINT) == 0 ||
      strcmp(path, LED_MNT_POINT "/") == 0) {
    entry->type = FS_DIR_ENTRY_DIR;
    entry->size = 0;
    return 0;
  }

  /* Resolve root path prefix */
  const char *led_root_slash = LED_MNT_POINT "/";
  size_t led_root_len = strlen(led_root_slash);

  if (strncmp(path, led_root_slash, led_root_len) != 0) {
    LOG_ERROR("[LED_DEV] Path does not start with %s", led_root_slash);
    return -EINVAL;
  }

  const char *after_leds = path + led_root_len;
  const char *slash = strchr(after_leds, '/');
  if (!slash || slash == after_leds) { // Prevent empty led_name
    LOG_ERROR("[LED_DEV] Invalid path format");
    return -EINVAL;
  }

  const char *led_name = after_leds;
  const char *attr_name = slash + 1;
  size_t led_name_len = slash - after_leds;

  /* Find matching LED devices and attributes */
  for (size_t i = 0; i < ARRAY_SIZE(led_instances); i++) {
    struct led_file_data *led = &led_instances[i];

    if (led->name == NULL || led->led_dev == NULL ||
        !device_is_ready(led->led_dev)) {
      continue;
    }

    if (strlen(led->name) != led_name_len) {
      continue;
    }

    if (strncmp(led->name, led_name, led_name_len) != 0) {
      continue;
    }

    for (int j = 0; led_attrs[j]; j++) {
      if (strcmp(attr_name, led_attrs[j]) == 0) {
        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = LED_ATTR_FILE_SIZE;
        return 0;
      }
    }

    return -ENOENT;
  }

  return -ENOENT;
}

/* Configuration constants */
#define LED_DEFAULT_MAX_BRIGHTNESS 100
#define LED_DEFAULT_DELAY_ON_MS 500
#define LED_DEFAULT_DELAY_OFF_MS 500

static int leddev_mount(struct fs_mount_t *mountp) {
  LOG_ERROR("[LED_DEV] LED filesystem mounting start...");

  if (mountp->mnt_point == NULL) {
    LOG_ERROR("[LED_DEV] Mount point is NULL");
    return -EINVAL;
  }

  /* Initialize all LED devices */
  const size_t device_count = ARRAY_SIZE(led_devices);
  LOG_ERROR("[LED_DEV] device_count[%zu]", device_count);

  for (size_t i = 0; i < device_count; i++) {
    struct led_file_data *led = &led_instances[i];
    struct led_device_info *dev_info = &led_devices[i];

    if (dev_info->dev == NULL) {
      LOG_ERROR("[LED_DEV] led_devices[%zu].dev == NULL, skipping", i);
      continue;
    }

    if (!device_is_ready(dev_info->dev)) {
      LOG_ERROR("[LED_DEV] LED%zu device not ready: %s", i, dev_info->label);
      continue;
    }

    *led = (struct led_file_data){
        .led_dev = dev_info->dev,
        .name = dev_info->label,
        .channel = 0,
        .max_brightness = LED_DEFAULT_MAX_BRIGHTNESS,
        .brightness = 0,
        .delay_on = LED_DEFAULT_DELAY_ON_MS,
        .delay_off = LED_DEFAULT_DELAY_OFF_MS,
        .trigger_active = false,
        .is_led_on = false,
    };

    /* Re-initialize only if necessary in production code */
    k_timer_init(&led->blink_timer, blink_timer_handler, NULL);
    led->blink_timer.user_data = led;

    LOG_ERROR("[LED_DEV] Initialized LED: %s", led->name);
  }

  LOG_ERROR("[LED_DEV] LED filesystem mounted successfully end");
  return 0;
}

static int leddev_unmount(struct fs_mount_t *mountp) {
  LOG_ERROR("[LED_DEV] leddev_unmount start");
  int ret = 0;
  if (mountp->mnt_point == NULL) {
    LOG_ERROR("[LED_DEV] Invalid mount point");
    return -EINVAL;
  }

  /* Turn off all leds and stop the timer */
  for (size_t i = 0; i < ARRAY_SIZE(led_instances); i++) {
    struct led_file_data *led = &led_instances[i];
    LOG_ERROR("[LED_DEV] leddev_unmount i=[%d]", i);
    if (led->led_dev && device_is_ready(led->led_dev)) {
      led->trigger_active = false;
      k_timer_stop(&led->blink_timer);
      if (led->led_dev == NULL) {
        LOG_ERROR("[LED_DEV] led->led_dev == NULL");
        return -EINVAL;
      }
      ret = led_off(led->led_dev, led->channel);
      if (ret) {
        LOG_ERROR("[LED_DEV] Failed to set off");
        return -EINVAL;
      }
      LOG_ERROR("[LED_DEV] Turned off LED: %s", led->name);
    }
  }
  LOG_ERROR("[LED_DEV] leddev_unmount successfully end ");
  return 0;
}

/* led file system interface */
static const struct fs_file_system_t leddev_fs = {
    .mount = leddev_mount,
    .unmount = leddev_unmount,
    .open = leddev_open,
    .close = leddev_close,
    .read = leddev_read,
    .write = leddev_write,
    .lseek = NULL,
    .opendir = leddev_opendir,
    .readdir = leddev_readdir,
    .closedir = leddev_closedir,
    .stat = leddev_stat,
};

/* Initialization functions */
static int led_dev_init(void) {
  LOG_ERROR("[LED_DEV] led_dev_init start");
  LOG_ERROR("[LED_DEV] MAX_LED_DEVICES = %d", MAX_LED_DEVICES);

  for (size_t i = 0; i < ARRAY_SIZE(led_devices); i++) {
    LOG_ERROR("[LED_DEV] LED[%zu]:", i);
    if (led_devices[i].label) {
      LOG_ERROR("  [LED_DEV] - Label: %s", led_devices[i].label);
    } else {
      LOG_ERROR("  [LED_DEV] - No label assigned");
    }
  }

  ledfs_mnt.mnt_point = LED_MNT_POINT;
  LOG_ERROR("[LED_DEV] ledfs_mnt: type: %d, mnt_point: %s, fs_data: %p\n",
            ledfs_mnt.type, ledfs_mnt.mnt_point, ledfs_mnt.fs_data);

  LOG_ERROR("[LED_DEV] Mounting LED filesystem at '%s'", ledfs_mnt.mnt_point);

  int rc = fs_mount(&ledfs_mnt);
  if (rc != 0) {
    switch (rc) {
    case -EINVAL:
      LOG_ERROR("[LED_DEV] Mount failed: invalid parameters");
      break;
    case -ENOENT:
      LOG_ERROR("[LED_DEV] Mount failed: mount point not found");
      break;
    default:
      LOG_ERROR("[LED_DEV] Mount failed with error code: %d", rc);
      break;
    }

    ledfs_mnt.mnt_point = NULL;
    return rc;
  }

  LOG_ERROR("[LED_DEV] LED filesystem led_dev_init successfully");
  return 0;
}

static int led_init(void) {
  LOG_ERROR("[LED_DEV] led_init start\n");

  int rc = fs_register(FS_LED, &leddev_fs);
  if (rc < 0) {
    LOG_ERROR("[LED_DEV] led_init register error (%d)\n", rc);
    return rc;
  } else {
    LOG_ERROR("[LED_DEV] led_init fs registered\n");
  }

  rc = led_dev_init();
  if (rc != 0) {
    LOG_ERROR("[LED_DEV] led_init init failed\n");
    return rc;
  }

  LOG_ERROR("[LED_DEV] led_init----------end\n");
  return rc;
}

// void led_main(void)
// {
//     LOG_ERROR("[LED_DEV] led_main----------start\n");

//     print_leds();

//     led_init();

//     LOG_ERROR("[LED_DEV] led_main----------end\n");
// }

/* System initialization */
SYS_INIT(led_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);