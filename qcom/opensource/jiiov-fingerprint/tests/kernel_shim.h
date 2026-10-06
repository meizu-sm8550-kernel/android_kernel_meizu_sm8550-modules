/* SPDX-License-Identifier: GPL-2.0-only */
/* Host boundaries only. Driver state and decisions are extracted unchanged. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int irqreturn_t;
typedef struct { int value; } atomic_t;
#define ATOMIC_INIT(value_) { (value_) }
static inline int atomic_cmpxchg(atomic_t *a, int old, int value)
{ int previous = a->value; if (previous == old) a->value = value; return previous; }
static inline void atomic_set(atomic_t *a, int value) { a->value = value; }
static atomic_t jiiov_claimed = ATOMIC_INIT(0);
#define __user
#define __init
#define __exit
#define IRQ_HANDLED 1
#define IRQF_TRIGGER_FALLING 2
#define IRQF_ONESHOT 0x2000
#define GFP_KERNEL 0
#define MSG_DONTWAIT 0x40
#define THIS_MODULE NULL
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define dev_err(dev, format, ...) ((void)(dev))
#define dev_warn(dev, format, ...) ((void)(dev))
#define dev_info(dev, format, ...) ((void)(dev))

struct mutex { pthread_mutex_t native; };
#define DEFINE_MUTEX(name) struct mutex name = { PTHREAD_MUTEX_INITIALIZER }
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->native, NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->native)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->native)); }
struct cdev { int removed; void *owner; };
struct property { const char *name; struct property *next; };
struct device_node {
    bool pmic, gpio, direct, boot, supply; u32 volts[3]; int reset, irq, property_error;
    struct device_node *parent, *supplier; struct property *properties;
    const char *compatible; int refs;
};
#define for_each_property_of_node(node, prop) for (prop = (node)->properties; prop; prop = prop->next)
struct class { int unused; };
struct device {
    void *data; int refs; int released; struct { int unused; } kobj;
    struct device_node *of_node; void (*release)(struct device *);
    struct device *parent; struct class *class; dev_t devt;
};
struct platform_device { struct device dev; };
struct inode { struct cdev *i_cdev; };
struct file { void *private_data; };
struct device_attribute { int unused; };
struct pinctrl { int unused; };
struct pinctrl_state { int id; };
struct regulator { int unused; };
struct wakeup_source { int alive; };
struct work_struct { bool pending; void (*fn)(struct work_struct *); };

static struct pinctrl fake_pinctrl;
static struct pinctrl_state states[] = { { 10 }, { 11 }, { 12 } };
static struct regulator fake_regulator;
static struct wakeup_source fake_wakeup = { 1 };
static int fail_at, step, failure = -EIO, journal[512], njournal;
static int requested_gpios, pinctrl_refs, regulator_refs, enable_count, voltage_calls;
static int power_refs, irq_refs, irq_enable_calls, irq_disable_calls, cancel_calls;
static int irq_wake_depth;
static int copy_fail, raw_level, wake_ms, wake_stay, wake_relax, send_count;
static int (*request_hook)(void *);
static unsigned long requested_irq_flags;
static int jiiov_attribute_group;
static int jiiov_fops, jiiov_driver;
static dev_t jiiov_devt;
static struct class fake_class, *jiiov_class = &fake_class;
static int live_allocations, live_wakeups, init_work_count, probe_fault, early_open_result;
static int live_regions, live_classes, live_drivers, module_fault;
static int module_refs, regulator_load, disable_calls, late_drms_fault, module_going;
static inline bool try_module_get(void *module) { if (module_going) return false; module_refs++; return true; }
static inline void module_put(void *module) { assert(module_refs > 0); module_refs--; }
static int jiiov_open(struct inode *inode, struct file *file);
static void record(int value) { assert(njournal < 512); journal[njournal++] = value; }
static int fail(void) { return ++step == fail_at ? failure : 0; }
static void clear_trace(void) { njournal = step = fail_at = 0; failure = -EIO; }
static const char *dev_name(struct device *d) { return "jiiov_fp"; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static void platform_set_drvdata(struct platform_device *p, void *d) { p->dev.data = d; }
static struct device *get_device(struct device *d) { assert(d->refs > 0); ++d->refs; return d; }
static void put_device(struct device *d)
{ assert(d->refs > 0); if (!--d->refs) { d->released++; if (d->release) d->release(d); } }
static void cdev_device_del(struct cdev *c, struct device *d) { c->removed++; record(60); }
static void sysfs_remove_group(void *kobj, void *group) { record(61); }
static void *kzalloc(size_t size, int flags)
{ void *p; if (probe_fault == 1) return NULL; p = calloc(1, size); assert(p); live_allocations++; return p; }
static void kfree(void *p) { live_allocations--; free(p); }
static void device_initialize(struct device *d) { d->refs = 1; }
static int dev_set_name(struct device *d, const char *name)
{ assert(!strcmp(name, "jiiov_fp")); return probe_fault == 2 ? -ENOMEM : 0; }
static void init_work(struct work_struct *w, void (*fn)(struct work_struct *))
{ w->pending = false; w->fn = fn; init_work_count++; }
#define INIT_WORK(w, fn) init_work(w, fn)
static void cdev_init(struct cdev *c, void *fops) { c->removed = 0; }
static int cdev_device_add(struct cdev *c, struct device *d)
{
    struct inode inode = { c }; struct file f = {0};
    early_open_result = jiiov_open(&inode, &f); /* device_add may expose open even on failure. */
    return probe_fault == 4 ? -EIO : 0;
}
static int sysfs_create_group(void *kobj, void *group) { return probe_fault == 5 ? -EIO : 0; }
static struct wakeup_source *wakeup_source_register(struct device *d, const char *name)
{ if (probe_fault == 3) return NULL; live_wakeups++; fake_wakeup.alive = 1; return &fake_wakeup; }
static bool of_property_read_bool(struct device_node *n, const char *name)
{
    if (!strcmp(name, "anc,vdd_use_pmic")) return n->pmic;
    if (!strcmp(name, "anc,vdd_use_gpio")) return n->gpio;
    if (!strcmp(name, "anc,use_gpio_init")) return n->direct;
    assert(!strcmp(name, "anc,enable-on-boot")); return n->boot;
}
static void *of_find_property(struct device_node *n, const char *name, void *length)
{
    struct property *p;
    if (!strcmp(name, "vdd-supply")) return n->supply ? n : NULL;
    if (!strcmp(name, "compatible")) return (void *)n->compatible;
    for_each_property_of_node(n, p) if (!strcmp(name, p->name)) return p;
    return NULL;
}
static inline struct device_node *of_parse_phandle(struct device_node *n, const char *name, int index)
{ assert(!strcmp(name, "vdd-supply") && !index); if (n->supplier) n->supplier->refs++; return n->supplier; }
static inline struct device_node *of_get_parent(struct device_node *n)
{ if (n->parent) n->parent->refs++; return n->parent; }
static inline void of_node_put(struct device_node *n) { if (n) { assert(n->refs > 0); n->refs--; } }
static inline bool of_device_is_compatible(struct device_node *n, const char *compatible)
{ return n->compatible && !strcmp(n->compatible, compatible); }
static int of_property_read_u32_array(struct device_node *n, const char *name, u32 *out, size_t count)
{ assert(!strcmp(name, "anc,vdd_config") && count == 3); if (n->property_error) return n->property_error; memcpy(out, n->volts, 12); return 0; }
static int of_get_named_gpio(struct device_node *n, const char *name, int index)
{ assert(index == 0); if (!strcmp(name, "anc,gpio_rst")) return n->reset; assert(!strcmp(name, "anc,gpio_irq")); return n->irq; }
static bool gpio_is_valid(int gpio) { return gpio >= 0 && gpio <= 511; }
static int alloc_chrdev_region(dev_t *devt, unsigned int first, unsigned int count, const char *name)
{ record(72); if (module_fault == 2) return -ENOMEM; *devt = 123; live_regions++; return 0; }
static void unregister_chrdev_region(dev_t devt, unsigned int count) { record(77); live_regions--; }
static struct class *class_create(void *owner, const char *name)
{ record(73); if (module_fault == 3) return ERR_PTR(-ENOMEM); live_classes++; return &fake_class; }
static void class_destroy(struct class *class) { record(76); live_classes--; }
static int platform_driver_register(void *driver)
{ record(74); if (module_fault == 4) return -EIO; live_drivers++; return 0; }
static void platform_driver_unregister(void *driver) { record(75); live_drivers--; }

