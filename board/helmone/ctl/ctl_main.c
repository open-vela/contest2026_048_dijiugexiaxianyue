/**
 * @file ctl_main.c
 * @brief NSH `ctl`：产品控制面（persist + owner set）。读走 `sys`。
 *
 *   ctl bl <0-100>      背光
 *   ctl radio on|off    手机 Companion 广播
 *   ctl sensor on|off   HR/CSC/CPS Central 策略
 *   ctl mtp on|off      USB 控制器
 *   ctl notif on|off    手机通知
 *   ctl calls on|off    仅来电
 *   ctl sound on|off    蜂鸣器提示音
 *   ctl autopause on|off 骑行自动暂停
 *   ctl grade [<mdeg>]  坡度零点（毫度；省略则读取）
 *   ctl tz [+8]         时区（小时；省略则读取，默认 +8）
 *   ctl gnss dump_out   旁路倒 MAX-M10S 导航库到 mga_<utc>.ubx
 *   ctl gnss dump_in    把最新 mga_<utc>.ubx 灌进模组
 *   ctl gnss auto [on|off]  开机注入 / 定位后倒库
 *   ctl gnss still [on|off] 停车切 stationary（默认关，见 myvendor_gnss_dyn_still_enable）
 *   ctl idle              打印静止子命令
 *   ctl idle on|off|hour|stay|auto  调试静止；stay=本次开机不自动静止
 *   ctl dvfs auto|72|96|144|240  调试 HCPU 调频
 *   ctl wt <hexaddr> [1|2|4]  DWT 数据观察点：给写坏内存的人点名
 *   ctl wt [clear|test]       全部撤掉 / 自我触发验证交付路径
 *   ctl log [level]     syslog 屏蔽位（err 起，运行时全系统生效，落 KV）
 *   ctl pwr [on|off]    调试开机（persist.boot.pwr；2SFBL 不判键直接跳）
 *   ctl off             关机（拉低 PA29）
 *
 * 菜单搜表/连接走 C API（myvendor_devctl_sensor_scan 等），不挂 NSH。
 * 外设探针用 `test sensor`，逻辑独立。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_devctl.h"
#include "myvendor_bicycle_ctl.h"
#include "myvendor_gnss.h"
#include "sf32lb_dvfs.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/syslog/syslog.h>

/* 本文件是 nuttx_add_application（目标 `ctl`），include 路径只有
 * board/include，**芯片层头文件不在里面**，所以只能本地声明。
 * （对照：services/ 下的文件属于 board 目标，可以直接 `#include "sf32lb_sdio.h"`。）
 *
 * 两个函数的完整契约（绕过判死闸门、write 只允许卡尾 1 MiB）见
 * chips/sf32lb52/include/sf32lb_sdio.h 里 sf32lb_sd_raw_read / _raw_write 的
 * Doxygen —— 它们是 `ctl sd read|write` 的落点，用来在**不惊动文件系统**的
 * 前提下直接验卡：判死之后 LFS 层已经不下发 I/O 了，只有裸读能继续测卡。 */
extern int sf32lb_sd_raw_read(uint64_t byte_off, uint32_t nblocks);
extern int sf32lb_sd_raw_write(uint64_t byte_off, uint32_t nblocks);

/* DWT 数据观察点（chips/sf32lb52/myvendor_idle_stat.c）。芯片头在 NSH app 目标里
 * 不可见，沿用本文件既有的"调用点 extern"做法。 */
extern int myvendor_dwt_watch_arm(uint32_t addr, unsigned size_bytes);
extern void myvendor_dwt_watch_clear(void);
extern int myvendor_dwt_watch_status(char *buf, size_t buflen);

/* 路线规划自测（ui/bicycle/src/nav_test.c）：脱离 App 对同一对起终点跑
 * 直规与走廊(rolling)并打印长度/机动，用来复现"rolling 绕远路"那件事。 */
extern int myvendor_nav_test(int argc, char **argv);
extern int sf32lb52_bt_nvds_dump(void);   /* chips/sf32lb52/sf32lb52_bt_adapter.c */
extern int myvendor_bt_pair_cmd(int argc, char **argv);   /* services/myvendor_bt_pair.c */

#define CTL_FAV_PATH      MYVENDOR_DEVCTL_FAVORITES_PATH
#define CTL_FAV_TMP_PATH  "/mnt/kv/.bicycle_favorites.tmp"
#define CTL_FAV_MAX       MYVENDOR_DEVCTL_FAVORITE_MAX
#define CTL_FAV_NAME_MAX  MYVENDOR_DEVCTL_FAVORITE_NAME_MAX

struct ctl_favorite_s
{
  char name[CTL_FAV_NAME_MAX];
  double latitude;
  double longitude;
};

/**
 * @brief 解析 on/1 或 off/0。
 * @return 0 成功，-EINVAL 非法。
 */
static int parse_onoff(const char *s, bool *on)
{
  if (s == NULL)
    {
      return -EINVAL;
    }

  if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0)
    {
      *on = true;
      return 0;
    }

  if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0)
    {
      *on = false;
      return 0;
    }

  return -EINVAL;
}

