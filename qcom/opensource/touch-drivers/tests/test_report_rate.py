#!/usr/bin/env python3
"""Execute production protocol and recovery helpers against a scripted bus."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))


def function(text, signature):
    start=text.index(signature); end=text.index('{',start)+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]


def main():
    p=argparse.ArgumentParser(); p.add_argument('source',type=Path); a=p.parse_args()
    hw=(a.source/'goodix_brl_hw.c').read_text()
    if 'void goodix_restore_report_rate(' not in hw:
        raise SystemExit('FAIL: missing Note default report-rate restoration')
    h=(a.source/'goodix_ts_core.h').read_text()
    utils=(a.source/'goodix_ts_utils.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define MAX_CMD_DATA_LEN 12
#define MAX_CMD_BUF_LEN 16
#define CHECKSUM_MODE_U8_LE 0
#define CHECKSUM_MODE_U16_LE 1
#define GOODIX_CMD_RETRY 6
#define CMD_ACK_OK 0x80
#define CMD_ACK_BUSY 2
#define CMD_ACK_BUFFER_OVERFLOW 3
#define ts_info(...) ((void)0)
#define ts_debug(...) ((void)0)
#define ts_err(...) ((void)0)

#define mutex_lock(x) do { int *v=(x); assert(!*v); *v=1; } while(0)
#define mutex_unlock(x) do { int *v=(x); assert(*v); *v=0; } while(0)
#define msleep(x) ((void)0)
#define usleep_range(x,y) ((void)0)
struct goodix_ts_core;
'''
    code+=function(h,'struct goodix_ts_cmd {')+';\n'
    code+=r'''
struct goodix_ic_info_misc { u32 cmd_addr; };
struct goodix_ts_hw_ops {
 int (*read)(struct goodix_ts_core *,unsigned int,unsigned char *,unsigned int);
 int (*write)(struct goodix_ts_core *,unsigned int,unsigned char *,unsigned int);
 int (*send_cmd)(struct goodix_ts_core *,struct goodix_ts_cmd *);
};
struct goodix_ts_core { bool is_m2468, report_rate_ready, report_rate_powered; unsigned long report_rate_blocked; int cmd_lock, report_rate_lock;
 struct { struct goodix_ic_info_misc misc; } ic_info; struct goodix_ts_hw_ops *hw_ops; };
'''
    code+=function(utils,'u32 goodix_append_checksum(')+'\n'
    code+=function(hw,'static int brl_send_cmd_locked(')+'\n'
    code+=function(hw,'static int brl_send_cmd(')+'\n'
    code+=function(hw,'static void goodix_restore_report_rate_locked(struct goodix_ts_core *cd)\n{')+'\n'
    code+=function(hw,'void goodix_restore_report_rate(')+'\n'
    code+=r'''
#define GOODIX_NORMAL_RESET_DELAY_MS 100
static int operation_error;
static int brl_reset_locked(struct goodix_ts_core *cd,int delay) {
 assert(!cd->is_m2468 || cd->report_rate_lock); return operation_error;
}
static int brl_power_on_locked(struct goodix_ts_core *cd,bool on) {
 assert(!cd->is_m2468 || cd->report_rate_lock); return operation_error;
}
static int brl_send_config_locked(struct goodix_ts_core *cd,u8 *cfg,int len) {
 assert(!cd->is_m2468 || cd->report_rate_lock); return operation_error;
}
static int brl_read_config_locked(struct goodix_ts_core *cd,u8 *cfg,int size) {
 assert(!cd->is_m2468 || cd->report_rate_lock); return operation_error;
}
'''
    for sig in ['static int brl_reset(', 'static int brl_power_on(', 'static int brl_send_config(', 'static int brl_read_config(']:
        code+=function(hw,sig)+'\n'
    code+=r'''
struct device_node { bool compatible; }; static struct device_node root;
static int board0,board1,dt_error,no_root,put_count;
static bool of_device_is_compatible(struct device_node *n,const char *s) { return n && n->compatible && !strcmp(s,"goodix,brl-d"); }
static struct device_node *of_find_node_by_path(const char *s) { return no_root?NULL:&root; }
static int of_property_read_u32_array(struct device_node *n,const char *p,u32 *v,int len) {
 assert(n==&root && !strcmp(p,"meizu,board-id") && len==2); v[0]=board0;v[1]=board1;return dt_error;
}
static void of_node_put(struct device_node *n) { assert(n==&root);put_count++; }
'''
    code+=function(utils,'bool goodix_is_m2468(')+'\n'
    code+=r'''
static int writes,reads,write_error,read_error,ack=0x80;
static u8 sent[MAX_CMD_BUF_LEN];
static int wr(struct goodix_ts_core *cd,unsigned int addr,u8 *buf,unsigned int len) {
 assert(cd->cmd_lock && cd->report_rate_lock); assert(addr==0x12345); assert(len==sizeof(sent));
 memcpy(sent,buf,len); writes++; return write_error;
}
static int rd(struct goodix_ts_core *cd,unsigned int addr,u8 *buf,unsigned int len) {
 assert(cd->cmd_lock && cd->report_rate_lock); reads++; memset(buf,0,len); buf[1]=ack; return read_error;
}
int main(void) {
 struct goodix_ts_hw_ops ops={.read=rd,.write=wr,.send_cmd=brl_send_cmd};
 struct goodix_ts_core cd={.hw_ops=&ops,.ic_info.misc.cmd_addr=0x12345};
 goodix_restore_report_rate(&cd); assert(!writes);
 cd.report_rate_ready=true; goodix_restore_report_rate(&cd); assert(!writes);
 cd.is_m2468=true; cd.report_rate_ready=false; goodix_restore_report_rate(&cd); assert(!writes);
 cd.report_rate_ready=true; cd.report_rate_powered=true; cd.ic_info.misc.cmd_addr=0; goodix_restore_report_rate(&cd); assert(!writes);
 cd.ic_info.misc.cmd_addr=0x12345; goodix_restore_report_rate(&cd);
 const u8 expected[]={0,0,5,0x9d,2,0xa4,0};
 assert(writes==1 && reads==1 && !memcmp(sent,expected,sizeof(expected)));
 for(int i=7;i<sizeof(sent);i++) assert(sent[i]==0);
 /* Each actual recovery invokes one transaction, no persistent success lie. */
 goodix_restore_report_rate(&cd); assert(writes==2);
 for(unsigned long block=1;block<8;block++) {
  cd.report_rate_blocked=block; goodix_restore_report_rate(&cd); assert(writes==2);
 }
 cd.report_rate_blocked=0; cd.report_rate_powered=false;
 goodix_restore_report_rate(&cd); assert(writes==2); cd.report_rate_powered=true;
 cd.report_rate_ready=false; goodix_restore_report_rate(&cd); assert(writes==2);
 cd.report_rate_ready=true; writes=reads=0; write_error=-EIO;
 goodix_restore_report_rate(&cd); assert(writes==1 && reads==0 && !cd.cmd_lock && !cd.report_rate_lock);
 writes=reads=0; write_error=0; read_error=-EIO;
 goodix_restore_report_rate(&cd); assert(writes==1 && reads==1 && !cd.cmd_lock && !cd.report_rate_lock);
 writes=reads=0; read_error=0; ack=CMD_ACK_BUSY;
 goodix_restore_report_rate(&cd); assert(writes==6 && reads==36 && !cd.cmd_lock && !cd.report_rate_lock);
 writes=reads=0; ack=4; goodix_restore_report_rate(&cd); assert(writes==6 && reads==6 && !cd.cmd_lock && !cd.report_rate_lock);
 writes=reads=0; ack=0x80;
 assert(brl_reset(&cd,5)==0 && writes==0);
 assert(brl_reset(&cd,100)==0 && writes==1);
 operation_error=-EIO; assert(brl_reset(&cd,100)==-EIO && writes==1);
 goodix_restore_report_rate(&cd); assert(writes==1 && !cd.report_rate_powered);
 operation_error=0; cd.report_rate_blocked=2; assert(brl_reset(&cd,100)==0 && writes==1);
 cd.report_rate_blocked=0;
 assert(brl_power_on(&cd,false)==0 && !cd.report_rate_powered && writes==1);
 operation_error=-EIO; assert(brl_power_on(&cd,true)==-EIO && !cd.report_rate_powered && writes==1);
 operation_error=0; assert(brl_power_on(&cd,true)==0 && cd.report_rate_powered && writes==2);
 assert(brl_send_config(&cd,NULL,0)==0 && writes==3);
 assert(brl_read_config(&cd,NULL,0)==0 && writes==3);
 operation_error=-EINVAL; assert(brl_send_config(&cd,NULL,0)==-EINVAL && writes==3);
 cd.is_m2468=false; operation_error=0;
 assert(brl_reset(&cd,100)==0 && brl_power_on(&cd,true)==0 && brl_send_config(&cd,NULL,0)==0 && writes==3);
 struct device_node node={.compatible=true}; board0=3;board1=5;
 assert(goodix_is_m2468(&node)); board1=4; assert(!goodix_is_m2468(&node));
 board1=5;board0=2;assert(!goodix_is_m2468(&node)); board0=3;
 dt_error=-EINVAL;assert(!goodix_is_m2468(&node));dt_error=0;
 no_root=1;assert(!goodix_is_m2468(&node));no_root=0;
 node.compatible=false;assert(!goodix_is_m2468(&node)); assert(put_count==4);
 puts("PASS: real protocol/ACK/restore and reset/power/config wrappers; profile isolation, lifecycle gates, bounded errors");
}
'''
    with tempfile.TemporaryDirectory(prefix='note-rate-test-') as tmp:
        c=Path(tmp)/'test.c'; binary=Path(tmp)/'test'; c.write_text(code)
        subprocess.run(['gcc','-std=gnu11','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__=='__main__': main()
