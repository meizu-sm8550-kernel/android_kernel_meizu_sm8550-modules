#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define ENOTSUPP 524
#define GFP_KERNEL 0
#define DSI_DEBUG(...) ((void)0)
#define DSI_ERR(...) ((void)0)
#define DUMP_PREFIX_NONE 0
#define print_hex_dump_debug(...) ((void)0)
#define MIPI_DSI_MSG_BATCH_COMMAND 64
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"parser line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
/* ENUM */
enum dsi_cmd_set_state { DSI_CMD_SET_STATE_LP, DSI_CMD_SET_STATE_HS };
struct msg { const void *tx_buf; u32 tx_len, flags, type, channel; };
struct dsi_cmd_desc { struct msg msg; u32 ctrl_flags, post_wait_ms, ctrl; bool last_command; };
struct dsi_panel_cmd_set { struct dsi_cmd_desc *cmds; u32 count; enum dsi_cmd_set_state state; enum dsi_cmd_set_type type; };
struct priv { struct dsi_panel_cmd_set cmd_sets[DSI_CMD_SET_MAX]; };
#define dsi_display_mode_priv_info priv
struct dsi_parser_utils { void *data; const void *(*get_property)(void *,const char *,u32 *); };
struct dsi_panel { struct { bool supported; } note_hbm; struct { bool configured; } note_bl; };
static int allocations, fail_allocation, live;
static void *allocate(size_t size) { if(++allocations==fail_allocation)return NULL;void *p=calloc(1,size);if(p)live++;return p; }
static void release(const void *p) { if(p){ CHECK(live>0);live--;free((void *)p); } }
#define kzalloc(size,flags) allocate(size)
#define kcalloc(n,size,flags) allocate((n)*(size))
#define kfree(p) release(p)
static void *kmemdup(const void *data,size_t size,int flags) { (void)flags;void *p=allocate(size);if(p)memcpy(p,data,size);return p; }
/* CORE_HEADER */
/* CHECK_BLOB */
/* PARSE_NOTE */
/* MAPS */
/* PARSER */
/* FIXTURE */
static u8 blobs[5][4096];
static u32 sizes[5];
static int lookups, bad_state=-1, missing=-1, malformed_state=-1, malformed_kind;
static const enum dsi_cmd_set_type ids[]={DSI_CMD_SET_NOTE_PWM_DC,DSI_CMD_SET_NOTE_DC_PWM,
 DSI_CMD_SET_NOTE_DEMURA_DC,DSI_CMD_SET_NOTE_DEMURA_MID,DSI_CMD_SET_NOTE_DEMURA_LOW};
static const void *property(void *data,const char *name,u32 *size) {
 (void)data;
 for(u32 i=0;i<5;i++) {
  if(!strcmp(name,cmd_set_prop_map[ids[i]])) { lookups++;if((int)i==missing)return NULL;if(size)*size=sizes[i];return blobs[i]; }
  if(!strcmp(name,cmd_set_state_map[ids[i]])) {
   const char *value=(int)i==bad_state ? "broken" : i<2 ? "dsi_lp_mode" : "dsi_hs_mode";
   if(size) *size=(int)i==malformed_state ? (malformed_kind==0 ? 0 : malformed_kind==1 ? 11 : 13) : strlen(value)+1;
   return value;
  }
 }
 return NULL;
}
static void clear(struct priv *priv) {
 for(u32 i=0;i<DSI_CMD_SET_MAX;i++) {
  if(priv->cmd_sets[i].cmds)dsi_panel_destroy_cmd_packets(&priv->cmd_sets[i]);
  dsi_panel_dealloc_cmd_packets(&priv->cmd_sets[i]);
 }
 memset(priv,0,sizeof(*priv));CHECK(!live);
}
static void reset(void) { CHECK(!live);allocations=fail_allocation=lookups=0;bad_state=missing=malformed_state=-1; }
int main(void) {
 struct priv priv={0};struct dsi_parser_utils utils={.get_property=property};
 struct dsi_panel p={.note_hbm.supported=true,.note_bl.configured=true};
 for(u32 i=0;i<5;i++)for(u32 j=0;j<fixture.cmd_sets[ids[i]].count;j++) {
  struct dsi_cmd_desc *c=&fixture.cmd_sets[ids[i]].cmds[j];u32 offset=sizes[i];
  blobs[i][offset]=c->msg.type;blobs[i][offset+3]=c->msg.flags;
  blobs[i][offset+4]=c->post_wait_ms;blobs[i][offset+5]=c->msg.tx_len>>8;blobs[i][offset+6]=c->msg.tx_len&255;
  memcpy(blobs[i]+offset+7,c->msg.tx_buf,c->msg.tx_len);sizes[i]+=c->msg.tx_len+7;
 }
 reset();CHECK(!dsi_panel_parse_cmd_sets(&priv,&utils,&p));CHECK(lookups==5);int total=allocations;
 for(u32 i=0;i<5;i++) {
  CHECK(priv.cmd_sets[ids[i]].count==(i<2 ? 47u : 3u));
  CHECK(priv.cmd_sets[ids[i]].state==(i<2 ? DSI_CMD_SET_STATE_LP : DSI_CMD_SET_STATE_HS));
  for(u32 j=0;j<priv.cmd_sets[ids[i]].count;j++) {
   struct dsi_cmd_desc *a=&priv.cmd_sets[ids[i]].cmds[j],*b=&fixture.cmd_sets[ids[i]].cmds[j];
   CHECK(a->msg.tx_len==b->msg.tx_len && !memcmp(a->msg.tx_buf,b->msg.tx_buf,a->msg.tx_len));
   CHECK(a->last_command==b->last_command && a->msg.flags==b->msg.flags);
  }
 }
 clear(&priv);
 for(int fail=2;fail<=total;fail++) { reset();fail_allocation=fail;CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-ENOMEM);CHECK(!live);clear(&priv); }
 for(u32 i=0;i<5;i++) {
  u32 size=sizes[i];
  for(u32 n=1;n<7;n++) { reset();sizes[i]=size+n;CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-EINVAL);CHECK(!live);clear(&priv); }
  reset();sizes[i]=size-1;CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-EINVAL);clear(&priv);sizes[i]=size;
  for(int kind=0;kind<3;kind++) { reset();malformed_state=i;malformed_kind=kind;
   CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-EINVAL);clear(&priv); }
  reset();bad_state=i;CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-EINVAL);clear(&priv);
  reset();missing=i;CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-ENOTSUPP);clear(&priv);
  u32 offset=0;
  for(u32 j=0;j<fixture.cmd_sets[ids[i]].count;j++) {
   reset();u8 flags=blobs[i][offset+3];blobs[i][offset+3]=1;
   CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-EINVAL);clear(&priv);blobs[i][offset+3]=flags;
   offset+=fixture.cmd_sets[ids[i]].cmds[j].msg.tx_len+7;
  }
 }
 reset();p.note_hbm.supported=false;p.note_bl.configured=false;
 CHECK(!dsi_panel_parse_cmd_sets(&priv,&utils,&p));CHECK(!lookups);clear(&priv);
 reset();p.note_hbm.supported=true;CHECK(dsi_panel_parse_cmd_sets(&priv,&utils,&p)==-EINVAL);CHECK(!lookups);clear(&priv);
 printf("PASS: real mode parser: exact tables, non-Note skip, trailing/truncated/state/header rejection, %d allocation-failure unwinds\n",total-1);
 return 0;
}
