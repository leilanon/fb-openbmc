/*
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This file contains code to support IPMI2.0 Specificaton available @
 * http://www.intel.com/content/www/us/en/servers/ipmi/ipmi-specifications.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <openbmc/kv.h>
#include <openbmc/libgpio.h>
#include <openbmc/nm.h>
#include <facebook/fbal_fruid.h>
#include "pal.h"

#define FBAL_PLATFORM_NAME "angelslanding"
#define LAST_KEY "last_key"

#define GPIO_LOCATE_LED "FP_LOCATE_LED"
#define GPIO_FAULT_LED "FP_FAULT_LED_N"
#define GPIO_NIC0_PRSNT "HP_LVC3_OCP_V3_1_PRSNT2_N"
#define GPIO_NIC1_PRSNT "HP_LVC3_OCP_V3_2_PRSNT2_N"

#define GUID_SIZE 16
#define OFFSET_SYS_GUID 0x17F0
#define OFFSET_DEV_GUID 0x1800

const char pal_fru_list[] = "all, mb, nic0, nic1, pdb, bmc";
const char pal_server_list[] = "mb";

static int key_func_por_policy (int event, void *arg);
static int key_func_lps (int event, void *arg);

enum key_event {
  KEY_BEFORE_SET,
  KEY_AFTER_INI,
};

struct pal_key_cfg {
  char *name;
  char *def_val;
  int (*function)(int, void*);
} key_cfg[] = {
  /* name, default value, function */
  {"pwr_server_last_state", "on", key_func_lps},
  {"sysfw_ver_server", "0", NULL},
  {"identify_sled", "off", NULL},
  {"timestamp_sled", "0", NULL},
  {"server_por_cfg", "lps", key_func_por_policy},
  {"server_sensor_health", "1", NULL},
  {"nic_sensor_health", "1", NULL},
  {"server_sel_error", "1", NULL},
  {"server_boot_order", "0100090203ff", NULL},
  {"ntp_server", "", NULL},
  /* Add more Keys here */
  {LAST_KEY, LAST_KEY, NULL} /* This is the last key of the list */
};

static int
pal_key_index(char *key) {

  int i;

  i = 0;
  while(strcmp(key_cfg[i].name, LAST_KEY)) {

    // If Key is valid, return success
    if (!strcmp(key, key_cfg[i].name))
      return i;

    i++;
  }

#ifdef DEBUG
  syslog(LOG_WARNING, "pal_key_index: invalid key - %s", key);
#endif
  return -1;
}

int
pal_get_key_value(char *key, char *value) {
  int index;

  // Check is key is defined and valid
  if ((index = pal_key_index(key)) < 0)
    return -1;

  return kv_get(key, value, NULL, KV_FPERSIST);
}

int
pal_set_key_value(char *key, char *value) {
  int index, ret;
  // Check is key is defined and valid
  if ((index = pal_key_index(key)) < 0)
    return -1;

  if (key_cfg[index].function) {
    ret = key_cfg[index].function(KEY_BEFORE_SET, value);
    if (ret < 0)
      return ret;
  }

  return kv_set(key, value, 0, KV_FPERSIST);
}

static int
fw_getenv(char *key, char *value) {
  char cmd[MAX_KEY_LEN + 32] = {0};
  char *p;
  FILE *fp;

  sprintf(cmd, "/sbin/fw_printenv -n %s", key);
  fp = popen(cmd, "r");
  if (!fp) {
    return -1;
  }
  if (fgets(value, MAX_VALUE_LEN, fp) == NULL) {
    pclose(fp);
    return -1;
  }
  for (p = value; *p != '\0'; p++) {
    if (*p == '\n' || *p == '\r') {
      *p = '\0';
      break;
    }
  }
  pclose(fp);
  return 0;
}

static int
fw_setenv(char *key, char *value) {
  char old_value[MAX_VALUE_LEN] = {0};
  if (fw_getenv(key, old_value) != 0 ||
      strcmp(old_value, value) != 0) {
    /* Set the env key:value if either the key
     * does not exist or the value is different from
     * what we want set */
    char cmd[MAX_VALUE_LEN] = {0};
    snprintf(cmd, MAX_VALUE_LEN, "/sbin/fw_setenv %s %s", key, value);
    return system(cmd);
  }
  return 0;
}

