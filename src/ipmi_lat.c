/* SPDX-License-Identifier: Apache-2.0 */

/*
 * IPMI request latency probe -- RAM ring buffer + "ipmi_lat" shell command.
 *
 * See openbmc/zephyr-osal/include/ipmi_lat.h for the sample points and for why
 * nothing is logged on the hot path: one IPMI request already costs ~33 ms of
 * blocking UART output, so printk-based instrumentation measures itself.
 *
 * Usage on the target:
 *   ipmi_lat clear
 *   <run the ipmitool command on the host>
 *   ipmi_lat dump
 *   ipmi_lat top [ms]   (who owns the core over a window)
 *   ipmi_lat brk [ms]   (dbus-broker dispatch rounds / time in poll)
 *   ipmi_lat bench      (isolated cost of one malloc/RAND_bytes/...)
 *
 * Interpreting the segments of one LAN request:
 *   rx ->bus_req     net-ipmid parsing / session decryption
 *   bus_req->brk:in  client-side write + broker poll wakeup + socket read
 *   brk:in->brk:fwd  broker routing (name lookup, policy, queue to receiver)
 *   brk:fwd->ipmid   ipmid (asio) wakeup + read + D-Bus dispatch
 *   ipmid in->out    the IPMI handler itself (includes its own UART traces)
 *   ipmid->brk:in    ipmid-side write + broker poll wakeup + socket read
 *   brk:in->brk:fwd  broker routing of the method_return
 *   brk:fwd->bus_rsp net-ipmid wakeup + read + callback dispatch
 *   bus_rsp->tx      build the RMCP+ packet, encrypt, sendto
 *   tx->next rx      wire + client turnaround + UDP socket wakeup
 */

#include <ipmi_lat.h>

/*
 * Timing helpers shared with modules that cannot include <ipmi_lat.h>
 * (sdbusplus' connection.hpp, basu's sd-bus.c). Defined unconditionally and
 * kept free of CONFIG_OPENBMC_IPMI_LAT so those modules always link; the
 * numbers they produce are only printed when the probe is enabled.
 *
 * k_cycle_get_32() is a static inline in <zephyr/kernel.h>, so it has no symbol
 * to link against -- it must be reached through a function compiled here.
 */
#define IPMI_LAT_BUS_STEPS 6U

/* Order: 0 process_timeout, 1 dispatch_wqueue, 2 dispatch_track,
 *        3 dispatch_rqueue, 4 process_message,
 *        5 bus_socket_write_message (a sub-step of 1: the sendmsg retry loop,
 *          which sleeps up to 12.7 ms before giving up on EAGAIN). */
static atomic_t lat_bus_step_us[IPMI_LAT_BUS_STEPS];

/*
 * CPU spent inside sd_bus_process() by every service connection, in us.
 *
 * Accumulated with atomics: five threads touch it at once, and a plain 64-bit
 * counter on this 32-bit core tore often enough to report 13 s of CPU inside a
 * 13 s run -- more than the core has. A 32-bit word in us makes the add a
 * single atomic instruction and wraps only after ~71 min of accumulated CPU.
 */
static atomic_t lat_conn_proc_n;
static atomic_t lat_conn_proc_us;

uint32_t ipmi_lat_cyc(void)
{
	return k_cycle_get_32();
}

void ipmi_lat_conn_proc(uint32_t cycles)
{
	atomic_inc(&lat_conn_proc_n);
	atomic_add(&lat_conn_proc_us,
		   (atomic_val_t)k_cyc_to_us_floor32(cycles));
}

void ipmi_lat_bus_step(unsigned int step, uint32_t cycles)
{
	if (step < IPMI_LAT_BUS_STEPS) {
		atomic_add(&lat_bus_step_us[step],
			   (atomic_val_t)k_cyc_to_us_floor32(cycles));
	}
}

/*
 * One call per bus_socket_write_message() attempt, with the number of EAGAIN
 * retries it needed. That loop sleeps 100+200+...+6400 us before giving up, so
 * "wsend" in the dump can be syscall time or pure sleeping; the retry count is
 * what tells the two apart.
 */
static atomic_t lat_wr_calls;
static atomic_t lat_wr_eagain;

void ipmi_lat_write_retry(unsigned int retries)
{
	atomic_inc(&lat_wr_calls);

	if (retries != 0U) {
		atomic_add(&lat_wr_eagain, (atomic_val_t)retries);
	}
}

#ifdef CONFIG_OPENBMC_IPMI_LAT

#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>

#include <stdlib.h>
#include <string.h>

/* 128 samples is ~4x the request count of one "sdr list"; the ring keeps the
 * most recent ones, which is what matters after a single command. */
#define IPMI_LAT_SLOTS 128
/* Rows printed by "ipmi_lat dump" in the per-request table. */
#define IPMI_LAT_ROWS 8

struct ipmi_lat_slot {
	uint32_t ts[IPMI_LAT_NUM_PTS];
	uint32_t valid; /* >= IPMI_LAT_NUM_PTS bits */
	uint16_t cmdkey; /* netfn<<8 | cmd of the request this slot describes */
	uint32_t hcpu_us; /* CPU the ipmid thread burnt inside this handler */
	/* Cache rebuilds that happened while this handler ran: the difference
	 * between a handler that is slow on its own and one that paid for a
	 * bulk ObjectMapper fetch. */
	uint8_t nmap;
	uint8_t nsub;
};

struct ipmi_lat_seg {
	enum ipmi_lat_pt a;
	enum ipmi_lat_pt b; /* b == IPMI_LAT_XFER: ts[a] already holds a delta */
	const char *what;   /* column of the statistics table */
	const char *abbr;   /* column header of the per-request table, <= 10 chars */
};

/*
 * BRQ_SENT is deliberately absent from the chain.
 *
 * It used to split bus_req->brk:in into "our send" and "broker wake", but the
 * measurement cannot do that: dbus-broker runs at priority 0 and the sender at
 * priority 4, so the broker preempts between sendmsg() returning and the mark
 * running. It therefore always lands after brk:wout, and sent->brk:in came out
 * permanently negative (-4..-23 ms). The single merged segment below is exact
 * regardless of where the sender resumed; the preemption itself is reported
 * per row as "preempt" (ts[BRQ_SENT] - ts[BRQ_IN]).
 */
static const struct ipmi_lat_seg lat_segs[] = {
	{IPMI_LAT_RX, IPMI_LAT_BUS_REQ, "rx ->bus_req     parse/decrypt",
	 "rx>bus"},
	{IPMI_LAT_BUS_REQ, IPMI_LAT_BRQ_IN, "bus_req->brk:in  client write+broker wake",
	 "bus>brk"},
	{IPMI_LAT_BRQ_IN, IPMI_LAT_BRQ_FWD, "brk:in->brk:fwd  broker routing",
	 "in>fwd"},
	{IPMI_LAT_BRQ_FWD, IPMI_LAT_BRQ_WOUT, "brk:fwd->brk:wout broker write",
	 "fwd>wout"},
	{IPMI_LAT_BRQ_WOUT, IPMI_LAT_IPMID_IN, "brk:wout->ipmid  ipmid wake+read",
	 "wout>ipmid"},
	/* Same span as wout>ipmid, split where ipmid's thread entered
	 * sd_bus_process(): "wake" is the part it was not running yet, "dispatch"
	 * the part it spent inside the bus machinery before the handler. */
	{IPMI_LAT_BRQ_WOUT, IPMI_LAT_BUS_PROC, "brk:wout->proc   ipmid wake",
	 "wake"},
	{IPMI_LAT_BUS_PROC, IPMI_LAT_IPMID_IN, "proc->ipmid in   dispatch",
	 "dispatch"},
	{IPMI_LAT_IPMID_IN, IPMI_LAT_IPMID_OUT, "ipmid in->out    handler(+printk)",
	 "ipmid"},
	{IPMI_LAT_IPMID_OUT, IPMI_LAT_BRS_IN, "ipmid->brk:in    send+broker wake",
	 "ip>brk"},
	{IPMI_LAT_BRS_IN, IPMI_LAT_BRS_FWD, "brk:in->brk:fwd  broker routing",
	 "in>fwd"},
	{IPMI_LAT_BRS_FWD, IPMI_LAT_BUS_RSP, "brk:fwd->bus_rsp write+net wake",
	 "fwd>bs"},
	{IPMI_LAT_BUS_RSP, IPMI_LAT_RSP_BUILD, "bus_rsp->rsp_bld callback+dispatch",
	 "rsp>bld"},
	{IPMI_LAT_RSP_BUILD, IPMI_LAT_IV_BEG, "rsp_bld->iv_beg  marshal",
	 "bld>iv"},
	{IPMI_LAT_IV_BEG, IPMI_LAT_IV_END, "iv_beg->iv_end   RAND_bytes",
	 "rand"},
	{IPMI_LAT_IV_END, IPMI_LAT_HMAC_BEG,
	 "iv_end->hmac_beg EVP ctx+init+encrypt", "evp"},
	{IPMI_LAT_HMAC_BEG, IPMI_LAT_HMAC_END, "hmac_beg->hmac_end HMAC-SHA1",
	 "hmac"},
	{IPMI_LAT_HMAC_END, IPMI_LAT_RSP_WRITE, "hmac_end->rsp_wr flatten+vectors",
	 "flat"},
	{IPMI_LAT_RSP_WRITE, IPMI_LAT_TX, "rsp_wr->tx       sendto",
	 "wr>tx"},
	{IPMI_LAT_XFER, IPMI_LAT_XFER, "tx->next rx      wire+client+udp wake",
	 "wire"},
};

