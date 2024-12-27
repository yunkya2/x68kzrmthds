/*
 * Copyright (c) 2024 Yuichi Nakamura (@yunkya2)
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>

#include <x68k/iocs.h>
#include <x68k/dos.h>

#include <zusb.h>
#include <scsi_cmd.h>
#include <config.h>
#include <vd_command.h>

#include "zremoteimg.h"
#include "zusbcomm.h"

//****************************************************************************
// Global variables
//****************************************************************************

struct dos_req_header *reqheader;         // Human68kからのリクエストヘッダ
jmp_buf jenv;                             // ZUSB通信エラー時のジャンプ先
int hds_scsiid;                           // リモートHDSのSCSI ID

extern struct zusb_rmtdata zusb_rmtdata;  // リモートドライブドライバ間で共有するデータ
extern void *scsidrv_org;                 // ベクタ変更前のIOCS _SCSIDRV処理アドレス
extern uint8_t hdsscsi_mask;              // リモートHDSドライバのSCSI ID処理マスク
extern void scsidrv_hds();                // 変更後のIOCS _SCSIDRV処理
#ifdef CONFIG_BOOTDRIVER
extern uint8_t scsiidd2;                  // デバイスドライバ登録時のSCSI ID + 1
#endif

#ifdef DEBUG
int debuglevel = 0;
#endif

static union {
  struct cmd_hdsread  cmd_hdsread;
  struct res_hdsread  res_hdsread;

  struct cmd_hdswrite cmd_hdswrite;
  struct res_hdswrite res_hdswrite;

  struct res_hdsread_full  res_hdsread_full;
  struct cmd_hdswrite_full cmd_hdswrite_full;
} b;

#define N_PART    15                      // 最大のパーティション数

const struct dos_bpb defaultbpb =
{ 512, 1, 2, 1, 224, 2880, 0xf7, 9, 0, 0 };

struct dos_bpb bpb[N_HDS][N_PART];
struct dos_bpb *bpbtable[26];
uint8_t drive_changed[26];

// ユニットごとの情報
struct unitinfo {
  uint32_t size;      // ユニット全体のサイズ
  uint8_t type;       // ユニットの種類
  uint8_t curparts;   // 現在のパーティション数
  uint8_t firstdrive; // ユニットの先頭ドライブ番号
  uint8_t lastdrive;  // ユニットの最終ドライブ番号+1
} unitinfo[N_HDS];

#ifdef CONFIG_BOOTDRIVER
#define _dos_putchar(...)   _iocs_b_putc(__VA_ARGS__)
#define _dos_print(...)     _iocs_b_print(__VA_ARGS__)
#endif

//****************************************************************************
// for debugging
//****************************************************************************

#ifdef DEBUG
char heap[1024];                // temporary heap for debug print
void *_HSTA = heap;
void *_HEND = heap + 1024;
void *_PSP;

#ifndef CONFIG_BOOTDRIVER
void DPRINTF(int level, char *fmt, ...)
{
  char buf[256];
  va_list ap;

  if (debuglevel < level)
    return;

  va_start(ap, fmt);
  vsiprintf(buf, fmt, ap);
  va_end(ap);
#ifndef DEBUG_UART
  _iocs_b_print(buf);
#else
  char *p = buf;
  while (*p) {
    if (*p == '\n') {
      while (_iocs_osns232c() == 0)
        ;
      _iocs_out232c('\r');
    }
    while (_iocs_osns232c() == 0)
      ;
    _iocs_out232c(*p++);
  }
#endif
}
#else
void DPRINTF(int level, char *fmt, ...)
{
}
#endif
#else
#define DPRINTF(...)
#endif

#define DPRINTF1(...)  DPRINTF(1, __VA_ARGS__)
#define DPRINTF2(...)  DPRINTF(2, __VA_ARGS__)
#define DPRINTF3(...)  DPRINTF(3, __VA_ARGS__)

//****************************************************************************
// Private functions
//****************************************************************************

static int sector_read(int unit, uint8_t *buf, uint32_t pos, int nsect)
{
  struct cmd_hdsread *cmd = &b.cmd_hdsread;
  struct res_hdsread *res = &b.res_hdsread;

  cmd->command = CMD_HDSREAD;
  cmd->unit = unit;
  cmd->nsect = nsect;
  cmd->pos = pos;
  com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res) + nsect * 512);

  if (res->status == VDERR_EINVAL) {
    return 0x1002;      // Drive not ready
  }
  if (res->status != VDERR_OK) {
    return 0x7007;      // Medium error
  }

  memcpy(buf, res->data, nsect * 512);
  return 0;
}

static int sector_write(int unit, uint8_t *buf, uint32_t pos, int nsect)
{
  struct cmd_hdswrite *cmd = &b.cmd_hdswrite;
  struct res_hdswrite *res = &b.res_hdswrite;

  cmd->command = CMD_HDSWRITE;
  cmd->unit = unit;
  cmd->nsect = nsect;
  cmd->pos = pos;
  memcpy(cmd->data, buf, 512 * nsect);
  com_cmdres(cmd, sizeof(*cmd) + 512 * nsect, res, sizeof(*res));

  if (res->status == VDERR_EINVAL) {
    return 0x1002;      // Drive not ready
  }
  if (res->status != VDERR_OK) {
    return 0x7007;      // Medium error
  }

  return 0;
}

static int read_bpb(int unit, int maxparts)
{
  int part = 0;
  uint8_t sector[512];

  for (int i = 0; i < N_PART; i++) {
    bpb[unit][i] = defaultbpb;    // BPBが取得できなかった場合のデフォルト値
  }

  // SCSIイメージ signature を確認する
  if (sector_read(unit, sector, 0, 1) != 0) {
    return 0;
  }
  if (memcmp(sector, "X68SCSI1", 8) != 0) {
    return 0;
  }

  // パーティション情報を取得する
  if (sector_read(unit, sector, 2 * 2, 1) != 0) {
    return 0;
  }
  if (memcmp(sector, "X68K", 4) != 0) {
    return 0;
  }

  // Human68kパーティションを検索してBPBを取得する
  uint8_t *p = sector + 16;
  for (int i = 0; i < 15; i++, p += 16) {
    if (memcmp(p, "Human68k", 8) == 0) {
      if (p[8] & 1) {   // パーティションフラグのbit0が立っている場合は使用不可
        continue;
      }

      uint32_t sect = *(uint32_t *)(p + 8) & 0xffffff;
      uint8_t bootsect[512];

      if (sector_read(unit, bootsect, sect * 2, 1) != 0) {
        return 0;
      }
      memcpy(&bpb[unit][part], &bootsect[0x12], sizeof(*bpb));
      part++;
      if (part >= maxparts) {
        break;
      }
    }
  }

  return part;
}

//****************************************************************************
// Device driver interrupt rountine
//****************************************************************************

int com_init(struct dos_req_header *req)
{
  _dos_print("\r\nX68000 Z Remote Image Driver (version " GIT_REPO_VERSION ")\r\n");

  int units;
  int drives;

  int ch = com_connect(true);
  if (ch < 0) {
    _dos_print("ZUSB デバイスが見つかりません\r\n");
    return -0x700d;
  } else if (com_rmtdata == NULL) {
    com_rmtdata = &zusb_rmtdata;
    com_rmtdata->zusb_ch = ch;
  }

  if (setjmp(jenv)) {
    com_disconnect();
    _dos_print("リモートドライブ用 Raspberry Pi Pico W が接続されていません\r\n");
    return -0x700d;
  }

  {
    struct cmd_getinfo cmd;
    struct res_getinfo res;
    cmd.command = CMD_GETINFO;
    com_cmdres(&cmd, sizeof(cmd), &res, sizeof(res));

    if (res.version != PROTO_VERSION) {
      com_disconnect();
      _dos_print("リモートドライブ用 Raspberry Pi Pico W のバージョンが異なります\r\n");
      return -0x700d;
    }

    // ファイル共有サーバから取得した現在時刻を設定する
    if (res.year > 0 && !(com_rmtdata->rmtflag & 0x80)) {
      *(volatile uint8_t *)0xe8e000 = 'T';
      *(volatile uint8_t *)0xe8e000 = 'W';
      *(volatile uint8_t *)0xe8e000 = 0;    // disable RTC auto adjust
      _iocs_timeset(_iocs_timebcd((res.hour << 16) | (res.min << 8) | res.sec));
      _iocs_bindateset(_iocs_bindatebcd((res.year << 16) | (res.mon << 8) | res.day));
      com_rmtdata->rmtflag |= 0x80;   // RTC adjusted
    }
    units = res.hdsunit;
  }

  if (units == 0) {
    com_disconnect();
    return -0x700d;   // リモートHDSが1台もないので登録しない
  }

  com_rmtdata->hds_changed = 0xff;
  com_rmtdata->hds_ready = 0;

  // 全ドライブの最初の利用可能なパーティションのBPBを読み込む
  drives = 0;
  for (int i = 0; i < units; i++) {
    struct cmd_hdssize cmd;
    struct res_hdssize res;
    cmd.command = CMD_HDSSIZE;
    cmd.unit = i;
    com_cmdres(&cmd, sizeof(cmd), &res, sizeof(res));
    unitinfo[i].size = res.size;
    unitinfo[i].type = res.type;
    unitinfo[i].firstdrive = drives;

    int parts = read_bpb(i, N_PART);
    if (parts > 0) {
      com_rmtdata->hds_ready |= (1 << i);   // BPBが読めたので利用可能
    } else {
      parts = 1;    // パーティションがない場合も1つは確保する
    }
    com_rmtdata->hds_parts[i] = parts;    // パーティション数
    for (int j = 0; j < parts; j++) {
      bpbtable[drives++] = &bpb[i][j];    // BPBへのポインタをBPBテーブルに記録
    }

    unitinfo[i].curparts = parts;
    unitinfo[i].lastdrive = drives;
    DPRINTF1("unit %d: size=%08x type=%02x parts=%d drive=%d-%d\r\n",
             i, unitinfo[i].size, unitinfo[i].type, unitinfo[i].curparts,
             unitinfo[i].firstdrive, unitinfo[i].lastdrive - 1);

    if (*(char *)&req->fcb + drives > 26) {
      com_disconnect();
      _dos_print("ドライブ数が多すぎます\r\n");
      return -0x700d;
    }
  }
  req->status = (uint32_t)bpbtable;

  if (!(com_rmtdata->rmtflag & 1)) {
    // リモートHDSドライバ用にIOCS _SCSIDRV処理を変更する
    volatile uint8_t *scsidrvflg = (volatile uint8_t *)0x000cec;
#ifdef CONFIG_BOOTDRIVER
    // 起動時ドライバであればHuman68kから渡されるSCSI IDを使用する
    hds_scsiid = scsiidd2 - 1;
#else
    // DEVICE=で登録する場合は未使用のSCSI IDを探して使用する
    hds_scsiid = -1;
    int id;
    for (id = 0; id < 7; id++) {
      if (!(*scsidrvflg & (1 << id))) {
        hds_scsiid = id;
        break;
      }
    }
#endif
    if (hds_scsiid >= 0) {
      for (int id = hds_scsiid; id < 7 && id < (hds_scsiid + units); id++) {
        hdsscsi_mask |= (1 << id);
      }
      *scsidrvflg |= hdsscsi_mask;

      // IOCS _SCSIDRV処理を変更する
      scsidrv_org = _iocs_b_intvcs(0x01f5, scsidrv_hds);
      com_rmtdata->rmtflag |= 1;    // SCSI IOCS patched
    }
  }

#ifndef CONFIG_BOOTDRIVER
  _dos_print("ドライブ");
  _dos_putchar('A' + *(char *)&req->fcb);
  _dos_putchar(':');
  if (units > 1) {
    _dos_putchar('-');
    _dos_putchar('A' + *(char *)&req->fcb + drives - 1);
    _dos_putchar(':');
  }
  _dos_print("でリモートイメージが利用可能です\r\n");
#endif
  DPRINTF1("Debug level: %d\r\n", debuglevel);

#ifdef CONFIG_BOOTDRIVER
  /* SCSI ROM のデバイスドライバ組み込み処理から渡される値
   *「何番目のパーティションから起動するか」を Human68k のドライバ初期化処理に返す
   * この値に基づいてどのドライブから起動するか (CONFIG.SYSをどのドライブから読むか) が決まる
   */
  extern uint8_t bootpart;
  *(char *)&req->fcb = bootpart;