static int
key_func_por_policy(int event, void *arg) {
  char value[MAX_VALUE_LEN] = {0};
  int ret = 0;

  switch (event) {
    case KEY_BEFORE_SET:
      if (strcmp((char *)arg, "lps") && strcmp((char *)arg, "on") && strcmp((char *)arg, "off"))
        return -1;

      if (pal_is_fw_update_ongoing(FRU_BMC)) {
        syslog(LOG_WARNING, "key_func_por_policy: cannot setenv por_policy=%s", (char *)arg);
        break;
      }
      ret = fw_setenv("por_policy", (char *)arg);
      break;
    case KEY_AFTER_INI:
      kv_get("server_por_cfg", value, NULL, KV_FPERSIST);
      ret = fw_setenv("por_policy", value);
      break;
  }

  return ret;
}

static int
key_func_lps(int event, void *arg) {
  char value[MAX_VALUE_LEN] = {0};

  switch (event) {
    case KEY_BEFORE_SET:
      if (pal_is_fw_update_ongoing(FRU_BMC)) {
        syslog(LOG_WARNING, "key_func_lps: cannot setenv por_ls=%s", (char *)arg);
        break;
      }
      fw_setenv("por_ls", (char *)arg);
      break;
    case KEY_AFTER_INI:
      kv_get("pwr_server_last_state", value, NULL, KV_FPERSIST);
      fw_setenv("por_ls", value);
      break;
  }

  return 0;
}

int
pal_is_bmc_por(void) {
  FILE *fp;
  int por = 0;

  fp = fopen("/tmp/ast_por", "r");
  if (fp != NULL) {
    if (fscanf(fp, "%d", &por) != 1) {
      por = 0;
    }
    fclose(fp);
  }

  return (por)?1:0;
}

int
pal_get_platform_name(char *name) {
  strcpy(name, FBAL_PLATFORM_NAME);

  return 0;
}

int
pal_is_fru_prsnt(uint8_t fru, uint8_t *status) {
  gpio_desc_t *gdesc = NULL;
  gpio_value_t val;

  *status = 0;

  switch (fru) {
  case FRU_MB:
    *status = 1;
    break;
  case FRU_NIC0:
    if ((gdesc = gpio_open_by_shadow(GPIO_NIC0_PRSNT))) {
      if (!gpio_get_value(gdesc, &val)) {
        *status = !val;
      }
      gpio_close(gdesc);
    }
    break;
  case FRU_NIC1:
    if ((gdesc = gpio_open_by_shadow(GPIO_NIC1_PRSNT))) {
      if (!gpio_get_value(gdesc, &val)) {
        *status = !val;
      }
      gpio_close(gdesc);
    }
    break;
  case FRU_PDB:
    *status = 1;
    break;
  case FRU_BMC:
    *status = 1;
    break;
  default:
    return -1;
  }

  return 0;
}

int
pal_is_slot_server(uint8_t fru) {
  if (fru == FRU_MB)
    return 1;
  return 0;
}

// Update the Identification LED for the given fru with the status
int
pal_set_id_led(uint8_t fru, uint8_t status) {
  int ret;
  gpio_desc_t *gdesc = NULL;
  gpio_value_t val;

  if (fru != FRU_MB)
    return -1;

  gdesc = gpio_open_by_shadow(GPIO_LOCATE_LED);
  if (gdesc == NULL)
    return -1;

  val = status? GPIO_VALUE_HIGH: GPIO_VALUE_LOW;
  ret = gpio_set_value(gdesc, val);
  if (ret != 0)
    goto error;

  error:
    gpio_close(gdesc);
    return ret;
}

int
pal_set_fault_led(uint8_t fru, uint8_t status) {
  int ret;
  gpio_desc_t *gdesc = NULL;
  gpio_value_t val;

  if (fru != FRU_MB)
    return -1;

  gdesc = gpio_open_by_shadow(GPIO_FAULT_LED);
  if (gdesc == NULL)
    return -1;

  val = status? GPIO_VALUE_HIGH: GPIO_VALUE_LOW;
  ret = gpio_set_value(gdesc, val);
  if (ret != 0)
    goto error;

  error:
    gpio_close(gdesc);
    return ret;
}

