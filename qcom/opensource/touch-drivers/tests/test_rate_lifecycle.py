#!/usr/bin/env python3
"""Execute M2468 request/lifecycle functions, including an in-flight transfer."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_report_rate import function


def main():
    p=argparse.ArgumentParser(); p.add_argument('source',type=Path); a=p.parse_args()
    core=(a.source/'goodix_ts_core.c').read_text();hw=(a.source/'goodix_brl_hw.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#define BIT(x) (1UL<<(x))
#define GOODIX_RATE_SUSPEND BIT(0)
#define GOODIX_RATE_FWUPDATE BIT(1)
#define GOODIX_RATE_REMOVE BIT(2)
#define CORE_INIT_STAGE2 2
#define REQUEST_TYPE_CONFIG 1
#define REQUEST_TYPE_RESET 3
#define CONFIG_TYPE_NORMAL 0
#define GOODIX_NORMAL_RESET_DELAY_MS 100
#define ts_err(...) ((void)0)
#define ts_info(...) ((void)0)
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_SEQ_CST)
#define xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define set_bit(n,p) __atomic_fetch_or(p,BIT(n),__ATOMIC_SEQ_CST)
struct work_struct { int unused; };
struct goodix_ts_cmd { int len,cmd,data[12]; };
struct goodix_ts_event { int request_code; };
struct goodix_ts_core;
struct goodix_ts_hw_ops {
 int (*send_cmd)(struct goodix_ts_core *,struct goodix_ts_cmd *);
 int (*reset)(struct goodix_ts_core *,int);
};
struct goodix_ts_core {
 bool is_m2468,report_rate_ready,report_rate_powered;
 unsigned long report_rate_blocked,m2468_requests;
 int init_stage,irq;
 pthread_mutex_t report_rate_lock;
 struct work_struct m2468_request_work;
 struct { struct { unsigned int cmd_addr; } misc; } ic_info;
 struct goodix_ts_hw_ops *hw_ops;
};
static int irq_context,scheduled,synced,cancelled,ops_order;
static atomic_int sends,transfer_started,release_transfer,block_started,block_finished;
static int hold_transfer;
static void schedule_work(struct work_struct *w) { assert(irq_context); scheduled++; }
static void synchronize_irq(int irq) { assert(!irq_context); synced++; }
static void cancel_work_sync(struct work_struct *w) { assert(!irq_context && synced); cancelled++; }
static int reset(struct goodix_ts_core *cd,int delay) { assert(!irq_context); ops_order=ops_order*10+1; return 0; }
static int goodix_send_ic_config(struct goodix_ts_core *cd,int type) { assert(!irq_context);ops_order=ops_order*10+2;return 0; }
static int send(struct goodix_ts_core *cd,struct goodix_ts_cmd *cmd) {
 assert(!irq_context && cmd->cmd==0x9d && cmd->data[0]==2);
 atomic_fetch_add(&sends,1); atomic_store(&transfer_started,1);
 while(hold_transfer && !atomic_load(&release_transfer)) usleep(1000);
 return 0;
}
'''
    code+=function(hw,'static void goodix_restore_report_rate_locked(struct goodix_ts_core *cd)\n{')+'\n'
    code+=function(hw,'void goodix_restore_report_rate(')+'\n'
    for sig in ['static void goodix_m2468_request_work(', 'static void goodix_m2468_rate_block(', 'static void goodix_m2468_rate_unblock(', 'static int goodix_ts_request_handle(']:
        code+=function(core,sig)+'\n'
    code+=r'''
static void *transfer(void *arg) { goodix_restore_report_rate(arg);return NULL; }
static void *block(void *arg) {
 atomic_store(&block_started,1);goodix_m2468_rate_block(arg,GOODIX_RATE_FWUPDATE);
 atomic_store(&block_finished,1);return NULL;
}
int main(void) {
 struct goodix_ts_hw_ops ops={.send_cmd=send,.reset=reset};
 struct goodix_ts_core cd={.is_m2468=true,.report_rate_ready=true,.report_rate_powered=true,
  .init_stage=2,.irq=42,.report_rate_lock=PTHREAD_MUTEX_INITIALIZER,.hw_ops=&ops,.ic_info.misc.cmd_addr=1};
 struct goodix_ts_event event={.request_code=REQUEST_TYPE_CONFIG};
 irq_context=1; assert(goodix_ts_request_handle(&cd,&event)==0);
 event.request_code=REQUEST_TYPE_RESET;assert(goodix_ts_request_handle(&cd,&event)==0);
 assert(scheduled==2 && ops_order==0 && atomic_load(&sends)==0);
 irq_context=0;goodix_m2468_request_work(&cd.m2468_request_work);
 assert(ops_order==12 && !cd.m2468_requests);
 goodix_m2468_rate_block(&cd,GOODIX_RATE_SUSPEND);
 irq_context=1;goodix_ts_request_handle(&cd,&event);irq_context=0;
 assert(scheduled==2 && !cd.m2468_requests);
 goodix_m2468_rate_block(&cd,GOODIX_RATE_FWUPDATE);
 goodix_m2468_rate_unblock(&cd,GOODIX_RATE_SUSPEND);
 assert(cd.report_rate_blocked==GOODIX_RATE_FWUPDATE && atomic_load(&sends)==0);
 goodix_m2468_rate_unblock(&cd,GOODIX_RATE_FWUPDATE);assert(atomic_load(&sends)==1);
 /* A blocker must wait for a transfer already inside the real rate lock. */
 hold_transfer=1;atomic_store(&transfer_started,0);pthread_t t,b;
 assert(!pthread_create(&t,NULL,transfer,&cd));
 while(!atomic_load(&transfer_started)) usleep(1000);
 assert(!pthread_create(&b,NULL,block,&cd));
 while(!atomic_load(&block_started)) usleep(1000);
 usleep(20000);assert(!atomic_load(&block_finished));
 atomic_store(&release_transfer,1);pthread_join(t,NULL);pthread_join(b,NULL);
 assert(atomic_load(&block_finished) && cd.report_rate_blocked==GOODIX_RATE_FWUPDATE);
 int before=atomic_load(&sends);goodix_restore_report_rate(&cd);assert(atomic_load(&sends)==before);
 cd.m2468_requests=BIT(REQUEST_TYPE_RESET);goodix_m2468_rate_block(&cd,GOODIX_RATE_REMOVE);
 assert(!cd.m2468_requests && synced==cancelled);
 goodix_m2468_rate_unblock(&cd,GOODIX_RATE_FWUPDATE);
 goodix_m2468_rate_unblock(&cd,GOODIX_RATE_SUSPEND);
 assert(cd.report_rate_blocked==GOODIX_RATE_REMOVE && atomic_load(&sends)==before);
 cd.is_m2468=false;cd.report_rate_blocked=0;ops_order=0;
 goodix_ts_request_handle(&cd,&event);assert(ops_order==1 && scheduled==2);
 puts("PASS: real request/block/unblock functions; IRQ queues only, reset/config order, independent blockers, in-flight transfer drain, removal");
}
'''
    with tempfile.TemporaryDirectory(prefix='rate-lifecycle-') as tmp:
        c=Path(tmp)/'test.c';c.write_text(code);binary=Path(tmp)/'test'
        subprocess.run(['gcc','-std=gnu11','-Wall','-Werror','-pthread',str(c),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

if __name__=='__main__':main()
