/*
 * TrimUI Brick USB packet bridge. Transport/framing and usbfs loop adapted
 * from sMainero's MIT-licensed nextui-netplay-cable fork.
 *
 * The bottom port exposes a FunctionFS cable interface beside the firmware's
 * adb function in g1. The top port is a permanent USB host. Neither role
 * reads the vendor role-trigger nodes or replaces the stock gadget identity.
 * A saved UDC name lets --repair undo an interrupted gadget setup. The gadget
 * side suspends battery charging while a peer is connected and restores the
 * original AXP2202 register values on stop or repair.
 */
#include "usbnet.h"
#include "netsetup.h"
#include "brick-power.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <sys/eventfd.h>   /* gadget role only; the host role needs no eventfd */
#endif
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Darwin's mount(2) takes four arguments and has no umount2/MNT_DETACH, and
 * every call below is device-only: configfs, functionfs, and taking them down.
 * A host build (testing/test-cable.sh) never reaches one - its fake tree stops
 * the daemon long before - so these exist to compile, and refuse in a way that
 * would be visible rather than pretend to have mounted something. The device
 * build is Linux and uses the real calls, unchanged. */
#if !defined(__linux__)
#define MNT_DETACH 0
static int cp_mount_unsupported(const char* source, const char* target,
                                const char* type, unsigned long flags,
                                const void* data) {
	(void)source; (void)target; (void)type; (void)flags; (void)data;
	errno = ENOSYS;
	return -1;
}
static int cp_umount_unsupported(const char* target, int flags) {
	(void)target; (void)flags;
	errno = ENOSYS;
	return -1;
}
#define mount(source, target, type, flags, data) \
	cp_mount_unsupported(source, target, type, flags, data)
#define umount2(target, flags) cp_umount_unsupported(target, flags)
#endif

//////////////////////////////////////////////////////////////////////////////
// transcribed kernel constants
//////////////////////////////////////////////////////////////////////////////

/* <linux/if_tun.h> and <linux/if.h> in 4.9: TUNSETIFF is _IOW('T', 202, int)
 * and the two flags say "layer 3" and "no packet information header". The
 * device build's include path is the platform's, and a uapi header that is not
 * there would be a build failure for a feature that does not need it. */
#define CP_TUNSETIFF _IOW('T', 202, int)
#define CP_IFF_TUN   0x0001
#define CP_IFF_NO_PI 0x1000

/* <linux/usb/functionfs.h> in 4.9: the events the kernel leaves on ep0 and the
 * 12-byte structure each arrives in - an 8-byte control request, a type byte,
 * three of padding. The order is the enum's, so an off-by-one would be read as
 * a different event's type rather than as an error. */
enum {
	CP_FFS_EV_BIND = 0,
	CP_FFS_EV_UNBIND,
	CP_FFS_EV_ENABLE,
	CP_FFS_EV_DISABLE,
	CP_FFS_EV_SETUP,
	CP_FFS_EV_SUSPEND,
	CP_FFS_EV_RESUME,
};

typedef struct {
	uint8_t  bRequestType;
	uint8_t  bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
} __attribute__((packed)) CP_CtrlRequest;

typedef struct {
	CP_CtrlRequest setup;
	uint8_t        type;
	uint8_t        pad[3];
} __attribute__((packed)) CP_FFS_Event;

/* <linux/usbdevice_fs.h> in 4.9: the host role's only way to reach the peer.
 *
 * The ioctl numbers are the _IOC-encoded values for a 64-bit target, which is
 * the only ABI this daemon is built for. Three of them encode a size - two carry
 * sizeof(void *) and one carries sizeof(struct usbdevfs_urb) - so a 32-bit
 * build would need the ...32 variants and a different urb layout. Writing them
 * as numbers rather than as _IOR/_IOW on a hand-made struct is deliberate: the
 * numbers are what the kernel compares against, and a macro that quietly
 * encoded a different size would look correct and fail at run time.
 *
 *   USBDEVFS_CONTROL        _IOWR('U',  0, struct usbdevfs_ctrltransfer)  24 bytes
 *   USBDEVFS_SUBMITURB      _IOR ('U', 10, struct usbdevfs_urb)           56 bytes
 *   USBDEVFS_REAPURBNDELAY  _IOW ('U', 13, void *)                         8 bytes
 *   USBDEVFS_CLAIMINTERFACE _IOR ('U', 15, unsigned int)                   4 bytes
 *   USBDEVFS_CLEAR_HALT     _IOR ('U', 21, unsigned int)                   4 bytes */
#define CP_USBDEVFS_CONTROL        0xc0185500u
#define CP_USBDEVFS_SUBMITURB      0x8038550au
#define CP_USBDEVFS_REAPURBNDELAY  0x4008550du
#define CP_USBDEVFS_CLAIMINTERFACE 0x8004550fu
#define CP_USBDEVFS_CLEAR_HALT     0x80045515u

/* The one urb type the host role submits, and the one flag it sets. */
#define CP_URB_TYPE_BULK   3u      /* USBDEVFS_URB_TYPE_BULK */
#define CP_URB_ZERO_PACKET 0x40u   /* USBDEVFS_URB_ZERO_PACKET */

/* The kernel reads both of these by offset and writes the completion back into
 * the same memory, so the layout is the ABI rather than a detail. Neither is
 * packed: the padding after `endpoint` and the padding before `buffer` are part
 * of what the kernel expects, and a packed struct would be two bytes short.
 * The assertions below are the transcription's own test - the sizes they pin
 * are the ones the ioctl numbers above encode. */
typedef struct {
	uint8_t  bRequestType;
	uint8_t  bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
	uint32_t timeout;
	void*    data;
} CP_CtrlTransfer;

typedef struct {
	uint8_t  type;
	uint8_t  endpoint;
	int32_t  status;
	uint32_t flags;
	void*    buffer;
	int32_t  buffer_length;
	int32_t  actual_length;
	int32_t  start_frame;
	union {
		int32_t  number_of_packets;
		uint32_t stream_id;
	} u;
	int32_t  error_count;
	uint32_t signr;
	void*    usercontext;
} CP_Urb;

typedef char cp_ctrltransfer_is_24_bytes[sizeof(CP_CtrlTransfer) == 24 ? 1 : -1];
typedef char cp_urb_is_56_bytes[sizeof(CP_Urb) == 56 ? 1 : -1];

/* <linux/usb/ch9.h>, the handful of values needed to read a descriptor out of a
 * device and decide whether it is ours. */
#define CP_DT_DEVICE              1u
#define CP_DT_CONFIG              2u
#define CP_DT_INTERFACE           4u
#define CP_DT_ENDPOINT            5u
#define CP_USB_DIR_IN             0x80u
#define CP_USB_RECIP_DEVICE       0x00u
#define CP_USB_REQ_GET_DESCRIPTOR 6u
#define CP_MAXPACKET_MASK         0x07ffu
#define CP_USB_CTRL_TIMEOUT_MS    1000

//////////////////////////////////////////////////////////////////////////////
// cadence and limits
//////////////////////////////////////////////////////////////////////////////

/* Two cadences, for the two kinds of work in one loop.
 *
 * CP_TICK_MS is how long the poll behind the data path waits, and it is the
 * broker's tick (app/broker.c:98-103) for the same reason: it is a cadence, not
 * a latency. The status file, the peer question and the lifetime question are
 * not per-packet work, so they run on every tenth tick, exactly as the broker
 * throttles its own side effect (app/broker.c:102, `status_tick++ % 10`) - the
 * app reads the status file itself, and once a second is what that reader
 * needs.
 *
 * A record waiting for the transmit endpoint shortens the poll, because a held
 * packet is latency and a status refresh is not. */
#define CP_TICK_MS         100
#define CP_SIDE_EFFECT_MS  (CP_TICK_MS * 10)
#define CP_TX_RETRY_MS     5

/* How long a kernel step may take before it is reported as not having
 * happened. Both are deadline-bounded on purpose: nothing this daemon does may
 * be the reason a screen sits on "presenting USB gadget..." forever. */
#define CP_EP_WAIT_TICKS  20   /* ~2 s for the endpoint files after the strings blob */
#define CP_ADDR_WAIT_TICKS 10  /* ~1 s for the address to land on the interface */
#define CP_EP0_ATTEMPTS   10   /* ep0 writes that come back EAGAIN while the mutex is held */

/* Limit one busy USB direction from starving the other. */
#define CP_REAP_MAX        8

/* How long the controller may take to let go before the teardown gives up and
 * leaves the gadget alone, in 100 ms ticks.
 *
 * Five seconds rather than one, because the thing being waited for is the peer:
 * the controller cannot be released while the other end still has the interface
 * claimed over usbfs with transfers outstanding (the write comes back ENODEV),
 * and the peer only lets go when *its* side tears down. Observed on hardware -
 * the one-second version gave up while the peer was still mid-session. The
 * ordering rule this implies is in the docs: the host end tears down first, and
 * the gadget end finishes afterwards. */
#define CP_UNBIND_TICKS 50
/* How long to keep offering the controller to the gadget before calling the bind a
 * failure: 50 x 100 ms. A bind is not instantaneous even when everything is right
 * - the controller has to start the gadget, and its functionfs instance must
 * already carry descriptors, which for the firmware's own userspace means waiting
 * for a process that was started microseconds earlier. The kernel log from the
 * h700 freeze is what makes this a rule rather than a nicety:
 *
 *   ffs_data_put(): freeing
 *   ERR: sunxi_udc_dequeue: driver is null
 *   configfs-gadget 5100000.udc-controller: failed to start g1: -19
 *   ... and then kmem_cache_alloc faulting on a wild address, with every
 *   allocation afterwards in every subsystem dying on it
 *
 * i.e. a bind attempted against a descriptorless instance did not merely fail,
 * it left the kernel's heap corrupted. So the write is offered repeatedly and the
 * attribute is read back, instead of being written once and believed. */
#define CP_BIND_TICKS 50

/* After a bind with VBUS present: how long the controller may stay "not
 * attached" (a host enumerates it in ~0.5 s), how many binds to try, and how
 * long the controller rests between them. */
#define CP_UDC_ATTACH_TICKS 20
#define CP_BIND_ATTEMPTS    3
#define CP_REBIND_PAUSE_MS  1000

/* How often the host role looks for the peer on the bus. A gadget that has just
 * been plugged or powered takes a moment to appear, and a directory scan plus a
 * control transfer is not work to do ten times a second. */
#define CP_ATTACH_TICKS 5
#define CP_HELLO_INTERVAL 1
#define CP_HELLO_GRACE    3
static const uint8_t cp_hello[] = { 0, 'N', 'P', 'C', 'L', 1, 'H' };
static const uint8_t cp_hello_ack[] = { 0, 'N', 'P', 'C', 'L', 1, 'A' };

/* The only persistent gadget repair field is the stock g1 UDC name. */
#define CP_RESTORE_MAX_LINES 1
#define CP_RESTORE_KEY_UDC "udc"

/* Device paths are overridable for a fake configfs tree in the native test. */
#define CP_ENV_CONFIGFS "NP_CABLE_CONFIGFS"
#define CP_ENV_FFS_DIR  "NP_CABLE_FFS_DIR"
#define CP_ENV_UDC_DIR  "NP_CABLE_UDC_DIR"
#define CP_ENV_TUN      "NP_CABLE_TUN"
#define CP_ENV_TOP_HCD  "NP_CABLE_TOP_HCD"
#define CP_ENV_USB_DIR  "NP_CABLE_USB_DIR"
#define CP_ENV_USB_SYSFS "NP_CABLE_USB_SYSFS"
#define CP_ENV_VBUS     "NP_CABLE_VBUS"