int
pal_get_fru_id(char *str, uint8_t *fru) {
  if (!strcmp(str, "all")) {
    *fru = FRU_ALL;
  } else if (!strcmp(str, "mb") || !strcmp(str, "cpld")) {
    *fru = FRU_MB;
  } else if (!strcmp(str, "pdb")) {
    *fru = FRU_PDB;
  } else if (!strcmp(str, "nic0") || !strcmp(str, "nic")) {
    *fru = FRU_NIC0;
  } else if (!strcmp(str, "nic1")) {
    *fru = FRU_NIC1;
  } else if (!strcmp(str, "ocpdbg")) {
    *fru = FRU_DBG;
  } else if (!strcmp(str, "bmc")) {
    *fru = FRU_BMC;
  } else {
    syslog(LOG_WARNING, "pal_get_fru_id: Wrong fru#%s", str);
    return -1;
  }

  return 0;
}

int
pal_get_fru_name(uint8_t fru, char *name) {
  switch (fru) {
    case FRU_MB:
      strcpy(name, "mb");
      break;
    case FRU_PDB:
      strcpy(name, "pdb");
      break;
    case FRU_NIC0:
      strcpy(name, "nic0");
      break;
    case FRU_NIC1:
      strcpy(name, "nic1");
      break;
    case FRU_DBG:
      strcpy(name, "ocpdbg");
      break;
    case FRU_BMC:
      strcpy(name, "bmc");
      break;
    default:
      if (fru > MAX_NUM_FRUS)
        return -1;
      sprintf(name, "fru%d", fru);
      break;
  }

  return 0;
}

void
pal_dump_key_value(void) {
  int ret;
  int i = 0;
  char value[MAX_VALUE_LEN] = {0x0};

  while (strcmp(key_cfg[i].name, LAST_KEY)) {
    printf("%s:", key_cfg[i].name);
    if ((ret = kv_get(key_cfg[i].name, value, NULL, KV_FPERSIST)) < 0) {
    printf("\n");
  } else {
    printf("%s\n",  value);
  }
    i++;
    memset(value, 0, MAX_VALUE_LEN);
  }
}

void
pal_get_chassis_status(uint8_t slot, uint8_t *req_data, uint8_t *res_data, uint8_t *res_len) {

   char str_server_por_cfg[64];
   char buff[MAX_VALUE_LEN];
   int policy = 3;
   unsigned char *data = res_data;

   // Platform Power Policy
   memset(str_server_por_cfg, 0 , sizeof(char) * 64);
   sprintf(str_server_por_cfg, "%s", "server_por_cfg");

   if (pal_get_key_value(str_server_por_cfg, buff) == 0)
   {
     if (!memcmp(buff, "off", strlen("off")))
       policy = 0;
     else if (!memcmp(buff, "lps", strlen("lps")))
       policy = 1;
     else if (!memcmp(buff, "on", strlen("on")))
       policy = 2;
     else
       policy = 3;
   }
   *data++ = ((is_server_off())?0x00:0x01) | (policy << 5);
   *data++ = 0x00;   // Last Power Event
   *data++ = 0x40;   // Misc. Chassis Status
   *data++ = 0x00;   // Front Panel Button Disable
   *res_len = data - res_data;
}

void
pal_update_ts_sled() {
  char key[MAX_KEY_LEN] = {0};
  char tstr[MAX_VALUE_LEN] = {0};
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  sprintf(tstr, "%ld", ts.tv_sec);

  sprintf(key, "timestamp_sled");

  pal_set_key_value(key, tstr);
}

int
pal_get_fruid_path(uint8_t fru, char *path) {
  char fname[16] = {0};

  switch(fru) {
  case FRU_MB:
    sprintf(fname, "mb");
    break;
  case FRU_NIC0:
    sprintf(fname, "nic0");
    break;
  case FRU_NIC1:
    sprintf(fname, "nic1");
    break;
  case FRU_PDB:
    sprintf(fname, "pdb");
    break;
  case FRU_BMC:
    sprintf(fname, "bmc");
    break;
  default:
    return -1;
  }

  sprintf(path, "/tmp/fruid_%s.bin", fname);
  return 0;
}

