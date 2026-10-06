/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel_shim.h"
#include "driver.inc"
#include "netlink.inc"

static void setup(struct jiiov_data *d, struct platform_device *p)
{
    memset(d, 0, sizeof(*d)); memset(p, 0, sizeof(*p)); clear_trace();
    assert(!requested_gpios && !pinctrl_refs && !regulator_refs && !irq_refs && !power_refs && !irq_wake_depth);
    d->dev = &p->dev; p->dev.data = d; d->chardev.refs = 1; mutex_init(&d->lock);
    d->rst_gpio = 41; d->irq_gpio = 40; d->irq = -1;
    d->vdd_config[0] = 3200000; d->vdd_config[1] = 3200000; d->vdd_config[2] = 150000;
    fake_wakeup.alive = 1; d->wakeup = &fake_wakeup;
    d->irq_work.fn = jiiov_irq_work;
    copy_fail = raw_level = wake_ms = wake_stay = wake_relax = 0;
    enable_count = voltage_calls = irq_enable_calls = irq_disable_calls = cancel_calls = 0;
    assert(!module_refs && !jiiov_claimed.value); regulator_load = disable_calls = late_drms_fault = 0;
    request_hook = NULL;
}

static void test_resource_rollback_and_reset(void)
{
    struct jiiov_data d; struct platform_device p; int i;
    /* Each acquisition/direction/selection failure must release only owned objects. */
    for (i = 1; i <= 10; ++i) {
        setup(&d, &p); fail_at = i;
        assert(jiiov_resources_request(&d) == -EIO);
        assert(!d.resources && !requested_gpios && !pinctrl_refs);
        assert(!d.pinctrl && !d.reset_owned && !d.irq_gpio_owned);
    }
    setup(&d, &p); assert(jiiov_reset(&d) == -ENODEV);
    assert(!jiiov_resources_request(&d)); assert(d.resources && requested_gpios == 3);
    assert(journal[0] == 12 && journal[1] == 10);
    clear_trace(); assert(!jiiov_resources_request(&d)); assert(step == 0);
    assert(!jiiov_reset(&d));
    assert(njournal == 6 && journal[0] == 10 && journal[1] == 10000 && journal[2] == 10100 &&
        journal[3] == 11 && journal[4] == 10000 && journal[5] == 10100);
    clear_trace(); fail_at = 1; assert(jiiov_reset(&d) == -EIO); assert(njournal == 1);
    clear_trace(); fail_at = 2; assert(jiiov_reset(&d) == -EIO); assert(njournal == 4);
    clear_trace(); assert(!jiiov_resources_release(&d)); assert(!requested_gpios && !pinctrl_refs);
    assert(!jiiov_resources_release(&d));
    puts("PASS resource acquisition failure rollback, idempotence and reset 10000/10100 us sleep requests");
}

static void test_power_errors_and_balance(void)
{
    struct jiiov_data d; struct platform_device p; int i;
    for (i = 1; i <= 5; ++i) {
        setup(&d, &p); assert(!jiiov_resources_request(&d)); clear_trace(); fail_at = i;
        assert(jiiov_power(&d, true) == -EIO);
        assert(!d.powered && !d.vdd && !regulator_refs && !power_refs);
        clear_trace(); assert(!jiiov_resources_release(&d));
    }
    setup(&d, &p); assert(jiiov_power(&d, true) == -ENODEV); assert(!jiiov_power(&d, false));
    assert(!jiiov_resources_request(&d)); module_going = 1;
    assert(jiiov_power(&d, true) == -ENODEV && !module_refs && !regulator_refs); module_going = 0;
    assert(!jiiov_power(&d, true)); assert(!jiiov_power(&d, true)); assert(module_refs == 1);
    assert(enable_count == 1 && power_refs == 1 && regulator_refs == 1 && d.powered);
    clear_trace(); fail_at = 2; assert(jiiov_power(&d, false) == -EIO);
    assert(d.powered && d.vdd && power_refs == 1 && regulator_refs == 1 && module_refs == 1);
    clear_trace(); assert(!jiiov_power(&d, true));
    assert(regulator_load == 150000 && enable_count == 1 && module_refs == 1);
    clear_trace(); fail_at = 2; assert(jiiov_resources_release(&d) == -EIO);
    assert(d.resources && requested_gpios == 3);
    clear_trace(); assert(!jiiov_power(&d, false)); assert(!jiiov_power(&d, false));
    assert(!d.vdd && !d.powered && !power_refs && !regulator_refs);
    assert(!jiiov_resources_release(&d));
    puts("PASS regulator failures propagate, duplicate on balances, disable failure retains retry state");
}