/* Where the host role looks for the peer. A constant rather than a CP_Facts
 * entry: only this role needs it, it is the same path on every platform, and
 * the override below is what makes it testable off the device. */
#define CP_USB_DIR "/dev/bus/usb"

/* Interface attributes are checked here before any usbfs node is opened. */
#define CP_USB_SYSFS "/sys/bus/usb/devices"

//////////////////////////////////////////////////////////////////////////////
// state this process owns
//////////////////////////////////////////////////////////////////////////////

static volatile sig_atomic_t cp_stopping;

static char cp_state_dir[CP_LINE_MAX];
static char cp_pid_path[CP_LINE_MAX];
static char cp_status_path[CP_LINE_MAX];
static char cp_log_path[CP_LINE_MAX];

static CP_Status cp_status;

/* The descriptors the daemon drives, so that teardown is one function that
 * unwinds whatever exists rather than a set of paths each bring-up step has to
 * remember to undo. -1 is "never opened". */
static int  cp_tun_fd  = -1;
static int  cp_ep_in   = -1;
static int  cp_ep_out  = -1;
static int  cp_ep0     = -1;
static int  cp_evfd    = -1;
static bool cp_gadget_bound;
static bool cp_ffs_mounted;
static bool cp_session_seen;

/* The receive thread talks to the main thread through these two words rather
 * than through the status file: a lock around every packet is a cost this link
 * does not need to pay, and the status file is published by the main thread
 * alone. */
static volatile sig_atomic_t cp_rx_records;
static volatile sig_atomic_t cp_rx_failed;
static volatile sig_atomic_t cp_rx_errno;
static volatile sig_atomic_t cp_peer_last_seen;
static volatile sig_atomic_t cp_hello_ack_pending;
static sig_atomic_t cp_host_last_hello;

static sig_atomic_t cp_now_s(void) {
	struct timespec now;
	return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? (sig_atomic_t)now.tv_sec : 0;
}

static bool cp_peer_recent(void) {
	sig_atomic_t now = cp_now_s(), seen = cp_peer_last_seen;
	return seen > 0 && now >= seen && now - seen <= CP_HELLO_GRACE;
}

/* The one packet waiting for the transmit endpoint. One slot, not a queue -
 * while it is occupied the interface is simply not read, which is backpressure
 * rather than a buffer that can overflow. */
static uint8_t cp_tx_record[CP_RECORD_MAX];

/* The host role's device, its two endpoints, and the two URBs it keeps in the
 * kernel. The URBs are file scope because the kernel holds a pointer to them for
 * the whole life of the transfer and writes the completion back into the same
 * memory: they are the kernel's handle on the transfer, not this process's
 * scratch space.
 *
 * The read buffer is a whole number of packets at both speeds (4 x 512, 32 x 64)
 * for the same reason the gadget role's is: the controller fills what it is
 * given, and a request that is not a multiple of the packet size can only be one
 * that ends in the middle of what the peer is sending. */
static int      cp_usb_fd = -1;
static uint8_t  cp_usb_in_addr;
static uint8_t  cp_usb_out_addr;
static unsigned cp_usb_maxpacket = CP_MAXPACKET_HS;
/* Only the transmit side needs a flag: its record is only read from the
 * interface while no transfer is outstanding, which is the backpressure. The
 * receive URB is in flight for exactly as long as a peer is attached, and
 * saying so twice - once in a variable and once in the attachment - would be
 * two answers to one question. */
static bool     cp_urb_out_in_flight;
static CP_Urb   cp_urb_in;
static CP_Urb   cp_urb_out;
static uint8_t  cp_urb_in_buffer[CP_READ_MAX];
static const char* cp_usb_dir = CP_USB_DIR;
static const char* cp_usb_sysfs = CP_USB_SYSFS;
//////////////////////////////////////////////////////////////////////////////
// logging
//
// The same shape as the app's and the watchdog's: wall-clock with milliseconds
// and a tag of our own, so one session's three logs read side by side.
//////////////////////////////////////////////////////////////////////////////

static void cp_stamp(char* out, size_t len) {
	struct timeval tv;
	gettimeofday(&tv, NULL);

	static time_t cached_sec = 0;
	static char   cached_hms[32] = "00:00:00";
	if (tv.tv_sec != cached_sec) {
		struct tm tm;
		localtime_r(&tv.tv_sec, &tm);
		strftime(cached_hms, sizeof(cached_hms), "%H:%M:%S", &tm);
		cached_sec = tv.tv_sec;
	}
	snprintf(out, len, "%s.%03d", cached_hms, (int)(tv.tv_usec / 1000));
}

static void cp_log(const char* fmt, ...) {
	char ts[48];
	cp_stamp(ts, sizeof(ts));

	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[%s] " CP_LOG_TAG, ts);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

static long cp_ms_between(const struct timeval* then, const struct timeval* now) {
	return (now->tv_sec - then->tv_sec) * 1000L + (now->tv_usec - then->tv_usec) / 1000L;
}

//////////////////////////////////////////////////////////////////////////////
// small files
//////////////////////////////////////////////////////////////////////////////

static bool cp_file_exists(const char* path) {
	return path && path[0] && access(path, F_OK) == 0;
}

static bool cp_dir_exists(const char* path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A sysfs or configfs attribute read as a value: one read, then trimmed of the
 * newline it is written with by the pure layer's rule. False means "not there,
 * or nothing but blanks in it" - an unbound UDC attribute, for instance. */
static bool cp_read_attr(const char* path, char* out, size_t cap) {
	if (!out || !cap) return false;
	out[0] = '\0';
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	ssize_t n = read(fd, out, cap - 1);
	close(fd);
	if (n <= 0) { out[0] = '\0'; return false; }
	out[n] = '\0';
	return cp_attr_trim(out);
}

/* One write of the whole value: sysfs and configfs parse what they are given in
 * a single call, so a short write is a different value rather than a partial
 * one. */
static bool cp_write_attr(const char* path, const char* value) {
	if (!path || !value) return false;
	int fd = open(path, O_WRONLY | O_TRUNC);
	if (fd < 0) return false;
	size_t want = strlen(value);
	ssize_t n = write(fd, value, want);
	close(fd);
	return n == (ssize_t)want;
}

static bool cp_mkdir_p(const char* path) {
	char buffer[CP_LINE_MAX];
	if (!path || !path[0]) return false;
	if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer)) return false;

	for (char* p = buffer + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return false;
		*p = '/';
	}
	return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

//////////////////////////////////////////////////////////////////////////////
// the platform's facts
//////////////////////////////////////////////////////////////////////////////

/* The AXP2202's USB input: "1" while something on the bottom socket supplies
 * VBUS. Only the bind check reads it, to tell a controller that has nothing to
 * attach to from one that failed to start. */
static const char* cp_vbus_path = "/sys/class/power_supply/axp2202-usb/online";

static CP_Facts cp_facts = {
	.configfs = "/sys/kernel/config",
	.ffs_dir  = "/dev/usb-ffs/cable",
	.udc_dir  = "/sys/class/udc",
	.tun_dev  = "/dev/net/tun",
	.top_hcd  = "5200000.ehci1",
	.gadget   = "g1",
	.function = "cable",
	.iface    = CP_IFACE_NAME,
};

static void cp_facts_init(void) {
	const char* env;
	if ((env = getenv(CP_ENV_CONFIGFS)) && env[0]) cp_facts.configfs = env;
	if ((env = getenv(CP_ENV_FFS_DIR)) && env[0]) cp_facts.ffs_dir = env;
	if ((env = getenv(CP_ENV_UDC_DIR)) && env[0]) cp_facts.udc_dir = env;
	if ((env = getenv(CP_ENV_TUN)) && env[0]) cp_facts.tun_dev = env;
	if ((env = getenv(CP_ENV_TOP_HCD)) && env[0]) cp_facts.top_hcd = env;
	if ((env = getenv(CP_ENV_USB_DIR)) && env[0]) cp_usb_dir = env;
	if ((env = getenv(CP_ENV_USB_SYSFS)) && env[0]) cp_usb_sysfs = env;
	if ((env = getenv(CP_ENV_VBUS)) && env[0]) cp_vbus_path = env;
}

//////////////////////////////////////////////////////////////////////////////
// the usb_restore record
//
// A record of a repair this device owes the firmware, kept until the repair
// succeeds. wifi_restore is the same idea for the radio (app/netsetup.c:2444-
// 2483) and the policy is copied deliberately: the record's existence *is* the
// recovery state, so only a repair that worked may remove it.
//
// Keyed rather than positional because the two roles write it for different
// reasons - the gadget role owes a controller rebind, the host role owes a port
// role - and neither may erase the other's line.
//////////////////////////////////////////////////////////////////////////////

typedef struct {
	char key[16];
	char value[CP_LINE_MAX];   /* wide enough for a mount point or a command line */
} CP_RestoreLine;

static bool cp_restore_path(char* out, size_t cap) {
	int n = snprintf(out, cap, "%s/%s", NS_statePath(), CP_RESTORE_FILE);
	return n > 0 && (size_t)n < cap;
}

/* Unreadable lines are dropped rather than made to fail the file: the record's
 * job is to survive a crash, and half of it is worth more than none. */
static int cp_restore_load(CP_RestoreLine* lines, int max) {
	char path[CP_LINE_MAX];
	if (!cp_restore_path(path, sizeof(path))) return 0;

	FILE* f = fopen(path, "r");
	if (!f) return 0;

	int count = 0;
	char line[CP_LINE_MAX];
	while (count < max && fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		char* value = strchr(line, '=');
		if (!value) continue;
		*value++ = '\0';
		size_t key_len = strlen(line);
		if (!key_len || key_len >= sizeof(lines[count].key) ||
		    !cp_value_ok(value)) continue;
		memcpy(lines[count].key, line, key_len + 1);
		snprintf(lines[count].value, sizeof(lines[count].value), "%s", value);
		count++;
	}
	fclose(f);
	return count;
}

static bool cp_restore_save(const CP_RestoreLine* lines, int count) {
	char path[CP_LINE_MAX], temporary[CP_LINE_MAX + 32];
	if (!cp_restore_path(path, sizeof(path))) return false;
	if (count <= 0) return remove(path) == 0 || errno == ENOENT;
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%d", path, getpid()) >= (int)sizeof(temporary)) return false;

	FILE* f = fopen(temporary, "w");
	if (!f) return false;
	bool ok = true;
	for (int i = 0; i < count; i++)
		if (fprintf(f, "%s=%s\n", lines[i].key, lines[i].value) < 0) ok = false;
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
	if (fclose(f) != 0) ok = false;
	if (ok) ok = rename(temporary, path) == 0;
	if (!ok) remove(temporary);
	return ok;
}

/* Sets one key. An empty value removes it, and a record whose last key is
 * removed goes away: the file exists only while a repair is owed. */
static bool cp_restore_put(const char* key, const char* value) {
	CP_RestoreLine lines[CP_RESTORE_MAX_LINES];
	int count = cp_restore_load(lines, CP_RESTORE_MAX_LINES);

	int found = -1;
	for (int i = 0; i < count; i++)
		if (!strcmp(lines[i].key, key)) { found = i; break; }

	if (value && value[0]) {
		if (found < 0) {
			if (count >= CP_RESTORE_MAX_LINES) return false;
			found = count++;
			snprintf(lines[found].key, sizeof(lines[found].key), "%s", key);
		}
		snprintf(lines[found].value, sizeof(lines[found].value), "%s", value);
	} else if (found >= 0) {
		lines[found] = lines[--count];
	}

	return cp_restore_save(lines, count);
}

static bool cp_restore_get(const char* key, char* out, size_t cap) {
	CP_RestoreLine lines[CP_RESTORE_MAX_LINES];
	int count = cp_restore_load(lines, CP_RESTORE_MAX_LINES);

	if (out && cap) out[0] = '\0';
	for (int i = 0; i < count; i++)
		if (!strcmp(lines[i].key, key)) {
			if (out && cap) snprintf(out, cap, "%s", lines[i].value);
			return true;
		}
	return false;
}

//////////////////////////////////////////////////////////////////////////////
// paths, pidfile, status
//////////////////////////////////////////////////////////////////////////////

static bool cp_paths_init(void) {
	const char* state = NS_statePath();
	if (!state || !state[0]) return false;
	if (snprintf(cp_state_dir, sizeof(cp_state_dir), "%s", state) >= (int)sizeof(cp_state_dir)) return false;
	if (snprintf(cp_pid_path, sizeof(cp_pid_path), "%s/%s", state, CP_PID_FILE) >= (int)sizeof(cp_pid_path)) return false;
	if (snprintf(cp_status_path, sizeof(cp_status_path), "%s/%s", state, CP_STATUS_FILE) >= (int)sizeof(cp_status_path)) return false;
	if (snprintf(cp_log_path, sizeof(cp_log_path), "%s/%s", state, CP_LOG_FILE) >= (int)sizeof(cp_log_path)) return false;
	return true;
}

/* One generation, by size, before the daemon writes its first line.
 *
 * The rename has to happen here rather than in whatever started us: the log is
 * opened for append before this process exists, so a rotation done by someone
 * else would leave this process's descriptors on the rotated inode.
 * Re-opening after the rename and putting the result back on both descriptors
 * keeps the two ends of the log the same file.
 * NETPLAY_CABLE_LOG_MAX overrides the cap, as NETPLAY_WATCHDOG_LOG_MAX does for
 * launcher/wifi-watchdog.sh:56-62. */
static void cp_log_open(void) {
	long max = 65536;
	const char* env = getenv(CP_LOG_MAX_ENV);
	if (env && env[0]) {
		char* end = NULL;
		long value = strtol(env, &end, 10);
		if (end && !*end && value > 0) max = value;
	}

	struct stat st;
	if (stat(cp_log_path, &st) == 0 && st.st_size > max) {
		char backup[CP_LINE_MAX + 4];
		if (snprintf(backup, sizeof(backup), "%s.1", cp_log_path) < (int)sizeof(backup)) {
			remove(backup);
			rename(cp_log_path, backup);   /* a failure here only means the log keeps growing */
		}
	}

	int fd = open(cp_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) return;
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	if (fd > STDERR_FILENO) close(fd);
}

/* The pidfile is the single-instance lock and the app's handle on us
 * (app/broker.c:27-36). O_EXCL is what makes it one. */
static bool cp_pid_write(void) {
	int fd = open(cp_pid_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) return false;

	char pid[32];
	int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
	bool ok = write(fd, pid, (size_t)n) == n;
	close(fd);
	if (!ok) remove(cp_pid_path);
	return ok;
}

/* Whether the pid in the pidfile is still a running netplay-cable.
 *
 * The same rule the app applies (netsetup.c cable_pid): the file names a
 * process, and the process's own command line is what proves the number still
 * refers to it. Without this check a daemon killed by a crash, or by a device
 * that rebooted with the card still holding its pidfile, makes every later
 * launch refuse to start - reporting "another cable daemon owns the session"
 * about a process that does not exist. Observed on hardware, from exactly that
 * sequence. */
static bool cp_pid_live(void) {
	char text[32];
	if (!cp_read_attr(cp_pid_path, text, sizeof(text))) return false;

	pid_t pid = (pid_t)atoi(text);
	if (pid <= 1) return false;

	char path[64], cmdline[256];
	if (snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid) >= (int)sizeof(path)) return false;
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
	close(fd);
	if (n <= 0) return false;
	cmdline[n] = '\0';
	return strstr(cmdline, "netplay-cable.elf") != NULL;
}