int
pal_get_fruid_eeprom_path(uint8_t fru, char *path) {
  switch(fru) {
  case FRU_MB:
    sprintf(path, FRU_EEPROM_MB);
    break;
  case FRU_NIC0:
    sprintf(path, FRU_EEPROM_NIC0);
    break;
  case FRU_NIC1:
    sprintf(path, FRU_EEPROM_NIC1);
    break;
  case FRU_BMC:
    sprintf(path, FRU_EEPROM_BMC);
    break;
  default:
    return -1;
  }

  return 0;
}

int
pal_get_fruid_name(uint8_t fru, char *name) {
  switch(fru) {
  case FRU_MB:
    sprintf(name, "Mother Board");
    break;

  case FRU_NIC0:
    sprintf(name, "Mezz Card 0");
    break;

  case FRU_NIC1:
    sprintf(name, "Mezz Card 1");
    break;

  case FRU_PDB:
    sprintf(name, "PDB");
    break;

  case FRU_BMC:
    sprintf(name, "BMC");
    break;

  default:
    return -1;
  }
  return 0;
}

int
pal_fruid_write(uint8_t fru, char *path) {
  if (fru == FRU_PDB) {
    return fbal_write_pdb_fruid(0, path);
  }
  return -1;
}

int
pal_is_fru_ready(uint8_t fru, uint8_t *status) {
  *status = 1;

  return 0;
}

int
pal_channel_to_bus(int channel) {
  switch (channel) {
    case IPMI_CHANNEL_0:
      return I2C_BUS_0; // USB (LCD Debug Board)

    case IPMI_CHANNEL_2:
      return I2C_BUS_2; // Slave BMC

    case IPMI_CHANNEL_6:
      return I2C_BUS_5; // ME

    case IPMI_CHANNEL_8:
      return I2C_BUS_8; // CM

    case IPMI_CHANNEL_9:
      return I2C_BUS_6; // EP
  }

  // Debug purpose, map to real bus number
  if (channel & 0x80) {
    return (channel & 0x7f);
  }

  return channel;
}

bool
pal_is_fw_update_ongoing_system(void) {
  uint8_t i;

  for (i = 1; i <= MAX_NUM_FRUS; i++) {
    if (pal_is_fw_update_ongoing(i) == true)
      return true;
  }

  return false;
}

int
read_device(const char *device, int *value) {
  FILE *fp;
  int rc;

  fp = fopen(device, "r");
  if (!fp) {
    int err = errno;
    syslog(LOG_INFO, "failed to open device %s", device);
    return err;
  }

  rc = fscanf(fp, "%d", value);

  fclose(fp);
  if (rc != 1) {
    syslog(LOG_INFO, "failed to read device %s", device);
    return ENOENT;
  } else {
    return 0;
  }
}

int
pal_set_def_key_value() {
  int i;
  char key[MAX_KEY_LEN] = {0};

  for(i = 0; strcmp(key_cfg[i].name, LAST_KEY) != 0; i++) {
    if (kv_set(key_cfg[i].name, key_cfg[i].def_val, 0, KV_FCREATE | KV_FPERSIST)) {
#ifdef DEBUG
      syslog(LOG_WARNING, "pal_set_def_key_value: kv_set failed.");
#endif
    }
    if (key_cfg[i].function) {
      key_cfg[i].function(KEY_AFTER_INI, key_cfg[i].name);
    }
  }

  /* Actions to be taken on Power On Reset */
  if (pal_is_bmc_por()) {
    /* Clear all the SEL errors */
    memset(key, 0, MAX_KEY_LEN);
    strcpy(key, "server_sel_error");

    /* Write the value "1" which means FRU_STATUS_GOOD */
    pal_set_key_value(key, "1");

    /* Clear all the sensor health files*/
    memset(key, 0, MAX_KEY_LEN);
    strcpy(key, "server_sensor_health");

    /* Write the value "1" which means FRU_STATUS_GOOD */
    pal_set_key_value(key, "1");
  }

  return 0;
}