/** @brief 解析 `AA:BB:CC:DD:EE:FF` / `AA-BB-...` 成 6 字节。 */
static bool sensor_mac_parse(const char *text, uint8_t *out)
{
  unsigned b[6];
  int      n;

  if (text == NULL || out == NULL)
    {
      return false;
    }

  n = sscanf(text, "%x:%x:%x:%x:%x:%x",
             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  if (n != 6)
    {
      n = sscanf(text, "%x-%x-%x-%x-%x-%x",
                 &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    }
  if (n != 6)
    {
      return false;
    }

  for (n = 0; n < 6; n++)
    {
      if (b[n] > 0xffu)
        {
          return false;
        }
      out[n] = (uint8_t)b[n];
    }
  return true;
}

/**
 * @brief `ctl sensor bind <kind> <mac> [addr_type]`：按 MAC 重绑一台传感器。
 *
 * 为什么要有：绑定记录（`/mnt/kv/bicycle_sensors.tsv`）被删掉之后设备就不再回连
 * 那台外设，而重绑原本只能在菜单里按好几下键；远端（串口 / monitor-ctl）需要一条
 * 能把现场恢复回去的路。走 UI 点选同一条投递路径（companion 线程执行），
 * NSH 线程不直接碰协议栈。
 */
static int cmd_sensor_bind(int argc, char *argv[])
{
  uint8_t  addr[MYVENDOR_DEVCTL_SENSOR_ADDR_LEN];
  unsigned kind;
  unsigned type;
  int      rc;

  if (argc < 3 || argv[1] == NULL || argv[2] == NULL)
    {
      printf("usage: ctl sensor bind <0HR|1CSC|2CPS> <mac> [addr_type 0pub|1rnd]\n");
      return -EINVAL;
    }

  kind = (unsigned)strtoul(argv[1], NULL, 10);
  if (kind > MYVENDOR_DEVCTL_SENSOR_KIND_CPS)
    {
      printf("ctl sensor bind: kind must be 0/1/2 (got %s)\n", argv[1]);
      return -EINVAL;
    }

  memset(addr, 0, sizeof(addr));
  if (!sensor_mac_parse(argv[2], addr))
    {
      printf("ctl sensor bind: bad mac '%s' (want AA:BB:CC:DD:EE:FF)\n", argv[2]);
      return -EINVAL;
    }

  if (argc >= 4 && argv[3] != NULL)
    {
      type = (unsigned)strtoul(argv[3], NULL, 10);
      if (type > 1u)
        {
          printf("ctl sensor bind: addr_type must be 0(public)/1(random)\n");
          return -EINVAL;
        }
    }
  else
    {
      /* 省掉 type 就按地址位模式猜：静态随机地址最高两位是 11（0xC0…）。 */
      type = (addr[0] & 0xc0u) == 0xc0u ? 1u : 0u;
    }

  /* 显示顺序（`AA:BB:…`）→ 框架顺序（最低位在前）：`ui.found[].addr` 是这个序，
   * 反着填会去连一个不存在的地址（2026-09-20 实测：日志打出来是 `AC:7B:F3:…`）。 */
  for (rc = 0; rc < 3; rc++)
    {
      uint8_t swap = addr[rc];

      addr[rc]     = addr[5 - rc];
      addr[5 - rc] = swap;
    }

  rc = myvendor_devctl_sensor_connect_addr(addr, (uint8_t)type, (uint8_t)kind, NULL);
  if (rc != 0)
    {
      printf("ctl sensor bind: failed rc=%d\n", rc);
      return rc;
    }

  printf("ctl sensor bind: %s kind=%u type=%u -> companion\n",
         argv[2], kind, type);
  return 0;
}

/** @brief 打印用法。 */
static void usage(void)
{
  printf("ctl — write policy (persist + owners)\n");
  printf("  ctl bl <0-100>      backlight PWM percent (0=off)\n");
  printf("  ctl radio <on|off>  phone BLE advertising (upstream / 手机蓝牙)\n");
  printf("  ctl bt <on|off>     alias of radio\n");
  printf("  ctl sensor <on|off> HR/CSC/CPS central (downstream / 外设蓝牙)\n");
  printf("  ctl sensor bind <0HR|1CSC|2CPS> <mac> [type]  按 MAC 重绑（记录被删后恢复）\n");
  printf("  ctl mtp <on|off>    USB controller (PHY+clk; worker stays)\n");
  printf("  ctl notif <on|off>  phone 0xFF17 banners + inbox\n");
  printf("  ctl calls <on|off>  calls-only filter (needs notif on)\n");
  printf("  ctl sound <on|off>  piezo UI beeps (test sound still works)\n");
  printf("  ctl autopause <on|off>  ride auto-pause by speed/cadence\n");
  printf("  ctl grade [<mdeg>]  slope zero (millidegree pitch; omit to read)\n");
  printf("  ctl tz [<hours>]    timezone vs UTC, e.g. +8 (omit to read)\n");
  printf("  ctl fav <name> <lat> <lon>  save/update favorite point\n");
  printf("  ctl fav list              list favorite points\n");
  printf("  ctl fav del <name>        delete favorite point\n");
  printf("  ctl gnss dump_out   dump MAX-M10S nav DB -> /mnt/lfs/eph/mga_<utc>.ubx\n");
  printf("  ctl gnss dump_in    inject newest /mnt/lfs/eph/mga_<utc>.ubx\n");
  printf("  ctl gnss auto [on|off]  boot inject + dump after first fix\n");
  printf("  ctl gnss bdsonly [on|off]  只留北斗（看 gsv 的 bd 项）/ 恢复\n");
  printf("  ctl gnss b1c        try enabling B1C (1575.42, 与 L1 同频)\n");
  printf("  ctl gnss ver        log UBX-MON-VER (fw/hw version)\n");
  printf("  ctl idle              still-screen help\n");
  printf("  ctl idle on|off|hour|stay|auto  stay=no auto still this boot\n");
  printf("  ctl dvfs auto|72|96|144|240  HCPU DVFS (ondemand)\n");
  printf("  ctl dvfs hop [ms]   stress-test: hop gear every ms (def 10000)\n");
  printf("  ctl log [level]     syslog mask: err|warn|notice|info|all|silent\n");
  printf("  ctl pwr [on|off]    debug auto-boot (skip PWR key in 2SFBL)\n");
  printf("  ctl off             power off (PA29 / PWR_KEY_CTL low)\n");
  printf("Reads: sys / sys bl / sys bat / ...\n");
}

/**
 * @brief `ctl bl <pct>`：只设置，不查询。
 */
static int cmd_bl(int argc, char *argv[])
{
  int v;
  int ret;

  if (argc < 2)
    {
      fprintf(stderr, "ctl bl: need 0..100 (read: sys bl)\n");
      return EXIT_FAILURE;
    }

  v = atoi(argv[1]);
  if (v < 0 || v > 100)
    {
      fprintf(stderr, "ctl bl: need 0..100\n");
      return EXIT_FAILURE;
    }

  ret = myvendor_devctl_bl_set((uint8_t)v);
  if (ret < 0)
    {
      fprintf(stderr, "ctl bl: set failed %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("ctl bl %d  (sys bl to read)\n", v);
  return 0;
}

/**
 * @brief `ctl radio|sensor|mtp <on|off>`。
 */
static int cmd_switch(const char *name, int argc, char *argv[],
                      int (*setfn)(bool))
{
  bool on;
  int ret;

  if (argc < 2)
    {
      fprintf(stderr, "ctl %s: need on|off (read: sys %s)\n",
              name, strcmp(name, "sensor") == 0 ? "hr" : name);
      return EXIT_FAILURE;
    }

  if (parse_onoff(argv[1], &on) != 0)
    {
      fprintf(stderr, "ctl %s: use on|off\n", name);
      return EXIT_FAILURE;
    }

  ret = setfn(on);
  if (ret == -ENOTSUP)
    {
      fprintf(stderr, "ctl %s: not in this image\n", name);
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl %s: set failed %d\n", name, ret);
      return EXIT_FAILURE;
    }

  printf("ctl %s %s\n", name, on ? "on" : "off");
  return 0;
}

/**
 * @brief `ctl pwr` 读调试开机标记；`ctl pwr on|off` 写 persist.boot.pwr。
 */

static int cmd_pwr(int argc, char *argv[])
{
  bool on;

  if (argc < 2)
    {
      printf("ctl pwr %s\n", myvendor_devctl_pwr_get() ? "on" : "off");
      return 0;
    }

  if (parse_onoff(argv[1], &on) != 0)
    {
      fprintf(stderr, "ctl pwr: use on|off\n");
      return EXIT_FAILURE;
    }

  if (myvendor_devctl_pwr_set(on) < 0)
    {
      fprintf(stderr, "ctl pwr: set failed\n");
      return EXIT_FAILURE;
    }

  printf("ctl pwr %s\n", on ? "on" : "off");
  return 0;
}

/**
 * @brief `ctl grade` 读零点；`ctl grade <mdeg>` 写零点（毫度）。
 */
static int cmd_grade(int argc, char *argv[])
{
  int32_t v;
  char *end = NULL;

  if (argc < 2)
    {
      v = myvendor_devctl_grade_offset_get();
      printf("ctl grade  %ld mdeg\n", (long)v);
      return 0;
    }

  v = (int32_t)strtol(argv[1], &end, 0);
  if (end == argv[1] || (end != NULL && *end != '\0'))
    {
      fprintf(stderr, "ctl grade: need millidegrees, e.g. 0 or -2500\n");
      return EXIT_FAILURE;
    }

  if (myvendor_devctl_grade_offset_set(v) < 0)
    {
      fprintf(stderr, "ctl grade: set failed\n");
      return EXIT_FAILURE;
    }

  printf("ctl grade %ld\n", (long)myvendor_devctl_grade_offset_get());
  return 0;
}

/**
 * @brief `ctl tz` 读时区；`ctl tz +8` 写小时偏置。
 */
static int cmd_tz(int argc, char *argv[])
{
  int16_t min;
  int h;
  unsigned mag;
  unsigned hh;
  unsigned mm;
  char sign;
  char *end = NULL;

  if (argc < 2)
    {
      min = myvendor_devctl_tz_min_get();
      if (min < 0)
        {
          sign = '-';
          mag = (unsigned)(-min);
        }
      else
        {
          sign = '+';
          mag = (unsigned)min;
        }

      hh = mag / 60u;
      mm = mag % 60u;
      if (mm == 0)
        {
          printf("ctl tz  UTC%c%u\n", sign, hh);
        }
      else
        {
          printf("ctl tz  UTC%c%u:%02u\n", sign, hh, mm);
        }

      return 0;
    }

  h = (int)strtol(argv[1], &end, 10);
  if (end == argv[1] || (end != NULL && *end != '\0'))
    {
      fprintf(stderr, "ctl tz: need hours, e.g. +8 or -5\n");
      return EXIT_FAILURE;
    }

  if (h < -12 || h > 14)
    {
      fprintf(stderr, "ctl tz: hours -12..+14\n");
      return EXIT_FAILURE;
    }

  if (myvendor_devctl_tz_min_set((int16_t)(h * 60)) < 0)
    {
      fprintf(stderr, "ctl tz: set failed\n");
      return EXIT_FAILURE;
    }

  printf("ctl tz  UTC%+d\n", h);
  return 0;
}

static bool favorite_name_valid(const char *name)
{
  size_t n;

  if (name == NULL)
    {
      return false;
    }

  n = strlen(name);
  if (n == 0 || n >= CTL_FAV_NAME_MAX)
    {
      return false;
    }

  return strchr(name, '\t') == NULL &&
         strchr(name, '\r') == NULL &&
         strchr(name, '\n') == NULL;
}

static bool favorite_coord_parse(const char *text, double min, double max,
                                 double *value)
{
  char *end = NULL;
  double v;

  if (text == NULL || value == NULL)
    {
      return false;
    }

  errno = 0;
  v = strtod(text, &end);
  if (errno != 0 || end == text || end == NULL || *end != '\0' ||
      !isfinite(v) || v < min || v > max)
    {
      return false;
    }

  *value = v;
  return true;
}

static int favorite_load(struct ctl_favorite_s *items, size_t cap,
                         size_t *count)
{
  FILE *fp;
  char line[160];
  size_t n = 0;

  if (items == NULL || count == NULL)
    {
      return -EINVAL;
    }

  *count = 0;
  fp = fopen(CTL_FAV_PATH, "r");
  if (fp == NULL)
    {
      return errno == ENOENT ? 0 : -errno;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      char *name;
      char *lat_text;
      char *lon_text;
      char *extra;
      char *save = NULL;
      size_t len = strlen(line);
      double lat;
      double lon;

      if (len > 0 && line[len - 1] == '\n')
        {
          line[--len] = '\0';
        }
      if (len > 0 && line[len - 1] == '\r')
        {
          line[--len] = '\0';
        }
      if (len == 0)
        {
          continue;
        }

      name = strtok_r(line, "\t", &save);
      lat_text = strtok_r(NULL, "\t", &save);
      lon_text = strtok_r(NULL, "\t", &save);
      extra = strtok_r(NULL, "\t", &save);
      if (!favorite_name_valid(name) || lat_text == NULL || lon_text == NULL ||
          extra != NULL ||
          !favorite_coord_parse(lat_text, -90.0, 90.0, &lat) ||
          !favorite_coord_parse(lon_text, -180.0, 180.0, &lon))
        {
          fclose(fp);
          return -EINVAL;
        }
      if (n >= cap)
        {
          fclose(fp);
          return -E2BIG;
        }

      snprintf(items[n].name, sizeof(items[n].name), "%s", name);
      items[n].latitude = lat;
      items[n].longitude = lon;
      n++;
    }

  if (ferror(fp))
    {
      int err = errno != 0 ? errno : EIO;

      fclose(fp);
      return -err;
    }

  fclose(fp);
  *count = n;
  return 0;
}

static int favorite_save(const struct ctl_favorite_s *items, size_t count)
{
  FILE *fp;
  size_t i;
  int ret = 0;

  fp = fopen(CTL_FAV_TMP_PATH, "w");
  if (fp == NULL)
    {
      return -errno;
    }

  for (i = 0; i < count; i++)
    {
      if (fprintf(fp, "%s\t%.7f\t%.7f\n", items[i].name,
                  items[i].latitude, items[i].longitude) < 0)
        {
          ret = -(errno != 0 ? errno : EIO);
          break;
        }
    }

  if (ret == 0 && fflush(fp) != 0)
    {
      ret = -errno;
    }
  if (ret == 0 && fsync(fileno(fp)) != 0)
    {
      ret = -errno;
    }
  if (fclose(fp) != 0 && ret == 0)
    {
      ret = -errno;
    }

  if (ret == 0 && rename(CTL_FAV_TMP_PATH, CTL_FAV_PATH) != 0)
    {
      ret = -errno;
    }
  if (ret != 0)
    {
      unlink(CTL_FAV_TMP_PATH);
    }

  return ret;
}

static int cmd_fav(int argc, char *argv[])
{
  struct ctl_favorite_s *items;
  const char *name;
  const char *lat_text;
  const char *lon_text;
  size_t count = 0;
  size_t i;
  int ret;
  int result = EXIT_FAILURE;

  items = calloc(CTL_FAV_MAX, sizeof(*items));
  if (items == NULL)
    {
      fprintf(stderr, "ctl fav: out of memory\n");
      return EXIT_FAILURE;
    }
  ret = favorite_load(items, CTL_FAV_MAX, &count);
  if (ret < 0)
    {
      fprintf(stderr, "ctl fav: load %s failed %d\n", CTL_FAV_PATH, ret);
      goto out;
    }

  if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0))
    {
      printf("favorite points: %u (%s)\n", (unsigned)count, CTL_FAV_PATH);
      for (i = 0; i < count; i++)
        {
          printf("  %s  lat %.7f  lon %.7f\n", items[i].name,
                 items[i].latitude, items[i].longitude);
        }
      result = EXIT_SUCCESS;
      goto out;
    }

  if (argc == 3 && (strcmp(argv[1], "del") == 0 ||
                    strcmp(argv[1], "delete") == 0))
    {
      for (i = 0; i < count; i++)
        {
          if (strcmp(items[i].name, argv[2]) == 0)
            {
              memmove(&items[i], &items[i + 1],
                      (count - i - 1) * sizeof(items[0]));
              count--;
              ret = favorite_save(items, count);
              if (ret < 0)
                {
                  fprintf(stderr, "ctl fav: save failed %d\n", ret);
                  goto out;
                }
              printf("ctl fav: deleted %s\n", argv[2]);
              result = EXIT_SUCCESS;
              goto out;
            }
        }

      fprintf(stderr, "ctl fav: point '%s' not found\n", argv[2]);
      goto out;
    }

  if (argc == 5 && strcmp(argv[1], "set") == 0)
    {
      name = argv[2];
      lat_text = argv[3];
      lon_text = argv[4];
    }
  else if (argc == 4)
    {
      name = argv[1];
      lat_text = argv[2];
      lon_text = argv[3];
    }
  else
    {
      fprintf(stderr,
              "ctl fav: use <name> <lat> <lon>, list, or del <name>\n");
      goto out;
    }

  if (!favorite_name_valid(name))
    {
      fprintf(stderr, "ctl fav: name must be 1..%u bytes without tab/newline\n",
              CTL_FAV_NAME_MAX - 1);
      goto out;
    }

  for (i = 0; i < count; i++)
    {
      if (strcmp(items[i].name, name) == 0)
        {
          break;
        }
    }
  if (i == count && count >= CTL_FAV_MAX)
    {
      fprintf(stderr, "ctl fav: at most %u points\n", CTL_FAV_MAX);
      goto out;
    }
  if (!favorite_coord_parse(lat_text, -90.0, 90.0, &items[i].latitude) ||
      !favorite_coord_parse(lon_text, -180.0, 180.0, &items[i].longitude))
    {
      fprintf(stderr,
              "ctl fav: invalid coordinates (latitude -90..90, longitude -180..180)\n");
      goto out;
    }

  snprintf(items[i].name, sizeof(items[i].name), "%s", name);
  if (i == count)
    {
      count++;
    }
  ret = favorite_save(items, count);
  if (ret < 0)
    {
      fprintf(stderr, "ctl fav: save failed %d\n", ret);
      goto out;
    }

  printf("ctl fav: saved %s  lat %.7f  lon %.7f\n", name,
         items[i].latitude, items[i].longitude);
  result = EXIT_SUCCESS;

out:
  free(items);
  return result;
}

/**
 * @brief `ctl gnss dump_in|dump_out|auto`。
 */
static int cmd_gnss(int argc, char *argv[])
{
  char path[96];
  uint32_t start;
  int ret;
  bool dump_out;
  bool on;

  if (argc >= 2 && strcmp(argv[1], "auto") == 0)
    {
      if (argc < 3)
        {
          printf("ctl gnss auto %s\n",
                 myvendor_devctl_eph_auto_get() ? "on" : "off");
          return 0;
        }

      if (parse_onoff(argv[2], &on) != 0)
        {
          fprintf(stderr, "ctl gnss auto: use on|off\n");
          return EXIT_FAILURE;
        }

      (void)myvendor_devctl_eph_auto_set(on);
      if (on)
        {
          myvendor_gnss_eph_reload();
        }

      printf("ctl gnss auto %s\n", on ? "on" : "off");
      return 0;
    }

  if (argc >= 2 && (strcmp(argv[1], "bdsonly") == 0 ||
                    strcmp(argv[1], "b1c") == 0 ||
                    strcmp(argv[1], "ver") == 0))
    {
      /* 星座实验：见 myvendor_gnss_probe。真正写串口在 GNSS 线程，
       * 这里只是落请求，所以立刻返回、效果看随后的日志。 */
      if (strcmp(argv[1], "b1c") == 0)
        {
          myvendor_gnss_probe(3);
          printf("ctl gnss b1c: 试开 B1C 中（看 gnss: probe b1c key=… rc=…）\n");
          return 0;
        }

      if (strcmp(argv[1], "ver") == 0)
        {
          myvendor_gnss_probe(4);
          printf("ctl gnss ver: 读 MON-VER 中（看 gnss: mon ver sw=… hw=…）\n");
          return 0;
        }

      on = true;
      if (argc >= 3 && parse_onoff(argv[2], &on) != 0)
        {
          fprintf(stderr, "ctl gnss bdsonly: use on|off\n");
          return EXIT_FAILURE;
        }

      myvendor_gnss_probe(on ? 1 : 2);
      printf("ctl gnss bdsonly %s: 看 link extra 的 gsv gp/gl/ga/bd/gq\n",
             on ? "on" : "off");
      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "still") == 0)
    {
      /* 停车切 stationary：默认关；开关在 GNSS 线程 1 s 内生效
       * （见 myvendor_gnss_dyn_still_enable 的说明）。 */
      if (argc < 3 || parse_onoff(argv[2], &on) != 0)
        {
          fprintf(stderr, "ctl gnss still: use on|off\n");
          return EXIT_FAILURE;
        }

      myvendor_gnss_dyn_still_enable(on);
      printf("ctl gnss still %s（看 gnss: still switch / motion still|resume）\n",
             on ? "on" : "off");
      return 0;
    }

  if (argc < 2 ||
      (strcmp(argv[1], "dump_out") != 0 &&
       strcmp(argv[1], "dump_in") != 0 &&
       strcmp(argv[1], "dump") != 0))
    {
      fprintf(stderr,
              "ctl gnss dump_out  — poll UBX-MGA-DBD -> mga_<utc>.ubx\n"
              "ctl gnss dump_in   — inject newest mga_<utc>.ubx\n"
              "ctl gnss auto [on|off]\n"
              "ctl gnss still [on|off]  停车切 stationary（默认关）\n");
      return EXIT_FAILURE;
    }

  dump_out = (strcmp(argv[1], "dump_in") != 0);
  if (dump_out)
    {
      ret = myvendor_gnss_dump_request();
    }
  else
    {
      ret = myvendor_gnss_inject_request();
    }

  if (ret == -ENOTSUP)
    {
      fprintf(stderr, "ctl gnss %s: GNSS not in this image\n", argv[1]);
      return EXIT_FAILURE;
    }

  if (ret == -ENODEV)
    {
      fprintf(stderr, "ctl gnss %s: GNSS thread not started\n", argv[1]);
      return EXIT_FAILURE;
    }

  if (ret == -EBUSY)
    {
      fprintf(stderr, "ctl gnss %s: already running\n", argv[1]);
      return EXIT_FAILURE;
    }

  if (ret == -ENODATA)
    {
      fprintf(stderr, "ctl gnss dump_out: need a live 2D/3D fix first\n");
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl gnss %s: request failed %d\n", argv[1], ret);
      return EXIT_FAILURE;
    }

  printf("ctl gnss %s: started\n", argv[1]);
  fflush(stdout);
  start = 0;
  while (start < 120)
    {
      if (dump_out)
        {
          if (!myvendor_gnss_dump_busy())
            {
              if (myvendor_gnss_dump_last_path(path, sizeof(path)))
                {
                  printf("ctl gnss dump_out: done -> %s\n", path);
                }
              else
                {
                  printf("ctl gnss dump_out: finished without file (see syslog)\n");
                }

              return 0;
            }
        }
      else if (!myvendor_gnss_eph_busy())
        {
          if (myvendor_gnss_inject_last_path(path, sizeof(path)))
            {
              printf("ctl gnss dump_in: done <- %s (see syslog)\n", path);
            }
          else
            {
              printf("ctl gnss dump_in: finished without file (see syslog)\n");
            }

          return 0;
        }

      usleep(100000);
      start++;
    }

  fprintf(stderr, "ctl gnss %s: timeout, still running (syslog)\n", argv[1]);
  return EXIT_FAILURE;
}

/**
 * @brief `ctl idle` 无参数打印子命令；`on` 立刻进静止（等同 10 分钟耗尽）；
 *        `hour` 耗尽 1 小时并关机；`off` 唤醒；`stay` 本次开机禁止自动静止；
 *        `auto` 恢复。
 */
static void idle_usage(void)
{
  printf("ctl idle — still-screen debug\n");
  printf("  ctl idle on     enter still now (same as 10 min timeout)\n");
  printf("  ctl idle off    wake backlight + GNSS\n");
  printf("  ctl idle hour   exhaust 1 h still then power off  (1h)\n");
  printf("  ctl idle stay   this boot: no auto still  (hold|never)\n");
  printf("  ctl idle auto   restore auto still\n");
}

static int cmd_idle(int argc, char *argv[])
{
  bool on = true;
  int ret;
  myvendor_bicycle_ctl_op_t op;
  const char *arg = NULL;
  const char *ok = NULL;

  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0)
    {
      idle_usage();
      return 0;
    }

  if (strcmp(argv[1], "hour") == 0 || strcmp(argv[1], "1h") == 0 ||
      strcmp(argv[1], "offhour") == 0)
    {
      op = MYVENDOR_BICYCLE_CTL_OP_IDLE_HOUR;
      ok = "hour";
    }
  else if (strcmp(argv[1], "stay") == 0 ||
           strcmp(argv[1], "hold") == 0 ||
           strcmp(argv[1], "never") == 0)
    {
      op = MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY;
      arg = "1";
      ok = "stay";
    }
  else if (strcmp(argv[1], "auto") == 0)
    {
      op = MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY;
      arg = "0";
      ok = "auto";
    }
  else if (parse_onoff(argv[1], &on) == 0)
    {
      op = on ? MYVENDOR_BICYCLE_CTL_OP_IDLE_ENTER
              : MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE;
      ok = on ? "on" : "off";
    }
  else
    {
      idle_usage();
      fprintf(stderr, "ctl idle: unknown '%s'\n", argv[1]);
      return EXIT_FAILURE;
    }

  ret = myvendor_bicycle_ctl_post(op, arg);
  if (ret == -ENODEV)
    {
      fprintf(stderr, "ctl idle: UI not running\n");
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl idle: post failed %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("ctl idle %s\n", ok);
  return 0;
}

/**
 * @brief 打印跳频自检状态（没在跑就不打）。
 *
 * @details
 * 跳频期间档位是走强制通道设的，`sys dvfs` 只会显示 `force`，看不出是谁设的；
 * 这里补一行周期与累计跳变次数，方便确认命令生效、估算跑了多久。
 */
static void dvfs_hop_status_line(void)
{
#ifdef CONFIG_MYVENDOR_DVFS
  unsigned period;
  unsigned count;

  if (!sf32lb_dvfs_hop_active())
    {
      return;
    }

  period = (unsigned)sf32lb_dvfs_hop_period_ms();
  count = (unsigned)sf32lb_dvfs_hop_count();
  printf("  dvfs    hop     every %u ms, %u jumps done\n", period, count);
  printf("  dvfs    hop     stop with: ctl dvfs hop off\n");

  /* 调频 worker 心跳。这两行是判"调频是不是已经悄悄死了"的唯一依据：
   * age 应该始终只有个位数毫秒，且 runs 一直在涨；两者任意一个不动，
   * 就说明 gov_worker 的重挂链断了 —— 那时 hop 的 jumps done 也会冻住。 */
  {
    uint32_t age_ms = 0;
    uint32_t runs = 0;

    sf32lb_dvfs_gov_stat(&age_ms, &runs);
    printf("  dvfs    gov     last run %u ms ago, %u runs, %u recoveries\n",
           (unsigned)age_ms, (unsigned)runs,
           (unsigned)sf32lb_dvfs_gov_recoveries());
  }
#endif
}

/**
 * @brief `ctl log [level]`：全系统 syslog 屏蔽位，省略则打印当前值。
 *
 * @details
 * 改的是 `g_syslog_mask`，并且 `nx_setlogmask()` 会**遍历所有已存在的任务**
 * 一起改（drivers/syslog/setlogmask.c 的 task_syslogmask），新任务在
 * sched/tls/task_initinfo.c 里继承这个全局值 —— 所以立刻全系统生效、
 * 不需要重编译，也能运行时随时调回来。
 *
 * **屏蔽的是 syslog 输出**（带 `[  INFO]` / `[  WARN]` 前缀的那些，闸门在
 * libs/libc/syslog/lib_syslog.c 的 `ta_syslog_mask & LOG_MASK(priority)`）。
 * `printf` 不过 syslog（NSH 提示符、test_app、ctl 自己的输出），**不受影响** ——
 * 所以这个开关能压掉驱动/内核的刷屏，但压不掉应用 printf。
 *
 * **档位落 KV**（`persist.log.mask`，落在 `myvendor_devctl_load()` 里恢复）：
 * 以前每次开机都要重敲一遍。写盘失败只警告，不回滚当前档位。
 *
 * 建议值：平时 `err`（只留错误），查问题前 `info` 或 `all`。
 */
static int cmd_log(int argc, char *argv[])
{
  /* POSIX 优先级：数值越小越严重，LOG_UPTO(p) 保留 0..p。 */
  static const char * const names[] =
  {
    "EMERG", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"
  };

  uint8_t mask;
  uint8_t old;
  int want = -1;
  int ret;
  int i;

  if (argc < 2)
    {
      mask = myvendor_devctl_log_mask_get();
      printf("syslog mask 0x%02x  (%s)\n", (unsigned)mask,
             MYVENDOR_DEVCTL_LOG_KEY);
      for (i = 0; i < 8; i++)
        {
          printf("  %-6s %s\n", names[i],
                 (mask & LOG_MASK(i)) != 0 ? "on" : "off");
        }

      return 0;
    }

  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0)
    {
      printf("ctl log                 print current syslog mask\n");
      printf("ctl log err|warn|notice|info|all   keep levels up to this\n");
      printf("ctl log silent          only EMERG\n");
      printf("ctl log 0x1f            set raw mask\n");
      printf("  saved as %s, restored on boot\n", MYVENDOR_DEVCTL_LOG_KEY);
      printf("  (printf is NOT syslog; NSH/apps keep printing)\n");
      return 0;
    }

  if (strcmp(argv[1], "err") == 0 || strcmp(argv[1], "error") == 0)
    {
      want = LOG_UPTO(LOG_ERR);
    }
  else if (strcmp(argv[1], "warn") == 0 || strcmp(argv[1], "warning") == 0)
    {
      want = LOG_UPTO(LOG_WARNING);
    }
  else if (strcmp(argv[1], "notice") == 0)
    {
      want = LOG_UPTO(LOG_NOTICE);
    }
  else if (strcmp(argv[1], "info") == 0)
    {
      want = LOG_UPTO(LOG_INFO);
    }
  else if (strcmp(argv[1], "all") == 0 || strcmp(argv[1], "debug") == 0)
    {
      want = 0xff;
    }
  else if (strcmp(argv[1], "silent") == 0 || strcmp(argv[1], "none") == 0)
    {
      want = LOG_MASK(LOG_EMERG);
    }
  else
    {
      char *end = NULL;
      unsigned long raw = strtoul(argv[1], &end, 0);

      if (end == argv[1] || *end != '\0' || raw > 0xff)
        {
          fprintf(stderr, "ctl log: unknown level '%s'\n", argv[1]);
          return EXIT_FAILURE;
        }

      want = (int)raw;
    }

  mask = (uint8_t)want;
  ret = myvendor_devctl_log_mask_set(mask, &old);

  printf("syslog mask 0x%02x -> 0x%02x\n", (unsigned)old, (unsigned)mask);
  for (i = 0; i < 8; i++)
    {
      printf("  %-6s %s\n", names[i],
             (mask & LOG_MASK(i)) != 0 ? "on" : "off");
    }

  if (ret < 0)
    {
      /* 档位已经生效，只是没写进 KV：说清楚，免得下次开机发现"又回去了"。 */
      printf("  WARN  %s write failed (%d), lost after reboot\n",
             MYVENDOR_DEVCTL_LOG_KEY, ret);
    }

  return 0;
}