static int immediate_irq(void *data) { return jiiov_irq_thread(1040, data); }
static void test_irq_mask_work_and_teardown(void)
{
    struct jiiov_data d; struct platform_device p; int sends;
    setup(&d, &p); assert(!jiiov_irq_free(&d)); assert(jiiov_irq_request(&d) == -ENODEV);
    assert(jiiov_irq_enable(&d, true) == -ENODEV);
    assert(!jiiov_resources_request(&d));
    module_going = 1; assert(jiiov_irq_request(&d) == -ENODEV && !module_refs && !irq_refs); module_going = 0;
    clear_trace(); fail_at = 2; assert(jiiov_irq_request(&d) == -EIO); assert(!irq_refs && !module_refs);
    clear_trace(); fail_at = 3; assert(jiiov_irq_request(&d) == -EIO); assert(!irq_refs && !d.irq_requested && !module_refs);
    clear_trace(); request_hook = immediate_irq;
    assert(!jiiov_irq_request(&d)); assert(d.irq_work.pending && wake_ms == 125);
    assert(d.irq_requested && d.irq_enabled && requested_irq_flags == 0x2002);
    assert(module_refs == 1);
    assert(!jiiov_irq_request(&d)); assert(d.irq_work.pending);
    d.irq_mask = true; sends = send_count; d.irq_work.fn(&d.irq_work);
    assert(send_count == sends + 1 && last_event == 1); /* Mask does not revoke queued work. */
    d.irq_work.pending = false; wake_ms = 0; jiiov_irq_thread(1040, &d);
    assert(!d.irq_work.pending && wake_ms == 0);
    d.irq_mask = false; jiiov_irq_thread(1040, &d); assert(d.irq_work.pending);
    assert(!jiiov_irq_enable(&d, true)); assert(!irq_enable_calls);
    assert(!jiiov_irq_enable(&d, false)); assert(!jiiov_irq_enable(&d, false)); assert(irq_disable_calls == 1);
    assert(!jiiov_irq_enable(&d, true)); assert(irq_enable_calls == 1);
    clear_trace(); assert(!jiiov_resources_release(&d));
    assert(journal[0] == 50 && journal[1] == 51 && journal[2] == 52);
    assert(!d.irq_requested && !d.irq_enabled && !d.irq_work.pending && !irq_refs && !module_refs);
    puts("PASS IRQ immediate delivery, falling/oneshot, mask semantics and teardown ordering");
}

static void test_power_off_load_before_disable(void)
{
    struct jiiov_data d; struct platform_device p;
    setup(&d, &p); assert(!jiiov_resources_request(&d)); assert(!jiiov_power(&d, true));
    clear_trace(); fail_at = 1;
    assert(jiiov_power(&d, false) == -EIO);
    assert(disable_calls == 0 && d.powered && power_refs == 1 && regulator_load == 150000);
    clear_trace(); late_drms_fault = 1;
    assert(!jiiov_power(&d, false));
    assert(!power_refs && !d.powered && !d.vdd && !regulator_load && !module_refs);
    assert(!jiiov_resources_release(&d));
    puts("PASS load is cleared before disable, load failure preserves count, no late DRMS underflow");
}

static void test_irq_wake_disable_failure_retry(void)
{
    struct jiiov_data d; struct platform_device p; struct file f; int cancels;
    setup(&d, &p); f.private_data = &d;
    assert(!jiiov_ioctl(&f, 0x6101, 0)); assert(!jiiov_ioctl(&f, 0x610b, 0));
    assert(!jiiov_ioctl(&f, 0x6105, 0)); assert(irq_wake_depth == 1);
    jiiov_irq_thread(1040, &d); cancels = cancel_calls;
    clear_trace(); fail_at = 1;
    assert(jiiov_ioctl(&f, 0x6106, 0) == -EIO);
    assert(d.irq_requested && d.irq_wake && d.irq == 1040 && irq_refs == 1);
    assert(irq_wake_depth == 1 && cancel_calls == cancels && module_refs == 2);
    assert(d.irq_work.pending && d.resources && d.powered);
    clear_trace(); assert(!jiiov_ioctl(&f, 0x6105, 0));
    assert(irq_wake_depth == 1); /* Retained request must not increment wake depth. */
    clear_trace(); fail_at = 1;
    assert(jiiov_ioctl(&f, 0x6102, 0) == -EIO);
    assert(d.resources && requested_gpios == 3 && d.powered && power_refs == 1);
    assert(d.irq_requested && d.irq_wake && irq_wake_depth == 1);
    clear_trace(); assert(!jiiov_ioctl(&f, 0x6106, 0));
    assert(!irq_wake_depth && !irq_refs && !d.irq_requested && !d.irq_wake && module_refs == 1);
    assert(!jiiov_ioctl(&f, 0x6105, 0)); assert(irq_wake_depth == 1);
    assert(!jiiov_ioctl(&f, 0x6102, 0)); assert(!irq_wake_depth && !requested_gpios && !power_refs);
    puts("PASS IRQ wake-disable failure retains IRQ/resources, retries and balances wake_depth");
}