static int
get_gpio_shadow_array(const char **shadows, int num, uint8_t *mask) {
  int i;
  *mask = 0;

  for (i = 0; i < num; i++) {
    int ret;
    gpio_value_t value;
    gpio_desc_t *gpio = gpio_open_by_shadow(shadows[i]);
    if (!gpio) {
      return -1;
    }

    ret = gpio_get_value(gpio, &value);
    gpio_close(gpio);

    if (ret != 0) {
      return -1;
    }
    *mask |= (value == GPIO_VALUE_HIGH ? 1 : 0) << i;
  }
  return 0;
}

int
pal_get_blade_id(uint8_t *id) {
  static bool cached = false;
  static uint8_t cached_id = 0;

  if (!cached) {
    const char *shadows[] = {
      "FM_BLADE_ID_0",
      "FM_BLADE_ID_1"
    };
    if (get_gpio_shadow_array(shadows, ARRAY_SIZE(shadows), &cached_id)) {
      return -1;
    }
    cached = true;
  }
  *id = cached_id;
  return 0;
}

int 
pal_get_bmc_ipmb_slave_addr(uint16_t* slave_addr, uint8_t bus_id) {
  uint8_t val;
  int ret;
  static uint8_t addr=0;

  if ((bus_id == I2C_BUS_2) || (bus_id == I2C_BUS_5)) {

    if (addr == 0) {
      ret = pal_get_blade_id (&val);
      if (ret != 0) {
        return -1;
      }
      addr = 0x10 | val;
      *slave_addr = addr;
    } else {
      *slave_addr = addr;
    }
  } else {
    *slave_addr = 0x10;
  }

#ifdef DEBUG
  syslog(LOG_DEBUG,"%s BMC Slave Addr=%d bus=%d", __func__, *slave_addr, bus_id);
#endif
  return 0;
}

int
pal_ipmb_processing(int bus, void *buf, uint16_t size) {
  char key[MAX_KEY_LEN];
  char value[MAX_VALUE_LEN];
  struct timespec ts;
  static time_t last_time = 0;

  switch (bus) {
    case I2C_BUS_0:
      if (((uint8_t *)buf)[0] == 0x20) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec >= (last_time + 5)) {
          last_time = ts.tv_sec;
          ts.tv_sec += 20;

          sprintf(key, "ocpdbg_lcd");
          sprintf(value, "%ld", ts.tv_sec);
          if (kv_set(key, value, 0, 0) < 0) {
            return -1;
          }
        }
      }
      break;
  }

  return 0;
}

int
pal_is_mcu_ready(uint8_t bus) {
  char key[MAX_KEY_LEN];
  char value[MAX_VALUE_LEN] = {0};
  struct timespec ts;

  switch (bus) {
    case I2C_BUS_0:
      sprintf(key, "ocpdbg_lcd");
      if (kv_get(key, value, NULL, 0)) {
        return false;
      }

      clock_gettime(CLOCK_MONOTONIC, &ts);
      if (strtoul(value, NULL, 10) > ts.tv_sec) {
         return true;
      }
      break;

    case I2C_BUS_8:
      return true;
  }

  return false;
}

int
pal_set_sysfw_ver(uint8_t slot, uint8_t *ver) {
  int i;
  char str[MAX_VALUE_LEN] = {0};
  char tstr[8] = {0};

  for (i = 0; i < SIZE_SYSFW_VER; i++) {
    sprintf(tstr, "%02x", ver[i]);
    strcat(str, tstr);
  }

  return pal_set_key_value("sysfw_ver_server", str);
}

int
pal_get_sysfw_ver(uint8_t slot, uint8_t *ver) {
  int ret;
  int i, j;
  char str[MAX_VALUE_LEN] = {0};
  char tstr[8] = {0};

  ret = pal_get_key_value("sysfw_ver_server", str);
  if (ret) {
    return ret;
  }

  for (i = 0, j = 0; i < 2*SIZE_SYSFW_VER; i += 2) {
    sprintf(tstr, "%c%c", str[i], str[i+1]);
    ver[j++] = strtol(tstr, NULL, 16);
  }
  return 0;
}