/**
 * @brief `ctl dvfs auto|72|96|144|240|hop [ms]`：强制/自动 HCLK，或跑跳频自检。
 *
 * @details
 * `hop` 每 period（默认 10 s）跳到下一个档位，用来反复走切频路径；档位次序
 * 保证相邻不重复且每轮覆盖 72/96/144/240。`hop off`、`auto` 或显式指定档位
 * 都会停止自检并把控制权交还 governor。
 */
static int cmd_dvfs(int argc, char *argv[])
{
#ifdef CONFIG_MYVENDOR_DVFS
  int ret;
  uint32_t mhz = 0;

  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0)
    {
      printf("ctl dvfs auto|72|96|144|240  (ondemand; not 48/24)\n");
      printf("ctl dvfs hop [ms]   random-ish hop every ms (default %u)\n",
             (unsigned)SF32LB_DVFS_HOP_DEFAULT_MS);
      printf("ctl dvfs hop off    stop hopping, back to ondemand\n");
      sf32lb_dvfs_print();
      return 0;
    }

  if (strcmp(argv[1], "hop") == 0 || strcmp(argv[1], "rand") == 0)
    {
      if (argc >= 3 &&
          (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "stop") == 0 ||
           strcmp(argv[2], "0") == 0))
        {
          sf32lb_dvfs_hop_stop();
          sf32lb_dvfs_print();
          return 0;
        }

      {
        uint32_t period = SF32LB_DVFS_HOP_DEFAULT_MS;

        if (argc >= 3)
          {
            char *end = NULL;
            unsigned long ms = strtoul(argv[2], &end, 10);

            if (end == argv[2] || *end != '\0')
              {
                fprintf(stderr, "ctl dvfs hop: period must be ms (>=%u)\n",
                        (unsigned)SF32LB_DVFS_HOP_MIN_MS);
                return EXIT_FAILURE;
              }

            period = (uint32_t)ms;
          }

        ret = sf32lb_dvfs_hop_start(period);
        if (ret == -EINVAL)
          {
            fprintf(stderr, "ctl dvfs hop: period %u out of [%u, %u] ms\n",
                    (unsigned)period, (unsigned)SF32LB_DVFS_HOP_MIN_MS,
                    (unsigned)SF32LB_DVFS_HOP_MAX_MS);
            return EXIT_FAILURE;
          }

        if (ret < 0)
          {
            fprintf(stderr, "ctl dvfs hop: start failed %d\n", ret);
            return EXIT_FAILURE;
          }

        sf32lb_dvfs_print();
        dvfs_hop_status_line();
        return 0;
      }
    }

  if (strcmp(argv[1], "auto") == 0 || strcmp(argv[1], "off") == 0)
    {
      mhz = 0;
    }
  else if (strcmp(argv[1], "low") == 0)
    {
      mhz = SF32LB_DVFS_MHZ_72;
    }
  else if (strcmp(argv[1], "high") == 0)
    {
      mhz = SF32LB_DVFS_MHZ_240;
    }
  else
    {
      char *end = NULL;
      unsigned long v = strtoul(argv[1], &end, 10);

      if (end == argv[1] || *end != '\0' || v > 240ul)
        {
          fprintf(stderr, "ctl dvfs: need auto|72|96|144|240|hop\n");
          return EXIT_FAILURE;
        }

      mhz = (uint32_t)v;
    }

  /* 走到这里说明是 auto 或显式档位：先停掉跳频自检，否则下一次跳变会把
   * 用户刚设的档位覆盖掉。hop 分支在上面已经 return 了。 */
  sf32lb_dvfs_hop_stop();

  ret = sf32lb_dvfs_force(mhz);
  if (ret == -EINVAL)
    {
      fprintf(stderr, "ctl dvfs: gear must be 72, 96, 144 or 240\n");
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl dvfs: apply failed %d\n", ret);
      return EXIT_FAILURE;
    }

  sf32lb_dvfs_print();
  return 0;