static struct pinctrl *pinctrl_get(struct device *d)
{ int e = fail(); if (e) return ERR_PTR(e); ++pinctrl_refs; return &fake_pinctrl; }
static void pinctrl_put(struct pinctrl *p) { assert(p == &fake_pinctrl); --pinctrl_refs; record(30); }
static struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *p, const char *name)
{
    int e = fail(); if (e) return ERR_PTR(e);
    if (!strcmp(name, "anc_reset_low")) return &states[0];
    if (!strcmp(name, "anc_reset_high")) return &states[1];
    assert(!strcmp(name, "anc_irq_default")); return &states[2];
}
static int pinctrl_select_state(struct pinctrl *p, struct pinctrl_state *state)
{ assert(p == &fake_pinctrl); record(state->id); return fail(); }
static int gpio_request(unsigned int gpio, const char *name)
{ int e = fail(); assert(gpio == 40 || gpio == 41); if (!e) requested_gpios |= gpio == 41 ? 1 : 2; return e; }
static void gpio_free(unsigned int gpio)
{ int bit = gpio == 41 ? 1 : 2; assert(requested_gpios & bit); requested_gpios &= ~bit; record(gpio); }
static int gpio_direction_output(unsigned int gpio, int value)
{ assert(gpio == 41 && value == 0); return fail(); }
static int gpio_direction_input(unsigned int gpio) { assert(gpio == 40); return fail(); }
static int gpio_get_value_cansleep(unsigned int gpio) { assert(gpio == 40); return raw_level; }
static int gpio_to_irq(unsigned int gpio) { int e = fail(); assert(gpio == 40); return e ? e : 1040; }
static void usleep_range(unsigned long minimum, unsigned long maximum)
{ record((int)minimum); record((int)maximum); }