int
pal_uart_select (uint32_t base, uint8_t offset, int option, uint32_t para) {
  uint32_t mmap_fd;
  uint32_t ctrl;
  void *reg_base;
  void *reg_offset;

  mmap_fd = open("/dev/mem", O_RDWR | O_SYNC );
  if (mmap_fd < 0) {
    return -1;
  }

  reg_base = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, mmap_fd, base);
  reg_offset = (char*) reg_base + offset;
  ctrl = *(volatile uint32_t*) reg_offset;

  switch(option) {
    case UARTSW_BY_BMC:                //UART Switch control by bmc
      ctrl &= 0x00ffffff;
      break;
    case UARTSW_BY_DEBUG:           //UART Switch control by debug card
      ctrl |= 0x01000000;
      break;
    case SET_SEVEN_SEGMENT:      //set channel on the seven segment display
      ctrl &= 0x00ffffff;
      ctrl |= para;
      break;
    default:
      syslog(LOG_WARNING, "pal_mmap: unknown option");
      break;
  }
  *(volatile uint32_t*) reg_offset = ctrl;

  munmap(reg_base, PAGE_SIZE);
  close(mmap_fd);

  return 0;
}

int
pal_fw_update_prepare(uint8_t fru, const char *comp) {
  int ret = 0, retry = 3;
  uint8_t status;
  gpio_desc_t *desc;

  if ((fru == FRU_MB) && !strcmp(comp, "bios")) {
    pal_set_server_power(FRU_MB, SERVER_POWER_OFF);
    while (retry > 0) {
      if (!pal_get_server_power(FRU_MB, &status) && (status == SERVER_POWER_OFF)) {
        break;
      }
      if ((--retry) > 0) {
        sleep(1);
      }
    }
    if (retry <= 0) {
      printf("Failed to Power Off Server. Stopping the update!\n");
      return -1;
    }

    sleep(10);
    if (system("/usr/local/bin/me-util 0xB8 0xDF 0x57 0x01 0x00 0x01 > /dev/null")) {
      syslog(LOG_WARNING, "Unable to put ME in recovery mode!");
    }
    sleep(1);

    ret = -1;
    desc = gpio_open_by_shadow("FM_BIOS_SPI_BMC_CTRL");
    if (desc) {
      if (!gpio_set_direction(desc, GPIO_DIRECTION_OUT) && !gpio_set_value(desc, GPIO_VALUE_HIGH)) {
        ret = 0;
      } else {
        printf("Failed to switch BIOS ROM to BMC\n");
      }
      gpio_close(desc);
    } else {
      printf("Failed to open SPI-Switch GPIO\n");
    }

    if (system("echo -n spi0.0 > /sys/bus/spi/drivers/m25p80/bind")) {
      syslog(LOG_ERR, "Unable to mount MTD partitions for BIOS SPI-Flash\n");
      ret = -1;
    }
  }

  return ret;
}

int
pal_fw_update_finished(uint8_t fru, const char *comp, int status) {
  int ret = 0;
  gpio_desc_t *desc;

  if ((fru == FRU_MB) && !strcmp(comp, "bios")) {
    if (system("echo -n spi0.0 > /sys/bus/spi/drivers/m25p80/unbind")) {
      syslog(LOG_ERR, "Unable to unmount MTD partitions\n");
    }

    desc = gpio_open_by_shadow("FM_BIOS_SPI_BMC_CTRL");
    if (desc) {
      gpio_set_value(desc, GPIO_VALUE_LOW);
      gpio_set_direction(desc, GPIO_DIRECTION_IN);
      gpio_close(desc);
    }

    ret = status;
    if (status == 0) {
      sleep(1);
      pal_power_button_override(FRU_MB);
      sleep(10);
      pal_set_server_power(FRU_MB, SERVER_POWER_ON);
    }
  }

  return ret;
}

int
pal_uart_select_led_set(void) {
  static uint32_t pre_channel = 0xffffffff;
  uint8_t vals;
  uint32_t channel = 0;
  const char *shadows[] = {
    "FM_UARTSW_LSB_N",
    "FM_UARTSW_MSB_N"
  };

  //UART Switch control by bmc
  pal_uart_select(AST_GPIO_BASE, UARTSW_OFFSET, UARTSW_BY_BMC, 0);

  if (get_gpio_shadow_array(shadows, ARRAY_SIZE(shadows), &vals)) {
    return -1;
  }
  // The GPIOs are active-low. So, invert it.
  channel = (uint32_t)(~vals & 0x3);
  // Shift to get to the bit position of the led.
  channel = channel << 24;

  // If the requested channel is the same as the previous, do nothing.
  if (channel == pre_channel) {
     return -1;
  }
  pre_channel = channel;

  //show channel on 7-segment display
  pal_uart_select(AST_GPIO_BASE, SEVEN_SEGMENT_OFFSET, SET_SEVEN_SEGMENT, channel); 
  return 0;
}