#endif

  return drives;
}

int interrupt(void)
{
  uint16_t err = 0;
  struct dos_req_header *req = reqheader;

  //--------------------------------------------------------------------------
  // Initialization
  //--------------------------------------------------------------------------

  if (req->command == 0x00) {
    // Initialize
    int r = com_init(req);
    if (r >= 0) {
      req->attr = r; /* Number of units */
      extern char _end;
      req->addr = &_end;
      return 0;
    } else {
      return -r;
    }
  }

  //--------------------------------------------------------------------------
  // Command request
  //--------------------------------------------------------------------------

  if (setjmp(jenv)) {
    // USBデバイスが切り離された
    zusb_disconnect_device();
    return 0x7002;      // ドライブの準備が出来ていない
  }

  DPRINTF1("[%d%d:%02x%02x", req->command, req->unit, com_rmtdata->hds_changed, com_rmtdata->hds_ready);

  // ドライブ番号をユニット番号、パーティション番号に変換する
  int unit;
  int part;
  struct unitinfo *ui;
  for (unit = 0; unit < N_HDS; unit++) {
    ui = &unitinfo[unit];
    if (req->unit >= ui->firstdrive && req->unit < ui->lastdrive) {
      part = req->unit - ui->firstdrive;
      break;
    }
  }
  if (unit == N_HDS) {
    DPRINTF1("x]");
    return 0x1001;      // bad unit number
  }

  DPRINTF1(":%d]", unit);

  switch (req->command) {
  case 0x01: /* disk check */
  {
    // 交換されたディスクに対応する全パーティションの交換フラグをセットする
    if (com_rmtdata->hds_changed & (1 << unit)) {
      for (int i = ui->firstdrive; i < ui->lastdrive; i++) {
        drive_changed[i] = true;
      }
      com_rmtdata->hds_changed &= ~(1 << unit);
    }

    if (!drive_changed[req->unit]) {
      *(int8_t *)&req->addr = 1;    // media not changed
    } else {
      DPRINTF1("media changed %d\r\n", req->unit);
      *(int8_t *)&req->addr = -1;   // media changed
      drive_changed[req->unit] = false;
    }
    break;
  }

  case 0x02: /* rebuild BPB */
  {
    struct cmd_hdssize cmd;
    struct res_hdssize res;
    cmd.command = CMD_HDSSIZE;
    cmd.unit = unit;
    com_cmdres(&cmd, sizeof(cmd), &res, sizeof(res));
    ui->size = res.size;
    ui->type = res.type;
    ui->curparts = read_bpb(unit, com_rmtdata->hds_parts[unit]);
    if (ui->curparts > part) {
      req->status = (uint32_t)&bpbtable[req->unit];
    }
    DPRINTF1("(size=%08x type=%02x parts=%d)", ui->size, ui->type, ui->curparts);
    break;
  }

  case 0x05: /* drive control & sense */
  {
    if (((com_rmtdata->hds_changed) & (1 << unit)) ||
        ((com_rmtdata->hds_ready & (1 << unit)) && part < ui->curparts)) {
      req->attr = (ui->type & 1) ? 0x0a : 0x02;
    } else {
      req->attr = 0x04;   // drive not ready
    }
    DPRINTF1("(%x)", req->attr);
    break;
  }

  case 0x04: /* read */
  {
    if (!(com_rmtdata->hds_ready & (1 << unit)) ||
        part >= ui->curparts) {
      return 0x7002;      // ドライブの準備が出来ていない
    }

    DPRINTF1("Read #%06x %04x:", (uint32_t)req->fcb, req->status);

    int sectors = req->status * 2;
    uint32_t pos = (uint32_t)req->fcb * 2 + bpbtable[req->unit]->firstsect * 2;
    uint8_t *p = req->addr;

    while (sectors > 0) {
      int nsect = sectors > HDS_MAX_SECT ? HDS_MAX_SECT : sectors;

      if ((err = sector_read(unit, p, pos, nsect)) != 0) {
        break;
      }
      p += 512 * nsect;
      pos += nsect;
      sectors -= nsect;
    }
    break;
  }

  case 0x08: /* write */
  case 0x09: /* write+verify */
  {
    if (!(com_rmtdata->hds_ready & (1 << unit)) ||
        part >= ui->curparts) {
      return 0x7002;      // ドライブの準備が出来ていない
    }

    DPRINTF1("Write #%06x %04x:", (uint32_t)req->fcb, req->status);

    int sectors = req->status * 2;
    uint32_t pos = (uint32_t)req->fcb * 2 + bpbtable[req->unit]->firstsect * 2;
    uint8_t *p = req->addr;

    while (sectors > 0) {
      int nsect = sectors > HDS_MAX_SECT ? HDS_MAX_SECT : sectors;

      if ((err = sector_write(unit, p, pos, nsect)) != 0) {
        break;
      }
      p += 512 * nsect;
      pos += nsect;
      sectors -= nsect;
    }
    break;
  }

  case 0x03: /* ioctl in */
  {
    DPRINTF1("Ioctl in\r\n");
    break;
  }

  case 0x0c: /* ioctl out */
  {
    DPRINTF1("Ioctl out\r\n");
    break;
  }

  default:
    DPRINTF1("Invalid command\r\n");
    err = 0x1003;   // Invalid command
    break;
  }

  return err;
}