#define IPMI_LAT_NSEG ARRAY_SIZE(lat_segs)
/* Segments that show up as a column in the per-request table (XFER is only a
 * statistic, it belongs to the previous request). */
#define NSEG_REAL ((uint32_t)IPMI_LAT_NSEG - 1U)
/* Columns printed per line in the per-request table. */
#define LAT_COLS 6U

/* Points that must all be present for a sample to show up in the table. */
static const uint32_t lat_required =
	BIT(IPMI_LAT_RX) | BIT(IPMI_LAT_BUS_REQ) | BIT(IPMI_LAT_IPMID_IN) |
	BIT(IPMI_LAT_IPMID_OUT) | BIT(IPMI_LAT_BUS_RSP) | BIT(IPMI_LAT_TX) |
	BIT(IPMI_LAT_XFER);

/* The "basu send" counters (z_basu_wr_*) lived in basu's bus-socket.c. They
 * only made sense while that file returned 0 on EAGAIN and left the message in
 * wqueue; with the upstream exponential back-off restored the send path never
 * surfaces a blocked write, so the counters are gone together with it. */

/*
 * Bus read/watch loop counters, incremented by
 * sdbusplus/asio/connection.hpp in every service that owns a bus connection.
 *
 * They answer the one question the per-request table cannot: whether a thread
 * that shows up in "ipmi_lat top" with a large %cpu at idle is *doing* work or
 * just spinning on a readiness notification that keeps firing. Sample once
 * with no IPMI traffic in flight:
 *
 *   read-wake growing fast and no-work == read-wake  -> busy loop, nothing to
 *       read: the io_context never blocks and the core is being wasted
 *   read-wake flat                                   -> the thread is really
 *       working on something else
 */
volatile uint32_t z_lat_conn_rwake;
volatile uint32_t z_lat_conn_ridle;
volatile uint32_t z_lat_conn_warem;

/* CONFIG_SCHED_THREAD_USAGE is what actually defines the per-thread counters
 * (execution_cycles); it comes in with CONFIG_THREAD_RUNTIME_STATS. */
#if defined(CONFIG_SCHED_THREAD_USAGE)
/*
 * "Who ate the CPU?" profiler.
 *
 * The handoff segments (bus_req->sent, brk:wout->ipmid, ...) are 10-25ms wide
 * although none of the participating threads has more than a couple of ms of
 * work to do. Each of those windows is bracketed by two marks executed by
 * *different* threads, so the time in between is simply "somebody else ran".
 *
 * The first mark snapshots the per-thread scheduler runtime counters, the second
 * one diffs them and accumulates into a per-window table. The dump prints the
 * top consumers by thread name, which turns the guesswork into a list.
 *
 * Both the counters and the window width are hardware cycle counts (600 MHz
 * here). That only holds while THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS is off,
 * which is the default. The snapshot itself costs a few tens of us with
 * interrupts off and lands inside the window, so read the per-thread numbers as
 * "which thread was running", not as exact work estimates.
 */
#define LAT_PROF_WINS 5
#define LAT_PROF_MAX 48
#define LAT_PROF_ROWS 4

struct lat_prof_ent {
	const struct k_thread *th;
	uint64_t cyc;
};

struct lat_prof {
	struct lat_prof_ent snap[LAT_PROF_MAX];
	uint32_t n_snap;
	struct lat_prof_ent acc[LAT_PROF_MAX];
	uint32_t n_acc;
	uint32_t t0;      /* k_cycle_get_32() taken at the opening mark */
	uint64_t win_cyc; /* summed width of all completed windows */
	uint32_t win_cyc_max; /* widest single window: the rare 0.8 s outliers
			       * vanish into an average of 48 samples */
	uint32_t n_win;   /* completed windows */
	bool armed;
};

static const char *const lat_prof_what[LAT_PROF_WINS] = {
	"brk:wout -> proc",
	"proc -> ipmid in",
	"rx -> bus_req",
	"ipmid_out -> brk:in",
	"brk:fwd -> bus_rsp",
};

static struct lat_prof lat_prof[LAT_PROF_WINS];
static struct lat_prof_ent lat_prof_sorted[LAT_PROF_MAX];

static void lat_prof_snap_cb(const struct k_thread *th, void *ud)
{
	struct lat_prof *p = ud;
	k_thread_runtime_stats_t st;

	if (p->n_snap >= LAT_PROF_MAX ||
	    k_thread_runtime_stats_get((k_tid_t)th, &st) != 0) {
		return;
	}

	p->snap[p->n_snap].th = th;
	p->snap[p->n_snap].cyc = st.execution_cycles;
	p->n_snap++;
}

static void lat_prof_diff_cb(const struct k_thread *th, void *ud)
{
	struct lat_prof *p = ud;
	k_thread_runtime_stats_t st;
	uint32_t i;
	uint64_t delta;

	if (k_thread_runtime_stats_get((k_tid_t)th, &st) != 0) {
		return;
	}

	for (i = 0; i < p->n_snap; i++) {
		if (p->snap[i].th == th) {
			break;
		}
	}

	if (i == p->n_snap) {
		return;
	}

	delta = st.execution_cycles - p->snap[i].cyc;
	if (delta == 0U) {
		return;
	}

	for (i = 0; i < p->n_acc; i++) {
		if (p->acc[i].th == th) {
			p->acc[i].cyc += delta;
			return;
		}
	}

	if (p->n_acc < LAT_PROF_MAX) {
		p->acc[p->n_acc].th = th;
		p->acc[p->n_acc].cyc = delta;
		p->n_acc++;
	}
}

/*
 * What the profiler itself costs: every window boundary walks the whole thread
 * list twice (snapshot + diff, 40+ threads, scheduler lock held). The probe runs
 * inside the very windows it measures, so this has to be known: if a walk is
 * hundreds of us, part of the "ipmi_thread" time below is the probe, not the
 * code under test.
 */
static atomic_t lat_walk_n;
static atomic_t lat_walk_us;

static void lat_walk_add(uint32_t cycles)
{
	atomic_inc(&lat_walk_n);
	atomic_add(&lat_walk_us, (atomic_val_t)k_cyc_to_us_floor32(cycles));
}

static void lat_prof_begin(int win)
{
	struct lat_prof *p = &lat_prof[win];
	uint32_t w0 = k_cycle_get_32();

	p->n_snap = 0;
	p->armed = true;
	p->t0 = w0;
	k_thread_foreach(lat_prof_snap_cb, p);
	lat_walk_add(k_cycle_get_32() - w0);
}

static void lat_prof_end(int win)
{
	struct lat_prof *p = &lat_prof[win];
	uint32_t now;

	if (!p->armed) {
		return;
	}

	p->armed = false;
	now = k_cycle_get_32();
	p->win_cyc += (uint32_t)(now - p->t0);
	p->win_cyc_max = MAX(p->win_cyc_max, (uint32_t)(now - p->t0));
	p->n_win++;

	{
		uint32_t w0 = k_cycle_get_32();

		k_thread_foreach(lat_prof_diff_cb, p);
		lat_walk_add(k_cycle_get_32() - w0);
	}
}

/* Bracketed by the marks themselves, so no call sites outside this file.
 *
 * Every handoff of one request is covered, because "who burned the CPU in
 * this window" is the only thing that separates the two possible causes of a
 * 10-30 ms handoff: the receiver was not scheduled yet (a wakeup problem, the
 * window shows an idle core) or the receiver was running and busy (a CPU
 * problem, the window shows one thread owning it). Windows open and close in
 * different threads, so a window must not be opened twice in a row; a lost
 * closing mark just means the next opening overwrites the pending window.
 */
static void lat_prof_track(enum ipmi_lat_pt pt)
{
	switch (pt) {
	case IPMI_LAT_RX:
		lat_prof_begin(2);
		break;
	case IPMI_LAT_BUS_REQ:
		lat_prof_end(2);
		break;
	case IPMI_LAT_BRQ_SENT:
		break;
	case IPMI_LAT_BRQ_WOUT:
		lat_prof_begin(0);
		break;
	case IPMI_LAT_BUS_PROC:
		/* ipmid's thread entered sd_bus_process(): everything up to here
		 * was "not scheduled yet", everything after is dispatch cost. */
		lat_prof_end(0);
		lat_prof_begin(1);
		break;
	case IPMI_LAT_IPMID_IN:
		lat_prof_end(1);
		break;
	case IPMI_LAT_IPMID_OUT:
		lat_prof_begin(3);
		break;
	case IPMI_LAT_BRS_IN:
		lat_prof_end(3);
		break;
	case IPMI_LAT_BRS_FWD:
		lat_prof_begin(4);
		break;
	case IPMI_LAT_BUS_RSP:
		lat_prof_end(4);
		break;
	default:
		break;
	}
}

static void lat_prof_reset(void)
{
	memset(lat_prof, 0, sizeof(lat_prof));
}