int
pal_set_boot_order(uint8_t slot, uint8_t *boot, uint8_t *res_data, uint8_t *res_len) {
  int i, j, network_dev = 0;
  char key[MAX_KEY_LEN] = {0};
  char str[MAX_VALUE_LEN] = {0};
  char tstr[10] = {0};
  *res_len = 0;
  sprintf(key, "server_boot_order");

  for (i = 0; i < SIZE_BOOT_ORDER; i++) {
    //Byte 0 is boot mode, Byte 1~5 is boot order
    if ((i > 0) && (boot[i] != 0xFF)) {
      for (j = i+1; j < SIZE_BOOT_ORDER; j++) {
        if ( boot[i] == boot[j])
          return CC_INVALID_PARAM;
      }

      //If Bit 2:0 is 001b (Network), Bit3 is IPv4/IPv6 order
      //Bit3=0b: IPv4 first
      //Bit3=1b: IPv6 first
      if ( boot[i] == BOOT_DEVICE_IPV4 || boot[i] == BOOT_DEVICE_IPV6)
        network_dev++;
    }

    snprintf(tstr, 3, "%02x", boot[i]);
    strncat(str, tstr, 3);
  }

  //Not allow having more than 1 network boot device in the boot order.
  if (network_dev > 1)
    return CC_INVALID_PARAM;

  return pal_set_key_value(key, str);
}

int
pal_get_boot_order(uint8_t slot, uint8_t *req_data, uint8_t *boot, uint8_t *res_len) {
  int i;
  int j = 0;
  int ret;
  int msb, lsb;
  char key[MAX_KEY_LEN] = {0};
  char str[MAX_VALUE_LEN] = {0};
  char tstr[4] = {0};

  sprintf(key, "server_boot_order");

  ret = pal_get_key_value(key, str);
  if (ret) {
    *res_len = 0;
    return ret;
  }

  for (i = 0; i < 2*SIZE_BOOT_ORDER; i += 2) {
    sprintf(tstr, "%c\n", str[i]);
    msb = strtol(tstr, NULL, 16);

    sprintf(tstr, "%c\n", str[i+1]);
    lsb = strtol(tstr, NULL, 16);
    boot[j++] = (msb << 4) | lsb;
  }
  *res_len = SIZE_BOOT_ORDER;
  return 0;
}

// Get ME Firmware Version
int
pal_get_me_fw_ver(uint8_t bus, uint8_t addr, uint8_t *ver) {
  ipmi_dev_id_t dev_id;
  NM_RW_INFO info;
  int ret;

  info.bus = bus;
  info.nm_addr = addr;
  ret = pal_get_bmc_ipmb_slave_addr(&info.bmc_addr, info.bus);
  if (ret != 0) {
    return ret;
  }

  ret = cmd_NM_get_dev_id(&info, &dev_id);
  if (ret != 0) {
    return ret;
  }

  ver[0] = dev_id.fw_rev1;
  ver[1] = dev_id.fw_rev2 >> 4; 
  ver[2] = dev_id.fw_rev2 & 0x0f;
  ver[3] = dev_id.aux_fw_rev[1];
  ver[4] = dev_id.aux_fw_rev[2] >> 4;
  return ret;
}

// GUID for System and Device
static int
pal_get_guid(uint16_t offset, char *guid) {
  int fd;
  ssize_t bytes_rd;

  errno = 0;

  // check for file presence
  if (access(FRU_EEPROM_MB, F_OK)) {
    syslog(LOG_ERR, "pal_get_guid: unable to access %s: %s", FRU_EEPROM_MB, strerror(errno));
    return errno;
  }

  fd = open(FRU_EEPROM_MB, O_RDONLY);
  if (fd < 0) {
    syslog(LOG_ERR, "pal_get_guid: unable to open %s: %s", FRU_EEPROM_MB, strerror(errno));
    return errno;
  }

  lseek(fd, offset, SEEK_SET);

  bytes_rd = read(fd, guid, GUID_SIZE);
  if (bytes_rd != GUID_SIZE) {
    syslog(LOG_ERR, "pal_get_guid: read from %s failed: %s", FRU_EEPROM_MB, strerror(errno));
  }

  close(fd);
  return errno;
}