//****************************************************************************
// HDS SCSI IOCS call
//****************************************************************************

int hdsscsi(uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4, uint32_t d5, void *a1)
{
  DPRINTF1("hdsscsi[%02x]", d1);
  DPRINTF1("d3=%d d4=%d d5=%d a1=%p\n", d3, d4, d5, a1);

  int unit = (d4 & 7) - hds_scsiid;
  if (unit < 0 || unit >= N_HDS) {
    return -1;
  }

  if (!(com_rmtdata->hds_ready & (1 << unit))) {
    return -1;
  }

  switch (d1) {
  case 0x01: // _S_SELECT
  case 0x05: // _S_DATAOUT
  case 0x06: // _S_STSIN
  case 0x07: // _S_MSGIN
  case 0x08: // _S_MSGOUT
    break;

  case 0x03: // _S_CMDOUT
    for (int i = 0; i < d3; i++) {
      DPRINTF1(" %02x", ((uint8_t *)a1)[i]);
    }
    DPRINTF1("\r\n");
    break;

  case 0x20: // _S_INQUIRY
    struct scsi_inquiry_resp inqr = {
      .peripheral_device_type = (unitinfo[unit].type & 0x80) ? 0x07 : 0x00,
      .is_removable = (unitinfo[unit].type & 0x80) ? 0x80 : 0x00,
      .version = 0x02,
      .response_data_format = 0x02,
      .additional_length = sizeof(inqr) - 5,
      .vendor_id = "X68000 Z",
      .product_id = "X68000 Z RMTHDS",
      .product_rev = "1.00",
    };
    memcpy(a1, &inqr, d3);
    break;

  case 0x21: // _S_READ
  case 0x26: // _S_READEXT
  case 0x2e: // _S_READI
  {
    int err;
    DPRINTF1("Read #%06x %04x %d:", d2, d3, d5);

    int sectors = d3 << (d5 - 1);
    uint32_t pos = d2 << (d5 - 1);
    uint8_t *p = a1;
    while (sectors > 0) {
      int nsect = sectors > HDS_MAX_SECT ? HDS_MAX_SECT : sectors;

      if ((err = sector_read(unit, p, pos, nsect)) != 0) {
        break;
      }
      p += (512 << (d5 - 1)) * nsect;
      pos += nsect;
      sectors -= nsect;
    }
    break;
  }

  case 0x22: // _S_WRITE
  case 0x27: // _S_WRITEEXT
  {
    int err;
    DPRINTF1("Write #%06x %04x %d:", d2, d3, d5);

    int sectors = d3 << (d5 - 1);
    uint32_t pos = d2 << (d5 - 1);
    uint8_t *p = a1;
    while (sectors > 0) {
      int nsect = sectors > HDS_MAX_SECT ? HDS_MAX_SECT : sectors;

      if ((err = sector_write(unit, p, pos, nsect)) != 0) {
        break;
      }
      p += (512 << (d5 - 1)) * nsect;
      pos += nsect;
      sectors -= nsect;
    }
    break;
  }

  case 0x23: // _S_FORMAT
    break;

  case 0x24: // _S_TESTUNIT
    break;

  case 0x25: // _S_READCAP
    uint32_t sz = unitinfo[unit].size;
    DPRINTF1("ReadCapacity %u %u\r\n", (sz >> 9) - 1, 512);
    struct scsi_read_capacity10_resp capr = {
      .last_lba = (sz >> 9) - 1,
      .block_size = 512,
    };
    memcpy(a1, &capr, sizeof(capr));
    break;

  case 0x28: // _S_VERIFYEXT
    break;

  case 0x04: // _S_DATAIN
    // FORMAT.XはMOのフォーマット時のみlow levelコマンドを使ってMODESENSEを行う
    // DATAINフェーズでMODESENSEの結果を返す
  case 0x29: // _S_MODESENSE
    struct {
      uint8_t mode_data_length;
      uint8_t medium_type_code;
      uint8_t wp_flag;
      uint8_t block_descriptor_length;
      uint32_t block_num;
      uint32_t block_size;
    } modr = {
      .mode_data_length = sizeof(modr) - 1,
      .medium_type_code = 0x00,
      .wp_flag = unitinfo[unit].type & 1 ? 0x80 : 0x00,
      .block_descriptor_length = 8,
      .block_num = unitinfo[unit].size >> 9,
      .block_size = 512,
    };
    memcpy(a1, &modr, d3);
    break;

  case 0x2a: // _S_MODESELECT
    break;
  case 0x2b: // _S_REZEROUNIT
    break;
  case 0x2c: // _S_REQUEST
    break;
  case 0x2d: // _S_SEEK
    break;
  case 0x2f: // _S_STARTSTOP
    break;
  }

  return 0;
}

//****************************************************************************
// Dummy program entry
//****************************************************************************

void _start(void)
{}