/* Selection sort, n is tiny. Returns the sum of what it sorted. */
static uint64_t lat_sort_desc(struct lat_prof_ent *arr, uint32_t n)
{
	uint64_t sum = 0U;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t best = i;

		for (uint32_t j = i + 1U; j < n; j++) {
			if (arr[j].cyc > arr[best].cyc) {
				best = j;
			}
		}

		if (best != i) {
			struct lat_prof_ent tmp = arr[i];

			arr[i] = arr[best];
			arr[best] = tmp;
		}

		sum += arr[i].cyc;
	}

	return sum;
}

/*
 * Read without the scheduler lock: a torn 64-bit accumulate is acceptable for a
 * diagnostic, sorting a snapshot of it only has to be approximately right.
 */
static void lat_prof_dump(const struct shell *sh)
{
	int win;

	for (win = 0; win < LAT_PROF_WINS; win++) {
		struct lat_prof *p = &lat_prof[win];
		uint64_t sum;
		uint32_t n;

		if (p->n_win == 0U || p->win_cyc == 0U) {
			shell_print(sh, "  cpu %-16s no sample", lat_prof_what[win]);
			continue;
		}

		n = MIN(p->n_acc, (uint32_t)LAT_PROF_MAX);
		memcpy(lat_prof_sorted, p->acc, n * sizeof(lat_prof_sorted[0]));
		sum = lat_sort_desc(lat_prof_sorted, n);

		shell_print(sh, "  cpu %-16s n=%u  win avg %u us  win max %u us"
			    "  accounted %u%%",
			    lat_prof_what[win], p->n_win,
			    (uint32_t)k_cyc_to_us_floor64(p->win_cyc / p->n_win),
			    (uint32_t)k_cyc_to_us_floor64(p->win_cyc_max),
			    (uint32_t)((sum * 100U) / p->win_cyc));
		shell_print(sh, "    %-24s %10s %6s", "thread", "us/window", "%win");

		for (uint32_t i = 0; i < MIN(n, (uint32_t)LAT_PROF_ROWS); i++) {
			const char *name = k_thread_name_get(
				(k_tid_t)lat_prof_sorted[i].th);

			shell_print(sh, "    %-24s %10u %6u",
				    (name != NULL) ? name : "(unnamed)",
				    (uint32_t)k_cyc_to_us_floor64(
					    lat_prof_sorted[i].cyc / p->n_win),
				    (uint32_t)((lat_prof_sorted[i].cyc * 100U) /
					       p->win_cyc));
		}

		/*
		 * The kernel idle thread is a thread like any other in this table,
		 * so the rows alone cannot separate a busy core from a sleeping
		 * one. Print idle explicitly, it is the whole point of the
		 * profiler: a 10-30 ms handoff that is mostly idle means the
		 * receiver was not scheduled (wakeup latency), one that is fully
		 * accounted means the listed threads were really running (CPU in
		 * the dispatch path).
		 */
		uint64_t idle = 0U;

		for (uint32_t i = 0; i < p->n_acc; i++) {
			const char *name =
				k_thread_name_get((k_tid_t)p->acc[i].th);

			if (name != NULL && strcmp(name, "idle") == 0) {
				idle += p->acc[i].cyc;
			}
		}

		shell_print(sh, "    %-24s %10u %6u", "idle",
			    (uint32_t)k_cyc_to_us_floor64(idle / p->n_win),
			    (uint32_t)((idle * 100U) / p->win_cyc));
	}
}

/*
 * Cumulative CPU ranking over a sampling window: who is eating the core.
 *
 * The per-request profiler above only sees the handoff windows of one request
 * and reports the threads that happened to run in them, which answers "who
 * delayed this request" but not "is this the steady state". This one takes a
 * snapshot, waits
 * (so the shell thread is out of the way), snapshots again and prints the
 * delta: it is the same numbers as "kernel threads" without the 5 lines per
 * thread and without having to subtract two dumps by hand.
 *
 * Sampling at idle is the point: if the broker still owns half the core with no
 * IPMI traffic in flight, that is a broker-side spin, not message volume.
 */
#define LAT_TOP_ROWS 10

static int lat_cmd_top(const struct shell *sh, size_t argc, char **argv)
{
	static struct lat_prof p; /* too big for the shell stack */
	uint32_t ms = 2000U;
	uint32_t win;
	uint32_t n;
	uint64_t sum;

	if (argc > 1U) {
		unsigned long v = strtoul(argv[1], NULL, 0);

		/* k_cycle_get_32() wraps after ~7 s at 600 MHz. */
		if (v < 50UL || v > 6000UL) {
			shell_error(sh, "ms must be 50..6000");
			return -EINVAL;
		}

		ms = (uint32_t)v;
	}

	p.n_snap = 0U;
	p.n_acc = 0U;
	memset(p.acc, 0, sizeof(p.acc));

	shell_print(sh, "ipmi_lat top: sampling %u ms...", ms);

	win = k_cycle_get_32();
	k_thread_foreach(lat_prof_snap_cb, &p);
	k_sleep(K_MSEC(ms));
	win = (uint32_t)(k_cycle_get_32() - win);
	k_thread_foreach(lat_prof_diff_cb, &p);

	if (win == 0U) {
		shell_error(sh, "no window");
		return -EIO;
	}

	n = MIN(p.n_acc, (uint32_t)LAT_PROF_MAX);
	memcpy(lat_prof_sorted, p.acc, n * sizeof(lat_prof_sorted[0]));
	sum = lat_sort_desc(lat_prof_sorted, n);

	shell_print(sh, "  window %u ms  %u threads ran  accounted %u%%", ms, n,
		    (uint32_t)((sum * 100U) / win));
	shell_print(sh, "  %-24s %10s %6s", "thread", "cpu_us", "%cpu");

	for (uint32_t i = 0; i < MIN(n, (uint32_t)LAT_TOP_ROWS); i++) {
		const char *name = k_thread_name_get((k_tid_t)lat_prof_sorted[i].th);

		shell_print(sh, "  %-24s %10u %6u",
			    (name != NULL) ? name : "(unnamed)",
			    (uint32_t)k_cyc_to_us_floor64(lat_prof_sorted[i].cyc),
			    (uint32_t)((lat_prof_sorted[i].cyc * 100U) / win));
	}

	return 0;
}
#else  /* !CONFIG_SCHED_THREAD_USAGE */
static inline void lat_prof_track(enum ipmi_lat_pt pt)
{
	ARG_UNUSED(pt);
}

static inline void lat_prof_reset(void) { }

static inline void lat_prof_dump(const struct shell *sh)
{
	ARG_UNUSED(sh);
}

static int lat_cmd_top(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_error(sh, "CONFIG_SCHED_THREAD_USAGE is off");
	return -ENOTSUP;
}
#endif /* CONFIG_SCHED_THREAD_USAGE */

static struct ipmi_lat_slot lat_slot[IPMI_LAT_SLOTS];
static struct k_spinlock lat_lock;
static uint32_t lat_cur;      /* slot being filled (ring index) */
static uint32_t lat_requests; /* RX marks seen since the last clear */
static bool lat_active;       /* at least one slot has been opened */
static uint16_t lat_pending_cmd; /* set right before IPMI_LAT_IPMID_IN */

/*
 * Counters that say whether one slot really describes one request.
 *
 * lat_pt_n[pt] counts every mark written, so the dump can be compared against
 * the number of LAN requests: an execute() from another client (the KCS bridge
 * sends the very same xyz.openbmc_project.Ipmi.Server.execute) inflates the
 * broker-side counts without adding an RX.
 *
 * lat_pt_ooo[pt] counts marks that were written into a slot which already held
 * a *later* point of the chain with a bigger timestamp. That cannot happen for
 * a single request and is the direct fingerprint of two requests sharing a slot.
 */
static uint32_t lat_pt_n[IPMI_LAT_NUM_PTS];
static uint32_t lat_pt_ooo[IPMI_LAT_NUM_PTS];

/*
 * CPU actually burnt inside executeIpmiCommand(), in cycles, sampled from the
 * current thread's runtime stats at IPMID_IN / IPMID_OUT.
 *
 * This is the number that separates the two very different readings of a long
 * "ipmid in->out" segment: the handler computing (cycles ~= wall time) versus
 * the handler waiting for D-Bus (cycles << wall time). Without it a 1.9 s
 * handler sample cannot be attributed at all.
 */
#if defined(CONFIG_SCHED_THREAD_USAGE)
static uint32_t lat_hcpu_in;
/* Rebuild counters as they were when the current handler started. */
static uint32_t lat_map_n_in;
static uint32_t lat_sub_n_in;
static bool lat_hcpu_have_in;
static uint32_t lat_hcpu_n;
static uint64_t lat_hcpu_sum_cyc;
static uint32_t lat_hcpu_min_cyc = UINT32_MAX;
static uint32_t lat_hcpu_max_cyc;
static uint32_t lat_hcpu_nested; /* IPMI_IN seen while one was still open */
#endif

/* getSensorMap() cache rebuilds (one full GetManagedObjects per rebuild). */
static uint32_t lat_map_n;
static uint64_t lat_map_sum_us;
static uint32_t lat_map_max_us;
static uint32_t lat_map_last_us;
static uint32_t lat_map_last_objs;

/* getSensorSubtree() rebuilds (signal invalidated it, next command redoes it). */
static uint32_t lat_sub_n;
static uint64_t lat_sub_sum_us;
static uint32_t lat_sub_max_us;

