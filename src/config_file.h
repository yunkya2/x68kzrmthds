/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Yuichi Nakamura
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
 *
 */

#ifndef _CONFIG_FILE_H
#define _CONFIG_FILE_H

#include <stdint.h>

/* configuration data */

extern char configtxt[2048];

extern char config_wifi_ssid[32];
extern char config_wifi_passwd[16];

extern char config_smb2_user[16];
extern char config_smb2_passwd[16];
extern char config_smb2_workgroup[16];

extern char config_smb2_server[32];
extern char config_smb2_share[32];
extern char config_id[7][128];

extern char config_tz[16];

/* configuration functions */

void config_read(void);
void config_write(void);
void config_erase(void);
void config_parse(uint8_t *buf);

#endif  /* _CONFIG_FILE_H */
