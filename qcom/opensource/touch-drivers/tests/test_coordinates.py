#!/usr/bin/env python3
"""Run the actual Goodix finger reporter with captured Linux input events."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', nargs='?', type=Path, default=Path(__file__).resolve().parents[1] / 'goodix_berlin_driver')
    args = parser.parse_args()
    header = (args.source / 'goodix_ts_core.h').read_text()
    core = (args.source / 'goodix_ts_core.c').read_text()
    helper = function(header, 'static inline void goodix_report_coordinate(') if 'static inline void goodix_report_coordinate(' in header else ''
    code = r'''
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define GOODIX_MAX_TOUCH 10
#define TS_TOUCH 1
#define MT_TOOL_FINGER 1
#define BTN_TOUCH 330
#define ABS_MT_POSITION_X 53
#define ABS_MT_POSITION_Y 54
#define ABS_MT_TOUCH_MAJOR 48
#define ts_debug(...) ((void)0)
#define swap(a,b) do { int temp=(a); (a)=(b); (b)=temp; } while(0)
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
struct goodix_ts_core { bool is_m2468; };
struct input_dev { struct goodix_ts_core *data; int slot, sync, down; int x[10],y[10],w[10],active[10]; };
struct goodix_touch_data { unsigned int touch_num; struct { int status,x,y,w; } coords[10]; };
static void *input_get_drvdata(struct input_dev *d) { return d->data; }
static void input_mt_slot(struct input_dev *d,int slot) { d->slot=slot; }
static void input_mt_report_slot_state(struct input_dev *d,int tool,bool active) { d->active[d->slot]=active; }
static void input_report_abs(struct input_dev *d,int code,int value) {
 if(code==ABS_MT_POSITION_X) d->x[d->slot]=value;
 if(code==ABS_MT_POSITION_Y) d->y[d->slot]=value;
 if(code==ABS_MT_TOUCH_MAJOR) d->w[d->slot]=value;
}
static void input_report_key(struct input_dev *d,int code,int down) { d->down=down; }
static void input_sync(struct input_dev *d) { d->sync++; }
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while(0)
'''
    code += helper + '\n' + function(core, 'static void goodix_ts_report_finger(')
    code += r'''
int main(void) {
 struct goodix_ts_core core={0}; struct input_dev d={.data=&core};
 struct goodix_touch_data t={0};
 for(int m2468=0;m2468<2;m2468++) for(int invert=0;invert<2;invert++) {
  core.is_m2468=m2468;
  /* Every slot, origin, subpixel truncation, centre and far edges. */
  int xs[10]={0,1,9,10,6319,6320,12630,12639,12640,65535};
  int ys[10]={0,9,1,10,13899,13900,27790,27799,27800,65535};
  memset(&t,0,sizeof(t)); t.touch_num=10;
  for(int i=0;i<10;i++) { t.coords[i].status=TS_TOUCH; t.coords[i].x=xs[i]; t.coords[i].y=ys[i]; t.coords[i].w=i+20; }
  int before=d.sync;
  goodix_ts_report_finger(&d,&t,invert);
  for(int i=0;i<10;i++) {
   CHECK(d.x[i]==(invert?ys[i]:xs[i])/(m2468?10:1));
   CHECK(d.y[i]==(invert?xs[i]:ys[i])/(m2468?10:1));
   CHECK(d.w[i]==i+20); CHECK(d.active[i]);
  }
  CHECK(d.down==1 && d.sync==before+1);
  memset(&t,0,sizeof(t)); goodix_ts_report_finger(&d,&t,invert);
  CHECK(d.down==0 && d.sync==before+2);
  for(int i=0;i<10;i++) CHECK(!d.active[i]);
 }
 puts("PASS: real finger reporter, 10 slots, M2468/other board, swap, edges, truncation, width and release");
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='m2468-touch-test-') as tmp:
        c, binary = Path(tmp)/'test.c', Path(tmp)/'test'
        c.write_text(code)
        subprocess.run(['gcc','-std=gnu11','-Wall','-Werror','-Wno-unused-function',str(c),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