/* Short names, one per point, in chain order (XFER is derived, not a mark). */
static const char *const lat_pt_name[IPMI_LAT_NUM_PTS] = {
	"rx", "bus_req", "sent", "brq_in", "fwd", "wout",
	"bus_proc", "ipmid_in", "ipmid_out",
	"brs_in", "brs_fwd", "bus_rsp",
	"rsp_build", "iv_beg", "iv_end", "hmac_beg", "hmac_end",
	"rsp_write", "tx", "xfer",
};

/*
 * The handful of requests that carry most of a run's wall time. Kept in the
 * dump as well as printed at the moment it happens, because a console line is
 * easy to lose among everything else that scrolls past during "sdr list".
 */
#define LAT_SLOW_TOP 3U
struct lat_slow {
	uint16_t cmd;
	uint8_t used;
	uint8_t nmap; /* getSensorMap rebuilds during the handler */
	uint8_t nsub; /* getSensorSubtree rebuilds during the handler */
	uint32_t wall_us;
	uint32_t cpu_us;
};
static struct lat_slow lat_slow_top[LAT_SLOW_TOP];

/* Handler wall time above which ipmi_lat_mark() prints the request at once. */
#define LAT_SLOW_MS 100U
static const uint32_t lat_slow_ticks =
	(CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000U) * LAT_SLOW_MS;

static inline uint32_t lat_now(void)
{
	return (uint32_t)k_uptime_ticks();
}