static void test_ioctl_exact_values_and_copies(void)
{
    struct jiiov_data d; struct platform_device p; struct file f;
    u8 level = 99, product[16]; unsigned int bad[] = {0x6109,0x610a,0x610d,0x6111,0x4004610d,0x40086111,0x6112,0x80026113};
    size_t i; setup(&d, &p); f.private_data = &d;
    assert(jiiov_ioctl(&f, 0x6200, 0) == -ENOTTY);
    for (i = 0; i < ARRAY_SIZE(bad); i++) assert(jiiov_ioctl(&f, bad[i], 0) == -EINVAL);
    assert(!jiiov_ioctl(&f, 0x610e, 0)); assert(!requested_gpios && !regulator_refs);
    assert(!jiiov_ioctl(&f, 0x6107, 0) && d.irq_mask);
    assert(!jiiov_ioctl(&f, 0x6108, 0) && !d.irq_mask);
    assert(!jiiov_ioctl(&f, 0x610f, 0) && wake_stay == 1);
    assert(!jiiov_ioctl(&f, 0x6110, 0) && wake_relax == 1 && wake_ms == 125);
    memset(product, 0xff, sizeof(product)); assert(!jiiov_ioctl(&f, 0x40106112, (unsigned long)product));
    assert(!memcmp(d.product_info, product, 16)); copy_fail = 1;
    assert(jiiov_ioctl(&f, 0x40106112, (unsigned long)product) == -EFAULT); copy_fail = 0;
    assert(jiiov_ioctl(&f, 0x80016113, (unsigned long)&level) == -ENODEV);
    assert(!jiiov_ioctl(&f, 0x6101, 0)); raw_level = 1;
    assert(!jiiov_ioctl(&f, 0x80016113, (unsigned long)&level) && level == 1);
    copy_fail = 1; assert(jiiov_ioctl(&f, 0x80016113, (unsigned long)&level) == -EFAULT); copy_fail = 0;
    raw_level = -EIO; assert(jiiov_ioctl(&f, 0x80016113, (unsigned long)&level) == -EINVAL);
    assert(!jiiov_ioctl(&f, 0x610b, 0)); assert(!jiiov_ioctl(&f, 0x6100, 0));
    assert(!jiiov_ioctl(&f, 0x6105, 0)); assert(!jiiov_ioctl(&f, 0x6104, 0)); assert(!jiiov_ioctl(&f, 0x6103, 0));
    assert(!jiiov_ioctl(&f, 0x6106, 0)); assert(!jiiov_ioctl(&f, 0x6106, 0));
    assert(!jiiov_ioctl(&f, 0x610c, 0)); assert(!jiiov_ioctl(&f, 0x6102, 0));
    puts("PASS exact ioctl dispatch, 16-byte product, raw one-byte IRQ, EFAULT and wake unlock grace");
}

