/* SPDX-License-Identifier: Apache-2.0 */

/*
 * IPMI request latency probe (diagnostic).
 *
 * Records k_uptime_ticks() at ten points of one IPMI request/response cycle in
 * a RAM ring buffer. Nothing is printed on the hot path: a single IPMI request
 * already spends ~33 ms in UART output, so any printk-based instrumentation
 * would end up measuring itself. Dump with the "ipmi_lat" shell command.
 *
 * Points, in the order they are hit for one LAN request:
 *
 *   RX         net-ipmid: handleRmcpPacket() entry. The datagram has been
 *              dequeued by the io thread, so the preceding TX->RX segment
 *              covers wire + client + UDP socket wakeup.
 *   BUS_REQ    net-ipmid: right before async_method_call() of
 *              xyz.openbmc_project.Ipmi.Server.execute.
 *   BRQ_SENT   basu: the mark right after sendmsg() of that call completed.
 *              Do NOT read it as "the bytes are in the pipe": dbus-broker runs
 *              at priority 0 and this thread at priority 4, so the broker is
 *              usually scheduled in between, dequeues (BRQ_IN) and even
 *              forwards (BRQ_WOUT) before the sender runs the mark again. It is
 *              therefore reported as "preempt" (BRQ_SENT - BRQ_IN), not as a
 *              latency boundary.
 *   BRQ_IN     broker: the execute() method_call was dequeued from net-ipmid's
 *              socket.
 *   BRQ_FWD    broker: the same message was queued onto ipmid's outbound
 *              connection (connection_queue()).
 *   BRQ_WOUT   broker: sendmmsg() put it into ipmid's socket.
 *   IPMID_IN   ipmid: executionEntry() entry (next to the 504 trace).
 *   IPMID_OUT  ipmid: executeIpmiCommand() returned (next to the 601 trace).
 *   BRS_IN     broker: ipmid's method_return was dequeued from its socket.
 *   BRS_FWD    broker: the return was queued onto net-ipmid's connection.
 *   BUS_RSP    net-ipmid: async_method_call() callback entered.
 *   RSP_BUILD  net-ipmid: ~Handler() about to call createResponse().
 *   IV_BEG     net-ipmid: AlgoAES128::encryptData() just before RAND_bytes().
 *   IV_END     net-ipmid: encryptData() just after RAND_bytes() returned.
 *   HMAC_BEG   net-ipmid: AlgoSHAx::generateIntegrityData(), before HMAC().
 *   HMAC_END   net-ipmid: generateIntegrityData(), after HMAC() returned.
 *   RSP_WRITE  net-ipmid: writeData() just before channel->write(); the space
 *              between RSP_BUILD and here is message marshal + RMCP+ encrypt +
 *              HMAC.
 *   TX         net-ipmid: channel->write() returned, i.e. the datagram was
 *              handed to the network stack.
 *
 * The dump prints a per-point mark count ("marks:") and an out-of-order
 * counter; both are there to prove the one-request-per-slot assumption instead
 * of assuming it. Equal counts and no out-of-order hits mean the sample is a
 * single request. (BRQ_SENT is the expected exception: it is always reported
 * out of order because of the priority inversion described above.)
 *
 * The probe assumes one request in flight at a time, which holds for both KCS
 * and LAN IPMI. Concurrent traffic would interleave samples in one slot.
 *
 * With CONFIG_OPENBMC_IPMI_LAT=n the mark expands to nothing, so call sites
 * cost no code and no external symbol is referenced.
 */