static struct regulator *regulator_get(struct device *d, const char *id)
{ int e = fail(); assert(!strcmp(id, "vdd")); if (e) return ERR_PTR(e); regulator_refs++; return &fake_regulator; }
static int regulator_count_voltages(struct regulator *r) { int e = fail(); return e ? e : 1; }
static int regulator_set_voltage(struct regulator *r, int min, int max)
{ assert(min == 3200000 && max == 3200000); voltage_calls++; return fail(); }
static int regulator_set_load(struct regulator *r, int ua)
{ int e = fail(); assert(ua == 150000 || ua == 0); if (!e) regulator_load = ua; return e; }
static int regulator_enable(struct regulator *r)
{ int e = fail(); enable_count++; if (!e) power_refs++; return e; }
static int regulator_disable(struct regulator *r)
{
    int e = fail(); record(31); disable_calls++;
    if (!e) {
        assert(power_refs == 1); power_refs--;
        /* Actual 5.15 DRMS failure can follow enable_count decrement. */
        if (regulator_load && late_drms_fault) return -EIO;
    }
    return e;
}
static void regulator_put(struct regulator *r)
{ assert(r == &fake_regulator && regulator_refs == 1 && !power_refs); regulator_refs--; record(32); }

static int request_threaded_irq(unsigned int irq, void *top,
    irqreturn_t (*thread)(int, void *), unsigned long flags, const char *name, void *data)
{
    int e = fail(); assert(irq == 1040 && !top); requested_irq_flags = flags;
    if (!e) { irq_refs++; if (request_hook) request_hook(data); }
    return e;
}
/* Match Linux 5.15 irq_set_irq_wake: a chip failure restores wake_depth. */
static int enable_irq_wake(unsigned int irq)
{
    int ret = 0;
    if (irq_wake_depth++ == 0) {
        ret = fail();
        if (ret) irq_wake_depth = 0;
    }
    return ret;
}
static int disable_irq_wake(unsigned int irq)
{
    int ret = 0; record(50); assert(irq_wake_depth > 0);
    if (--irq_wake_depth == 0) {
        ret = fail();
        if (ret) irq_wake_depth = 1;
    }
    return ret;
}
static void free_irq(unsigned int irq, void *data)
{ assert(irq_refs == 1); irq_refs--; record(51); }
static void enable_irq(unsigned int irq) { assert(irq_refs); irq_enable_calls++; }
static void disable_irq(unsigned int irq) { assert(irq_refs); irq_disable_calls++; }
static void cancel_work_sync(struct work_struct *work)
{ assert(work->fn); work->pending = false; cancel_calls++; record(52); }
static void *system_wq;
static bool queue_work(void *wq, struct work_struct *work)
{ bool was_pending = work->pending; assert(work->fn); work->pending = true; return !was_pending; }
static void pm_wakeup_ws_event(struct wakeup_source *w, unsigned int ms, bool hard)
{ assert(w && w->alive && !hard); wake_ms = ms; }
static void __pm_stay_awake(struct wakeup_source *w) { assert(w && w->alive); wake_stay++; }
static void __pm_relax(struct wakeup_source *w) { assert(w && w->alive); wake_relax++; }
static void wakeup_source_unregister(struct wakeup_source *w)
{ assert(!irq_refs); assert(w && w->alive); w->alive = 0; if (live_wakeups) live_wakeups--; record(53); }
static unsigned long copy_from_user(void *to, const void *from, unsigned long n)
{ if (copy_fail || !from) return n; memcpy(to, from, n); return 0; }
static unsigned long copy_to_user(void *to, const void *from, unsigned long n)
{ if (copy_fail || !to) return n; memcpy(to, from, n); return 0; }