static void test_sysfs_errors_and_explicit_events(void)
{
    struct jiiov_data d; struct platform_device p; const char *events[] =
        {"test", "irq", "screen_off", "screen_on", "touch_down", "touch_up", "ui_ready", "exit"};
    size_t i; setup(&d, &p);
    assert(resource_set_store(&p.dev, NULL, "requ", 4) == -EINVAL);
    assert(device_power_store(&p.dev, NULL, "on\n", 3) == -ENODEV);
    assert(hw_reset_store(&p.dev, NULL, "reset", 5) == -ENODEV);
    assert(irq_set_store(&p.dev, NULL, "enable", 6) == -ENODEV);
    assert(pinctl_set_store(&p.dev, NULL, "anc_reset_high", 14) == -ENODEV);
    fail_at = 1; assert(resource_set_store(&p.dev, NULL, "request", 7) == -EIO); clear_trace();
    assert(resource_set_store(&p.dev, NULL, "request_suffix", 14) == 14);
    assert(device_power_store(&p.dev, NULL, "on\n", 3) == 3);
    assert(hw_reset_store(&p.dev, NULL, "reset\n", 6) == 6);
    assert(pinctl_set_store(&p.dev, NULL, "anc_reset_high\n", 15) == 15);
    assert(irq_set_store(&p.dev, NULL, "enable", 6) == -ENODEV);
    assert(!jiiov_irq_request(&d)); assert(irq_set_store(&p.dev, NULL, "disable\n", 8) == 8);
    for (i = 0; i < ARRAY_SIZE(events); i++) {
        assert(netlink_event_store(&p.dev, NULL, events[i], strlen(events[i])) == 20);
        assert(last_event == i);
    }
    send_failure = -ECONNREFUSED;
    assert(netlink_event_store(&p.dev, NULL, "irq", 3) == -ECONNREFUSED); send_failure = 0;
    assert(netlink_event_store(&p.dev, NULL, "bad", 3) == -EINVAL);
    assert(resource_set_store(&p.dev, NULL, "release\n", 8) == 8);
    puts("PASS six sysfs stores, bounded prefix parsing, errors and explicit event bytes");
}

static void *power_on_thread(void *context)
{ struct file *f = context; int i; for (i = 0; i < 1000; i++) assert(!jiiov_ioctl(f, 0x610b, 0)); return NULL; }
static void test_fd_lifetime_and_serialization(void)
{
    struct jiiov_data d; struct platform_device p; struct inode inode; struct file f = {0}, denied = {0};
    pthread_t a, b; int i, irq_pos = -1, wake_pos = -1;
    setup(&d, &p); inode.i_cdev = &d.cdev;
    assert(!jiiov_open(&inode, &f)); assert(d.chardev.refs == 2);
    assert(!jiiov_ioctl(&f, 0x6101, 0));
    assert(!pthread_create(&a, NULL, power_on_thread, &f)); assert(!pthread_create(&b, NULL, power_on_thread, &f));
    assert(!pthread_join(a, NULL)); assert(!pthread_join(b, NULL)); assert(enable_count == 1);
    assert(!jiiov_ioctl(&f, 0x6105, 0)); jiiov_irq_thread(1040, &d);
    clear_trace(); assert(!jiiov_remove(&p));
    assert(d.dead && d.chardev.refs == 1 && !d.chardev.released && !p.dev.data);
    assert(!d.irq_work.pending && !requested_gpios && !power_refs);
    for (i = 0; i < njournal; i++) { if (journal[i] == 51) irq_pos = i; if (journal[i] == 53) wake_pos = i; }
    assert(irq_pos >= 0 && wake_pos > irq_pos);
    assert(jiiov_open(&inode, &denied) == -ENODEV && d.chardev.refs == 1);
    assert(jiiov_ioctl(&f, 0x6101, 0) == -ENODEV);
    assert(jiiov_release(&inode, &f) == 0 && d.chardev.released == 1);
    puts("PASS concurrent ioctl power idempotence, open/remove reference and dead-fd rejection");
}

static void test_netlink_bounds_and_exit(void)
{
    struct sk_buff input; struct nlmsghdr *n; int before, i;
    memset(&input, 0xa5, sizeof(input)); n = nlmsg_hdr(&input); input.len = 144;
    n->nlmsg_len = 144; n->nlmsg_type = 0; *(u8 *)nlmsg_data(n) = 0;
    before = send_count; jiiov_netlink_receive(&input);
    assert(send_count == before + 1 && last_event == 0);
    assert(last_destination == 100 && last_type == 30 && last_pid == 0 && last_length == 17);
    *(u8 *)nlmsg_data(n) = 7; jiiov_netlink_receive(&input); assert(last_event == 7 && send_count == before + 2);
    before = send_count;
    for (i = 0; i < 17; i++) { input.len = i; n->nlmsg_len = i; jiiov_netlink_receive(&input); }
    input.len = 144; n->nlmsg_len = 145; jiiov_netlink_receive(&input);
    n->nlmsg_len = UINT_MAX; jiiov_netlink_receive(&input);
    n->nlmsg_len = 16; jiiov_netlink_receive(&input);
    n->nlmsg_len = 144; *(u8 *)nlmsg_data(n) = 8; jiiov_netlink_receive(&input);
    pull_fail = 1; *(u8 *)nlmsg_data(n) = 7; jiiov_netlink_receive(&input); pull_fail = 0;
    assert(send_count == before);
    assert(jiiov_netlink_send(8) == -EINVAL);
    allocation_fail = 1; assert(jiiov_netlink_send(1) == -ENOMEM); allocation_fail = 0;
    header_fail = 1; assert(jiiov_netlink_send(1) == -EMSGSIZE); header_fail = 0;
    jiiov_nl_socket = NULL; assert(jiiov_netlink_send(1) == -ENOTCONN); jiiov_nl_socket = &fake_socket;
    puts("PASS netlink protocol/type30 port100, non-NUL 144-byte registration, exit7 and malformed bounds");
}