static void cp_publish(void) {
	char text[CP_STATUS_MAX], temporary[CP_LINE_MAX + 32];
	int n = cp_status_write(&cp_status, text, sizeof(text));
	if (n <= 0) return;
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%d", cp_status_path, getpid()) >= (int)sizeof(temporary)) return;

	FILE* f = fopen(temporary, "w");
	if (!f) return;
	bool written = fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (fclose(f) == 0 && written) rename(temporary, cp_status_path);
	else remove(temporary);
}

/* Every status field goes through the checked setter, so a key that does not
 * exist or a value the reader would have to reject fails at the writer instead
 * of becoming a line nobody can read. A refusal here is a bug in this file, and
 * it is worth a line in the log rather than a silently missing field. */
static bool cp_status_put(const char* key, const char* value) {
	if (cp_status_set(&cp_status, key, value)) return true;
	cp_log("internal: the status grammar refused %s\n", key);
	return false;
}

/* Every failure goes through here: the state, the message the app will show and
 * the log line are one action rather than three that can disagree. */
static void cp_fail(const char* fmt, ...) {
	char message[CP_ERROR_MAX];

	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/* The status grammar refuses a value carrying a line break, because such a
	 * value would forge a line. Our own messages are formatted, not quoted, so
	 * the defence is here rather than at the caller. */
	for (char* p = message; *p; p++)
		if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) *p = ' ';

	cp_status_put("error", message);
	cp_status_set_state(&cp_status, CP_STATE_FAILED);
	cp_publish();
	cp_log("%s\n", message);
}

static void cp_stop_signal(int sig) {
	(void)sig;
	cp_stopping = 1;
}

/* A daemon that dies without saying so leaves a repair owed and nobody to
 * describe it - which is what the first hardware run did, and why every step of
 * the teardown below now says where it is. This names the signals a bug in this
 * file would raise, so the log ends with a reason instead of mid-sentence.
 *
 * write() rather than cp_log: this runs in a signal handler, cp_log is not
 * async-signal-safe, and stderr is already the log file (cp_log_open), so the
 * bytes land in the same place a normal line would. */
static void cp_fault_signal(int sig) {
	const char* name = sig == SIGSEGV ? "segmentation fault"
	                 : sig == SIGBUS  ? "bus error"
	                 : sig == SIGABRT ? "abort"
	                 : "fatal signal";
	char line[160];
	int n = snprintf(line, sizeof(line),
	                 "fatal: %s (%d) - the repair record is still on the card\n",
	                 name, sig);
	if (n > 0) {
		ssize_t ignored = write(STDERR_FILENO, line, (size_t)n);
		(void)ignored;
	}
	_exit(128 + sig);
}

//////////////////////////////////////////////////////////////////////////////
// the point-to-point interface
//
// Layer 3 with no packet information header, so a read gives one IP packet and
// there is nothing to strip. The interface belongs to this process: closing the
// descriptor destroys it, which is why nothing here removes it afterwards.
//////////////////////////////////////////////////////////////////////////////