#else
  (void)argc;
  (void)argv;
  fprintf(stderr, "ctl dvfs: CONFIG_MYVENDOR_DVFS is off\n");
  return EXIT_FAILURE;
#endif
}

/**
 * @brief 裸块读写自检：不进 LFS、绕过判死闸门。
 *
 * @details 存在的理由：卡一旦判死，**LFS 层就不下发 I/O 了**，常规读写全部
 *          立刻返回 -EIO，于是没法再判断"卡到底还响不响应"。这条命令刻意
 *          绕开闸门，是判死之后唯一还能直接验卡的手段。逐块报时间也是必要的：
 *          坏页/磨损页的典型特征是**某一块明显慢或直接失败**，只看总结果分辨不出来。
 *
 * @note 它同时证伪过一个假设，别再捡回来：曾经怀疑"首次失败总是落在 LFS 块 194
 *       （偏移 0xC2000）"，用 `ctl sd read 200c2000 8` 实测 8 个块全部正常
 *       （约 880 µs），而另一次记录的首次失败偏移是 0x20000000（块 0）。
 *       "卡在某个坏块上"这个方向因此被排除，真正的原因是控制器卡在 CMD_BUSY
 *       （见 docs/sd_recovery.md）。本命令保留为通用的验卡工具，不再代表那个假设。
 */