static struct device_node note_node(void)
{
    static struct device_node aggregate = { .compatible = "qcom,rpmh-vrm-regulator" };
    static struct device_node supplier = { .parent = &aggregate };
    struct device_node n = { .pmic = true, .supply = true,
        .volts = {3200000, 3200000, 150000}, .reset = 41, .irq = 40, .supplier = &supplier };
    return n;
}

static void test_dt_probe_failures_and_open_unbind(void)
{
    struct platform_device p = {0}; struct jiiov_data scratch = {0}, *d;
    struct device_node node = note_node(); struct inode inode; struct file f = {0}; int i;
    struct platform_device second = {0}; struct property property = { .name = "regulator-coupled-with" };
    p.dev.refs = 1; p.dev.of_node = &node; scratch.dev = &p.dev;
    assert(!jiiov_parse_dt(&scratch) && scratch.rst_gpio == 41 && scratch.irq_gpio == 40);
    node.supplier->properties = &property;
    assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP);
    property.name = "vin-supply"; assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP);
    node.supplier->properties = NULL; node.supplier->parent->properties = &property;
    property.name = "pm_humu_l14-parent-supply"; assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP);
    node.supplier->parent->properties = NULL;
    node.supplier->compatible = "other"; assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP);
    node.supplier->compatible = NULL;
    assert(!node.supplier->refs && !node.supplier->parent->refs);
    node.pmic = false; assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP); node = note_node();
    node.gpio = true; assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP); node = note_node();
    node.direct = true; assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP); node = note_node();
    node.boot = true; assert(jiiov_parse_dt(&scratch) == -EOPNOTSUPP); node = note_node();
    node.supply = false; assert(jiiov_parse_dt(&scratch) == -EINVAL); node = note_node();
    node.volts[1] = 1; assert(jiiov_parse_dt(&scratch) == -EINVAL); node = note_node();
    node.property_error = -EINVAL; assert(jiiov_parse_dt(&scratch) == -EINVAL); node = note_node();
    node.reset = -517; assert(jiiov_parse_dt(&scratch) == -517); node = note_node();
    node.irq = -517; assert(jiiov_parse_dt(&scratch) == -517); node = note_node();
    node.irq = 41; assert(jiiov_parse_dt(&scratch) == -EINVAL); node = note_node();
    for (i = 1; i <= 5; i++) {
        clear_trace(); probe_fault = i;
        assert(jiiov_probe(&p) < 0);
        assert(!live_allocations && !live_wakeups && p.dev.refs == 1 && !p.dev.data);
        assert(!requested_gpios && !power_refs && !irq_refs);
        if (i >= 4) assert(early_open_result == -ENODEV);
    }
    clear_trace(); probe_fault = 0; init_work_count = 0;
    assert(!jiiov_probe(&p)); d = p.dev.data;
    second.dev.refs = 1; second.dev.of_node = &node;
    assert(jiiov_probe(&second) == -EBUSY && live_allocations == 1 && jiiov_claimed.value == 1);
    assert(d && !d->dead && init_work_count == 1 && early_open_result == -ENODEV);
    assert(!requested_gpios && !power_refs && !irq_refs && live_wakeups == 1);
    inode.i_cdev = &d->cdev; assert(!jiiov_open(&inode, &f));
    assert(!jiiov_ioctl(&f, 0x6101, 0)); assert(!jiiov_ioctl(&f, 0x610b, 0));
    jiiov_shutdown(&p); assert(jiiov_ioctl(&f, 0x6101, 0) == -ENODEV);
    assert(!jiiov_remove(&p)); assert(live_allocations == 1 && !live_wakeups);
    assert(!jiiov_release(&inode, &f)); assert(!live_allocations && p.dev.refs == 1);
    puts("PASS Note DT validation/defer, five probe failures, off-by-default probe and final-fd object release");
}