#pragma once

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ipmi_lat_pt {
	IPMI_LAT_RX = 0,
	IPMI_LAT_BUS_REQ,
	IPMI_LAT_BRQ_SENT,
	IPMI_LAT_BRQ_IN,
	IPMI_LAT_BRQ_FWD,
	IPMI_LAT_BRQ_WOUT,
	/* ipmid: the read watch fired and this connection entered
	 * sd_bus_process(), i.e. just before it reads the message the broker
	 * wrote. Splits "brk:wout -> ipmid_in" into the part where the thread
	 * was not scheduled yet (real wakeup latency) and the part it spent
	 * dispatching inside sd_bus_process/. Only ipmid's own connection emits
	 * it, marks from other services would land in the same request slot. */
	IPMI_LAT_BUS_PROC,
	IPMI_LAT_IPMID_IN,
	IPMI_LAT_IPMID_OUT,
	IPMI_LAT_BRS_IN,
	IPMI_LAT_BRS_FWD,
	IPMI_LAT_BUS_RSP,
	/* Temporary: split bus_rsp->tx, which sits at a suspiciously constant
	 * ~5.4 ms. Remove both together with the probe. */
	IPMI_LAT_RSP_BUILD, /* net-ipmid: ~Handler, before createResponse() */
	IPMI_LAT_IV_BEG,    /* net-ipmid: AlgoAES128::encryptData, before RAND_bytes() */
	IPMI_LAT_IV_END,    /* net-ipmid: AlgoAES128::encryptData, after RAND_bytes() */
	/* Temporary: split iv_end->rsp_write, which carries EVP_CIPHER_CTX_new,
	 * EncryptInit/Update and the one-shot HMAC(). Remove with the probe. */
	IPMI_LAT_HMAC_BEG,  /* net-ipmid: generateIntegrityData(), before HMAC() */
	IPMI_LAT_HMAC_END,  /* net-ipmid: generateIntegrityData(), after HMAC() */
	IPMI_LAT_RSP_WRITE, /* net-ipmid: writeData(), before channel->write() */
	IPMI_LAT_TX,
	/* Derived, not a mark: TX of the previous request -> RX of this one. */
	IPMI_LAT_XFER,
	IPMI_LAT_NUM_PTS
};

#ifdef CONFIG_OPENBMC_IPMI_LAT
void ipmi_lat_mark(enum ipmi_lat_pt pt);

/*
 * Optional hook: one getSensorMap() cache rebuild took `us` microseconds.
 *
 * The per-request table cannot show this cost: a rebuild happens once per
 * updatePeriod and its GetManagedObjects reply is amortised over every command
 * in that window, so it only ever leaks out as "some request took a second".
 * Counting it explicitly separates "the handler is slow" from "the handler was
 * waiting for a bulk fetch that another request had already paid for".
 */
void ipmi_lat_map_rebuild(uint32_t us, uint32_t objects);

/*
 * Optional hook: getSensorSubtree() rebuilt the whole sensor tree in `us`
 * microseconds.
 *
 * The tree is only rebuilt after an InterfacesAdded/InterfacesRemoved signal
 * invalidates it, and a rebuild is a full GetSubTree plus a new sensor-number
 * map plus a wipe of the SDR tables, i.e. every SDR record gets regenerated
 * afterwards. That makes it the prime suspect for the rare 0.8-1.4 s requests
 * that dominate the average of a "sdr list" run; counting it separately says
 * whether a slow request was a rebuild or something else.
 */
void ipmi_lat_subtree_rebuild(uint32_t us);

/*
 * Optional hook: name the request that the next IPMI_LAT_IPMID_IN belongs to.
 * "ipmi_lat dump" prints it as nf/cmd, so the row that burned 70 ms can be
 * identified instead of guessed at.
 */
void ipmi_lat_set_cmd(uint8_t netfn, uint8_t cmd);

/*
 * Wrappers called from dbus-broker and basu, which cannot include this header
 * (it lives outside those modules). They declare them themselves, so keep the
 * signatures and the mapping stable.
 */
void ipmi_lat_brq_in(void);
void ipmi_lat_brq_fwd(void);
void ipmi_lat_brs_in(void);
void ipmi_lat_brs_fwd(void);
void ipmi_lat_brq_sent(void);
void ipmi_lat_brq_wout(void);
#else
#define ipmi_lat_mark(pt) do { (void)(pt); } while (0)
#define ipmi_lat_map_rebuild(us, objects) do { (void)(us); (void)(objects); } while (0)
#define ipmi_lat_subtree_rebuild(us) do { (void)(us); } while (0)
#define ipmi_lat_set_cmd(netfn, cmd) \
	do { (void)(netfn); (void)(cmd); } while (0)
#endif

#ifdef __cplusplus
}
#endif