static int cmd_sd(int argc, char **argv)
{
  uint64_t off;
  uint32_t n;

  if (argc >= 3 && strcmp(argv[1], "read") == 0)
    {
      off = strtoull(argv[2], NULL, 16);
      n = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 1u;
      if (n == 0u || n > 256u)
        {
          n = 1u;
        }

      return sf32lb_sd_raw_read(off, n);
    }

  if (argc >= 3 && strcmp(argv[1], "write") == 0)
    {
      off = strtoull(argv[2], NULL, 16);
      n = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 1u;
      if (n == 0u || n > 256u)
        {
          n = 1u;
        }

      /* 写目标只有驱动器尾部那 1 MiB 是安全的（唯一不属于任何文件系统的
       * 区域）；驱动内部还有一道不可绕过的闸门，这里只是提前给用法。 */
      return sf32lb_sd_raw_write(off, n);
    }

  printf("ctl sd read  <hex_off> [n]    raw 512B block read, per-block timing\n");
  printf("ctl sd write <hex_off> [n]    0xFF program + readback verify\n");
  printf("  both bypass the dead gate on purpose, so they work on a dead card\n");
  printf("  e.g. ctl sd read  200c2000 8        (LFS block 194)\n");
  printf("       ctl sd write <card_size-4096> 32   (sacrificial tail ONLY)\n");
  return -EINVAL;
}

