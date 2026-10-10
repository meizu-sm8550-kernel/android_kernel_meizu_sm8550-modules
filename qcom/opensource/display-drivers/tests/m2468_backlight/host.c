#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
#define DSI_DEBUG(...) ((void)0)
#define DSI_ERR(...) ((void)0)
#define pr_warn_ratelimited(...) ((void)0)
#define ENOTSUPP 524
#define atomic_read(p) (*(p))
#define MIPI_DSI_MSG_USE_LPM 2
#define MIPI_DSI_MSG_BATCH_COMMAND 64
#define DSI_CTRL_CMD_FETCH_MEMORY 1
#define DSI_CTRL_CMD_LAST_COMMAND 0x40
#define DSI_CTRL_CMD_ASYNC_WAIT 0x80
#define DSI_CTRL_CMD_BROADCAST 0x100
#define SDE_EVT32(...) ((void)0)
#define DSI_ALL_CLKS 3
#define DSI_CLK_ON 1
#define DSI_CLK_OFF 0
#define DSI_OP_CMD_MODE 0
#define DSI_OP_VIDEO_MODE 1
#define SDE_MODE_DPMS_ON 0
#define SDE_MODE_DPMS_LP1 1
#define SDE_MODE_DPMS_LP2 2
#define SDE_MODE_DPMS_OFF 3
#define MSM_ENC_VBLANK 2
#define REGULATOR_MODE_IDLE 1
#define REGULATOR_MODE_NORMAL 2
#define REGULATOR_MODE_STANDBY 3
#define IRQ_HANDLED 1
#define IRQF_TRIGGER_RISING 1
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
/* CORE_HEADER */
/* BL_HEADER */
/* HBM_HEADER */
enum { DSI_BACKLIGHT_WLED, DSI_BACKLIGHT_DCS, DSI_BACKLIGHT_EXTERNAL, DSI_BACKLIGHT_PWM };
enum dsi_cmd_set_type { DSI_CMD_SET_M2468_PWM_DC, DSI_CMD_SET_M2468_DC_PWM,
 DSI_CMD_SET_M2468_DEMURA_DC, DSI_CMD_SET_M2468_DEMURA_MID, DSI_CMD_SET_M2468_DEMURA_LOW,
 DSI_CMD_SET_ON, DSI_CMD_SET_CMD_ON, DSI_CMD_SET_VID_ON, DSI_CMD_SET_OFF,
 DSI_CMD_SET_LP1, DSI_CMD_SET_LP2, DSI_CMD_SET_NOLP,
 DSI_CMD_SET_CMD_SWITCH_OUT, DSI_CMD_SET_VID_SWITCH_OUT, DSI_CMD_SET_VID_SWITCH_IN,
 DSI_CMD_SET_CMD_SWITCH_IN, DSI_CMD_SET_TIMING_SWITCH, DSI_CMD_SET_POST_TIMING_SWITCH,
 DSI_CMD_SET_M2468_LOCAL_ON, DSI_CMD_SET_M2468_LOCAL_OFF, DSI_CMD_SET_M2468_LOCAL_LEVEL,
 DSI_CMD_SET_M2468_ADFR_ON, DSI_CMD_SET_M2468_ADFR_OFF, DSI_CMD_SET_MAX };
enum dsi_cmd_set_state { DSI_CMD_SET_STATE_LP, DSI_CMD_SET_STATE_HS };
struct msg { const void *tx_buf; u32 tx_len, flags, type, channel; };
struct dsi_cmd_desc { struct msg msg; u32 ctrl_flags, post_wait_ms, ctrl; bool last_command; };
struct dsi_panel_cmd_set { struct dsi_cmd_desc *cmds; u32 count; enum dsi_cmd_set_state state; };
struct priv { struct dsi_panel_cmd_set cmd_sets[DSI_CMD_SET_MAX]; };
struct mode { struct priv *priv_info; struct { u32 refresh_rate; } timing; };
struct dsi_parser_utils { void *data; int (*read_u32)(void *, const char *, u32 *); };
struct dsi_backlight_config { int type; void *raw_bd; u32 bl_max_level, bl_level; bool lp_mode, bl_inverted_dbv; };
struct device_node { const char *name; };
struct dsi_panel { struct mode *cur_mode; bool panel_initialized; int esd_recovery_pending;
 int power_mode, panel_mode, panel_lock, power_info; const char *name;
 struct device_node *panel_of_node;
 struct dsi_parser_utils utils; struct dsi_backlight_config bl_config;
 struct { bool ext_bridge_mode; } host_config;
 struct { bool supported, low_power; u32 vblanks; struct m2468_hbm_state state; } m2468_hbm;
 struct m2468_bl_panel m2468_bl; void *host; };