struct sock { int alive; };
struct nlmsghdr { u32 nlmsg_len; uint16_t nlmsg_type, nlmsg_flags; u32 nlmsg_seq, nlmsg_pid; };
struct sk_buff { unsigned int len; unsigned char bytes[256]; };
#define NLMSG_HDRLEN 16
#define NLMSG_ALIGN(len) (((len) + 3) & ~3)
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_HDRLEN + (len))
static struct sock fake_socket = { 1 };
static struct sock *jiiov_nl_socket = &fake_socket;
struct netlink_kernel_cfg { void (*input)(struct sk_buff *); };
static int init_net;
static struct sock *netlink_kernel_create(void *net, int protocol, struct netlink_kernel_cfg *cfg)
{ assert(protocol == 30 && cfg->input); record(70); if (module_fault == 1) return NULL; fake_socket.alive = 1; return &fake_socket; }
static void netlink_kernel_release(struct sock *s) { assert(s->alive); s->alive = 0; record(78); }
static DEFINE_MUTEX(jiiov_nl_lock);
static int allocation_fail, header_fail, pull_fail, send_failure;
static u8 last_event;
static u32 last_destination, last_pid;
static unsigned int last_length, last_type;
static struct nlmsghdr *nlmsg_hdr(struct sk_buff *skb) { return (void *)skb->bytes; }
static void *nlmsg_data(struct nlmsghdr *n) { return (char *)n + 16; }
static int nlmsg_len(struct nlmsghdr *n) { return n->nlmsg_len - 16; }
static bool nlmsg_ok(struct nlmsghdr *n, int remaining)
{ return remaining >= 16 && n->nlmsg_len >= 16 && n->nlmsg_len <= (unsigned int)remaining; }
static bool pskb_may_pull(struct sk_buff *skb, unsigned int size) { return !pull_fail && skb->len >= size; }
static struct sk_buff *nlmsg_new(size_t size, int flags)
{ return allocation_fail ? NULL : calloc(1, sizeof(struct sk_buff)); }
static struct nlmsghdr *nlmsg_put(struct sk_buff *skb, u32 pid, u32 seq, int type, int size, int flags)
{
    struct nlmsghdr *n = nlmsg_hdr(skb); if (header_fail) return NULL;
    n->nlmsg_len = 16 + size; n->nlmsg_pid = pid; n->nlmsg_seq = seq;
    n->nlmsg_type = type; n->nlmsg_flags = flags; skb->len = NLMSG_SPACE(size); return n;
}
static void kfree_skb(struct sk_buff *skb) { free(skb); }
static int netlink_unicast(struct sock *socket, struct sk_buff *skb, u32 port, int flags)
{
    struct nlmsghdr *n = nlmsg_hdr(skb); int result = send_failure ? send_failure : (int)skb->len;
    assert(socket && socket->alive && flags == MSG_DONTWAIT);
    last_event = *(u8 *)nlmsg_data(n); last_destination = port; last_pid = n->nlmsg_pid;
    last_length = n->nlmsg_len; last_type = n->nlmsg_type; send_count++; free(skb); return result;
}
int jiiov_netlink_send(u8 event);
int jiiov_netlink_init(void);
void jiiov_netlink_exit(void);