/**
 * @brief DWT 数据观察点：给"谁改坏了这块内存"点名。
 *
 * @details 为什么需要它：n004 的 GATT 回调表、2026-09-18 的 wdog 活动链表都是
 *          被外力改坏的，日志只留下"值变成什么"，没有"谁写的"。挂一个观察点
 *          之后：命中由硬件在 `DWT_FUNCTIONn.MATCHED` 上留痕（事后 `ctl wt`
 *          直接读得到），并且本板无调试器时会交付成异常 → 既有 coredump，
 *          `pc` 就是那条访问指令，按 n004 的老办法 addr2line 反解。
 *
 * @note DWT 接在核的 load/store 通路上，**DMA 写不会命中**："值坏了但
 *       MATCHED=0" 本身就是结论（不是 CPU 写的）。观察点复位即失效，不持久。
 */
/**
 * @brief DWT 观察点只接受 HCPU 自己的两块 RAM。
 *
 * @details 两道理由，缺一不可：
 *          1. 安全：地址来自命令行，不能让任意数值直接变成被监视的内存；
 *             白名单之外一律拒绝（与 wdog 覆写里的 wd_node_sane 同一套边界）。
 *          2. 工程上：观察外设/保留区没有意义，DWT 比较器只有 4 个。
 */
static bool wt_addr_ok(uint32_t addr)
{
  if (addr >= 0x20000000u && addr < 0x20080000u)   /* HCPU SRAM */
    {
      return true;
    }

  if (addr >= 0x60000000u && addr < 0x68000000u)   /* PSRAM / heap */
    {
      return true;
    }

  return false;
}