static int cp_tun_open(char* err, int errlen) {
	int fd = open(cp_facts.tun_dev, O_RDWR);
	if (fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", cp_facts.tun_dev, strerror(errno));
		return -1;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = CP_IFF_TUN | CP_IFF_NO_PI;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", cp_facts.iface);

	if (ioctl(fd, CP_TUNSETIFF, &ifr) < 0) {
		snprintf(err, errlen, "cannot create %s: %s", cp_facts.iface, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* The address the kernel actually put on the interface, read back rather than
 * assumed: the status file's ip= is handed to the peer's side as the address to
 * reach, so it has to be what is, not what was asked for. */
static bool cp_iface_address(char* out, size_t cap) {
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return false;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", cp_facts.iface);

	bool found = false;
	if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
		const struct sockaddr_in* sin = (const struct sockaddr_in*)&ifr.ifr_addr;
		char text[INET_ADDRSTRLEN];
		if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) {
			snprintf(out, cap, "%s", text);
			found = true;
		}
	}
	close(s);
	return found;
}

/* Shelled out for the same reason the hotspot path does it (app/netsetup.c:
 * 2640-2642): `ip` is what this device has, and the address has to be on the
 * interface before the peer's first packet arrives. The address is then
 * confirmed by reading it back, because a command that returned zero is not
 * evidence that the address landed. */
static bool cp_iface_up(const char* ip, char* err, int errlen) {
	char command[512];
	snprintf(command, sizeof(command),
	         "ip addr add %s/%s dev %s 2>/dev/null; "
	         "ip link set dev %s mtu %d 2>/dev/null; "
	         "ip link set dev %s up 2>/dev/null",
	         ip, NS_CABLE_PREFIX, cp_facts.iface, cp_facts.iface, CP_MTU, cp_facts.iface);
	if (system(command) != 0) {
		snprintf(err, errlen, "cannot bring %s up", cp_facts.iface);
		return false;
	}

	for (int i = 0; i < CP_ADDR_WAIT_TICKS; i++) {
		char address[CP_ADDR_MAX];
		if (cp_iface_address(address, sizeof(address)) && !strcmp(address, ip)) return true;
		usleep(CP_TICK_MS * 1000);
	}

	snprintf(err, errlen, "%s came up without an address", cp_facts.iface);
	return false;
}

//////////////////////////////////////////////////////////////////////////////
// Brick's stock configfs gadget
//////////////////////////////////////////////////////////////////////////////

static bool cp_gadget_path(const char* name, const char* attr, char* out, size_t cap) {
	if (!name || !name[0] || !attr || !out || !cap) return false;

	char dir[CP_LINE_MAX];
	int n = snprintf(dir, sizeof(dir), "%s/usb_gadget/%s", cp_facts.configfs, name);
	if (n <= 0 || (size_t)n >= sizeof(dir)) return false;

	n = snprintf(out, cap, "%s/%s", dir, attr);
	return n > 0 && (size_t)n < cap;
}

/* configfs is where a gadget is built, and on this firmware it is already
 * mounted because the adbd gadget lives in it. Mounting is therefore the repair
 * for the case where it is not, rather than part of the happy path. */
static bool cp_configfs_ensure(char* err, int errlen) {
	char group[CP_LINE_MAX];
	if (snprintf(group, sizeof(group), "%s/usb_gadget", cp_facts.configfs) >= (int)sizeof(group)) {
		snprintf(err, errlen, "the configfs path does not fit");
		return false;
	}
	if (cp_dir_exists(group)) return true;

	if (mkdir(cp_facts.configfs, 0755) != 0 && errno != EEXIST) {
		snprintf(err, errlen, "cannot use %s: %s", cp_facts.configfs, strerror(errno));
		return false;
	}
	if (mount("none", cp_facts.configfs, "configfs", 0, NULL) != 0 && errno != EBUSY) {
		snprintf(err, errlen, "cannot mount configfs on %s: %s", cp_facts.configfs, strerror(errno));
		return false;
	}
	if (!cp_dir_exists(group)) {
		snprintf(err, errlen, "%s has no usb_gadget group - is the gadget stack built in?", cp_facts.configfs);
		return false;
	}
	return true;
}

static bool cp_udc_configured(const char* udc) {
	char path[CP_LINE_MAX], state[32];
	if (!udc || !udc[0]) return false;
	if (snprintf(path, sizeof(path), "%s/%s/state", cp_facts.udc_dir, udc) >= (int)sizeof(path)) return false;
	return cp_read_attr(path, state, sizeof(state)) && !strncmp(state, "configured", 10);
}

/* The packet size a record is delimited against, at the speed the controller
 * actually negotiated. The aligned case - the record whose length is an exact
 * multiple of the packet size, the one that needs an explicit terminating
 * packet - is the shape of 512 at high speed and of 64 at full speed, so the
 * answer has to come from the controller rather than from a constant. Anything
 * that is not high speed is treated as full speed: the descriptor blob declares
 * no super-speed list, because this SoC is USB 2.0. */
static unsigned cp_maxpacket_for(const char* udc) {
	char path[CP_LINE_MAX], speed[32];
	if (!udc || !udc[0]) return CP_MAXPACKET_FS;
	if (snprintf(path, sizeof(path), "%s/%s/current_speed", cp_facts.udc_dir, udc) >= (int)sizeof(path)) return CP_MAXPACKET_FS;
	if (!cp_read_attr(path, speed, sizeof(speed))) return CP_MAXPACKET_FS;
	return !strcmp(speed, "high-speed") ? CP_MAXPACKET_HS : CP_MAXPACKET_FS;
}

/* The mount source is the instance name *without* its ffs. prefix: configfs
 * names the directory ffs.cable and the mount has to name "cable"
 * (Documentation/usb/functionfs.txt, 4.9). Getting that wrong is an ENOENT that
 * reads like the filesystem is missing, which is why both spellings come from
 * one fact. */
static bool cp_ffs_mount(char* err, int errlen) {
	char source[CP_NAME_MAX];

	if (!cp_ffs_source(&cp_facts, source, sizeof(source))) {
		snprintf(err, errlen, "the functionfs source does not fit");
		return false;
	}
	if (!cp_dir_exists(cp_facts.ffs_dir) && !cp_mkdir_p(cp_facts.ffs_dir)) {
		snprintf(err, errlen, "cannot create %s: %s", cp_facts.ffs_dir, strerror(errno));
		return false;
	}
	if (mount(source, cp_facts.ffs_dir, "functionfs", 0, NULL) != 0) {
		/* A mount left by a previous run is a state we can use rather than a
		 * failure: the files in it belong to this instance name, and the
		 * kernel would refuse to mount a second one over the same directory. */
		if (errno != EBUSY) {
			snprintf(err, errlen, "cannot mount functionfs on %s: %s", cp_facts.ffs_dir, strerror(errno));
			return false;
		}
		cp_log("reusing the functionfs mount left on %s\n", cp_facts.ffs_dir);
	}
	cp_ffs_mounted = true;
	return true;
}

/* One write, the whole blob, or nothing. The kernel checks the blob's own
 * length field against the number of bytes written (drivers/usb/gadget/
 * function/f_fs.c: get_unaligned_le32(data + 4) != len), so a short write is a
 * rejected gadget rather than a smaller one. */
static bool cp_ep0_write(const uint8_t* blob, size_t len, const char* what, char* err, int errlen) {
	for (int attempt = 0; attempt < CP_EP0_ATTEMPTS; attempt++) {
		ssize_t n = write(cp_ep0, blob, len);
		if (n == (ssize_t)len) return true;
		if (n < 0 && errno == EAGAIN) { usleep(CP_TICK_MS * 1000); continue; }
		snprintf(err, errlen, "the kernel refused the %s blob: %s", what,
		         n < 0 ? strerror(errno) : "short write");
		return false;
	}
	snprintf(err, errlen, "the kernel would not take the %s blob", what);
	return false;
}

/* Open ep0, then hand the kernel the two blobs, in the only order it accepts.
 * Both count and layout are the pure layer's (app/cableproto.c), which is where
 * they are pinned byte for byte. */
static bool cp_ffs_open_ep0(char* err, int errlen) {
	char path[CP_LINE_MAX];
	uint8_t blob[CP_DESCS_MAX];

	if (!cp_ffs_ep_file(&cp_facts, "ep0", path, sizeof(path))) {
		snprintf(err, errlen, "the ep0 path does not fit");
		return false;
	}
	cp_ep0 = open(path, O_RDWR | O_NONBLOCK);
	if (cp_ep0 < 0) {
		snprintf(err, errlen, "cannot open cable ep0: %s", strerror(errno));
		return false;
	}

	/* The eventfd has to exist before the blob is written: the kernel takes it
	 * out of the descriptor header, so it cannot be attached afterwards. It is a
	 * Linux mechanism, so a host build (testing/test-cable.sh) has no gadget role
	 * at all - the host role, which is what a host can exercise, never uses one.
	 * See testing/cable-host-stubs.c. */
#if defined(__linux__)
	cp_evfd = eventfd(0, EFD_NONBLOCK);
	if (cp_evfd < 0) {
		snprintf(err, errlen, "cannot create an eventfd: %s", strerror(errno));
		return false;
	}
#else
	snprintf(err, errlen, "the gadget role needs a Linux eventfd");
	return false;
#endif

	int n = cp_build_descriptors(blob, sizeof(blob), cp_evfd);
	if (n <= 0) {
		snprintf(err, errlen, "cannot build the descriptor blob");
		return false;
	}
	if (!cp_ep0_write(blob, (size_t)n, "descriptors", err, errlen)) return false;

	n = cp_build_strings(blob, sizeof(blob));
	if (n <= 0) {
		snprintf(err, errlen, "cannot build the strings blob");
		return false;
	}
	if (!cp_ep0_write(blob, (size_t)n, "strings", err, errlen)) return false;
	return true;
}

/* The endpoint files appear when the strings blob is accepted: that write is
 * what makes the function active in 4.9 (f_fs.c: the strings branch of
 * ffs_ep0_write calls ffs_epfiles_create), and the kernel creates every
 * declared endpoint's file with it. There is no `ready` attribute to wait on in
 * this kernel - it arrives years later - so the files themselves are the
 * readiness signal, they are waited for with a deadline, and the controller is
 * only written after they are open.
 *
 * The transmit side is opened non-blocking so that a stalled host cannot park
 * the loop that also has to service ep0; the receive side is opened blocking,
 * because it is read from a thread of its own (see cp_rx_worker) and there is
 * nothing else for that thread to do. */
static bool cp_eps_open(char* err, int errlen) {
	char path[CP_LINE_MAX];

	for (int tick = 0; tick < CP_EP_WAIT_TICKS; tick++) {
		if (!cp_ffs_ep_file(&cp_facts, CP_EP_FILE_IN, path, sizeof(path))) {
			snprintf(err, errlen, "the %s path does not fit", CP_EP_FILE_IN);
			return false;
		}
		if (cp_ep_in < 0) cp_ep_in = open(path, O_RDWR | O_NONBLOCK);

		if (!cp_ffs_ep_file(&cp_facts, CP_EP_FILE_OUT, path, sizeof(path))) {
			snprintf(err, errlen, "the %s path does not fit", CP_EP_FILE_OUT);
			return false;
		}
		if (cp_ep_out < 0) cp_ep_out = open(path, O_RDWR);

		if (cp_ep_in >= 0 && cp_ep_out >= 0) {
			cp_log("endpoints ready: %s -> host, %s -> us\n", CP_EP_FILE_IN, CP_EP_FILE_OUT);
			return true;
		}
		usleep(CP_TICK_MS * 1000);
	}

	snprintf(err, errlen, "the gadget never presented its endpoints");
	return false;
}

/* The order inside this function is the whole point of it: the record is
 * written first, then the current owner is released, then ours is bound. A
 * crash between any two of those steps leaves a record that says what to put
 * back, which is the only thing that makes taking the controller defensible -
 * the firmware's adb lives on the other side of it. */
/* Offer the controller to a gadget until the gadget takes it.
 *
 * The write's return code says nothing useful on this UDC (2.4), and neither
 * does a single read-back: a bind that is too early - the common case being the
 * firmware's userspace restarted by the teardown, which writes the instance's
 * descriptors as it starts - fails with ENODEV and leaves the attribute empty.
 * Retrying is therefore the correct behaviour and not a workaround, and the tick
 * count is logged when it takes more than one so the next reader can see whether
 * the race is real. */
static void* cp_rx_worker(void* arg) {
	(void)arg;
	uint8_t buffer[CP_READ_MAX];
	const uint8_t* packet = NULL;

	while (!cp_stopping) {
		ssize_t n = read(cp_ep_out, buffer, sizeof(buffer));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;

			/* The peer went away or the endpoint was disabled under us: the
			 * same read is valid again as soon as a host configures the
			 * interface, so this is a pause rather than the end of the link -
			 * and the pause is bounded, because an endpoint that refuses every
			 * read must not become a spin.
			 *
			 * ECONNRESET and EPIPE belong here and are the *common* case: they
			 * are what a bumped cable produces, and treating them as fatal is
			 * what made an unplug end the daemon while the host end of the same
			 * cable simply waited. A session then had to be re-armed from
			 * scratch, with nothing on screen to say why. */
			if (errno == ESHUTDOWN || errno == EPROTO || errno == ENODEV ||
			    errno == EIO || errno == ECONNRESET || errno == EPIPE) {
				cp_log("receive endpoint reset: %s\n", strerror(errno));
				usleep(CP_TICK_MS * 1000);
				continue;
			}
			cp_rx_errno = errno;
			cp_rx_failed = 1;
			return NULL;
		}

		/* A zero-length read is the terminating packet that ended an aligned
		 * record: the record itself arrived in the transfer before it. */
		if (n == 0) continue;

		int len = cp_frame_decode(buffer, (size_t)n, &packet);
		if (len < 0) {
			/* The writer puts one whole record in one transfer, so a transfer
			 * that is not a whole record is a stream that has already lost its
			 * framing - and forwarding a guess from here would be worse than
			 * stopping. */
			cp_rx_errno = EPROTO;
			cp_rx_failed = 1;
			return NULL;
		}
		if (len == 0) continue;
		if (len == (int)sizeof(cp_hello) &&
		    !memcmp(packet, cp_hello, sizeof(cp_hello))) {
			cp_peer_last_seen = cp_now_s();
			cp_hello_ack_pending = 1;
			continue;
		}

		if (write(cp_tun_fd, packet, (size_t)len) < 0 && errno != EAGAIN) {
			cp_rx_errno = errno;
			cp_rx_failed = 1;
			return NULL;
		}
		cp_rx_records++;
	}
	return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// the data path, transmit side
//////////////////////////////////////////////////////////////////////////////

/* One packet from the interface into the record slot. The slot is only read
 * when it is empty, which is the backpressure: an endpoint that is not taking
 * packets stops the interface being read, instead of a queue that grows. */
static int cp_tun_read(void) {
	uint8_t packet[CP_READ_MAX];

	ssize_t n = read(cp_tun_fd, packet, sizeof(packet));
	if (n <= 0) return 0;

	int record = cp_frame_encode(cp_tx_record, sizeof(cp_tx_record), packet, (size_t)n);
	if (record <= 0) {
		cp_log("dropping a %d-byte packet the link cannot carry\n", (int)n);
		return 0;
	}
	return record;
}

/* Hands the waiting record to the endpoint, and writes the terminating packet
 * when the record's length is an exact multiple of the packet size - 4.9's f_fs
 * appends nothing for us, and a transfer with no short packet leaves the peer
 * waiting for the rest of something that will never come. The question is the
 * pure layer's, so both ends answer it the same way. */
static void cp_tx_flush(int* pending, unsigned maxpacket) {
	if (*pending <= 0) return;

	ssize_t n = write(cp_ep_in, cp_tx_record, (size_t)*pending);
	if (n == *pending) {
		if (cp_needs_terminator((size_t)*pending, maxpacket) && write(cp_ep_in, "", 0) < 0 && errno != EAGAIN)
			cp_log("terminating packet refused: %s\n", strerror(errno));
		*pending = 0;
		return;
	}

	/* Not ready: the slot keeps the record and the loop comes back to it. */
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;

	/* Anything else is the endpoint being gone. The record is dropped rather
	 * than retried forever: the transport above this is TCP, and a resend is
	 * something it already knows how to do. */
	cp_log("transmit endpoint: %s\n", n < 0 ? strerror(errno) : "short write");
	*pending = 0;
}

//////////////////////////////////////////////////////////////////////////////
// ep0 and the slow tick
//////////////////////////////////////////////////////////////////////////////

/* ep0 is where the kernel says what happened to the cable. Two of its events
 * matter here: ENABLE, which says a host has configured the interface so the
 * endpoints may be used, and DISABLE, which says they may not. A SETUP arrives
 * for a control request the composite layer did not handle; we send none, so an
 * unexpected one is answered with a zero-length write, which stalls a
 * control-OUT request and completes a control-IN one. Leaving it alone would
 * keep the host's transfer pending until it timed out. */
static void cp_ep0_events(void) {
	if (cp_evfd >= 0) {
		uint64_t counter;
		while (read(cp_evfd, &counter, sizeof(counter)) == (ssize_t)sizeof(counter)) ;
	}

	for (int i = 0; i < 8; i++) {
		CP_FFS_Event event;
		ssize_t n = read(cp_ep0, &event, sizeof(event));
		if (n < 0) {
			if (errno != EAGAIN && errno != EINTR) cp_log("ep0: %s\n", strerror(errno));
			return;
		}
		if (n != (ssize_t)sizeof(event)) return;

		switch (event.type) {
		case CP_FFS_EV_ENABLE:
			cp_log("a host configured us\n");
			break;
		case CP_FFS_EV_DISABLE:
			cp_log("the host released us\n");
			cp_peer_last_seen = 0;
			break;
		case CP_FFS_EV_SETUP:
			cp_log("refusing control request %02x/%02x\n", event.setup.bRequestType, event.setup.bRequest);
			if (write(cp_ep0, "", 0) < 0 && errno != EAGAIN) cp_log("ep0 reply: %s\n", strerror(errno));
			break;
		default:
			break;
		}
	}
}

/* The daemon's reason to exist is the session file, and the app's handle on it
 * is the pidfile (app/broker.c:98-104 states the same rule for the broker). A
 * session that has never been seen is not a session that has ended: the link
 * has to be up before the app writes the session, so there is a window in which
 * a running daemon legitimately has nothing to point at. */
static bool cp_lifetime_ok(void) {
	if (!cp_file_exists(cp_pid_path)) return false;
	if (NS_isArmed()) { cp_session_seen = true; return true; }
	if (!cp_session_seen) return true;
	cp_log("the session is gone\n");
	return false;
}

/* The cadence that is not the data path: the peer question, the lifetime
 * question and the status file. A second is what the app needs - it reads the
 * status file itself - and asking more often would put a stat() and a
 * session-file read in the way of every burst. Called once per
 * CP_SIDE_EFFECT_MS, not once per tick.
 *
 * Both questions are the caller's, because only the role knows how to ask them.
 * "Our side is up" and "the peer is here" are different answers and only the
 * second is worth showing: a gadget can be bound and enabled with the far end
 * unplugged, and a port can be in host mode with nothing on it. The gadget role
 * asks the controller; the host role knows whether it is holding a device. The
 * receive thread belongs to the gadget role alone and is passed in the same way,
 * so this function stays one implementation of one cadence.
 *
 * A failed state is left alone - it is terminal, and this tick must not quietly
 * turn it back into "waiting". */
static bool cp_slow_tick(bool peer_present, bool link_failed) {
	if (link_failed) {
		cp_fail("the receive endpoint stopped: %s", strerror(cp_rx_errno ? cp_rx_errno : EIO));
		return false;
	}

	if (cp_status.state != CP_STATE_FAILED) {
		if (cp_status.role == CP_ROLE_GADGET) {
			bool power_ok = peer_present ? BP_limit(cp_state_dir)
			              : cp_udc_configured(cp_status.udc) ? true
			              : BP_restore(cp_state_dir);
			if (!power_ok) {
				cp_fail("cannot keep peer-supplied charging disabled");
				return false;
			}
		}
		CP_State want = peer_present ? CP_STATE_UP : CP_STATE_WAIT;
		if (want != cp_status.state) {
			cp_status_set_state(&cp_status, want);
			cp_log("%s\n", peer_present ? "peer present - link up" : "no peer yet - waiting");
		}
	}

	if (!cp_lifetime_ok()) return false;
	cp_publish();
	return true;
}

//////////////////////////////////////////////////////////////////////////////
// bring-up, pump, teardown
//////////////////////////////////////////////////////////////////////////////

/* Brick's stock g1 owns adb. Keep that function and its USB identity intact;
 * add ffs.cable to c.1, with a repair breadcrumb before touching UDC. */
static bool cp_brick_paths(char* udc_path, char* function_path, char* link_path) {
	return cp_gadget_path("g1", "UDC", udc_path, CP_LINE_MAX) &&
	       cp_gadget_path("g1", "functions/ffs.cable", function_path, CP_LINE_MAX) &&
	       cp_gadget_path("g1", "configs/c.1/ffs.cable", link_path, CP_LINE_MAX);
}

/* An unreadable UDC attribute is not proof that the controller was released. */
static bool cp_brick_read_udc(const char* path, char* out, size_t len) {
	if (!len) return false;
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	ssize_t n = read(fd, out, len - 1);
	close(fd);
	if (n < 0) return false;
	out[n] = '\0';
	(void)cp_attr_trim(out);
	return true;
}

static bool cp_brick_unbind(const char* path) {
	char held[CP_NAME_MAX];
	for (int tick = 0; tick < CP_UNBIND_TICKS; tick++) {
		if (!cp_brick_read_udc(path, held, sizeof(held))) return false;
		if (!held[0]) return true;
		(void)cp_write_attr(path, "\n");
		usleep(CP_TICK_MS * 1000);
	}
	return cp_brick_read_udc(path, held, sizeof(held)) && !held[0];
}

static bool cp_brick_bind_once(const char* path, const char* udc) {
	char held[CP_NAME_MAX];
	for (int tick = 0; tick < CP_BIND_TICKS; tick++) {
		(void)cp_write_attr(path, udc);
		usleep(CP_TICK_MS * 1000);
		if (cp_brick_read_udc(path, held, sizeof(held)) && !strcmp(held, udc)) return true;
	}
	return false;
}

static bool cp_vbus_present(void) {
	char online[8];
	return cp_read_attr(cp_vbus_path, online, sizeof(online)) && online[0] == '1';
}

static bool cp_udc_attached(const char* udc) {
	char path[CP_LINE_MAX], state[32];
	if (snprintf(path, sizeof(path), "%s/%s/state", cp_facts.udc_dir, udc) >= (int)sizeof(path)) return false;
	/* No readable state is no evidence of a failure; only "not attached" is. */
	return !cp_read_attr(path, state, sizeof(state)) || strcmp(state, "not attached") != 0;
}

/* The UDC attribute reading back the controller's name is not proof that the
 * gadget started. Binding while the sunxi UDC is still tearing down the last
 * binding - a daemon restarted right after its predecessor was SIGKILLed, when
 * the kernel has just re-attached g1 by itself - logs "failed to start g1: -19"
 * yet leaves the name in place, and the controller then sits "not attached"
 * with VBUS present until another unbind and bind (seen 2 runs in 4,
 * 2026-11-14). So with VBUS present, the controller has to leave "not
 * attached"; if it does not, release it, let it settle, and bind again. Without
 * VBUS there is nothing to attach to, and the read-back is all there is. */
static bool cp_brick_bind(const char* path, const char* udc) {
	for (int attempt = 1; attempt <= CP_BIND_ATTEMPTS; attempt++) {
		if (!cp_brick_bind_once(path, udc)) return false;
		if (!cp_vbus_present()) return true;
		for (int tick = 0; tick < CP_UDC_ATTACH_TICKS; tick++) {
			if (cp_udc_attached(udc)) return true;
			usleep(CP_TICK_MS * 1000);
		}
		cp_log("g1 bound to %s but the controller never attached (attempt %d); rebinding\n",
		       udc, attempt);
		if (!cp_brick_unbind(path)) return false;
		usleep(CP_REBIND_PAUSE_MS * 1000);
	}
	cp_log("g1 is bound to %s but the controller will not attach\n", udc);
	return false;
}

static bool cp_brick_link_exists(const char* path) {
	struct stat st;
	return lstat(path, &st) == 0;
}

static void cp_brick_remove_unlinked(const char* function_path) {
	(void)umount2(cp_facts.ffs_dir, MNT_DETACH);
	if (rmdir(function_path) != 0 && errno != ENOENT)
		cp_log("function directory cleanup: %s\n", strerror(errno));
}

/* Works after SIGKILL as well as normal exit. The link leaves the configuration
 * before ep0 closes; otherwise the stock gadget may fail to bind and adb stays
 * unavailable. A failed unbind leaves everything in place for another repair. */
static bool cp_brick_repair(void) {
	char udc_path[CP_LINE_MAX], function_path[CP_LINE_MAX], link_path[CP_LINE_MAX];
	char udc[CP_NAME_MAX];
	if (!cp_brick_paths(udc_path, function_path, link_path)) return false;
	bool linked = cp_brick_link_exists(link_path);
	if (!cp_restore_get(CP_RESTORE_KEY_UDC, udc, sizeof(udc))) {
		if (linked) {
			cp_log("cable function remains linked without a saved UDC; refusing to guess\n");
			return false;
		}
		cp_brick_remove_unlinked(function_path);
		return true;
	}
	char bound[CP_NAME_MAX] = "";
	if (!cp_brick_read_udc(udc_path, bound, sizeof(bound))) return false;
	if (linked) {
		if (!cp_brick_unbind(udc_path)) {
			cp_log("cannot release g1; leaving repair record and function intact\n");
			return false;
		}
		if (unlink(link_path) != 0 && errno != ENOENT) return false;
		bound[0] = '\0';
	}
	if (bound[0] && strcmp(bound, udc)) {
		cp_log("g1 is bound to unexpected UDC %s; keeping repair record\n", bound);
		return false;
	}
	if (!bound[0] && !cp_brick_bind(udc_path, udc)) {
		cp_log("cannot rebind g1 to %s; keeping repair record\n", udc);
		return false;
	}
	if (cp_ep_in >= 0) { close(cp_ep_in); cp_ep_in = -1; }
	if (cp_ep_out >= 0) { close(cp_ep_out); cp_ep_out = -1; }
	if (cp_ep0 >= 0) { close(cp_ep0); cp_ep0 = -1; }
	if (cp_evfd >= 0) { close(cp_evfd); cp_evfd = -1; }
	cp_brick_remove_unlinked(function_path);
	cp_restore_put(CP_RESTORE_KEY_UDC, NULL);
	cp_gadget_bound = false;
	cp_ffs_mounted = false;
	cp_log("stock g1 rebound; adb function preserved\n");
	return true;
}

static bool cp_gadget_start(char* err, int errlen) {
	char udc_path[CP_LINE_MAX], function_path[CP_LINE_MAX], link_path[CP_LINE_MAX];
	char udc[CP_NAME_MAX];
	if (!cp_configfs_ensure(err, errlen) ||
	    !cp_brick_paths(udc_path, function_path, link_path)) {
		if (!err[0]) snprintf(err, errlen, "Brick gadget paths are unavailable");
		return false;
	}
	if (!cp_read_attr(udc_path, udc, sizeof(udc))) {
		snprintf(err, errlen, "stock g1 is not bound; use the bottom USB port");
		return false;
	}
	char adb_link[CP_LINE_MAX], adb_function[CP_LINE_MAX];
	if (!cp_gadget_path("g1", "configs/c.1/ffs.adb", adb_link, sizeof(adb_link)) ||
	    !cp_gadget_path("g1", "functions/ffs.adb", adb_function, sizeof(adb_function)) ||
	    !cp_brick_link_exists(adb_link) || !cp_dir_exists(adb_function)) {
		snprintf(err, errlen, "stock adb function is not in g1/c.1");
		return false;
	}
	if (cp_file_exists(link_path)) {
		snprintf(err, errlen, "cable function already linked; repair it first");
		return false;
	}
	cp_tun_fd = cp_tun_open(err, errlen);
	if (cp_tun_fd < 0 || !cp_iface_up(NS_CABLE_HOST_IP, err, errlen)) return false;
	cp_status_set_state(&cp_status, CP_STATE_GADGET);
	cp_publish();
	if (mkdir(function_path, 0755) != 0 && errno != EEXIST) {
		snprintf(err, errlen, "cannot create cable FunctionFS function: %s", strerror(errno));
		return false;
	}
	if (!cp_ffs_mount(err, errlen) || !cp_ffs_open_ep0(err, errlen) ||
	    !cp_eps_open(err, errlen)) return false;
	/* The record must be durable before the stock gadget is unbound. */
	if (!cp_restore_put(CP_RESTORE_KEY_UDC, udc)) {
		snprintf(err, errlen, "cannot save stock gadget controller");
		return false;
	}
	if (!cp_brick_unbind(udc_path)) {
		snprintf(err, errlen, "stock gadget would not release its controller");
		return false;
	}
	if (symlink(function_path, link_path) != 0) {
		snprintf(err, errlen, "cannot add cable beside adb: %s", strerror(errno));
		return false;
	}
	if (!cp_brick_bind(udc_path, udc)) {
		snprintf(err, errlen, "g1 could not bind with cable function");
		return false;
	}
	cp_gadget_bound = true;
	cp_facts.gadget = "g1";
	if (!cp_status_put("gadget", "g1") || !cp_status_put("udc", udc) ||
	    !cp_status_put("iface", cp_facts.iface) ||
	    !cp_status_put("ip", NS_CABLE_HOST_IP) ||
	    !cp_status_put("peer", NS_CABLE_CLIENT_IP)) {
		snprintf(err, errlen, "cannot publish cable status");
		return false;
	}
	cp_status_set_state(&cp_status, CP_STATE_WAIT);
	cp_publish();
	return true;
}

static void cp_gadget_teardown(void) {
	if (cp_brick_repair()) {
		if (!BP_restore(cp_state_dir)) cp_log("charger setting still needs repair\n");
	} else {
		cp_log("g1 still needs repair; usb_restore and charger settings retained\n");
	}
	if (cp_ep_in >= 0) { close(cp_ep_in); cp_ep_in = -1; }
	if (cp_ep_out >= 0) { close(cp_ep_out); cp_ep_out = -1; }
	if (cp_ep0 >= 0) { close(cp_ep0); cp_ep0 = -1; }
	if (cp_evfd >= 0) { close(cp_evfd); cp_evfd = -1; }
	if (cp_ffs_mounted) {
		char udc_path[CP_LINE_MAX], function_path[CP_LINE_MAX], link_path[CP_LINE_MAX];
		if (cp_brick_paths(udc_path, function_path, link_path) && !cp_file_exists(link_path)) {
			(void)umount2(cp_facts.ffs_dir, MNT_DETACH);
			(void)rmdir(function_path);
		}
	}
	if (cp_tun_fd >= 0) { close(cp_tun_fd); cp_tun_fd = -1; }
}

static void cp_gadget_pump(void) {
	struct pollfd fds[3];
	struct timeval last_tick;
	pthread_t rx;
	int pending = 0;

	fds[0].fd = cp_tun_fd; fds[0].events = POLLIN; fds[0].revents = 0;
	fds[1].fd = cp_ep0;    fds[1].events = POLLIN; fds[1].revents = 0;
	fds[2].fd = cp_evfd;   fds[2].events = POLLIN; fds[2].revents = 0;

	if (pthread_create(&rx, NULL, cp_rx_worker, NULL) != 0) {
		cp_fail("cannot start the receive thread: %s", strerror(errno));
		return;
	}

	unsigned maxpacket = cp_maxpacket_for(cp_status.udc);
	cp_log("gadget bound on %s, %s up as %s, %u-byte packets\n",
	       cp_status.udc, cp_facts.iface, NS_CABLE_HOST_IP, maxpacket);
	gettimeofday(&last_tick, NULL);

	while (!cp_stopping) {
		int n = poll(fds, 3, pending ? CP_TX_RETRY_MS : CP_TICK_MS);
		if (n < 0) {
			if (errno == EINTR) continue;
			cp_fail("poll: %s", strerror(errno));
			break;
		}

		if (fds[1].revents & POLLIN || fds[2].revents & POLLIN) cp_ep0_events();
		if (!pending && cp_hello_ack_pending) {
			pending = cp_frame_encode(cp_tx_record, sizeof(cp_tx_record),
			                          cp_hello_ack, sizeof(cp_hello_ack));
			cp_hello_ack_pending = 0;
		}
		if (!pending && (fds[0].revents & POLLIN)) pending = cp_tun_read();
		if (pending) cp_tx_flush(&pending, maxpacket);

		struct timeval now;
		gettimeofday(&now, NULL);
		if (cp_ms_between(&last_tick, &now) >= CP_SIDE_EFFECT_MS) {
			last_tick = now;
			/* The controller reports its speed only once a host has enumerated
			 * it, which is after bind: re-read it rather than keep the
			 * full-speed fallback for the whole session. */
			unsigned now_maxpacket = cp_maxpacket_for(cp_status.udc);
			if (now_maxpacket != maxpacket) {
				cp_log("negotiated %u-byte packets\n", now_maxpacket);
				maxpacket = now_maxpacket;
			}
			if (!cp_slow_tick(cp_udc_configured(cp_status.udc) && cp_peer_recent(), cp_rx_failed)) break;
		}
	}

	/* Cancelled rather than signalled: the thread is parked in a read() on a
	 * file that has no poll() to wake it, and read is a cancellation point, so
	 * this both interrupts the transfer inside the kernel and ends the thread
	 * before the descriptors underneath it are closed. Without the join the
	 * endpoints would be closed while that read is still in flight. */
	pthread_cancel(rx);
	pthread_join(rx, NULL);
	cp_log("received %d records\n", (int)cp_rx_records);
}

static void cp_gadget_run(void) {
	char err[CP_ERROR_MAX];

	if (!cp_gadget_start(err, sizeof(err))) {
		cp_fail("%s", err);
		cp_gadget_teardown();
		return;
	}
	cp_gadget_pump();
	cp_gadget_teardown();
}

// the host role
//
// The enumerating end of the cable. Three things happen in order: the port is
// switched to host (the role child above, because that write is the one that
// can wedge), the peer's gadget is found on the usbfs bus and claimed, and its
// two bulk endpoints are driven with asynchronous URBs bridged to the same TUN
// the gadget role builds.
//
// Asynchronous rather than USBDEVFS_BULK, and the reason is a property of usbfs
// rather than a preference: only one ioctl may be in flight on a device file at
// a time, so a synchronous read and a synchronous write on the same descriptor
// would serialize the two directions of a bridge whose entire job is to carry
// both at once. The cost is that every transfer needs memory that outlives the
// call, which is why the two URBs are file scope.
//
// Nothing here runs on a thread. usbfs does implement poll() - usbdev_poll
// reports POLLOUT once a completion is waiting - so unlike the gadget role's
// endpoint files, the device file can join the main loop's poll set. The set is
// named `link` rather than `fds` so that it reads separately from the gadget
// role's three-entry one; they describe different things and only one of them is
// pollable for the reason that matters.
//////////////////////////////////////////////////////////////////////////////

typedef enum {
	CP_LINK_OK = 0,   /* nothing happened that ends the attachment */
	CP_LINK_GONE,     /* the peer is no longer on the bus; keep waiting */
	CP_LINK_BROKEN,   /* something that is not our writer is on the pipe */
} CP_LinkResult;

/* One descriptor read, synchronously. usbfs's synchronous control transfer is
 * right here and nowhere else: these reads happen before anything is claimed or
 * running, and a descriptor is a few dozen bytes with no second direction to
 * interleave. */
static int cp_usb_descriptor(uint8_t type, void* out, size_t cap) {
	if (!out || !cap || cap > CP_DESC_MAX) return -1;

	CP_CtrlTransfer transfer;
	memset(&transfer, 0, sizeof(transfer));
	transfer.bRequestType = (uint8_t)(CP_USB_DIR_IN | CP_USB_RECIP_DEVICE);
	transfer.bRequest = (uint8_t)CP_USB_REQ_GET_DESCRIPTOR;
	transfer.wValue = (uint16_t)(type << 8);   /* descriptor type, index 0 */
	transfer.wLength = (uint16_t)cap;
	transfer.timeout = CP_USB_CTRL_TIMEOUT_MS;
	transfer.data = out;

	return (int)ioctl(cp_usb_fd, CP_USBDEVFS_CONTROL, &transfer);
}

/* The interface and the two endpoints, out of the configuration descriptor.
 *
 * The check is not decoration. The interface's class, its endpoint count and the
 * endpoints' transfer type together are what distinguish our peer from anything
 * else that may be on this bus, and the pair of ids above is unassigned
 * precisely so that a device claiming it is a device we built. A configuration
 * that does not match is left alone rather than guessed at.
 *
 * Walks by bLength, never by a fixed stride: descriptors are a byte stream whose
 * types are interleaved, and a reader that steps by the size it expected
 * desynchronizes at the first descriptor it did not know about - which is
 * exactly how the interface's own class-specific extras would arrive, if a
 * future revision ever sent any.
 *
 * The first matching interface wins; a second one is a shape this link does not
 * have. */
static bool cp_usb_parse_config(const uint8_t* config, size_t total, unsigned* interface_out,
                                uint8_t* in_out, uint8_t* out_out, unsigned* packet_out) {
	if (!config || total < 9 || config[1] != (uint8_t)CP_DT_CONFIG) return false;

	size_t at = config[0];
	while (at + 2 <= total) {
		size_t length = config[at];
		if (length < 2 || at + length > total) return false;
		if (config[at + 1] != (uint8_t)CP_DT_INTERFACE || length < 9) { at += length; continue; }

		unsigned count = 0, packet = 0;
		uint8_t in_addr = 0, out_addr = 0;
		bool bulk_only = true;

		size_t next = at + length;
		while (next + 2 <= total) {
			size_t entry = config[next];
			if (entry < 2 || next + entry > total) return false;
			if (config[next + 1] == (uint8_t)CP_DT_INTERFACE) break;

			if (config[next + 1] == (uint8_t)CP_DT_ENDPOINT && entry >= 7) {
				uint8_t address = config[next + 2];
				unsigned size = (unsigned)(config[next + 4] | ((unsigned)config[next + 5] << 8));
				size &= CP_MAXPACKET_MASK;
				if (size > CP_MAXPACKET_HS) size = CP_MAXPACKET_HS;

				if (!cp_ep_is_bulk(config[next + 3])) bulk_only = false;
				else if (cp_ep_dir(address) == CP_EP_IN)  { in_addr  = address; if (size > packet) packet = size; }
				else if (cp_ep_dir(address) == CP_EP_OUT) { out_addr = address; if (size > packet) packet = size; }
				else bulk_only = false;
				count++;
			}
			next += entry;
		}

		if (config[at + 5] == (uint8_t)CP_INTERFACE_CLASS_VENDOR &&
		    config[at + 6] == (uint8_t)CP_INTERFACE_SUBCLASS &&
		    config[at + 7] == (uint8_t)CP_INTERFACE_PROTOCOL && count == CP_BULK_ENDPOINTS &&
		    bulk_only && in_addr && out_addr && packet) {
			*interface_out = config[at + 2];
			*in_out = in_addr;
			*out_out = out_addr;
			*packet_out = packet;
			return true;
		}
		at = next;
	}
	return false;
}

/* Check the cable interface in sysfs before opening usbfs. Opening the wrong
 * node can hang in Brick's runtime-PM path; root hubs have no such interface.
 * The bus must also belong to the permanent top-port controller. */
static bool cp_bus_is_top_host(unsigned bus);

/* Select only an interface bearing our subclass and protocol, and only on
 * Brick's permanent top-port controller. Reading sysfs first avoids opening
 * root-hub usbfs nodes, which can hang in the vendor runtime-PM path. */
static bool cp_host_scan(char* node_out, size_t cap, unsigned* bus_out) {
	DIR* devices = opendir(cp_usb_sysfs);
	if (!devices) return false;
	bool found = false;
	struct dirent* entry;
	while (!found && (entry = readdir(devices)) != NULL) {
		if (!strchr(entry->d_name, ':')) continue;
		char dir[CP_LINE_MAX], path[CP_LINE_MAX], value[32];
		if (snprintf(dir, sizeof(dir), "%s/%s", cp_usb_sysfs, entry->d_name) >= (int)sizeof(dir)) continue;
		if (snprintf(path, sizeof(path), "%s/bInterfaceClass", dir) >= (int)sizeof(path) ||
		    !cp_read_attr(path, value, sizeof(value)) || strtoul(value, NULL, 16) != CP_INTERFACE_CLASS_VENDOR) continue;
		if (snprintf(path, sizeof(path), "%s/bInterfaceSubClass", dir) >= (int)sizeof(path) ||
		    !cp_read_attr(path, value, sizeof(value)) || strtoul(value, NULL, 16) != CP_INTERFACE_SUBCLASS) continue;
		if (snprintf(path, sizeof(path), "%s/bInterfaceProtocol", dir) >= (int)sizeof(path) ||
		    !cp_read_attr(path, value, sizeof(value)) || strtoul(value, NULL, 16) != CP_INTERFACE_PROTOCOL) continue;
		char parent[CP_LINE_MAX];
		snprintf(parent, sizeof(parent), "%s", entry->d_name);
		char* colon = strchr(parent, ':');
		if (!colon) continue;
		*colon = '\0';
		char bus[16], dev[16];
		if (snprintf(path, sizeof(path), "%s/%s/busnum", cp_usb_sysfs, parent) >= (int)sizeof(path) ||
		    !cp_read_attr(path, bus, sizeof(bus))) continue;
		if (snprintf(path, sizeof(path), "%s/%s/devnum", cp_usb_sysfs, parent) >= (int)sizeof(path) ||
		    !cp_read_attr(path, dev, sizeof(dev))) continue;
		unsigned bus_number = (unsigned)strtoul(bus, NULL, 10);
		if (!cp_bus_is_top_host(bus_number)) continue;
		int n = snprintf(node_out, cap, "%s/%03u/%03u", cp_usb_dir,
		                 bus_number, (unsigned)strtoul(dev, NULL, 10));
		if (n > 0 && (size_t)n < cap) {
			if (bus_out) *bus_out = bus_number;
			found = true;
		}
	}
	closedir(devices);
	return found;
}

static bool cp_bus_is_top_host(unsigned bus) {
	char dir[CP_LINE_MAX], target[CP_LINE_MAX];

	/* A missing controller identity cannot prove this is the top port. */
	if (!cp_facts.top_hcd) return false;
	if (!bus || snprintf(dir, sizeof(dir), "%s/usb%u", cp_usb_sysfs, bus) >= (int)sizeof(dir)) return false;
	ssize_t n = readlink(dir, target, sizeof(target) - 1);
	if (n <= 0) return false;
	target[n] = '\0';
	return strstr(target, cp_facts.top_hcd) != NULL;
}

/* Claim the interface and learn the two endpoints.
 *
 * The claim is explicit even though usbfs claims an interface on first use: this
 * interface is the peer's, it has no driver, and saying so is cheaper to read
 * than the warning usbfs logs when something is used without being claimed. A
 * refused claim is a real answer - another process already has the device - and
 * is reported as one.
 *
 * Failure leaves the device file open on purpose: the caller's detach closes it,
 * so there is exactly one place that knows how to undo an attachment. */
static bool cp_host_open(const char* node, char* err, int errlen) {
	uint8_t config[CP_DESC_MAX];

	cp_usb_fd = open(node, O_RDWR | O_NONBLOCK);
	if (cp_usb_fd < 0) {
		snprintf(err, errlen, "cannot open USB peer: %s", strerror(errno));
		return false;
	}

	int have = cp_usb_descriptor(CP_DT_CONFIG, config, 9);
	if (have < 9) {
		snprintf(err, errlen, "cannot read the configuration header: %s", strerror(errno));
		return false;
	}

	/* The header only, first: wTotalLength is what says how much there is to
	 * read, and it is not known before the header arrives. A device that claims
	 * more than this link could ever be told to do is refused rather than
	 * allocated for. */
	size_t total = (size_t)(config[2] | ((size_t)config[3] << 8));
	if (total < 9 || total > sizeof(config)) {
		snprintf(err, errlen, "the peer's configuration is %d bytes", (int)total);
		return false;
	}

	have = cp_usb_descriptor(CP_DT_CONFIG, config, total);
	if (have < (int)total) {
		/* A short read here is not a smaller configuration: the walk below
		 * would run past the end of what arrived. */
		snprintf(err, errlen, "the configuration came back short: %d of %d bytes", have, (int)total);
		return false;
	}

	unsigned interface = 0;
	if (!cp_usb_parse_config(config, total, &interface,
	                         &cp_usb_in_addr, &cp_usb_out_addr, &cp_usb_maxpacket)) {
		snprintf(err, errlen, "the peer is not presenting a cable link");
		return false;
	}

	unsigned int number = interface;
	if (ioctl(cp_usb_fd, CP_USBDEVFS_CLAIMINTERFACE, &number) < 0) {
		snprintf(err, errlen, "cannot claim interface %u: %s", interface, strerror(errno));
		return false;
	}

	cp_log("%s: interface %u claimed, %02x in / %02x out, %u-byte packets\n",
	       node, interface, (unsigned)cp_usb_in_addr, (unsigned)cp_usb_out_addr, cp_usb_maxpacket);
	return true;
}

/* Closing is the whole teardown: usbfs releases the claim and kills every URB
 * still in flight when the device file is released, so neither is undone by hand
 * here - doing so would be a second implementation of a rule the kernel already
 * applies. */
static void cp_host_detach(void) {
	if (cp_usb_fd >= 0) close(cp_usb_fd);
	cp_usb_fd = -1;
	cp_urb_out_in_flight = false;
	cp_usb_in_addr = 0;
	cp_usb_out_addr = 0;
	cp_usb_maxpacket = CP_MAXPACKET_HS;
	cp_peer_last_seen = 0;
	cp_host_last_hello = 0;
}

/* One receive URB, always in flight while a peer is attached.
 *
 * No USBDEVFS_URB_SHORT_NOT_OK, and that is the point: a short transfer is not an
 * error on this link, it is the only thing that ends a record. The writer's
 * two-byte length prefix exists because a bulk transfer carries no length of its
 * own, and asking the controller to treat a short packet as a failure would
 * reject every record whose length is not a multiple of the packet size - which
 * is most of them. */
static bool cp_urb_in_submit(void) {
	memset(&cp_urb_in, 0, sizeof(cp_urb_in));
	cp_urb_in.type = (uint8_t)CP_URB_TYPE_BULK;
	cp_urb_in.endpoint = cp_usb_in_addr;
	cp_urb_in.buffer = cp_urb_in_buffer;
	cp_urb_in.buffer_length = (int32_t)sizeof(cp_urb_in_buffer);

	if (ioctl(cp_usb_fd, CP_USBDEVFS_SUBMITURB, &cp_urb_in) < 0) return false;
	return true;
}

/* One transmit URB for one record.
 *
 * The zero-length packet the kernel appends when the flag is set is the host-side
 * half of the rule the gadget role applies by writing an empty transfer: 4.9's
 * f_fs appends nothing for us, and a record whose length is an exact multiple of
 * the packet size has no short packet of its own to end it. The question is the
 * pure layer's, so both ends answer it the same way. */
static bool cp_urb_out_submit(int length) {
	memset(&cp_urb_out, 0, sizeof(cp_urb_out));
	cp_urb_out.type = (uint8_t)CP_URB_TYPE_BULK;
	cp_urb_out.endpoint = cp_usb_out_addr;
	cp_urb_out.buffer = cp_tx_record;
	cp_urb_out.buffer_length = (int32_t)length;
	if (cp_needs_terminator((size_t)length, cp_usb_maxpacket)) cp_urb_out.flags |= CP_URB_ZERO_PACKET;

	if (ioctl(cp_usb_fd, CP_USBDEVFS_SUBMITURB, &cp_urb_out) < 0) return false;
	cp_urb_out_in_flight = true;
	return true;
}

/* A completed receive transfer.
 *
 * Two lengths are legal and mean nothing to forward: the record itself, one
 * transfer long, and the empty transfer that ends an aligned one. Anything else
 * means the bytes coming back are not our records - the same conclusion the
 * gadget role's receive thread draws, and for the same reason: the writer puts
 * one whole record in one transfer, so a transfer that is not a whole record is a
 * stream that has already lost its framing, and forwarding a guess from it would
 * be worse than stopping. */
static CP_LinkResult cp_host_deliver(size_t length) {
	const uint8_t* packet = NULL;

	if (length > sizeof(cp_urb_in_buffer)) return CP_LINK_BROKEN;

	int len = cp_frame_decode(cp_urb_in_buffer, length, &packet);
	if (len < 0) {
		cp_log("a %d-byte transfer is not a record\n", (int)length);
		return CP_LINK_BROKEN;
	}
	if (len == 0) return CP_LINK_OK;
	if (len == (int)sizeof(cp_hello_ack) &&
	    !memcmp(packet, cp_hello_ack, sizeof(cp_hello_ack))) {
		cp_peer_last_seen = cp_now_s();
		return CP_LINK_OK;
	}

	if (write(cp_tun_fd, packet, (size_t)len) < 0 && errno != EAGAIN) {
		cp_log("cannot hand a packet to %s: %s\n", cp_facts.iface, strerror(errno));
		return CP_LINK_BROKEN;
	}
	return CP_LINK_OK;
}

/* Everything the controller has finished, and the receive URB back in the kernel.
 *
 * The kernel hands the same struct back that it was given, with status and
 * actual_length filled in - which is why the URBs are file scope rather than
 * automatic: the struct *is* the kernel's handle on the transfer. */
static CP_LinkResult cp_host_reap(void) {
	for (int i = 0; i < CP_REAP_MAX; i++) {
		CP_Urb* done = NULL;

		/* The non-blocking reap, not USBDEVFS_REAPURB: this runs because poll()
		 * said a completion was waiting, and the blocking form would be a
		 * second place to wait that no deadline covers. */
		if (ioctl(cp_usb_fd, CP_USBDEVFS_REAPURBNDELAY, &done) < 0) return CP_LINK_OK;

		if (done == &cp_urb_in) {
			if (cp_urb_in.status != 0) {
				int status = cp_urb_in.status;
				cp_log("receive urb: %s\n", strerror(-status));

				/* The peer being gone is the ordinary end of a session rather
				 * than an error: the cable was pulled, or its daemon stopped. */
				if (status == -ENODEV || status == -ESHUTDOWN || status == -ECONNRESET) return CP_LINK_GONE;

				unsigned int address = cp_usb_in_addr;
				if (ioctl(cp_usb_fd, CP_USBDEVFS_CLEAR_HALT, &address) < 0)
					cp_log("cannot clear the receive endpoint: %s\n", strerror(errno));
				/* An endpoint that keeps failing fails instantly, so the retry
				 * is paced: an unpaced one would be a busy loop. */
				usleep(CP_TICK_MS * 1000);
			} else if (cp_urb_in.actual_length > 0) {
				CP_LinkResult result = cp_host_deliver((size_t)cp_urb_in.actual_length);
				if (result != CP_LINK_OK) return result;
			}

			if (!cp_urb_in_submit()) {
				cp_log("cannot keep receiving: %s\n", strerror(errno));
				return CP_LINK_GONE;
			}
		} else if (done == &cp_urb_out) {
			cp_urb_out_in_flight = false;
			/* A refused record is dropped rather than retried: the transport
			 * above this is TCP, and a resend is something it already knows how
			 * to do. */
			if (cp_urb_out.status != 0) cp_log("transmit urb: %s\n", strerror(-cp_urb_out.status));
		}
	}
	return CP_LINK_OK;
}

/* One attempt at becoming the peer's host.
 *
 * Every failure drops what it built, because the next attempt starts from
 * nothing: the bus is re-scanned, not resumed. "No peer on the bus" is not an
 * error and leaves err empty - it is the ordinary state of a cable with nothing
 * plugged into it. */
static bool cp_host_attach(char* err, int errlen) {
	char node[CP_LINE_MAX];
	unsigned bus = 0;

	if (errlen) err[0] = '\0';
	if (!cp_host_scan(node, sizeof(node), &bus)) return false;

	if (!cp_host_open(node, err, errlen)) {
		cp_host_detach();
		return false;
	}
	if (!cp_urb_in_submit()) {
		snprintf(err, errlen, "cannot start receiving: %s", strerror(errno));
		cp_host_detach();
		return false;
	}

	return true;
}

static void cp_host_pump(void) {
	struct pollfd link[2];
	struct timeval last_tick;
	int attach_ticks = 0;

	gettimeofday(&last_tick, NULL);

	while (!cp_stopping) {
		link[0].fd = cp_tun_fd; link[0].events = POLLIN;  link[0].revents = 0;
		link[1].fd = cp_usb_fd; link[1].events = POLLOUT; link[1].revents = 0;

		int n = poll(link, 2, CP_TICK_MS);
		if (n < 0) {
			if (errno == EINTR) continue;
			cp_fail("poll: %s", strerror(errno));
			break;
		}

		if (cp_usb_fd >= 0) {
			/* A device file is hung up by usbfs when the device goes away,
			 * which is the fastest notice of an unplug there is. */
			if (link[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				cp_log("the peer left the bus\n");
				cp_host_detach();
				cp_status_set_state(&cp_status, CP_STATE_WAIT);
				cp_publish();
			} else if (link[1].revents & POLLOUT) {
				CP_LinkResult result = cp_host_reap();

				if (result == CP_LINK_BROKEN) {
					cp_fail("the cable link lost its framing");
					break;
				}
				if (result == CP_LINK_GONE) {
					cp_log("the peer is gone; waiting for it to come back\n");
					cp_host_detach();
					cp_status_set_state(&cp_status, CP_STATE_WAIT);
					cp_publish();
				}
			}
		}

		/* A versioned heartbeat proves that the other endpoint runs our bridge.
		 * Merely seeing a USB device (or a configured gadget) is not enough. */
		if (!cp_urb_out_in_flight && cp_usb_fd >= 0 &&
		    cp_now_s() - cp_host_last_hello >= CP_HELLO_INTERVAL) {
			int record = cp_frame_encode(cp_tx_record, sizeof(cp_tx_record),
			                             cp_hello, sizeof(cp_hello));
			if (record > 0 && cp_urb_out_submit(record)) cp_host_last_hello = cp_now_s();
		}

		/* A packet is read from the interface only when the endpoint has room
		 * for it: one record in flight at a time, which is the backpressure - an
		 * endpoint that is not draining stops the interface being read rather
		 * than filling a queue that can overflow. */
		if (!cp_urb_out_in_flight && cp_usb_fd >= 0 && (link[0].revents & POLLIN)) {
			int record = cp_tun_read();
			if (record > 0 && !cp_urb_out_submit(record)) cp_log("cannot send: %s\n", strerror(errno));
		}

		if (cp_usb_fd < 0 && cp_status.state != CP_STATE_FAILED && ++attach_ticks >= CP_ATTACH_TICKS) {
			char err[CP_ERROR_MAX];

			attach_ticks = 0;
			if (cp_host_attach(err, sizeof(err))) {
				cp_status_set_state(&cp_status, CP_STATE_WAIT);
				cp_publish();
				cp_log("cable interface found; waiting for bridge heartbeat\n");
			} else if (err[0]) {
				/* A device that answers to our ids and then refuses to be
				 * opened is worth a line: it is not the ordinary "no cable
				 * yet", and it is the shape a half-built peer takes. */
				cp_log("%s\n", err);
			}
		}

		struct timeval now;
		gettimeofday(&now, NULL);
		if (cp_ms_between(&last_tick, &now) >= CP_SIDE_EFFECT_MS) {
			last_tick = now;
			if (!cp_slow_tick(cp_usb_fd >= 0 && cp_peer_recent(), false)) break;
		}
	}
}

/* The port first, then the interface, then the peer. Same order as the gadget
 * role's, and for the same reason: the step that touches the platform comes
 * before the step that expects something of it. */
/* Brick's top socket is host-only and already active. Never read the vendor
 * role-trigger nodes: those reads switch the bottom port and can strand adb. */
static bool cp_host_start(char* err, int errlen) {
	cp_status_set_state(&cp_status, CP_STATE_ROLE);
	cp_publish();
	if (!cp_facts.top_hcd || strcmp(cp_facts.top_hcd, "5200000.ehci1")) {
		snprintf(err, errlen, "this build supports the Brick top USB port only");
		return false;
	}
	if (!cp_status_put("iface", cp_facts.iface) ||
	    !cp_status_put("ip", NS_CABLE_CLIENT_IP) ||
	    !cp_status_put("peer", NS_CABLE_HOST_IP)) return false;
	cp_tun_fd = cp_tun_open(err, errlen);
	if (cp_tun_fd < 0 || !cp_iface_up(NS_CABLE_CLIENT_IP, err, errlen)) return false;
	cp_status_set_state(&cp_status, CP_STATE_WAIT);
	cp_publish();
	return true;
}

static void cp_host_teardown(void) {
	cp_host_detach();
	if (cp_tun_fd >= 0) { close(cp_tun_fd); cp_tun_fd = -1; }
}

static void cp_host_run(void) {
	char err[CP_ERROR_MAX];

	if (!cp_host_start(err, sizeof(err))) {
		cp_fail("%s", err);
		cp_host_teardown();
		return;
	}
	cp_host_pump();
	cp_host_teardown();
}

//////////////////////////////////////////////////////////////////////////////
// entry point
//////////////////////////////////////////////////////////////////////////////

static void cp_usage(FILE* out) {
	fprintf(out, "usage: netplay-cable --role gadget|host | --stop | --repair | --probe\n");
}

int main(int argc, char** argv) {
	CP_Role role = CP_ROLE_GADGET;
	bool role_given = false;
	bool stop = false, repair = false, probe = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--role") && i + 1 < argc) {
			if (!cp_role_from_name(argv[++i], &role)) {
				fprintf(stderr, "netplay-cable: unknown role '%s'\n", argv[i]);
				return 2;
			}
			role_given = true;
		} else if (!strcmp(argv[i], "--help")) {
			cp_usage(stdout);
			return 0;
		} else if (!strcmp(argv[i], "--stop")) {
			stop = true;
		} else if (!strcmp(argv[i], "--repair")) {
			repair = true;
		} else if (!strcmp(argv[i], "--probe")) {
			probe = true;
		} else {
			fprintf(stderr, "netplay-cable: unexpected argument '%s'\n", argv[i]);
			cp_usage(stderr);
			return 2;
		}
	}
	if ((role_given ? 1 : 0) + (stop ? 1 : 0) +
	    (repair ? 1 : 0) + (probe ? 1 : 0) != 1) {
		cp_usage(stderr);
		return 2;
	}
	/* The role comes from the command line, not from the session file: on both
	 * ends the link has to be up *before* the app arms the session, which is the
	 * same ordering the WiFi paths use. Both roles are served here, and which
	 * one runs is the only difference between them. */

	NS_init();
	cp_facts_init();
	if (probe) {
		char node[CP_LINE_MAX];
		if (!cp_host_scan(node, sizeof(node), NULL)) return 1;
		puts(node);
		return 0;
	}

	if (!cp_paths_init()) {
		fprintf(stderr, "netplay-cable: the state directory is unusable\n");
		return 1;
	}
	if (stop || repair) {
		if (stop && cp_pid_live()) {
			char pid[32];
			if (!cp_read_attr(cp_pid_path, pid, sizeof(pid)) ||
			    kill((pid_t)atoi(pid), SIGTERM) != 0) return 1;
			for (int tick = 0; tick < CP_UNBIND_TICKS && cp_pid_live(); tick++)
				usleep(CP_TICK_MS * 1000);
		}
		if (cp_pid_live()) return 1;  /* never repair beneath a running daemon */
		cp_log_open();
		bool ok = cp_brick_repair() && BP_restore(cp_state_dir);
		if (ok) { remove(cp_pid_path); remove(cp_status_path); }
		return ok ? 0 : 1;
	}

	/* Own our output before saying anything: the file is opened for append
	 * before this process exists, so rotating it is only correct here. */
	cp_log_open();
	cp_log("starting as %s (pid %d)\n", cp_role_name(role), (int)getpid());

	/* A pidfile left by a daemon that crashed, or by a reboot that outran its
	 * cleanup, names nobody. It is cleared rather than obeyed, so the next
	 * launch is not refused by a process that is not there. */
	if (cp_file_exists(cp_pid_path) && !cp_pid_live()) {
		cp_log("clearing a pidfile left by a daemon that is gone\n");
		remove(cp_pid_path);
	}

	if (!cp_pid_write()) {
		cp_log("another cable daemon owns the session: %s\n", strerror(errno));
		return 1;
	}

	signal(SIGINT, cp_stop_signal);
	signal(SIGTERM, cp_stop_signal);
	signal(SIGHUP, cp_stop_signal);

	/* Dying mid-restore is the one failure this process must not have silently:
	 * it owns the controller and a breadcrumb naming what to put back, and the
	 * next launch reads that breadcrumb. These say which signal it was. */
	struct sigaction fault;
	memset(&fault, 0, sizeof(fault));
	fault.sa_handler = cp_fault_signal;
	sigaction(SIGSEGV, &fault, NULL);
	sigaction(SIGBUS, &fault, NULL);
	sigaction(SIGABRT, &fault, NULL);

	cp_status_init(&cp_status, role);
	cp_publish();

	/* Repair a killed predecessor before creating another FunctionFS instance.
	 * This also makes the command safe to run after an interrupted session. */
	if (!cp_brick_repair()) {
		cp_fail("the stock USB gadget needs repair before another cable run");
		remove(cp_pid_path);
		return 1;
	}
	if (!BP_restore(cp_state_dir)) {
		cp_fail("the charger setting needs repair before another cable run");
		remove(cp_pid_path);
		return 1;
	}

	if (role == CP_ROLE_GADGET) cp_gadget_run();
	else cp_host_run();

	/* Both artifacts go on every path out, including the one where bring-up
	 * failed: a status file left behind is a session the next app believes is
	 * already running. */
	remove(cp_status_path);
	remove(cp_pid_path);
	cp_log("stopped\n");
	return 0;
}