static inline uint32_t lat_us(uint32_t ticks)
{
	return (uint32_t)(((uint64_t)ticks * 1000000ULL) /
			  (uint64_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
}

void ipmi_lat_mark(enum ipmi_lat_pt pt)
{
	k_spinlock_key_t key;
	bool have_prev;
	uint32_t prev_tx;
	uint32_t now;
	/* A handler slower than this is printed on the spot: a handful of such
	 * requests carry most of the wall time of a "sdr list" run, and one line
	 * per outlier is far cheaper than a build cycle spent guessing which one
	 * it was. */
	bool slow = false;
	uint32_t slow_wall = 0U;
	uint32_t slow_cpu = 0U;
	uint16_t slow_cmd = 0U;
#if defined(CONFIG_SCHED_THREAD_USAGE)
	uint32_t cyc = 0U;
	bool have_cyc = false;
	k_thread_runtime_stats_t st;
#endif

	if ((unsigned int)pt >= (unsigned int)IPMI_LAT_NUM_PTS) {
		return;
	}

	/*
	 * Sampled outside lat_lock: the stats call takes the scheduler's own
	 * lock, and lat_prof_* already calls into it, so keep the two apart.
	 */
#if defined(CONFIG_SCHED_THREAD_USAGE)
	if (k_thread_runtime_stats_get(k_current_get(), &st) == 0) {
		cyc = (uint32_t)st.execution_cycles;
		have_cyc = true;
	}
#endif

	key = k_spin_lock(&lat_lock);
	now = lat_now();

	if (pt == IPMI_LAT_RX) {
		/* A new request opens a new slot; tx->rx of the previous one is
		 * folded into the derived XFER field here, because once the slot
		 * is overwritten its TX is gone. */
		have_prev = lat_active &&
			    (lat_slot[lat_cur].valid & BIT(IPMI_LAT_TX)) != 0;
		prev_tx = have_prev ? lat_slot[lat_cur].ts[IPMI_LAT_TX] : 0U;

		lat_cur = (lat_cur + 1U) % IPMI_LAT_SLOTS;
		memset(&lat_slot[lat_cur], 0, sizeof(lat_slot[lat_cur]));
		lat_active = true;
		lat_requests++;

		if (have_prev) {
			lat_slot[lat_cur].ts[IPMI_LAT_XFER] = now - prev_tx;
			lat_slot[lat_cur].valid = BIT(IPMI_LAT_XFER);
		}
	}

	if (lat_active) {
		int later;

		lat_pt_n[pt]++;

		/* Every mark of the chain happens after the ones before it, so a
		 * *later* point being already present means the chain was fed out
		 * of order. For one request that is impossible unless this mark
		 * was taken before an earlier one, i.e. the writer was preempted
		 * (BRQ_SENT) or the slot holds two requests (broker marks).
		 * Reported as "out of order" in the dump. */
		for (later = (int)pt + 1; later < (int)IPMI_LAT_XFER; later++) {
			if ((lat_slot[lat_cur].valid & BIT(later)) != 0) {
				lat_pt_ooo[pt]++;
				break;
			}
		}

		/*
		 * bus_proc fires once per message ipmid dispatches (several per
		 * request: the request itself, then the replies to the calls its
		 * handler makes). Only the first one belongs to this request's
		 * handoff, so keep it and ignore the rest; overwriting would make
		 * "wake" point past ipmid_in and produce negative segments.
		 */
		if (pt != IPMI_LAT_BUS_PROC ||
		    (lat_slot[lat_cur].valid & BIT(IPMI_LAT_BUS_PROC)) == 0U) {
			lat_slot[lat_cur].ts[pt] = now;
			lat_slot[lat_cur].valid |= BIT(pt);
		}

		if (pt == IPMI_LAT_IPMID_IN) {
			lat_slot[lat_cur].cmdkey = lat_pending_cmd;
			lat_map_n_in = lat_map_n;
			lat_sub_n_in = lat_sub_n;
		} else if (pt == IPMI_LAT_IPMID_OUT) {
			lat_slot[lat_cur].nmap = (uint8_t)MIN(lat_map_n - lat_map_n_in,
							     255U);
			lat_slot[lat_cur].nsub = (uint8_t)MIN(lat_sub_n - lat_sub_n_in,
							     255U);
		}

#if defined(CONFIG_SCHED_THREAD_USAGE)
		/* Bracket executeIpmiCommand() on the critical path of the
		 * probe: two stats reads per request, a few us. */
		if (pt == IPMI_LAT_IPMID_IN) {
			/* A second IN before the previous OUT means the handler
			 * re-entered ipmid (a blocking sd_bus_call inside a
			 * handler drives the bus again and can dispatch the next
			 * request). That makes every "handler cpu" pair below
			 * describe two requests, so count it instead of pretending
			 * the number is clean. */
			if (lat_hcpu_have_in && have_cyc) {
				lat_hcpu_nested++;
			}
			lat_hcpu_in = cyc;
			lat_hcpu_have_in = have_cyc;
		} else if (pt == IPMI_LAT_IPMID_OUT &&
			   lat_hcpu_have_in && have_cyc) {
			uint32_t d = cyc - lat_hcpu_in;

			lat_hcpu_have_in = false;
			lat_hcpu_n++;
			lat_hcpu_sum_cyc += d;
			lat_hcpu_min_cyc = MIN(lat_hcpu_min_cyc, d);
			lat_hcpu_max_cyc = MAX(lat_hcpu_max_cyc, d);
			lat_slot[lat_cur].hcpu_us = k_cyc_to_us_floor32(d);
		}
#endif
		/* Outlier detection for the handler segment. */
		if (pt == IPMI_LAT_IPMID_OUT &&
		    (lat_slot[lat_cur].valid & BIT(IPMI_LAT_IPMID_IN)) != 0U) {
			uint32_t wall =
				now - lat_slot[lat_cur].ts[IPMI_LAT_IPMID_IN];

			if (wall >= lat_slow_ticks) {
				uint32_t i;

				slow = true;
				slow_wall = lat_us(wall);
				slow_cpu = lat_slot[lat_cur].hcpu_us;
				slow_cmd = lat_slot[lat_cur].cmdkey;

				/* Keep the widest three, sorted, for the dump. */
				for (i = 0U; i < LAT_SLOW_TOP; i++) {
					if (!lat_slow_top[i].used ||
					    slow_wall > lat_slow_top[i].wall_us) {
						break;
					}
				}

				if (i < LAT_SLOW_TOP) {
					uint32_t j = LAT_SLOW_TOP - 1U;

					for (; j > i; j--) {
						lat_slow_top[j] = lat_slow_top[j - 1U];
					}

					lat_slow_top[i].used = 1U;
					lat_slow_top[i].cmd = slow_cmd;
					lat_slow_top[i].wall_us = slow_wall;
					lat_slow_top[i].cpu_us = slow_cpu;
					lat_slow_top[i].nmap =
						lat_slot[lat_cur].nmap;
					lat_slow_top[i].nsub =
						lat_slot[lat_cur].nsub;
				}
			}
		}
	}

	k_spin_unlock(&lat_lock, key);

	if (slow) {
		printk("[lat] slow handler nf=%02x cmd=%02x wall=%u ms"
		       " cpu=%u ms map=%u sub=%u\n",
		       (unsigned int)(slow_cmd >> 8),
		       (unsigned int)(slow_cmd & 0xffU), slow_wall / 1000U,
		       slow_cpu / 1000U,
		       (unsigned int)lat_slot[lat_cur].nmap,
		       (unsigned int)lat_slot[lat_cur].nsub);
	}

	/* Outside the lock: the profiler walks every thread and takes the
	 * scheduler's own lock twice, far too long to hold lat_lock. */
	lat_prof_track(pt);
}

/* Wrapper for sdbusplus::asio::connection, which cannot see the enum. */
void ipmi_lat_mark_proc_in(void)
{
	ipmi_lat_mark(IPMI_LAT_BUS_PROC);
}

/* Called from dbus-broker and basu (single image, so a plain call is enough).
 *
 * Note: nothing calls ipmi_lat_brq_sent() any more. It only existed while
 * basu's send path returned 0 on EAGAIN; with the upstream back-off restored
 * the call is always complete inside sd_bus_send(), so "sent" is just
 * "bus_req plus the send". The mark is kept so that the dump layout does not
 * change; it reports 0 and the "preempt" column shows -1. */
void ipmi_lat_brq_sent(void)
{
	ipmi_lat_mark(IPMI_LAT_BRQ_SENT);
}

void ipmi_lat_brq_wout(void)
{
	ipmi_lat_mark(IPMI_LAT_BRQ_WOUT);
}

void ipmi_lat_brq_in(void)
{
	ipmi_lat_mark(IPMI_LAT_BRQ_IN);
}

void ipmi_lat_brq_fwd(void)
{
	ipmi_lat_mark(IPMI_LAT_BRQ_FWD);
}

void ipmi_lat_brs_in(void)
{
	ipmi_lat_mark(IPMI_LAT_BRS_IN);
}

void ipmi_lat_brs_fwd(void)
{
	ipmi_lat_mark(IPMI_LAT_BRS_FWD);
}

void ipmi_lat_set_cmd(uint8_t netfn, uint8_t cmd)
{
	lat_pending_cmd = (uint16_t)(((uint16_t)netfn << 8) | cmd);
}

void ipmi_lat_map_rebuild(uint32_t us, uint32_t objects)
{
	k_spinlock_key_t key = k_spin_lock(&lat_lock);

	lat_map_n++;
	lat_map_sum_us += us;
	lat_map_max_us = MAX(lat_map_max_us, us);
	lat_map_last_us = us;
	lat_map_last_objs = objects;

	k_spin_unlock(&lat_lock, key);
}

void ipmi_lat_subtree_rebuild(uint32_t us)
{
	k_spinlock_key_t key = k_spin_lock(&lat_lock);

	lat_sub_n++;
	lat_sub_sum_us += us;
	lat_sub_max_us = MAX(lat_sub_max_us, us);

	k_spin_unlock(&lat_lock, key);
}

static uint32_t lat_seg_mask(const struct ipmi_lat_seg *s)
{
	return (s->a == s->b) ? BIT(s->a) : (BIT(s->a) | BIT(s->b));
}

static uint32_t lat_seg_value(const struct ipmi_lat_slot *sl,
			      const struct ipmi_lat_seg *s)
{
	return (s->a == s->b) ? sl->ts[s->a] : (sl->ts[s->b] - sl->ts[s->a]);
}

/* Signed on purpose: when the two marks of a segment land in the wrong order the
 * unsigned difference wraps, and "-22400" is a far more useful thing to see than
 * "4294944896". */
static inline int32_t lat_us_neg(uint32_t ticks)
{
	return (int32_t)(int64_t)(((int64_t)(int32_t)ticks * 1000000LL) /
				  (int64_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
}

/* One fixed-width cell, or "--" when the marks for that segment are missing.
 * Printing 0 for a missing pair used to make the row's segments silently fail to
 * add up to rx>tx, which read like a latency mystery of its own. */
static size_t lat_cell(char *line, size_t off, size_t cap, bool have, int32_t v)
{
	int n;

	if (off >= cap) {
		return off;
	}

	n = (have) ? snprintk(line + off, cap - off, "%11d", v)
		   : snprintk(line + off, cap - off, "%11s", "--");

	return (n > 0) ? (off + (size_t)n) : off;
}

static void lat_dump_counts(const struct shell *sh, const char *tag,
			    const uint32_t *vals, bool hide_zero)
{
	char line[160];
	size_t off;
	size_t i;
	bool any = false;

	off = (size_t)snprintk(line, sizeof(line), "  %-12s", tag);

	for (i = 0; i < (size_t)IPMI_LAT_XFER; i++) {
		int n;

		if (hide_zero && vals[i] == 0U) {
			continue;
		}

		if (off + 24U >= sizeof(line)) {
			break;
		}

		n = snprintk(line + off, sizeof(line) - off, " %s=%u",
			     lat_pt_name[i], vals[i]);
		if (n > 0) {
			off += (size_t)n;
			any = true;
		}
	}

	if (!any) {
		snprintk(line + off, sizeof(line) - off, " none");
	}

	shell_print(sh, "%s", line);
}

static int lat_cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	struct {
		int32_t seg[IPMI_LAT_NSEG];
		bool have[IPMI_LAT_NSEG];
		uint32_t total_us;
		uint32_t miss; /* segments whose two marks are not both present */
		int ord;       /* first segment arriving out of chain order, or -1 */
		int32_t preempt; /* brk:in -> sent: how long the writer lost the CPU */
		uint16_t cmdkey; /* netfn<<8 | cmd, names the expensive rows */
		uint32_t hcpu_us; /* CPU the ipmid thread burnt in this handler */
	} rows[IPMI_LAT_ROWS];
	uint32_t cnt[IPMI_LAT_NSEG];
	int64_t sum_us[IPMI_LAT_NSEG];
	int32_t min_us[IPMI_LAT_NSEG], max_us[IPMI_LAT_NSEG];
	uint32_t pt_n[IPMI_LAT_NUM_PTS], pt_ooo[IPMI_LAT_NUM_PTS];
	uint32_t requests;
	uint32_t map_n = 0U, map_avg = 0U, map_max = 0U, map_last = 0U;
	uint32_t map_objs = 0U;
	uint32_t sub_n = 0U, sub_avg = 0U, sub_max = 0U;
#if defined(CONFIG_SCHED_THREAD_USAGE)
	uint32_t hcpu_n = 0U, hcpu_avg = 0U, hcpu_max = 0U;
	uint32_t hcpu_min = 0U, hcpu_nested = 0U;
#endif
	uint32_t nrows = 0;
	uint32_t idx;
	uint32_t line;
	k_spinlock_key_t key;
	size_t s;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (s = 0; s < IPMI_LAT_NSEG; s++) {
		cnt[s] = 0;
		sum_us[s] = 0;
		min_us[s] = INT32_MAX;
		max_us[s] = INT32_MIN;
	}

	key = k_spin_lock(&lat_lock);

	requests = lat_requests;
	memcpy(pt_n, lat_pt_n, sizeof(pt_n));
	memcpy(pt_ooo, lat_pt_ooo, sizeof(pt_ooo));

	map_n = lat_map_n;
	map_last = lat_map_last_us;
	map_max = lat_map_max_us;
	map_objs = lat_map_last_objs;
	if (map_n != 0U) {
		map_avg = (uint32_t)(lat_map_sum_us / map_n);
	}

	sub_n = lat_sub_n;
	sub_max = lat_sub_max_us;
	if (sub_n != 0U) {
		sub_avg = (uint32_t)(lat_sub_sum_us / sub_n);
	}
#if defined(CONFIG_SCHED_THREAD_USAGE)
	hcpu_n = lat_hcpu_n;
	hcpu_nested = lat_hcpu_nested;
	hcpu_max = k_cyc_to_us_floor32(lat_hcpu_max_cyc);
	if (hcpu_n != 0U) {
		hcpu_avg = (uint32_t)k_cyc_to_us_floor64(lat_hcpu_sum_cyc / hcpu_n);
	}
	if (lat_hcpu_min_cyc != UINT32_MAX) {
		hcpu_min = k_cyc_to_us_floor32(lat_hcpu_min_cyc);
	}
#endif

	for (s = 0; s < IPMI_LAT_NSEG; s++) {
		const struct ipmi_lat_seg *seg = &lat_segs[s];
		uint32_t mask = lat_seg_mask(seg);

		for (idx = 0; idx < IPMI_LAT_SLOTS; idx++) {
			int32_t us;

			if ((lat_slot[idx].valid & mask) != mask) {
				continue;
			}

			us = lat_us_neg(lat_seg_value(&lat_slot[idx], seg));
			cnt[s]++;
			sum_us[s] += us;
			if (us < min_us[s]) {
				min_us[s] = us;
			}
			if (us > max_us[s]) {
				max_us[s] = us;
			}
		}
	}

	/* Collect the most recent fully sampled requests, newest first. */
	for (idx = 0; idx < IPMI_LAT_SLOTS && nrows < IPMI_LAT_ROWS; idx++) {
		uint32_t slot = (lat_cur + IPMI_LAT_SLOTS - idx) % IPMI_LAT_SLOTS;
		const struct ipmi_lat_slot *sl = &lat_slot[slot];

		/* The broker-side marks are optional: a sample that missed them
		 * still shows the original segments (those columns print "--"),
		 * so a filter mismatch in dbus-broker stays visible instead of
		 * hiding every row. */
		if ((sl->valid & lat_required) != lat_required) {
			continue;
		}

		rows[nrows].miss = 0;
		rows[nrows].ord = -1;
		rows[nrows].cmdkey = sl->cmdkey;
		rows[nrows].hcpu_us = sl->hcpu_us;

		for (s = 0; s < IPMI_LAT_NSEG; s++) {
			uint32_t mask = lat_seg_mask(&lat_segs[s]);
			bool have = (sl->valid & mask) == mask;

			rows[nrows].have[s] = have;
			rows[nrows].seg[s] = 0;

			if (!have) {
				rows[nrows].miss++;
				continue;
			}

			rows[nrows].seg[s] =
				lat_us_neg(lat_seg_value(sl, &lat_segs[s]));

			/* XFER is a duration, not a difference of two marks. */
			if (s < IPMI_LAT_NSEG - 1U && rows[nrows].seg[s] < 0 &&
			    rows[nrows].ord < 0) {
				rows[nrows].ord = (int)s;
			}
		}

		/* rx -> tx, i.e. everything except the wire/client segment. */
		rows[nrows].total_us = lat_us(sl->ts[IPMI_LAT_TX] -
					      sl->ts[IPMI_LAT_RX]);

		/* How long the writer stayed off-CPU between the bytes reaching
		 * the pipe and it getting scheduled again to run the mark. */
		if ((sl->valid & (BIT(IPMI_LAT_BRQ_IN) | BIT(IPMI_LAT_BRQ_SENT))) ==
		    (BIT(IPMI_LAT_BRQ_IN) | BIT(IPMI_LAT_BRQ_SENT))) {
			rows[nrows].preempt =
				lat_us_neg(sl->ts[IPMI_LAT_BRQ_SENT] -
					   sl->ts[IPMI_LAT_BRQ_IN]);
		} else {
			rows[nrows].preempt = -1;
		}
		nrows++;
	}

	k_spin_unlock(&lat_lock, key);

	shell_print(sh, "ipmi_lat: requests=%u  ticks/s=%d  (all values in us)",
		    requests, CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	shell_print(sh, "  bus io: read-wake=%u  no-work=%u  write-arm=%u",
		    z_lat_conn_rwake, z_lat_conn_ridle, z_lat_conn_warem);
	{
		uint32_t procN = (uint32_t)atomic_get(&lat_conn_proc_n);
		uint32_t procUs = (uint32_t)atomic_get(&lat_conn_proc_us);

		shell_print(sh, "  bus proc: calls=%u  avg=%u us  total=%u us",
			    procN, procN == 0U ? 0U : procUs / procN, procUs);
		shell_print(sh, "  bus write: calls=%u eagain=%u (each retry sleeps"
			    " 100..6400 us)",
			    (uint32_t)atomic_get(&lat_wr_calls),
			    (uint32_t)atomic_get(&lat_wr_eagain));
		shell_print(sh, "  bus steps(us): timeout=%u wqueue=%u track=%u"
			    " rqueue=%u message=%u wsend=%u",
			    (uint32_t)atomic_get(&lat_bus_step_us[0]),
			    (uint32_t)atomic_get(&lat_bus_step_us[1]),
			    (uint32_t)atomic_get(&lat_bus_step_us[2]),
			    (uint32_t)atomic_get(&lat_bus_step_us[3]),
			    (uint32_t)atomic_get(&lat_bus_step_us[4]),
			    (uint32_t)atomic_get(&lat_bus_step_us[5]));
	}
	shell_print(sh, "  getSensorMap: rebuilds=%u  avg=%u us  max=%u us"
		    "  last=%u us  last objects=%u",
		    map_n, map_avg, map_max, map_last, map_objs);
	/* Non-zero means a signal from /xyz/openbmc_project/sensors invalidated
	 * the tree and the next command paid for the whole rebuild. */
	shell_print(sh, "  getSensorSubtree: rebuilds=%u  avg=%u us  max=%u us",
		    sub_n, sub_avg, sub_max);
#if defined(CONFIG_SCHED_THREAD_USAGE)
	/* Cycles really spent inside executeIpmiCommand(), i.e. computing.
	 * Compare with the "ipmid in->out" wall time: equal means the handler
	 * is the cost, far smaller means it was waiting on D-Bus. */
	/* nested>0 means a handler re-entered ipmid (blocking sd_bus_call inside
	 * a handler drivable back into the bus): every pair below then describes
	 * more than one request, so treat min/avg/max as an upper bound. */
	shell_print(sh, "  handler cpu: n=%u  min=%u us  avg=%u us  max=%u us"
		    "  nested=%u",
		    hcpu_n, hcpu_min, hcpu_avg, hcpu_max, hcpu_nested);
	{
	uint32_t wn = (uint32_t)atomic_get(&lat_walk_n);
	uint32_t wus = (uint32_t)atomic_get(&lat_walk_us);

	/* Cost of the probe itself (thread-list walks at every window
	 * boundary). Subtract roughly avg*2 per request from the
	 * per-window numbers before believing them. */
	shell_print(sh, "  prof walk: n=%u  avg=%u us  total=%u us",
		    wn, wn == 0U ? 0U : wus / wn, wus);
	}
	#endif
	/* Widest handlers of the run, the ones that hide in the average. */
	for (uint32_t i = 0U; i < LAT_SLOW_TOP; i++) {
	if (lat_slow_top[i].used == 0U) {
		continue;
	}

	shell_print(sh,
		    "  slow #%u: nf=%02x cmd=%02x wall=%u us cpu=%u us"
		    " map=%u sub=%u",
		    i + 1U, (unsigned int)(lat_slow_top[i].cmd >> 8),
		    (unsigned int)(lat_slow_top[i].cmd & 0xffU),
		    lat_slow_top[i].wall_us, lat_slow_top[i].cpu_us,
		    (unsigned int)lat_slow_top[i].nmap,
		    (unsigned int)lat_slow_top[i].nsub);
	}
	/* More marks than requests, or any out-of-order mark, means a slot is
	 * being fed by more than one request stream. */
	lat_dump_counts(sh, "marks", pt_n, false);
	lat_dump_counts(sh, "out of order", pt_ooo, true);
	shell_print(sh, "  %-38s %5s %8s %8s %8s", "segment", "n", "min", "avg",
		    "max");

	for (s = 0; s < IPMI_LAT_NSEG; s++) {
		int32_t avg = cnt[s] ? (int32_t)(sum_us[s] / (int64_t)cnt[s]) : 0;

		shell_print(sh, "  %-38s %5u %8d %8d %8d", lat_segs[s].what,
			    cnt[s], cnt[s] ? min_us[s] : 0, avg,
			    cnt[s] ? max_us[s] : 0);
	}

	shell_print(sh, "");
	lat_prof_dump(sh);

	if (nrows == 0U) {
		shell_print(sh, "no complete sample yet");
		return 0;
	}

	shell_print(sh, "");
	shell_print(sh, "  recent (newest first), us:");

	/* Column headers, wrapped: the probe has more segments than fit on one
	 * line, and the labels now come from the table instead of a literal. */
	for (line = 0U; line * LAT_COLS < NSEG_REAL; line++) {
		char hdr[160];
		size_t off = 0;
		uint32_t i;

		if (line == 0U) {
			off = (size_t)snprintk(hdr, sizeof(hdr), "   # nf/cmd");
		} else {
			off = (size_t)snprintk(hdr, sizeof(hdr), "      +%-2u",
					       line * LAT_COLS);
		}

		for (i = line * LAT_COLS; i < (line + 1U) * LAT_COLS && i < NSEG_REAL;
		     i++) {
			if (off < sizeof(hdr)) {
				int n = snprintk(hdr + off, sizeof(hdr) - off,
						 "%11s", lat_segs[i].abbr);

				if (n > 0) {
					off += (size_t)n;
				}
			}
		}

		shell_print(sh, "%s", hdr);
	}

	for (idx = 0; idx < nrows; idx++) {
		for (line = 0U; line * LAT_COLS < NSEG_REAL; line++) {
			char lineBuf[160];
			size_t off;
			uint32_t i;
			bool last = (line + 1U) * LAT_COLS >= NSEG_REAL;

			if (line == 0U) {
				off = (size_t)snprintk(lineBuf, sizeof(lineBuf),
						       "  %2u %02x/%02x", idx,
						       (unsigned)(rows[idx].cmdkey >> 8),
						       (unsigned)(rows[idx].cmdkey & 0xFFU));
			} else {
				off = (size_t)snprintk(lineBuf, sizeof(lineBuf),
						       "      +%-2u",
						       line * LAT_COLS);
			}

			for (i = line * LAT_COLS;
			     i < (line + 1U) * LAT_COLS && i < NSEG_REAL; i++) {
				off = lat_cell(lineBuf, off, sizeof(lineBuf),
					       rows[idx].have[i], rows[idx].seg[i]);
			}

			if (last && off < sizeof(lineBuf)) {
				int n;

				if (rows[idx].preempt >= 0) {
					n = snprintk(lineBuf + off,
						     sizeof(lineBuf) - off,
						     "  rx>tx=%u miss=%u preempt=%d",
						     rows[idx].total_us,
						     rows[idx].miss,
						     rows[idx].preempt);
				} else {
					n = snprintk(lineBuf + off,
						     sizeof(lineBuf) - off,
						     "  rx>tx=%u miss=%u preempt=--",
						     rows[idx].total_us,
						     rows[idx].miss);
				}

				if (n > 0) {
					off += (size_t)n;
				}

				/* CPU (not wall) the ipmid thread burnt in this
				 * handler; compare with the ipmid column. */
				if (rows[idx].hcpu_us != 0U &&
				    off < sizeof(lineBuf)) {
					n = snprintk(lineBuf + off,
						     sizeof(lineBuf) - off,
						     " hcpu=%u",
						     rows[idx].hcpu_us);

					if (n > 0) {
						off += (size_t)n;
					}
				}
			}

			if (last && rows[idx].ord >= 0 && off < sizeof(lineBuf)) {
				snprintk(lineBuf + off, sizeof(lineBuf) - off,
					 " out-of-order@%s",
					 lat_segs[rows[idx].ord].abbr);
			}

			shell_print(sh, "%s", lineBuf);
		}
	}

	return 0;
}

static int lat_cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	k_spinlock_key_t key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	key = k_spin_lock(&lat_lock);
	memset(lat_slot, 0, sizeof(lat_slot));
	memset(lat_pt_n, 0, sizeof(lat_pt_n));
	memset(lat_pt_ooo, 0, sizeof(lat_pt_ooo));
	lat_cur = 0;
	lat_requests = 0;
	lat_active = false;
	k_spin_unlock(&lat_lock, key);

	lat_prof_reset();

	z_lat_conn_rwake = 0;
	z_lat_conn_ridle = 0;
	z_lat_conn_warem = 0;
	atomic_clear(&lat_conn_proc_n);
	atomic_clear(&lat_conn_proc_us);
	for (uint32_t i = 0U; i < IPMI_LAT_BUS_STEPS; i++) {
		atomic_clear(&lat_bus_step_us[i]);
	}
	atomic_clear(&lat_wr_calls);
	atomic_clear(&lat_wr_eagain);

	lat_map_n = 0;
	lat_map_sum_us = 0;
	lat_map_max_us = 0;
	lat_map_last_us = 0;

	lat_sub_n = 0;
	lat_sub_sum_us = 0;
	lat_sub_max_us = 0;

	memset(lat_slow_top, 0, sizeof(lat_slow_top));

#if defined(CONFIG_SCHED_THREAD_USAGE)
	lat_hcpu_have_in = false;
	lat_hcpu_n = 0;
	lat_hcpu_sum_cyc = 0;
	lat_hcpu_min_cyc = UINT32_MAX;
	lat_hcpu_max_cyc = 0;
	lat_hcpu_nested = 0;
	atomic_clear(&lat_walk_n);
	atomic_clear(&lat_walk_us);
#endif

	shell_print(sh, "ipmi_lat: cleared");
	return 0;
}

/*
 * dbus-broker dispatch sample.
 *
 * src/util/dispatch.c exports its round counters for exactly this purpose
 * (they were read by the now-removed thread_stats.c). The interesting split is
 * "inside poll()" versus "everything else": the broker burns ~15 ms of CPU per
 * IPMI request according to the per-request profiler, and this says whether
 * that is Zephyr's 28-fd poll() itself or the message handling around it.
 *
 * All counters are cumulative since boot (32-bit), so everything printed is a
 * delta over the sampling window. poll_ms_sum is accumulated with
 * k_uptime_get(), i.e. 1 ms resolution: a round that blocks for 300 us counts as
 * 0, which is fine for the question above but do not read it as microseconds.
 */
extern volatile uint32_t z_dispatch_rounds;
extern volatile uint32_t z_dispatch_events;
extern volatile uint32_t z_dispatch_idle_rounds;
extern volatile uint32_t z_dispatch_sleep_ms;
extern volatile uint32_t z_dispatch_nfds;
extern volatile uint32_t z_dispatch_peak_nfds;
extern volatile int32_t z_dispatch_timeout_last;
extern volatile uint32_t z_dispatch_poll_ms_sum;
extern volatile uint32_t z_dispatch_poll_ms_last;
extern volatile uint32_t z_dispatch_short_returns;

static int lat_cmd_brk(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 2000U;
	uint32_t r0, e0, i0, s0, p0, q0;
	uint32_t rounds, poll_ms, sleep_ms, rest_ms;

	if (argc > 1U) {
		unsigned long v = strtoul(argv[1], NULL, 0);

		if (v < 50UL || v > 60000UL) {
			shell_error(sh, "ms must be 50..60000");
			return -EINVAL;
		}

		ms = (uint32_t)v;
	}

	r0 = z_dispatch_rounds;
	e0 = z_dispatch_events;
	i0 = z_dispatch_idle_rounds;
	s0 = z_dispatch_sleep_ms;
	p0 = z_dispatch_poll_ms_sum;
	q0 = z_dispatch_short_returns;

	shell_print(sh, "ipmi_lat brk: sampling %u ms...", ms);
	k_sleep(K_MSEC(ms));

	rounds = z_dispatch_rounds - r0;
	poll_ms = z_dispatch_poll_ms_sum - p0;
	sleep_ms = z_dispatch_sleep_ms - s0;
	rest_ms = (ms > poll_ms + sleep_ms) ? (ms - poll_ms - sleep_ms) : 0U;

	shell_print(sh, "  rounds %u (%u/s)  events %u  idle-rounds %u  short %u",
		    rounds, (rounds * 1000U) / ms, z_dispatch_events - e0,
		    z_dispatch_idle_rounds - i0,
		    z_dispatch_short_returns - q0);
	shell_print(sh, "  in-poll %u ms  in-sleep %u ms  outside-dispatch %u ms",
		    poll_ms, sleep_ms, rest_ms);
	shell_print(sh, "  nfds %u (peak %u)  last timeout %d",
		    z_dispatch_nfds, z_dispatch_peak_nfds,
		    z_dispatch_timeout_last);

	return 0;
}

/*
 * Isolated micro-benchmarks for the pieces the request table only ever shows
 * combined: one small malloc/free, one RAND_bytes, one k_uptime_ticks().
 *
 * These exist because the table's segments are (a) 100 us wide at best -- the
 * probe timestamps with k_uptime_ticks() and CONFIG_SYS_CLOCK_TICKS_PER_SEC is
 * 10000 -- and (b) always the sum of several operations, so "iv>wr is 2.4 ms"
 * cannot say whether the money goes into the crypto or into walking the heap.
 *
 * Everything below is timed with k_cycle_get_32() (600 MHz, 1.7 ns) instead.
 * The shell thread is preemptible, so treat "min" as the cost of the operation
 * and "avg" as the cost plus whatever else the system did in between; run it
 * twice and compare if a number looks odd.
 *
 * The wolfSSL entry points are declared by hand instead of pulling the OpenSSL
 * compatibility headers into this C file: the names below are exactly what the
 * wolfssl/openssl headers #define the EVP_xxx and HMAC macros to, so this is
 * the same code net-ipmid reaches. Passing void * for the opaque context types
 * is ABI-compatible and keeps this file free of wolfSSL build flags.
 */
#define LAT_BENCH_ITERS 64
#define LAT_BENCH_MAX_ITERS 128

extern int wolfSSL_RAND_bytes(unsigned char *buf, int num);
extern void *wolfSSL_EVP_CIPHER_CTX_new(void);
extern void wolfSSL_EVP_CIPHER_CTX_free(void *ctx);
extern const void *wolfSSL_EVP_aes_128_cbc(void);
extern int wolfSSL_EVP_EncryptInit_ex(void *ctx, const void *type, void *impl,
				      const unsigned char *key,
				      const unsigned char *iv);
extern int wolfSSL_EVP_CIPHER_CTX_set_padding(void *ctx, int pad);
extern int wolfSSL_EVP_CipherUpdate(void *ctx, unsigned char *out, int *outl,
				    const unsigned char *in, int inl);
extern const void *wolfSSL_EVP_sha1(void);
extern unsigned char *wolfSSL_HMAC(const void *md, const void *key,
				   int key_len, const unsigned char *d,
				   size_t n, unsigned char *md_out,
				   unsigned int *md_len);

static uint32_t lat_bench_d[LAT_BENCH_MAX_ITERS];
static void *lat_bench_p[LAT_BENCH_MAX_ITERS];
static volatile uint32_t lat_bench_sink;

static void lat_bench_line(const struct shell *sh, const char *what,
			   uint32_t n)
{
	uint64_t sum = 0U;
	uint32_t min = UINT32_MAX;
	uint32_t max = 0U;

	for (uint32_t i = 0; i < n; i++) {
		min = MIN(min, lat_bench_d[i]);
		max = MAX(max, lat_bench_d[i]);
		sum += lat_bench_d[i];
	}

	shell_print(sh, "  %-18s %9u %8u %8u %10u", what,
		    k_cyc_to_ns_floor32(min),
		    k_cyc_to_us_floor32((uint32_t)(sum / n)),
		    k_cyc_to_us_floor32(max), min);
}

static int lat_cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	static const uint32_t sizes[] = {32U, 512U, 4096U};
	static const size_t rand_sizes[] = {4U, 16U, 32U, 64U};
	uint32_t n = LAT_BENCH_ITERS;
	char what[24];
	uint8_t buf[64];
	uint32_t t0;
	uint32_t i;
	size_t s;

	if (argc > 1U) {
		unsigned long v = strtoul(argv[1], NULL, 0);

		if (v == 0UL || v > (unsigned long)LAT_BENCH_MAX_ITERS) {
			shell_error(sh, "iters must be 1..%d",
				    LAT_BENCH_MAX_ITERS);
			return -EINVAL;
		}

		n = (uint32_t)v;
	}

	shell_print(sh, "ipmi_lat bench: n=%u  %u cyc/s  tick=%u us", n,
		    CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
		    1000000U / CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	shell_print(sh, "  %-18s %9s %8s %8s %10s", "op", "min_ns", "avg_us",
		    "max_us", "min_cyc");

	/* Floor: one 32-bit cycle-counter read pair (two reads). */
	for (i = 0; i < n; i++) {
		t0 = k_cycle_get_32();
		lat_bench_d[i] = k_cycle_get_32() - t0;
	}
	lat_bench_line(sh, "cycle_get_32 x2", n);

	/* What one ipmi_lat_mark() costs by itself: the 32-bit tick read is the
	 * expensive part of it, the spinlock adds very little. */
	for (i = 0; i < n; i++) {
		t0 = k_cycle_get_32();
		(void)k_uptime_ticks();
		lat_bench_d[i] = k_cycle_get_32() - t0;
	}
	lat_bench_line(sh, "k_uptime_ticks", n);

	/*
	 * Execution-rate calibration. 1000 dependent ALU ops out of one cache
	 * line: 2-4 cycles per iteration on this core at 600 MHz, i.e. 2000-4000
	 * cycles total. Tens of cycles per iteration means the core actually
	 * runs slower than the timer reports, and then every microsecond figure
	 * in this file (and in the request table) is inflated by that factor.
	 */
	for (i = 0; i < n; i++) {
		uint32_t v = i + 1U;

		t0 = k_cycle_get_32();
		for (uint32_t k = 0; k < 1000U; k++) {
			v = v * 1103515245U + 12345U;
		}
		lat_bench_d[i] = k_cycle_get_32() - t0;
		lat_bench_sink = v;
	}
	lat_bench_line(sh, "alu x1000", n);

	/* Cross-check of the cycle/microsecond conversion: one millisecond of
	 * busy wait should be CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000 cycles. */
	for (i = 0; i < MIN(n, 8U); i++) {
		t0 = k_cycle_get_32();
		k_busy_wait(1000);
		lat_bench_d[i] = k_cycle_get_32() - t0;
	}
	lat_bench_line(sh, "busy_wait(1ms)", MIN(n, 8U));

	shell_print(sh, "");

	/* libc heap (CONFIG_NEWLIB_LIBC_MALLOC_LSQSH): the arena lives in PSRAM
	 * and every C++ vector/new/sd_bus message goes through it. */
	for (s = 0; s < ARRAY_SIZE(sizes); s++) {
		for (i = 0; i < n; i++) {
			t0 = k_cycle_get_32();
			lat_bench_p[i] = malloc(sizes[s]);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		snprintk(what, sizeof(what), "malloc(%u)", sizes[s]);
		lat_bench_line(sh, what, n);

		for (i = 0; i < n; i++) {
			t0 = k_cycle_get_32();
			free(lat_bench_p[i]);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		snprintk(what, sizeof(what), "free(%u)", sizes[s]);
		lat_bench_line(sh, what, n);
	}

	/* Kernel heap, for comparison: 40 KiB pool, different allocator. */
	for (i = 0; i < n; i++) {
		t0 = k_cycle_get_32();
		lat_bench_p[i] = k_malloc(32U);
		lat_bench_d[i] = k_cycle_get_32() - t0;
	}
	lat_bench_line(sh, "k_malloc(32)", n);

	for (i = 0; i < n; i++) {
		t0 = k_cycle_get_32();
		k_free(lat_bench_p[i]);
		lat_bench_d[i] = k_cycle_get_32() - t0;
	}
	lat_bench_line(sh, "k_free(32)", n);

	shell_print(sh, "");

	/* sys_rand_get() is what wolfSSL's RAND_bytes() calls internally to seed
	 * its DRBG (WOLFSSL_ZEPHYR: wc_GenerateSeed -> sys_rand_get). The
	 * entropy device hands out 2 bytes per TRNG match and blocks on a
	 * k_pipe_get in between, so the cost should scale with the length: if it
	 * does, the ~1 ms of "rand" in the request table is TRNG throughput, not
	 * crypto. Fewer iterations: one 64-byte request is milliseconds. */
	for (s = 0; s < ARRAY_SIZE(rand_sizes); s++) {
		uint32_t rn = MIN(n, 16U);

		for (i = 0; i < rn; i++) {
			t0 = k_cycle_get_32();
			sys_rand_get(buf, rand_sizes[s]);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		snprintk(what, sizeof(what), "sys_rand_get(%u)",
			 (unsigned int)rand_sizes[s]);
		lat_bench_line(sh, what, rn);
	}

	shell_print(sh, "");

	/*
	 * The wolfSSL primitives the reply path actually calls. If these
	 * reproduce the "rand"/"evp"/"hmac" columns, the anomaly is inside
	 * wolfCrypt (or inside what it allocates); if they come out two orders
	 * of magnitude faster, those columns are paying for preemption instead.
	 */
	for (i = 0; i < n; i++) {
		t0 = k_cycle_get_32();
		(void)wolfSSL_RAND_bytes(buf, 16);
		lat_bench_d[i] = k_cycle_get_32() - t0;
	}
	lat_bench_line(sh, "RAND_bytes(16)", n);

	{
		static unsigned char evp_key[16];
		static unsigned char evp_in[64];
		static unsigned char evp_out[96];
		const void *cbc = wolfSSL_EVP_aes_128_cbc();
		int outl = 0;

		for (i = 0; i < n; i++) {
			t0 = k_cycle_get_32();
			lat_bench_p[i] = wolfSSL_EVP_CIPHER_CTX_new();
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "EVP_CTX_new", n);

		for (i = 0; i < n; i++) {
			if (lat_bench_p[i] == NULL) {
				lat_bench_d[i] = 0U;
				continue;
			}

			t0 = k_cycle_get_32();
			(void)wolfSSL_EVP_EncryptInit_ex(lat_bench_p[i], cbc,
							 NULL, evp_key,
							 evp_key);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "EVP_EncryptInit_ex", n);

		for (i = 0; i < n; i++) {
			if (lat_bench_p[i] == NULL) {
				lat_bench_d[i] = 0U;
				continue;
			}

			t0 = k_cycle_get_32();
			(void)wolfSSL_EVP_CIPHER_CTX_set_padding(lat_bench_p[i],
								 0);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "EVP_set_padding", n);

		for (i = 0; i < n; i++) {
			if (lat_bench_p[i] == NULL) {
				lat_bench_d[i] = 0U;
				continue;
			}

			outl = 0;
			t0 = k_cycle_get_32();
			(void)wolfSSL_EVP_CipherUpdate(lat_bench_p[i], evp_out,
						       &outl, evp_in, 32);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "EVP_CipherUpdate(32B)", n);

		for (i = 0; i < n; i++) {
			if (lat_bench_p[i] == NULL) {
				lat_bench_d[i] = 0U;
				continue;
			}

			t0 = k_cycle_get_32();
			wolfSSL_EVP_CIPHER_CTX_free(lat_bench_p[i]);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "EVP_CTX_free", n);
	}

	{
		static unsigned char hmac_key[20];
		static unsigned char hmac_in[1024];
		static unsigned char hmac_md[32];
		const void *sha1 = wolfSSL_EVP_sha1();
		unsigned int mdlen = 0;

		for (i = 0; i < n; i++) {
			mdlen = 0U;
			t0 = k_cycle_get_32();
			(void)wolfSSL_HMAC(sha1, hmac_key, sizeof(hmac_key),
					   hmac_in, 40, hmac_md, &mdlen);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "HMAC-SHA1(40B)", n);

		/* Same call over 1024 bytes: a constant offset means the HMAC
		 * context setup (XMALLOC of the Hmac union + memset + one-off
		 * XFREE) is the cost, a proportional rise means the SHA1 core
		 * itself is slow. */
		for (i = 0; i < n; i++) {
			mdlen = 0U;
			t0 = k_cycle_get_32();
			(void)wolfSSL_HMAC(sha1, hmac_key, sizeof(hmac_key),
					   hmac_in, sizeof(hmac_in), hmac_md,
					   &mdlen);
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "HMAC-SHA1(1024B)", n);
	}

	{
		static unsigned char mc_src[1024];
		static unsigned char mc_dst[1024];

		for (i = 0; i < n; i++) {
			t0 = k_cycle_get_32();
			memcpy(mc_dst, mc_src, sizeof(mc_src));
			lat_bench_d[i] = k_cycle_get_32() - t0;
		}
		lat_bench_line(sh, "memcpy(1024B)", n);
	}

	shell_print(sh, "");
	shell_print(sh, "  min = pure cost, avg/max include preemption");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_ipmi_lat,
	SHELL_CMD(dump, NULL, "Dump latency samples", lat_cmd_dump),
	SHELL_CMD(clear, NULL, "Clear latency samples", lat_cmd_clear),
	SHELL_CMD(top, NULL, "Per-thread CPU ranking over a window [ms]",
		  lat_cmd_top),
	SHELL_CMD(brk, NULL, "dbus-broker dispatch rounds over a window [ms]",
		  lat_cmd_brk),
	SHELL_CMD(bench, NULL, "Micro-benchmark malloc/RAND/ticks [iters]",
		  lat_cmd_bench),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ipmi_lat, &sub_ipmi_lat, "IPMI request latency probe", NULL);

#endif /* CONFIG_OPENBMC_IPMI_LAT */