static int cmd_watch(int argc, char **argv)
{
  bool acted = false;

  if (argc >= 2 && strcmp(argv[1], "clear") == 0)
    {
      myvendor_dwt_watch_clear();
      printf("wt: cleared\n");
      acted = true;
    }
  else if (argc >= 2 && strcmp(argv[1], "test") == 0)
    {
      /* 自我触发：挂在一个我们自己写的变量上，然后写它。2026-09-18 实测：
       * 本板 **不会**把它交付成异常（这一行后面照常打印），所以能靠的只有
       * MATCHED 位 —— 它同样证明了"比较器抓到了这次写"。 */
      static volatile uint32_t probe __attribute__((aligned(4)));
      int ret = myvendor_dwt_watch_arm((uint32_t)(uintptr_t)&probe, 4);

      if (ret != 0)
        {
          printf("wt test: arm failed %d\n", ret);
          return ret;
        }

      printf("wt test: armed on probe(%08lx), writing now\n",
             (unsigned long)(uintptr_t)&probe);
      probe = 0x12345678u;
      printf("wt test: write returned (no fault on this part), check MATCHED\n");
      acted = true;
    }
  else if (argc >= 2 && argv[1][0] != '\0')
    {
      /* 裸十六进制地址（不带 0x 也接受，和 ctl sd 一致）。**白名单是真正的
       * 安全边界**：地址来自命令行，只允许 HCPU SRAM / PSRAM。 */
      char *end = NULL;
      unsigned long parsed = strtoul(argv[1], &end, 16);
      unsigned size = (argc >= 3) ? (unsigned)strtoul(argv[2], NULL, 10) : 4u;
      uint32_t addr = (uint32_t)parsed;
      int ret;

      if (end == NULL || *end != '\0' || parsed > 0xfffffffful)
        {
          printf("wt: '%s' is not a hex address\n", argv[1]);
          return -EINVAL;
        }

      if (!wt_addr_ok(addr))
        {
          printf("wt: only HCPU SRAM 0x20000000-0x2007ffff or PSRAM 0x60000000-0x67ffffff\n");
          return -EINVAL;
        }

      ret = myvendor_dwt_watch_arm(addr, size);
      if (ret != 0)
        {
          printf("wt: arm %08lx size=%u failed %d (comparators full?)\n",
                 (unsigned long)addr, size, ret);
          return ret;
        }

      printf("wt: armed %08lx size=%u\n", (unsigned long)addr, size);
      acted = true;
    }

  {
    char buf[256];

    myvendor_dwt_watch_status(buf, sizeof(buf));
    printf("%s", buf);
  }

  if (!acted || argc < 2)
    {
      printf("ctl wt <hexaddr> [1|2|4]   arm a DWT data watchpoint (RAM only)\n");
      printf("ctl wt clear               remove all watchpoints\n");
      printf("ctl wt test                self-test (may fault/reset on purpose)\n");
      printf("ctl wt                     show armed watchpoints + MATCHED bits\n");
      printf("  ON THIS BOARD a hit sets MATCHED but is NOT delivered as a fault,\n");
      printf("  so MATCHED answers \"did the CPU touch this word\" (DMA never hits);\n");
      printf("  naming the writer's pc needs a debugger attached\n");
    }

  return 0;
}