struct mipi_dsi_host { int dummy; };
struct drm_encoder { int marker; };
struct dsi_bridge { struct { struct drm_encoder *encoder; } base; };
struct dsi_ctrl { u32 cmd_len, pending_cmd_flags; bool owned, post_tx_queued;
 void *post_cmd_tx_workq; int post_cmd_tx_work; };
struct dsi_display_ctrl { struct dsi_ctrl *ctrl; };
struct dsi_display { struct dsi_panel *panel; bool trusted_vm_env, hw_ownership;
	void *dsi_clk_handle; struct mipi_dsi_host host; struct dsi_bridge *bridge;
	int disp_te_gpio; u32 ctrl_count; struct dsi_display_ctrl ctrl[1]; void *tx_cmd_buf; };
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define to_dsi_display(ptr) container_of(ptr, struct dsi_display, host)
#define lockdep_assert_held(lock) CHECK(*(lock)==1)
static void mutex_lock(int *lock) { CHECK(!(*lock)++); }
static void mutex_unlock(int *lock) { CHECK((*lock)--==1); }
static int sent, fail_packet, positive_packet, esd_packet, votes, fail_vote, positive_vote;
static int syncs, time_out, delays, legacy_sends, cleaned, lifecycle_rc;
static int wake_error, wait_error, wait_esd, vblank_waits;
static u32 last_delay;
static int clock_depth, ready_notifications;
static u8 packets[256][64];
static u32 lengths[256], flags_log[256];
static struct dsi_display *active_display;
static int prepare_error;
static int dsi_ctrl_transfer_cleanup(struct dsi_ctrl *c) {
 CHECK(c->owned);c->owned=false;cleaned++;return 0;
}
static int dsi_ctrl_post_cmd_transfer(struct dsi_ctrl *c) { return dsi_ctrl_transfer_cleanup(c); }
static void queue_work(void *queue,int *work) { (void)queue;(void)work;CHECK(false); }
/* REAL_UNPREPARE */
static int dsi_ctrl_transfer_prepare(struct dsi_ctrl *c,u32 flags) {
 if((flags & DSI_CTRL_CMD_FETCH_MEMORY) && c->cmd_len)return 0;
 if(prepare_error)return -EIO;
 CHECK(!c->owned);c->owned=true;return 0;
}
static int dsi_ctrl_cmd_transfer(struct dsi_ctrl *c,struct dsi_cmd_desc *cmd) {
 CHECK(clock_depth>0);CHECK(c->owned);CHECK(cmd->msg.tx_len<=64);CHECK(sent<256);
 memcpy(packets[sent],cmd->msg.tx_buf,cmd->msg.tx_len);
 lengths[sent]=cmd->msg.tx_len;flags_log[sent]=cmd->msg.flags;
 sent++;
 /* Match validation/create/copy failures BEFORE dsi_message_tx resets
  * cmd_len on its LAST packet. unprepare must still release exactly once.
  */
 if(sent==fail_packet)return -EIO;
 if(sent==positive_packet)return 3;
 if(cmd->msg.flags & MIPI_DSI_MSG_BATCH_COMMAND)c->cmd_len++;
 else c->cmd_len=0;
 if(sent==esd_packet)active_display->panel->esd_recovery_pending=1;
 return 0;
}
static int dsi_display_wake_up(struct dsi_display *d) { (void)d;return 0; }
static int dsi_host_alloc_cmd_tx_buffer(struct dsi_display *d) { d->tx_cmd_buf=(void *)1;return 0; }
static void dsi_display_set_cmd_tx_ctrl_flags(struct dsi_display *d,struct dsi_cmd_desc *cmd) {
 (void)d;cmd->ctrl_flags=DSI_CTRL_CMD_FETCH_MEMORY|DSI_CTRL_CMD_ASYNC_WAIT;
 if(cmd->last_command)cmd->ctrl_flags|=DSI_CTRL_CMD_LAST_COMMAND;
}
static int dsi_display_broadcast_cmd(struct dsi_display *d,struct dsi_cmd_desc *cmd) { (void)d;(void)cmd;CHECK(false);return -EIO; }
#define display_for_each_ctrl(i,d) for((i)=0;(u32)(i)<(d)->ctrl_count;(i)++)
/* REAL_HOST */
static int dsi_display_clk_ctrl(void *handle, u32 type, u32 state) {
 (void)handle; CHECK(type==DSI_ALL_CLKS);votes++;
 int rc=votes==fail_vote ? -ETIMEDOUT : votes==positive_vote ? 1 : 0;
 if(state==DSI_CLK_ON) { if(!rc)clock_depth++; }
 else { CHECK(state==DSI_CLK_OFF && clock_depth>0);clock_depth--; }
 return rc;
}
static void usleep_range(u32 a,u32 b) {
 CHECK(b>=a);
 last_delay=a/1000;delays++;
}
static int sde_encoder_m2468_early_wakeup(struct drm_encoder *encoder) {
 CHECK(encoder && active_display->bridge->base.encoder==encoder);
 syncs++;return wake_error;
}
static int sde_encoder_wait_for_event(struct drm_encoder *encoder,int event) {
 CHECK(encoder && event==MSM_ENC_VBLANK);vblank_waits++;
 if(wait_esd)active_display->panel->esd_recovery_pending=1;
 return time_out ? -ETIMEDOUT : wait_error;
}
static int dsi_panel_update_backlight(struct dsi_panel *p,u32 v) { (void)p;(void)v;legacy_sends++;return 0; }
static int backlight_device_set_brightness(void *p,u32 v) { (void)p;(void)v;return -99; }
static int dsi_panel_update_pwm_backlight(struct dsi_panel *p,u32 v) { (void)p;(void)v;return -99; }
static void m2468_clear_ready(struct dsi_panel *p) { (void)p;ready_notifications++; }
static bool dsi_panel_is_type_oled(struct dsi_panel *p) { (void)p;return false; }
static void dsi_pwr_panel_regulator_mode_set(int *p,const char *name,int mode) { (void)p;(void)name;(void)mode; }
static int dsi_panel_tx_cmd_set(struct dsi_panel *p,enum dsi_cmd_set_type type) { (void)p;(void)type;return lifecycle_rc; }
#define GFP_KERNEL 0
#define kcalloc(n,s,f) calloc(n,s)
#define kfree(p) free((void *)(p))
static void *kmemdup(const void *p,size_t len,int flags) { (void)flags;void *copy=malloc(len);if(copy)memcpy(copy,p,len);return copy; }
int dsi_m2468_hbm_backlight_adfr(struct dsi_panel *p,u32 value);
/* HBM_CORE */
/* CORE_SOURCE */
/* ADAPTER_SOURCE */
static int board_count=2, board_first=3, board_second=5, root_missing;
static struct device_node root_node={"/"};
static struct device_node *of_find_node_by_path(const char *path) { CHECK(!strcmp(path,"/"));return root_missing ? NULL : &root_node; }
static void of_node_put(struct device_node *node) { CHECK(node==&root_node); }
static int of_property_count_u32_elems(struct device_node *node,const char *name) { CHECK(node==&root_node && !strcmp(name,"meizu,board-id"));return board_count; }
static int of_property_read_u32_array(struct device_node *node,const char *name,u32 *values,u32 count) {
 CHECK(node==&root_node && !strcmp(name,"meizu,board-id") && count==2);values[0]=board_first;values[1]=board_second;return 0;
}
/* HBM_INIT */
/* HBM_ADFR */
/* HBM_INVALIDATE */
/* REAL_LIFECYCLE */
/* REAL_SETTER */
static u32 dsi_display_bl_to_panel(struct dsi_panel *p,u32 level) { (void)p;return level; }
/* REAL_RESTORE */
static void coupled_wait(void *ctx,u32 ms) { (void)ctx;usleep_range(ms*1000,ms*1000+100); }
static const struct m2468_hbm_ops coupled_ops={m2468_send,m2468_restore,coupled_wait};
/* FIXTURE */
static void reset_trace(void) {
 sent=fail_packet=positive_packet=esd_packet=votes=fail_vote=positive_vote=0;
 syncs=time_out=delays=legacy_sends=cleaned=lifecycle_rc=0;
 last_delay=0;prepare_error=0;clock_depth=0;
 wake_error=wait_error=wait_esd=vblank_waits=0;
}
static int set(struct dsi_panel *p,u32 level) {
 p->panel_lock=1;
 int rc=dsi_panel_set_backlight(p,level);
 p->panel_lock=0;
 return rc;
}
static void fresh(struct dsi_panel *p) {
 p->esd_recovery_pending=0;p->panel_initialized=true;p->power_mode=SDE_MODE_DPMS_ON;
 p->panel_mode=DSI_OP_CMD_MODE;p->m2468_hbm.supported=true;p->m2468_hbm.low_power=false;
 p->m2468_hbm.state=(struct m2468_hbm_state){.phase=M2468_HBM_OFF,.adfr_valid=true};
 p->m2468_bl=(struct m2468_bl_panel){.configured=true};
 reset_trace();
}
static int read_config(void *data,const char *name,u32 *value) {
 int bad=*(int *)data;
 if(!strcmp(name,"qcom,mdss-pwm-dc-siwtch")) { if(bad==1)return -EINVAL;*value=bad==2 ? 0 : 1; }
 else if(!strcmp(name,"qcom,mdss-dc-min-brightness")) *value=bad==3 ? 1106 : 1107;
 else if(!strcmp(name,"xjmz,display-ofp-type-config")) *value=2;
 else if(!strcmp(name,"xjmz,display-ofp-enable-config") || !strcmp(name,"qcom,display-adfr-config") ||
 !strcmp(name,"xjmz,display-ofp-hbm-take-effect-vblank-config")) *value=1;
 else return -EINVAL;
 return 0;
}
int main(void) {
 struct mode mode={.priv_info=&fixture,.timing.refresh_rate=144};
 struct dsi_ctrl ctrl={0};
 struct dsi_panel p={.cur_mode=&mode,.bl_config={.type=DSI_BACKLIGHT_DCS,.bl_inverted_dbv=true}};
 struct drm_encoder encoder={0};struct dsi_bridge bridge={.base.encoder=&encoder};
 struct dsi_display d={.panel=&p,.hw_ownership=true,.bridge=&bridge,.ctrl_count=1,.ctrl={{&ctrl}}};
 p.host=&d.host;active_display=&d;
 const u32 levels[]={0,1,408,409,1106,1107,4095};
 const u32 rates[]={144,120,90,60,30}, waits[]={14,18,26,34,66};
 for(u32 r=0;r<5;r++) for(u32 n=0;n<7;n++) {
  fresh(&p);mode.timing.refresh_rate=rates[r];u32 level=levels[n];
  CHECK(!set(&p,level)); CHECK(!legacy_sends && votes==2);
  CHECK(sent==(level ? 51 : 1));CHECK(syncs==(level ? 1 : 0));
  CHECK(delays==syncs && vblank_waits==syncs);if(level)CHECK(last_delay==waits[r]);
  if(level) {
   CHECK(p.m2468_bl.state.valid && p.m2468_bl.state.level==level);
   CHECK(packets[5][0]==0x51 && packets[5][1]==(level>>8) && packets[5][2]==(level&255));
   CHECK(flags_log[5]==66 && flags_log[46]==2 && flags_log[49]==0);
  }
  CHECK(packets[sent-1][0]==0x51 && packets[sent-1][1]==(level>>8) && packets[sent-1][2]==(level&255));
  reset_trace();CHECK(!set(&p,level));CHECK(sent==1 && !syncs && !delays);
 }
 fresh(&p);p.bl_config.bl_inverted_dbv=false;CHECK(!set(&p,0x453));
 CHECK(packets[5][1]==0x04 && packets[5][2]==0x53);
 CHECK(packets[50][1]==0x53 && packets[50][2]==0x04);p.bl_config.bl_inverted_dbv=true;
 puts("PASS: real setter boundaries, all five stock waits, panel units, immutable payloads, repeat, kernel DBV inversion");
 mode.timing.refresh_rate=144;
 fresh(&p);CHECK(!set(&p,4095));reset_trace();CHECK(!set(&p,200));
 CHECK(sent==51 && p.m2468_bl.state.band==M2468_BL_LOW && p.m2468_bl.state.compensation==M2468_BL_MID && !p.m2468_bl.state.dc);
 CHECK(!memcmp(packets[1],"\x88\x40\x80\xf0",4));CHECK(!memcmp(packets[10],"\x88\x20\x40\xf0",4));
 reset_trace();CHECK(!set(&p,408));CHECK(sent==1 && !syncs && p.m2468_bl.state.compensation==M2468_BL_MID);
 reset_trace();CHECK(!set(&p,409));CHECK(sent==4 && !syncs);
 reset_trace();CHECK(!set(&p,1107));CHECK(sent==48 && syncs==1);
 reset_trace();CHECK(!set(&p,1106));CHECK(sent==48 && syncs==1);
 reset_trace();CHECK(!set(&p,408));CHECK(sent==4 && !syncs && p.m2468_bl.state.compensation==M2468_BL_LOW);
 puts("PASS: stock DC-to-low LOW then PWM leaves actual MID; reverse and adjacent transitions");
 fresh(&p);fail_packet=47;CHECK(set(&p,1107)==-EIO && !ctrl.owned && !ctrl.cmd_len);
 fresh(&p);fail_packet=1;CHECK(set(&p,1107)==-EIO && !ctrl.owned && !ctrl.cmd_len);
 fresh(&p);prepare_error=1;CHECK(set(&p,1107)==-EIO && !ctrl.owned && !ctrl.cmd_len);
 for(u32 high=0;high<2;high++) for(int kind=0;kind<3;kind++) for(int packet=1;packet<=51;packet++) {
  fresh(&p);
  if(kind==0)fail_packet=packet;else if(kind==1)positive_packet=packet;else esd_packet=packet;
  CHECK(set(&p,high ? 4095 : 200)==-EIO);CHECK(sent==packet && !p.m2468_bl.state.valid);
  CHECK(!p.m2468_hbm.state.successful_valid && !ctrl.cmd_len && !ctrl.owned);
  p.esd_recovery_pending=0;reset_trace();CHECK(!set(&p,high ? 4095 : 200));CHECK(sent==51 && syncs==1);
 }
 puts("PASS: negative/positive/ESD injection at every packet, batch discard, full retry");
 for(int fail=1;fail<=2;fail++) for(int positive=0;positive<2;positive++) {
  fresh(&p);if(positive)positive_vote=fail;else fail_vote=fail;
  CHECK(set(&p,1107)==(positive ? -EIO : -ETIMEDOUT));CHECK(!p.m2468_bl.state.valid && !p.m2468_hbm.state.successful_valid);
  CHECK(sent==(fail==1 ? 0 : 51));
 }
 fresh(&p);time_out=1;CHECK(set(&p,1107)==-ETIMEDOUT && !sent && !p.m2468_bl.state.valid);
 CHECK(syncs==1 && vblank_waits==1);
 fresh(&p);wait_error=-EWOULDBLOCK;CHECK(!set(&p,1107) && sent==51);
 fresh(&p);wake_error=-EIO;CHECK(set(&p,1107)==-EIO && !sent && !vblank_waits);
 fresh(&p);wait_error=-EIO;CHECK(set(&p,1107)==-EIO && !sent && vblank_waits==1);
 fresh(&p);wait_esd=1;CHECK(set(&p,1107)==-EIO && !sent && !p.m2468_bl.state.valid);
 fresh(&p);d.bridge=NULL;CHECK(set(&p,1107)==-ENODEV && !sent);d.bridge=&bridge;
 fresh(&p);bridge.base.encoder=NULL;CHECK(set(&p,1107)==-ENODEV && !sent);bridge.base.encoder=&encoder;
 mode.timing.refresh_rate=144;
 puts("PASS: synchronous SDE wake/vblank, disabled encoder, timeout, ESD and clock failures");
 for(u32 c=0;c<5;c++) {
  u8 blob[4096]={0};u32 size=0;
  for(u32 j=0;j<fixture.cmd_sets[c].count;j++) {
   struct msg *msg=&fixture.cmd_sets[c].cmds[j].msg;
   blob[size]=msg->type;blob[size+3]=msg->flags;
   blob[size+5]=msg->tx_len>>8;blob[size+6]=msg->tx_len&255;
   memcpy(blob+size+7,msg->tx_buf,msg->tx_len);size+=msg->tx_len+7;
  }
  CHECK(!m2468_bl_check_blob(blob,size,fixture.cmd_sets[c].count));
  CHECK(m2468_bl_check_blob(blob,size-1,fixture.cmd_sets[c].count)==-EINVAL);
  for(u32 tail=1;tail<7;tail++) CHECK(m2468_bl_check_blob(blob,size+tail,fixture.cmd_sets[c].count)==-EINVAL);
  CHECK(m2468_bl_check_blob(blob,size,fixture.cmd_sets[c].count-1)==-EINVAL);
  blob[5]=blob[6]=0;CHECK(m2468_bl_check_blob(blob,size,fixture.cmd_sets[c].count)==-EINVAL);

  struct dsi_panel_cmd_set *t=&fixture.cmd_sets[c];u32 count=t->count;
  fresh(&p);t->count--;CHECK(set(&p,1107)==-EINVAL && !votes && !sent);t->count=count;
  for(u32 j=0;j<count;j++) {
   struct dsi_cmd_desc save=t->cmds[j];
   fresh(&p);t->cmds[j].msg.tx_buf=NULL;CHECK(set(&p,1107)==-EINVAL && !votes);t->cmds[j]=save;
   fresh(&p);t->cmds[j].msg.tx_len=0;CHECK(set(&p,1107)==-EINVAL && !votes);t->cmds[j]=save;
   fresh(&p);t->cmds[j].msg.type=0x15;CHECK(set(&p,1107)==-EINVAL && !votes);t->cmds[j]=save;
   fresh(&p);t->cmds[j].last_command=!save.last_command;CHECK(set(&p,1107)==-EINVAL && !votes);t->cmds[j]=save;
  }
  enum dsi_cmd_set_state state=t->state;t->state=state==DSI_CMD_SET_STATE_LP ? DSI_CMD_SET_STATE_HS : DSI_CMD_SET_STATE_LP;
  fresh(&p);CHECK(set(&p,1107)==-EINVAL && !votes);t->state=state;
 }
 int bad;
 p.utils=(struct dsi_parser_utils){.data=&bad,.read_u32=read_config};
 for(bad=0;bad<4;bad++) { fresh(&p);dsi_m2468_backlight_init(&p);CHECK(p.m2468_bl.configured==(bad==0));if(bad)CHECK(set(&p,1107)==-EINVAL && !votes); }
 struct device_node panel_node={"qcom,mdss_dsi_xjmz_amoled_tianma_cmd"};
 p.panel_of_node=&panel_node;p.name="ILI7838E_TIANMA_MEIZU";p.bl_config.bl_max_level=4095;bad=0;
 for(int mismatch=0;mismatch<9;mismatch++) {
  p.m2468_hbm.supported=false;p.m2468_bl.configured=false;
  if(mismatch==1)p.name="ILI7838E_TIANMA_OTHER";
  if(mismatch==2)panel_node.name="qcom,other";
  if(mismatch==3)board_first=4;
  if(mismatch==4)board_second=6;
  if(mismatch==5)board_count=3;
  if(mismatch==6)root_missing=1;
  if(mismatch==7)p.panel_of_node=NULL;
  if(mismatch==8)bad=3;
  dsi_m2468_hbm_init(&p);dsi_m2468_backlight_init(&p);
  CHECK(p.m2468_hbm.supported==(mismatch==0) && p.m2468_bl.configured==(mismatch==0));
  p.name="ILI7838E_TIANMA_MEIZU";panel_node.name="qcom,mdss_dsi_xjmz_amoled_tianma_cmd";
  board_first=3;board_second=5;board_count=2;root_missing=0;p.panel_of_node=&panel_node;bad=0;
 }
 puts("PASS: complete five-table validation, exact PWM/DC properties and real name/node/board profile");
 fresh(&p);CHECK(!set(&p,1107));p.panel_lock=0;CHECK(!dsi_panel_disable(&p));CHECK(!p.m2468_bl.state.valid);
 CHECK(!dsi_panel_enable(&p));CHECK(!p.m2468_bl.state.valid && p.m2468_hbm.state.phase==M2468_HBM_OFF);
 CHECK(p.power_mode==SDE_MODE_DPMS_ON);reset_trace();CHECK(!set(&p,1107));CHECK(sent==51);
 for(int lp=0;lp<2;lp++) {
  p.panel_lock=0;CHECK(!(lp ? dsi_panel_set_lp2(&p) : dsi_panel_set_lp1(&p)));CHECK(!p.m2468_bl.state.valid);
  reset_trace();CHECK(set(&p,1107)==-EOPNOTSUPP && !sent);
  p.power_mode=lp ? SDE_MODE_DPMS_LP2 : SDE_MODE_DPMS_LP1;
  reset_trace();CHECK(!set(&p,0) && sent==1 && !syncs && !p.m2468_bl.state.valid);
  p.power_mode=SDE_MODE_DPMS_ON;
  CHECK(!dsi_panel_set_nolp(&p));reset_trace();CHECK(!set(&p,1107) && sent==51);
 }
 p.panel_lock=0;lifecycle_rc=-EIO;CHECK(dsi_panel_enable(&p)==-EIO && !p.m2468_bl.state.valid);lifecycle_rc=0;
 fresh(&p);CHECK(!set(&p,1107));p.m2468_hbm.state.phase=M2468_HBM_ON;reset_trace();CHECK(!set(&p,200) && !sent);
 dsi_m2468_hbm_invalidate(&p,false);p.m2468_hbm.state.phase=M2468_HBM_DISABLING;reset_trace();CHECK(!set(&p,200) && sent==51);
 dsi_m2468_hbm_low_power(&p,false);reset_trace();mode.timing.refresh_rate=90;CHECK(!set(&p,200) && sent==51);
 int (*switches[])(struct dsi_panel *)={dsi_panel_switch_cmd_mode_out,dsi_panel_switch_video_mode_out,
  dsi_panel_switch_video_mode_in,dsi_panel_switch_cmd_mode_in,dsi_panel_switch,dsi_panel_post_switch};
 for(u32 i=0;i<6;i++) for(int fail=0;fail<2;fail++) {
  fresh(&p);CHECK(!set(&p,200));lifecycle_rc=fail ? -EIO : 0;
  CHECK(switches[i](&p)==lifecycle_rc && !p.m2468_bl.state.valid);
  reset_trace();CHECK(!set(&p,200) && sent==51 && syncs==1);
 }
 p.esd_recovery_pending=1;reset_trace();CHECK(set(&p,200)==-EIO && !sent && !p.m2468_bl.state.valid);
 fresh(&p);p.m2468_hbm.supported=false;CHECK(!set(&p,1107) && legacy_sends==1 && !votes && !sent);
 fresh(&p);d.hw_ownership=false;CHECK(set(&p,1107)==-EOPNOTSUPP && !votes);d.hw_ownership=true;
 fresh(&p);mode.timing.refresh_rate=45;CHECK(set(&p,1107)==-EOPNOTSUPP && !sent);
 fresh(&p);CHECK(set(&p,4096)==-ERANGE && !votes && !sent);
 puts("PASS: real ON/OFF/LP/NOLP lifecycle, HBM gate/restore, mode invalidation, ESD, non-M2468 isolation");
 CHECK(!memcmp(fixture.cmd_sets[0].cmds[5].msg.tx_buf,"\x51\x04\x53",3));
 CHECK(!memcmp(fixture.cmd_sets[1].cmds[5].msg.tx_buf,"\x51\x04\x52",3));
 fresh(&p);mode.timing.refresh_rate=120;p.m2468_hbm.state.adfr_valid=false;
 CHECK(!set(&p,1107) && sent==60 && p.m2468_hbm.state.adfr_valid && !p.m2468_hbm.state.adfr);
 for(u32 r=0;r<3;r++) {
  fresh(&p);mode.timing.refresh_rate=(u32[]){120,60,30}[r];p.m2468_hbm.state.adfr=32;
  CHECK(!set(&p,1107) && sent==78 && p.m2468_hbm.state.adfr_valid && p.m2468_hbm.state.adfr==32);
  CHECK(!memcmp(packets[75],"\xb1\xff\xff\xff\x05",5));
  reset_trace();CHECK(!set(&p,1107) && sent==1 && !syncs);
 }
 for(int kind=0;kind<3;kind++) for(int packet=1;packet<=78;packet++) {
  fresh(&p);mode.timing.refresh_rate=120;p.m2468_hbm.state.adfr=32;
  if(kind==0)fail_packet=packet;else if(kind==1)positive_packet=packet;else esd_packet=packet;
  CHECK(set(&p,1107)==-EIO && !p.m2468_bl.state.valid && !ctrl.owned && !ctrl.cmd_len);
  if(packet<=9 || packet>60 || kind==2)CHECK(!p.m2468_hbm.state.adfr_valid);
  else CHECK(p.m2468_hbm.state.adfr_valid && p.m2468_hbm.state.adfr==32);
  p.esd_recovery_pending=0;
 }
 fresh(&p);mode.timing.refresh_rate=120;p.m2468_hbm.state.adfr=32;time_out=1;
 CHECK(set(&p,1107)==-ETIMEDOUT && sent==27 && !p.m2468_bl.state.valid && p.m2468_hbm.state.adfr_valid && p.m2468_hbm.state.adfr==32);
 fresh(&p);mode.timing.refresh_rate=120;p.m2468_hbm.state.adfr=32;time_out=1;fail_packet=10;
 CHECK(set(&p,1107)==-ETIMEDOUT && !p.m2468_hbm.state.adfr_valid && !ctrl.owned && !ctrl.cmd_len);
 fresh(&p);mode.timing.refresh_rate=120;p.m2468_hbm.state.phase=M2468_HBM_DISABLING;
 p.bl_config.bl_level=1107;p.panel_lock=1;
 CHECK(!m2468_restore(&p) && sent==51 && !p.m2468_hbm.state.adfr);p.panel_lock=0;
 for(u32 r=0;r<2;r++) {
  fresh(&p);mode.timing.refresh_rate=r ? 90 : 144;p.m2468_hbm.state.adfr=32;
  struct dsi_panel_cmd_set saved_on=fixture.cmd_sets[DSI_CMD_SET_M2468_ADFR_ON],saved_off=fixture.cmd_sets[DSI_CMD_SET_M2468_ADFR_OFF];
  fixture.cmd_sets[DSI_CMD_SET_M2468_ADFR_ON]=(struct dsi_panel_cmd_set){0};
  fixture.cmd_sets[DSI_CMD_SET_M2468_ADFR_OFF]=(struct dsi_panel_cmd_set){0};
  CHECK(!set(&p,1107) && sent==51 && p.m2468_hbm.state.adfr==32);
  fixture.cmd_sets[DSI_CMD_SET_M2468_ADFR_ON]=saved_on;fixture.cmd_sets[DSI_CMD_SET_M2468_ADFR_OFF]=saved_off;
 }
 for(int fixed=0;fixed<2;fixed++) for(int high=0;high<2;high++) for(int fail=0;fail<2;fail++) {
  fresh(&p);mode.timing.refresh_rate=fixed ? 144 : 120;
  u32 level=high ? 3300 : 2000;p.bl_config.bl_level=level;p.m2468_hbm.state.requested=level;
  CHECK(!set(&p,level));p.m2468_hbm.state.adfr=32;p.m2468_hbm.state.adfr_valid=true;
  reset_trace();clock_depth=1;p.panel_lock=1;int ready_before=ready_notifications;
  CHECK(!m2468_hbm_run(&p.m2468_hbm.state,true,mode.timing.refresh_rate,!fixed,&coupled_ops,&p));
  CHECK(p.m2468_hbm.state.phase==M2468_HBM_ON && !p.m2468_bl.state.valid);
  CHECK(p.m2468_hbm.state.saved_adfr==(fixed ? 0u : 32u));
  reset_trace();clock_depth=1;
  if(fail)fail_packet=50+(high ? 3 : 0)+1;
  CHECK(m2468_hbm_run(&p.m2468_hbm.state,false,mode.timing.refresh_rate,!fixed,&coupled_ops,&p)==(fail ? -EIO : 0));
  CHECK(p.m2468_hbm.state.phase==(fail ? M2468_HBM_FAULT : M2468_HBM_OFF));
  CHECK(p.m2468_bl.state.valid==!fail && !ctrl.owned && !ctrl.cmd_len && clock_depth==1);
  CHECK(ready_notifications==ready_before);
  if(!fixed)CHECK(p.m2468_hbm.state.adfr_valid && p.m2468_hbm.state.adfr==32);
  else CHECK(!p.m2468_hbm.state.adfr_valid);
  if(!fail)CHECK(p.m2468_hbm.state.saved_adfr==0 && p.m2468_hbm.state.successful_valid);
  p.panel_lock=0;clock_depth=0;
 }
 puts("PASS: real ADFR helpers and coupled HBM run/send/restore/setter: saved ADFR, phase, ready isolation, nested votes, recovery failures");
 puts("ALL M2468 BACKLIGHT HOST TESTS PASSED (hardware, SDE events and time are test doubles)");
 return 0;
}