static void test_module_registration_lifecycle(void)
{
    int i; jiiov_nl_socket = NULL;
    for (i = 1; i <= 4; i++) {
        clear_trace(); module_fault = i;
        assert(jiiov_init() < 0);
        assert(!jiiov_nl_socket && !live_regions && !live_classes && !live_drivers);
    }
    clear_trace(); module_fault = 0; assert(!jiiov_init());
    assert(journal[0] == 70 && journal[1] == 72 && journal[2] == 73 && journal[3] == 74);
    jiiov_exit();
    assert(journal[4] == 75 && journal[5] == 76 && journal[6] == 77 && journal[7] == 78);
    assert(!jiiov_nl_socket && !live_regions && !live_classes && !live_drivers);
    puts("PASS module init rollback and platform-before-netlink exit ordering");
}

static void test_forced_remove_wake_failure(void)
{
    struct device_node node = note_node(); struct platform_device p = {0}, second = {0};
    struct jiiov_data *d;
    clear_trace(); p.dev.refs = 1; p.dev.of_node = &node;
    assert(!jiiov_probe(&p)); d = p.dev.data;
    assert(!jiiov_resources_request(d)); assert(!jiiov_power(d, true));
    assert(!jiiov_irq_request(d)); jiiov_irq_thread(1040, d);
    clear_trace(); fail_at = 1;
    assert(!jiiov_remove(&p));
    assert(live_allocations == 1 && jiiov_claimed.value == 1 && module_refs == 1);
    assert(d->dead && !d->irq_work.pending && !irq_refs && !requested_gpios && !power_refs);
    assert(!d->wakeup && !live_wakeups && irq_wake_depth == 1 && d->irq_wake && d->irq == 1040);
    second.dev.refs = 1; second.dev.of_node = &node;
    assert(jiiov_probe(&second) == -EBUSY && irq_wake_depth == 1 && module_refs == 1);
    /* Exercise real wake-disable balancing only to dispose this host fixture.
     * The detached driver's public entry points do not provide recovery. */
    clear_trace(); assert(!jiiov_irq_free(d));
    assert(!irq_wake_depth && !module_refs && !d->irq_wake);
    put_device(&d->chardev); atomic_set(&jiiov_claimed, 0);
    puts("PASS wake-failed unbind retains IRQ identity/device/module/claim and rejects rebind");
}

static void test_forced_remove_power_failure_quarantine(void)
{
    struct device_node node = note_node(); struct platform_device p = {0}, second = {0};
    struct jiiov_data *d;
    clear_trace(); p.dev.refs = 1; p.dev.of_node = &node;
    assert(!jiiov_probe(&p)); d = p.dev.data;
    assert(!jiiov_resources_request(d)); assert(!jiiov_power(d, true));
    clear_trace(); fail_at = 2;
    assert(!jiiov_remove(&p));
    assert(d->dead && d->vdd && d->powered && !d->wakeup && !requested_gpios);
    assert(power_refs == 1 && regulator_refs == 1 && module_refs == 1);
    assert(jiiov_claimed.value == 1 && live_allocations == 1 && p.dev.refs == 2);
    second.dev.refs = 1; second.dev.of_node = &node;
    assert(jiiov_probe(&second) == -EBUSY);
    /* Simulate hardware recovery only to dispose the host fixture, not a driver entry point. */
    clear_trace(); assert(!jiiov_power(d, false)); put_device(&d->chardev); atomic_set(&jiiov_claimed, 0);
    assert(!module_refs && !live_allocations && !regulator_refs);
    puts("PASS failed remove keeps regulator/device/module/claim, prevents a second powered consumer");
}

int main(void)
{
    test_forced_remove_wake_failure();
    test_resource_rollback_and_reset(); test_power_errors_and_balance(); test_power_off_load_before_disable();
    test_irq_mask_work_and_teardown(); test_irq_wake_disable_failure_retry(); test_ioctl_exact_values_and_copies();
    test_sysfs_errors_and_explicit_events(); test_fd_lifetime_and_serialization();
    test_netlink_bounds_and_exit(); test_dt_probe_failures_and_open_unbind();
    test_module_registration_lifecycle();
    test_forced_remove_power_failure_quarantine(); return 0;
}