/**
 * @brief NSH 入口。
 */
int main(int argc, FAR char *argv[])
{
  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0)
    {
      usage();
      return 0;
    }

  if (strcmp(argv[1], "bl") == 0 || strcmp(argv[1], "backlight") == 0)
    {
      return cmd_bl(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "radio") == 0 || strcmp(argv[1], "bt") == 0)
    {
      return cmd_switch("radio", argc - 1, argv + 1,
                        myvendor_devctl_radio_set);
    }

  if (strcmp(argv[1], "sensor") == 0)
    {
      /* `ctl sensor bind <kind 0/1/2> <mac> [addr_type]`：按 MAC 重绑一台传感器。
       * 存在的意义：记录删掉之后设备就不再回连它，而重绑要进菜单按好几下键 ——
       * 远端（串口/monitor）要能把这条恢复回去。走的是 UI 点的同一条投递路径
       * （`myvendor_devctl_sensor_connect_addr` → companion 线程执行），
       * NSH 线程不直接碰协议栈。 */
      if (argc >= 3 && strcmp(argv[2], "bind") == 0)
        {
          return cmd_sensor_bind(argc - 2, argv + 2);
        }

      return cmd_switch("sensor", argc - 1, argv + 1,
                        myvendor_devctl_sensor_set);
    }

  if (strcmp(argv[1], "sd") == 0)
    {
      return cmd_sd(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "wt") == 0 || strcmp(argv[1], "watch") == 0)
    {
      return cmd_watch(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "mtp") == 0)
    {
      return cmd_switch("mtp", argc - 1, argv + 1,
                        myvendor_devctl_mtp_set);
    }

  if (strcmp(argv[1], "notif") == 0)
    {
      return cmd_switch("notif", argc - 1, argv + 1,
                        myvendor_devctl_notif_set);
    }

  if (strcmp(argv[1], "calls") == 0)
    {
      return cmd_switch("calls", argc - 1, argv + 1,
                        myvendor_devctl_notif_calls_only_set);
    }

  if (strcmp(argv[1], "sound") == 0)
    {
      return cmd_switch("sound", argc - 1, argv + 1,
                        myvendor_devctl_sound_set);
    }

  if (strcmp(argv[1], "autopause") == 0)
    {
      return cmd_switch("autopause", argc - 1, argv + 1,
                        myvendor_devctl_autopause_set);
    }

  if (strcmp(argv[1], "grade") == 0)
    {
      return cmd_grade(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "tz") == 0)
    {
      return cmd_tz(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "fav") == 0 ||
      strcmp(argv[1], "favorite") == 0)
    {
      return cmd_fav(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "gnss") == 0)
    {
      return cmd_gnss(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "pair") == 0)
    {
      return myvendor_bt_pair_cmd(argc - 2, argv + 2);
    }

  if (strcmp(argv[1], "nvds") == 0)
    {
      return sf32lb52_bt_nvds_dump();
    }

  if (strcmp(argv[1], "nav") == 0)
    {
      return myvendor_nav_test(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "idle") == 0 || strcmp(argv[1], "still") == 0)
    {
      return cmd_idle(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "dvfs") == 0 || strcmp(argv[1], "hclk") == 0)
    {
      return cmd_dvfs(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "log") == 0 || strcmp(argv[1], "loglevel") == 0)
    {
      return cmd_log(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "pwr") == 0 || strcmp(argv[1], "power") == 0)
    {
      return cmd_pwr(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "poweroff") == 0)
    {
      myvendor_devctl_poweroff();
      return EXIT_FAILURE;
    }

  fprintf(stderr, "ctl: unknown '%s'\n", argv[1]);
  usage();
  return EXIT_FAILURE;
}
