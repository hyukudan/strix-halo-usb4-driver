/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by stream_cases.h after stream_detach_cases.h. open() of an
 * endpoint with no stream attached (0019). Before, it slept without bound
 * and only a signal ended it: a two-node run sat 27 min in
 * tbstream_dev_fops_open on endpoints of a domain that no longer existed.
 * Runs the actual open path; tb_xdomain_presence() is the core's answer. */

static double open_ms(struct file *f, int *ret)
{
    double t0 = ktime_get() / 1e6;

    *ret = tbstream_dev_fops_open(NULL, f);
    return ktime_get() / 1e6 - t0;
}

/* An endpoint whose stream went away with nothing open. */
static void no_stream(struct endpoint *e, int presence)
{
    endpoint_init(e, 0);
    unplug_and_remove(e, 0);
    assert(!e->dev.stream);
    atomic_store(&presence_result, presence);
    atomic_store(&presence_calls, 0);
}

static void assert_nothing_opened(struct endpoint *e)
{
    assert(!e->dev.users && !e->dev.tx_ring.ring && !tunnel_count());
    assert(!atomic_load(&dev_refs));
}

/* The endpoint's domain is gone (NHI unbound or wedged): -ENODEV at once,
 * blocking or not, whatever the timeout. */
static void open_fails_fast_without_domain(void)
{
    struct endpoint e;
    int ret;

    no_stream(&e, -ENODEV);
    e.dev.cfg_open_timeout_ms = 600000;
    assert(open_ms(&e.file, &ret) < 50 && ret == -ENODEV);
    e.file.f_flags = O_NONBLOCK;
    assert(open_ms(&e.file, &ret) < 50 && ret == -ENODEV);
    assert(atomic_load(&presence_calls) == 2);
    assert_nothing_opened(&e);
}

/* The domain is there but the XDomain is not (no cable, no peer, or it is
 * being unplugged): -ENXIO at once. */
static void open_fails_fast_without_xdomain(void)
{
    struct endpoint e;
    int ret;

    no_stream(&e, -ENXIO);
    e.dev.cfg_open_timeout_ms = 600000;
    assert(open_ms(&e.file, &ret) < 50 && ret == -ENXIO);
    assert_nothing_opened(&e);
}

/* The XDomain is there but its stream service not yet: non-blocking open
 * says try again. */
static void open_nonblock_eagain(void)
{
    struct endpoint e;
    int ret;

    no_stream(&e, 0);
    e.file.f_flags = O_NONBLOCK;
    assert(open_ms(&e.file, &ret) < 50 && ret == -EAGAIN);
    assert_nothing_opened(&e);
}

/* Blocking open waits for the stream service, but not past the timeout. */
static void open_wait_is_bounded(void)
{
    struct endpoint e;
    double spent;
    int ret;

    no_stream(&e, 0);
    e.dev.cfg_open_timeout_ms = 300;
    spent = open_ms(&e.file, &ret);
    assert(ret == -ETIMEDOUT && spent >= 290 && spent < 2000);
    assert(atomic_load(&presence_calls) >= 1);
    assert_nothing_opened(&e);
}

struct opener { struct endpoint *e; struct file *file; atomic_bool started; int ret; double spent; };
static void *opener_thread(void *data)
{
    struct opener *o = data;

    atomic_store(&o->started, true);
    o->spent = open_ms(o->file, &o->ret);
    return NULL;
}

static void start_opener(struct opener *o, pthread_t *thread)
{
    assert(!pthread_create(thread, NULL, opener_thread, o));
    while (!atomic_load(&o->started))
        sched_yield();
    usleep(100000);
}

/* The peer's stream service appears during the wait: open succeeds and
 * starts the rings on it. */
static void open_waits_for_peer(void)
{
    struct endpoint e;
    struct opener o = { .e = &e, .file = &e.file };
    pthread_t thread;

    no_stream(&e, 0);
    e.dev.cfg_open_timeout_ms = 10000;
    start_opener(&o, &thread);
    tbstream_group_attach_stream(&e.stream[1]);
    assert(!pthread_join(thread, NULL));
    assert(o.ret == 0 && o.spent < 5000);
    assert(e.dev.users == 1 && e.dev.tx_ring.ring && tunnel_count() == 1);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert_nothing_held();
    unplug_and_remove(&e, 1);
    assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
}

/* While it waits, the XDomain goes away (or its domain): the wait ends
 * within about a second, with the matching error; rmdir ends it too. */
static void open_wait_ends_when_xdomain_goes(void)
{
    int gone[] = { -ENXIO, -ENODEV, 0 };

    for (unsigned int i = 0; i < 3; ++i) {
        struct endpoint e;
        struct opener o = { .e = &e, .file = &e.file };
        pthread_t thread;

        no_stream(&e, 0);
        e.dev.cfg_open_timeout_ms = 30000;
        start_opener(&o, &thread);
        if (gone[i]) {
            atomic_store(&presence_result, gone[i]);
        } else {
            tbstream_dev_drop_item(&e.sg.group, &e.dev.group.cg_item);
            gone[i] = -ENXIO;
        }
        assert(!pthread_join(thread, NULL));
        assert(o.ret == gone[i] && o.spent < 2500);
        assert_nothing_opened(&e);
    }
}

/* A signal ends the wait at once. */
static void open_wait_is_interruptible(void)
{
    struct endpoint e;
    struct opener o = { .e = &e, .file = &e.file };
    pthread_t thread;

    no_stream(&e, 0);
    e.dev.cfg_open_timeout_ms = 30000;
    start_opener(&o, &thread);
    atomic_store(&interrupted, true);
    assert(!pthread_join(thread, NULL));
    atomic_store(&interrupted, false);
    assert(o.ret == -ERESTARTSYS && o.spent < 1000);
    assert_nothing_opened(&e);
}

/* -1 follows the module parameter (capped at 600 s); 0 does not wait. */
static void open_timeout_settings(void)
{
    struct endpoint e;
    double spent;
    int ret;

    no_stream(&e, 0);
    assert(e.dev.cfg_open_timeout_ms == -1 && tbstream_dev_open_timeout(&e.dev) == 30000);
    open_timeout_ms = 200;
    spent = open_ms(&e.file, &ret);
    assert(ret == -ETIMEDOUT && spent >= 190 && spent < 2000);
    open_timeout_ms = 0;
    assert(open_ms(&e.file, &ret) < 50 && ret == -ETIMEDOUT);
    open_timeout_ms = 4000000;
    assert(tbstream_dev_open_timeout(&e.dev) == 600000);
    open_timeout_ms = 30000;
    e.dev.cfg_open_timeout_ms = 0;
    assert(open_ms(&e.file, &ret) < 50 && ret == -ETIMEDOUT);
    e.dev.cfg_open_timeout_ms = 1234;
    assert(tbstream_dev_open_timeout(&e.dev) == 1234);
    assert_nothing_opened(&e);
}