static int
pal_set_guid(uint16_t offset, char *guid) {
  int fd;
  ssize_t bytes_wr;

  errno = 0;

  // check for file presence
  if (access(FRU_EEPROM_MB, F_OK)) {
    syslog(LOG_ERR, "pal_set_guid: unable to access %s: %s", FRU_EEPROM_MB, strerror(errno));
    return errno;
  }

  fd = open(FRU_EEPROM_MB, O_WRONLY);
  if (fd < 0) {
    syslog(LOG_ERR, "pal_set_guid: unable to open %s: %s", FRU_EEPROM_MB, strerror(errno));
    return errno;
  }

  lseek(fd, offset, SEEK_SET);

  bytes_wr = write(fd, guid, GUID_SIZE);
  if (bytes_wr != GUID_SIZE) {
    syslog(LOG_ERR, "pal_set_guid: write to %s failed: %s", FRU_EEPROM_MB, strerror(errno));
  }

  close(fd);
  return errno;
}

// GUID based on RFC4122 format @ https://tools.ietf.org/html/rfc4122
static void
pal_populate_guid(char *guid, char *str) {
  unsigned int secs;
  unsigned int usecs;
  struct timeval tv;
  uint8_t count;
  uint8_t lsb, msb;
  int i, r;

  // Populate time
  gettimeofday(&tv, NULL);

  secs = tv.tv_sec;
  usecs = tv.tv_usec;
  guid[0] = usecs & 0xFF;
  guid[1] = (usecs >> 8) & 0xFF;
  guid[2] = (usecs >> 16) & 0xFF;
  guid[3] = (usecs >> 24) & 0xFF;
  guid[4] = secs & 0xFF;
  guid[5] = (secs >> 8) & 0xFF;
  guid[6] = (secs >> 16) & 0xFF;
  guid[7] = (secs >> 24) & 0x0F;

  // Populate version
  guid[7] |= 0x10;

  // Populate clock seq with randmom number
  //getrandom(&guid[8], 2, 0);
  srand(time(NULL));
  //memcpy(&guid[8], rand(), 2);
  r = rand();
  guid[8] = r & 0xFF;
  guid[9] = (r>>8) & 0xFF;

  // Use string to populate 6 bytes unique
  // e.g. LSP62100035 => 'S' 'P' 0x62 0x10 0x00 0x35
  count = 0;
  for (i = strlen(str)-1; i >= 0; i--) {
    if (count == 6) {
      break;
    }

    // If alphabet use the character as is
    if (isalpha(str[i])) {
      guid[15-count] = str[i];
      count++;
      continue;
    }

    // If it is 0-9, use two numbers as BCD
    lsb = str[i] - '0';
    if (i > 0) {
      i--;
      if (isalpha(str[i])) {
        i++;
        msb = 0;
      } else {
        msb = str[i] - '0';
      }
    } else {
      msb = 0;
    }
    guid[15-count] = (msb << 4) | lsb;
    count++;
  }

  // zero the remaining bytes, if any
  if (count != 6) {
    memset(&guid[10], 0, 6-count);
  }

  return;
}

int
pal_get_sys_guid(uint8_t fru, char *guid) {
  pal_get_guid(OFFSET_SYS_GUID, guid);
  return 0;
}

int
pal_set_sys_guid(uint8_t fru, char *str) {
  char guid[GUID_SIZE] = {0};

  pal_populate_guid(guid, str);
  return pal_set_guid(OFFSET_SYS_GUID, guid);
}

int
pal_get_dev_guid(uint8_t fru, char *guid) {
  pal_get_guid(OFFSET_DEV_GUID, guid);
  return 0;
}

int
pal_set_dev_guid(uint8_t fru, char *str) {
  char guid[GUID_SIZE] = {0};

  pal_populate_guid(guid, str);
  return pal_set_guid(OFFSET_DEV_GUID, guid);
}
