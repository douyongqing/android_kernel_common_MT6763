#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <asm/barrier.h>

#include "linux/tee_core.h"
#include "linux/tee_kernel_lowlevel_api.h"
#include "arm_common/teesmc_st.h"
#include "arm_common/teesmc.h"

#include "tee_procfs.h"
#include "tee_core_priv.h"

#ifdef CONFIG_OF
#include <linux/of_irq.h>
#endif

#define TKCORE_LOG_KMSG

const char tkcore_procfs_tag[] = "tkcore_proc";

#define LOG_AREA_PAGE_ORDER	(4)
#define LOG_AREA_PAGE_COUNT			(1 << LOG_AREA_PAGE_ORDER)
#define LOG_AREA_SIZE				(LOG_AREA_PAGE_COUNT << PAGE_SHIFT)

#if 0
#define PROC_DBG(fmt, ...) pr_alert("[%s][%s] " fmt "\n", tkcore_procfs_tag, __func__, ##__VA_ARGS__)
#else
#define PROC_DBG(fmt, ...) do{} while (0)
#endif

struct tkcore_trace {
	int level;
	const int idx;
	const char *desc;
};

#define xstr(s) str(s)
#define str(s) #s

#define TKCORE_DEFINE_TRACE(__x, __idx) \
	{ \
		.level = 0, \
		.idx = __idx, \
		.desc = xstr(__x) \
	}

#define TEE_LOG_TIMEOUT_MS	(500)

#define TEE_LOG_TIMEOUT_USER_MS	(1000)

static struct mutex log_mutex;
static struct mutex trace_mutex;

int log_irq_num __read_mostly = -1;

int log_wakeup = 0;

DECLARE_WAIT_QUEUE_HEAD(log_wqh);

DECLARE_WAIT_QUEUE_HEAD(user_log_wqh);

struct proc_dir_entry *tee_proc_dir;

struct proc_dir_entry *tee_proc_log_file;
struct proc_dir_entry *tee_proc_trace;

struct proc_dir_entry *tee_proc_drv_version;
struct proc_dir_entry *tee_proc_tee_version;
struct proc_dir_entry *tee_proc_teed_version;

typedef union tee_log_ctrl
{
	struct
	{
		unsigned int tee_buf_addr;
		unsigned int tee_buf_size;
		unsigned int tee_write_pos;
		unsigned int tee_read_pos;

		unsigned int tee_buf_unread_size;

		unsigned int tee_irq_count;
		unsigned int tee_reader_alive;

		unsigned int tee_write_seq;
		unsigned int tee_read_seq;
	} info;
	unsigned char data[TEE_LOG_CTL_BUF_SIZE];
} tee_log_ctrl_t;

static bool tee_proc_enable_log = true;

static tee_log_ctrl_t *tee_buf_vir_ctl;
static unsigned long tee_buf_phy_ctl;
static unsigned int tee_buf_len;
static unsigned char *tee_log_vir_addr;
static unsigned int tee_log_len;

char *tee_log_area;
char *tee_log_buf;

/* klog_head points to the first place
   where TEE log can be stored into.
   klog_tail points to the first place where
   klog can be read from.
*/

uint32_t klog_head;
uint32_t klog_tail;

struct task_struct *log_ts;

static void tee_log_lock(void)
{
	mutex_lock(&log_mutex);
}

static void tee_log_unlock(void)
{
	mutex_unlock(&log_mutex);
}

/*
static void tee_dump_log_ctl(const char *prefix, int lineno)
{
	(void) prefix;
	(void) lineno;

	PROC_DBG("location %s line %d", prefix, lineno);

	if (tee_buf_vir_ctl) {
		PROC_DBG("tee_buf_vir_ctl: %p", tee_buf_vir_ctl);
		PROC_DBG("tee_buf_addr: 0x%x", tee_buf_vir_ctl->info.tee_buf_addr);
		PROC_DBG("tee_buf_size: %u", tee_buf_vir_ctl->info.tee_buf_size);
		//PROC_DBG("tee_write_pos: 0x%x", tee_buf_vir_ctl->info.tee_write_pos);
		//PROC_DBG("tee_read_pos: 0x%x", tee_buf_vir_ctl->info.tee_read_pos);
		PROC_DBG("tee_buf_unread_size: %u", tee_buf_vir_ctl->info.tee_buf_unread_size);
		PROC_DBG("tee_irq_count: %u", tee_buf_vir_ctl->info.tee_irq_count);

		PROC_DBG("tee_write_seq: %u", tee_buf_vir_ctl->info.tee_write_seq);
		PROC_DBG("tee_read_seq: %u", tee_buf_vir_ctl->info.tee_read_seq);

		PROC_DBG("read_index: %u", read_index);
	}
}

*/

static uint32_t log_seq2idx(uint32_t seq)
{
	return (seq % tee_log_len);
}

static ssize_t __copy_log_to_user(char __user *buf, char *kbuf, size_t count, const char *func, int lineno)
{
	ssize_t r;

	if (unlikely(r = copy_to_user(buf, kbuf, count))) {
		pr_warn("[%s][%s:%d] copy_to_user returns %Zd\n",
				tkcore_procfs_tag, func, lineno, r);

		if (unlikely(r < 0)) {
			pr_err("[%s][%s:%d] copy_to_user returns %Zd < 0\n",
					tkcore_procfs_tag, func, lineno, r);
			return r;
		} else if (unlikely(r > count)) {
			pr_err("[%s][%s:%d] copy_to_user returns %Zd > count %Zd\n",
				tkcore_procfs_tag, func, lineno, r, count);
			return -1;
		}
	}

	return count - r;
}

#define copy_log_to_user(buf, kbuf, count)  __copy_log_to_user((buf), (kbuf), (count), __func__, __LINE__)

static ssize_t do_ulog(char *kbuf, uint32_t head, uint32_t tail, size_t count)
{
	ssize_t len;

	len = (head + LOG_AREA_SIZE - tail) % LOG_AREA_SIZE;

	/* copy at most 'count' byte */
	len = len < count ? len : count;

	if (LOG_AREA_SIZE - tail > len) {
		memcpy(kbuf, &tee_log_area[tail], len);
	} else {
		memcpy(kbuf, &tee_log_area[tail], LOG_AREA_SIZE - tail);
		memcpy(kbuf + (LOG_AREA_SIZE - tail),
			tee_log_area, len - (LOG_AREA_SIZE - tail));
	}

	return len;
}

static ssize_t tee_log_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;
	uint32_t head;
	uint32_t tail, new_tail;
	char *kbuf;

	if (!(kbuf = kmalloc(count, GFP_KERNEL))) {
		printk("[%s][%s] bad malloc\n", tkcore_procfs_tag, __func__);
		return -ENOMEM;
	}

	tee_log_lock();

	while (1) {

		head = ACCESS_ONCE(klog_head);
		tail = ACCESS_ONCE(klog_tail);

		smp_mb();

		{
			DEFINE_WAIT(wait);

			prepare_to_wait(&user_log_wqh, &wait, TASK_INTERRUPTIBLE);

			if (file->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				finish_wait(&user_log_wqh, &wait);

				goto exit;
			}

			if (signal_pending(current)) {
				ret = -EINTR;
				finish_wait(&user_log_wqh, &wait);

				goto exit;
			}

			if (tee_proc_enable_log && head != tail) {
				finish_wait(&user_log_wqh, &wait);
				break;
			}

			tee_log_unlock();

			/* allow user mode reader to be interrupted by signal */
			schedule_timeout_interruptible(msecs_to_jiffies(TEE_LOG_TIMEOUT_USER_MS));

			tee_log_lock();

			finish_wait(&user_log_wqh, &wait);
		}

	}

	/*printk("[%s][%s] head tail %u %u\n", tkcore_procfs_tag, __func__, head, tail); */

	ret = do_ulog(kbuf, head, tail, count);

	smp_mb();

	if (ret > 0) {
		uint32_t updated_tail;

		new_tail = (tail + ret) % LOG_AREA_SIZE;

		/*printk("[%s][%s] tail %u to new_tail %u\n",
			tkcore_procfs_tag, __func__, tail, new_tail); */

		if ((updated_tail = cmpxchg(&klog_tail, tail, new_tail)) != tail) {
			char notice[50];
			int notice_len;
			uint32_t obsolete_count;

			printk("[%s][%s] detect tail change from %u to %u\n",
				tkcore_procfs_tag, __func__, tail, updated_tail);

			obsolete_count = (updated_tail + LOG_AREA_SIZE - tail) % LOG_AREA_SIZE;

			/* guarantee there won't be overflow */
			notice_len = snprintf(notice, 50,
				"**** missing at least %u bytes ****\n", obsolete_count);

			if (obsolete_count < ret) {

				if (notice_len >= count) {
					/* user buffer is too short, even for holding warning message */
					ret = copy_log_to_user(buf, notice, count);
				} else {
					ssize_t r1, r2;
					r1 = copy_log_to_user(buf, notice, notice_len);

					if (r1 < 0) {
						ret = r1;
						goto exit;
					}

					r2 = copy_log_to_user(buf + notice_len,
						kbuf + obsolete_count,
						count - notice_len > ret - obsolete_count ? ret - obsolete_count :
						count - notice_len);

					if (r2 < 0) {
						ret = r2;
						goto exit;
					}

					ret += r2;
				}
			} else {
				/* klog_tail increases too fast. we do best we can... */
				ret = copy_log_to_user(buf, notice, notice_len < count ? notice_len : count);
			}
		} else {
			ret = copy_log_to_user(buf, kbuf, ret);
		}
	}

exit:
	tee_log_unlock();
	kfree(kbuf);

	return ret;
}

static ssize_t tee_log_write(struct file *filp, const char __user *buf, size_t count, loff_t *pos)
{
	int r;
	char flag;

	if (count == 0) {
		return -EINVAL;
	}

	if ((r = copy_from_user(&flag, buf, 1)) < 0) {
		return r;
	}

	if (flag == '1') {
		tee_proc_enable_log = true;
	} else if (flag == '0') {
		tee_proc_enable_log = false;
	}

	return count;
}

/* for the sake of compatibility with old linux kernel */
int tee_log_open(struct inode *inode, struct file *file)
{
	int ret;

	//tee_dump_log_ctl(__func__, __LINE__);

	ret = nonseekable_open(inode, file);

	if (unlikely(ret)) {
		pr_err("[%s][%s] open failed: %d\n",
			tkcore_procfs_tag, __func__, ret);
		return ret;
	}

	file->private_data = NULL;

	return 0;
}

int tee_log_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations log_tee_ops = {
	.read = tee_log_read,
	.open = tee_log_open,
	.write = tee_log_write,
	.release = tee_log_release,
//	.poll = tee_log_poll,
};

/* Guarantee the idx defined here is consistent with TEE */
struct tkcore_trace tkcore_traces[] = {
	TKCORE_DEFINE_TRACE(ree_fs, 0),
	TKCORE_DEFINE_TRACE(enc_fs, 1),
	TKCORE_DEFINE_TRACE(rpmb_blk, 2),
	TKCORE_DEFINE_TRACE(rpmb_fs, 3),
	TKCORE_DEFINE_TRACE(ta_mgmt, 4),
	TKCORE_DEFINE_TRACE(tee_comm, 5),
	TKCORE_DEFINE_TRACE(tee_boot, 6),
	TKCORE_DEFINE_TRACE(core_mm, 7),
	TKCORE_DEFINE_TRACE(uart_print, 8),
	TKCORE_DEFINE_TRACE(htfat, 9),
	TKCORE_DEFINE_TRACE(spi, 10),
};

#define NTRACES (sizeof(tkcore_traces) / sizeof(tkcore_traces[0]))

static int tee_trace_open(struct inode *inode, struct file *filp)
{
	int ret;

	mutex_lock(&trace_mutex);


	ret = nonseekable_open(inode, filp);
	if (unlikely(ret)) {
		mutex_unlock(&trace_mutex);
		return ret;
	}

	filp->f_pos = 0UL;
	filp->private_data = PDE_DATA(inode);

	return 0;
}

#define TRACE_BUF_SIZE 128

char trace_buf[TRACE_BUF_SIZE + 1];

static ssize_t tee_trace_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	size_t i;
	ssize_t r;

	char *p = trace_buf;

	loff_t __pos;

	size_t len = count > TRACE_BUF_SIZE - 1? TRACE_BUF_SIZE - 1: count;

	if (buf == NULL) {
		return -EINVAL;
	}

	printk(KERN_ALERT "%s buf %p count %Zd pos %p\n", __func__, buf, count, pos);

	for (i = 0; i < NTRACES; i++) {
		int l = snprintf(p, trace_buf + len - p, "%s %d ",
			tkcore_traces[i].desc, tkcore_traces[i].level);

		if (l <= 0) {
			return -EINVAL;
		}

		p += l;
	}

	*p++ = '\n';

	if (pos == NULL) {
		__pos = 0;
	} else {
		__pos = *pos;
	}


	printk(KERN_ALERT "%s pos %lld\n",
		__func__, __pos);


	if (__pos >= p - trace_buf) {
		return 0;
	}

	r = copy_to_user(buf, trace_buf, (p - trace_buf) - __pos);


	if (r < 0)
		return r;

	*pos += ((p - trace_buf) - __pos) - r;

	return ((p - trace_buf) - __pos) - r;
}

static ssize_t tee_trace_write(struct file *filp, const char __user *buf, size_t count, loff_t *pos)
{
	char *p;
	ssize_t r;
	uint32_t level;
	size_t i, len = count > TRACE_BUF_SIZE ? TRACE_BUF_SIZE : count;

	struct smc_param param = { 0 };

	struct tee *tee = filp->private_data;

	if (tee == NULL) {
		pr_err("bad proc file: %p\n", filp);
		return -EINVAL;
	}

	(void) pos;

	PROC_DBG("Count %lu Actual Count %lu\n", count, len);

	if ((r = copy_from_user(trace_buf, buf, len)) < 0) {
		return r;
	}

	len = len - r;
	trace_buf[len] = '\0';

	PROC_DBG("Buffer: %s\n", trace_buf);

	if ((p = strchr(trace_buf, '=')) == NULL) {
		PROC_DBG("Bad parameter. Expecting format: <trace_item>=<loglevel>[0|1]\n");
		return -EINVAL;
	}

	*p = '\0';

	if ((r = kstrtouint(p + 1, 10, &level)) < 0) {
		PROC_DBG("Bad parameter. Expecting format: <trace_item>=<loglevel>[0|1]\n");
		return r;
	}

	for (i = 0; i < NTRACES; i++ ) {
		if (strcmp(tkcore_traces[i].desc, trace_buf) == 0) {
			param.a0 = TKCORE_FASTCALL_TRACE_CONFIG;
			param.a1 = tkcore_traces[i].idx;
			param.a2 = level;

			if (tkcore_traces[i].level != level) {
				tee->ops->call_tee(&param);

				PROC_DBG("TRACE_CONFIG return value: 0x%x\n",
					param.a0);

				if (param.a0 == TEESMC_RETURN_OK) {
					tkcore_traces[i].level = level;
					return len;
				} else {
#if defined(CONFIG_ARM)
					pr_err("[%s][%s] trace config Failed with 0x%x\n",
						tkcore_procfs_tag, __func__, param.a0);
#elif defined(CONFIG_ARM64)
					pr_err("[%s][%s] trace config Failed with 0x%llx\n",
						tkcore_procfs_tag, __func__, param.a0);
#endif
					return -EINVAL;
				}
			} else {
				PROC_DBG("Request level same with current level: %d\n",
					tkcore_traces[i].level);
				return len;
			}
		}
	}

	PROC_DBG("Can't find a matching trace_item\n");

	return -EINVAL;
}

static int tee_trace_release(struct inode *inode, struct file *file)
{
	mutex_unlock(&trace_mutex);

	return 0;
}

static const struct file_operations log_tee_trace_ops = {
	.read = tee_trace_read,
	.write = tee_trace_write,
	.open = tee_trace_open,
	.release = tee_trace_release
};

#include <version.h>

static ssize_t copy_to_user_str(char __user *buf, ssize_t count, loff_t *pos, const char *version)
{
	ssize_t r;
	size_t cnt;
	loff_t __pos;

	if ((__pos = *pos) > strlen(version) + 1) {
		pr_err("invalid pos: %lld len: %zu\n", __pos, strlen(version));
		return -EINVAL;
	}

	cnt = count < strlen(version) + 1 - __pos ? count: strlen(version) + 1 - __pos;

	r = copy_to_user(buf, version + __pos, cnt);

	if (r < 0)
		return r;

	*pos += cnt -r;

	return cnt - r;

}

int tee_version_major, tee_version_minor;

static ssize_t tee_version_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	char tee_version[20];

	if (buf == NULL || pos == NULL) {
		return -EINVAL;
	}

	snprintf(tee_version, sizeof(tee_version),
		"0.%d.%d-gp\n", tee_version_major, tee_version_minor);

	return copy_to_user_str(buf, count, pos, tee_version);
}

static const struct file_operations tee_version_ops = {
	.read = tee_version_read,
	.write = NULL,
	.open = NULL,
	.release = NULL
};


static ssize_t drv_version_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	if (buf == NULL || pos == NULL) {
		return -EINVAL;
	}

	return copy_to_user_str(buf, count, pos, tkcore_nsdrv_version);
}

static const struct file_operations drv_version_ops = {
	.read = drv_version_read,
	.write = NULL,
	.open = NULL,
	.release = NULL
};

#define TEED_VERSION_SIZE 50
char teed_version[TEED_VERSION_SIZE + 1] = "unknown\n";

static ssize_t teed_version_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	if (buf == NULL || pos == NULL) {
		return -EINVAL;
	}

	return copy_to_user_str(buf, count, pos, teed_version);
}

static ssize_t teed_version_write(struct file *filp, const char __user *buf, size_t count, loff_t *pos)
{
	ssize_t r;

	if (count > TEED_VERSION_SIZE) {
		return -ENOMEM;
	}

	if ((r = copy_from_user(teed_version, buf, count)) < 0) {
		return r;
	}

	teed_version[count + 1] = '\0';

	return count;
}

static const struct file_operations teed_version_ops = {
	.read = teed_version_read,
	.write = teed_version_write,
	.open = NULL,
	.release = NULL
};

static void remove_entry(void)
{
	proc_remove(tee_proc_dir);

	tee_proc_dir = NULL;
	tee_proc_log_file = NULL;
	tee_proc_trace = NULL;
	tee_proc_drv_version = NULL;
	tee_proc_tee_version = NULL;
}

static int create_entry(struct tee *tee)
{
	if ((tee_proc_dir = proc_mkdir("tkcore", NULL)) == NULL) {
		pr_err("[%s][%s] proc_mkdir tkcore failed\n", tkcore_procfs_tag, __func__);
		return -1;
	}

	if ((tee_proc_log_file = proc_create_data("tkcore_log",
		0444, tee_proc_dir, &log_tee_ops, (void *) tee)) == NULL) {
		pr_err("[%s][%s] proc_create failed\n", tkcore_procfs_tag, __func__);
		goto err;
	}

	if ((tee_proc_trace = proc_create_data("tkcore_trace",
		0444, tee_proc_dir, &log_tee_trace_ops, (void *) tee)) == NULL) {
		pr_err("[%s][%s] proc_create tkcore_trace failed\n", tkcore_procfs_tag, __func__);
		goto err;
	}

	if ((tee_proc_drv_version = proc_create_data("tkcore_drv_version",
		0444, tee_proc_dir, &drv_version_ops, (void *) tee)) == NULL) {
		pr_err("[%s][%s] proc_create tkcore_drv_version failed\n", tkcore_procfs_tag, __func__);
		goto err;
	}

	if ((tee_proc_tee_version = proc_create_data("tkcore_os_version",
		0444, tee_proc_dir, &tee_version_ops, (void *) tee)) == NULL) {
		pr_err("[%s][%s] proc_create tkcore_os_version failed\n", tkcore_procfs_tag, __func__);
		goto err;
	}

	if ((tee_proc_teed_version = proc_create_data("tkcore_teed_version",
		0666, tee_proc_dir, &teed_version_ops, (void *) tee)) == NULL) {
		pr_err("[%s][%s] proc_create tkcore_teed_version failed\n", tkcore_procfs_tag, __func__);
		goto err;
	}

	return 0;

err:
	remove_entry();
	return -1;
}

static irqreturn_t tkcore_log_irq_handler(int irq, void *dev_id)
{
	log_wakeup = 1;

	/* need there be barrier or something like that ?
	   I guess not. the Cache Coherence is responsible for
	   that
	*/

	wake_up_interruptible(&log_wqh);

	return IRQ_HANDLED;
}

#define LINEBUF_SIZE	255
char linebuf[LINEBUF_SIZE + 1];
char *linebuf_last;

static bool ring_buffer_on_segment(uint32_t start, uint32_t count, uint32_t point)
{
	if (point > start) {
		return point - start <= count;
	} else {
		uint32_t end = (start + count) % LOG_AREA_SIZE;
		if (end >= start) {
			return false;
		} else {
			//wrap around case
			return end >= point;
		}
	}
}

/* TODO protect with lock to avoid contention with usr lock */
static uint32_t adjust_klog_tail(uint32_t head, uint32_t tail, uint32_t count)
{

	if (ring_buffer_on_segment(head, count, tail)) {
		/* if wrap around happens, move log_tail forward. */
		return (head + count + 1) % LOG_AREA_SIZE;
	}

	return tail;
}

#ifdef TKCORE_LOG_KMSG

static void print_to_kmsg(char *buf, uint32_t count)
{
	size_t n = 0;

	if (count == 0) {
		pr_warn("[%s][%s] count == 0!!\n",
			tkcore_procfs_tag, __func__);
		return;
	}

	while (n < (size_t) count) {
		ssize_t i;
		bool found = false;

		if (!buf[n]) {
			if (linebuf_last != linebuf) {
				/* print log that is left in the linebuf (if any) out
				   if encountered with a '\0' */

				/* linebuf_last will not exceed &linebuf[LINEBUF] */
				*(linebuf_last) = '\0';
				printk("[TKCORE] %s", linebuf);
				linebuf_last = linebuf;
				continue;
			}
		}

		while (!buf[n] && n < (size_t) count)
			++n;

		for (i = n; i < count; i++) {
			if (i - n == linebuf + LINEBUF_SIZE - linebuf_last) {
				memcpy(linebuf_last, buf + n, i - n);
				linebuf[LINEBUF_SIZE] = '\0';

				if (buf[i] == '\0') {
					/* this character happens to be a '\0' */
					n = i + 1;
				} else {
					n = i;
				}
				found = true;
				break;
			} else {
				if (buf[i] == '\0') {
					/* copy the trailing '\0' */
					memcpy(linebuf_last, buf + n, i - n + 1);
					n = i + 1;
					found = true;
					break;
				}
			}
		}

		if (!found) {
			/* update linebuf_last */
			memcpy(linebuf_last, buf + n, count - n);
			linebuf_last += (count - n);
			n = count;
		} else {
			/* No matter whether '\0' is found, we have to print
			   log out, since there is not enough space left. */
			printk("[TKCORE] %s", linebuf);
			linebuf_last = linebuf;
		}

	}
}

#endif

static uint32_t do_klog(uint32_t rseq, uint32_t wseq, uint32_t head, uint32_t tail)
{
	uint32_t count, capacity, idx;
	uint32_t new_tail;

	/* this function does not guarantee write within capacity */
	capacity = LOG_AREA_SIZE - (head + LOG_AREA_SIZE - tail) % LOG_AREA_SIZE;

	/* can tolerate integer overflow */
	if (capacity < wseq - rseq) {
		/* do something */
	}

	count = wseq - rseq;

	if (count > LOG_AREA_SIZE) {
		printk("[%s][%s] log size %u longer than kbuffer by more than %u. Truncate it.\n",
			tkcore_procfs_tag, __func__, count, count - LOG_AREA_SIZE);
		idx = log_seq2idx(rseq + (count - LOG_AREA_SIZE));
		count = LOG_AREA_SIZE;
	} else {
		idx = log_seq2idx(rseq);
	}

	/* copy from TEE to tmpbuf */
	if (tee_log_len - idx >= count) {
		memcpy(tee_log_buf, tee_log_vir_addr + idx, count);
	} else {
		memcpy(tee_log_buf, tee_log_vir_addr + idx, tee_log_len - idx);
		memcpy(tee_log_buf + (tee_log_len - idx), tee_log_vir_addr,
			count - (tee_log_len - idx));
	}
#ifdef TKCORE_LOG_KMSG
	print_to_kmsg(tee_log_buf, count);
#endif

	new_tail = adjust_klog_tail(head, tail, count);

	/* copy from tmpbuf to kernel ring buffer */
	if (LOG_AREA_SIZE - head >= count) {
		memcpy(tee_log_area + head, tee_log_buf, count);
	} else {
		memcpy(tee_log_area + head, tee_log_buf, LOG_AREA_SIZE - head);

		memcpy(tee_log_area,
			tee_log_buf + (LOG_AREA_SIZE - head),
			count - (LOG_AREA_SIZE - head));
	}

	smp_mb();

	if (new_tail != tail) {
		/*printk("[%s][%s] update new tail from %u to %u (head %u count %u)\n",
			tkcore_procfs_tag, __func__, tail, new_tail, head, count); */
		*((volatile uint32_t *) &klog_tail) = new_tail;
	}

	klog_head = (head + count) % LOG_AREA_SIZE;

/*	printk("[%s][%s] new head 0x%x new tail 0x%x\n",
		tkcore_procfs_tag, __func__, klog_head, new_tail); */

	wake_up_interruptible(&user_log_wqh);

	return wseq;
}

int logd(void *data)
{
	++tee_buf_vir_ctl->info.tee_reader_alive;

	while (1) {
		uint32_t head, tail;
		uint32_t read_seq, new_read_seq, write_seq;

		read_seq = tee_buf_vir_ctl->info.tee_read_seq;
		write_seq = tee_buf_vir_ctl->info.tee_write_seq;

		while (read_seq == write_seq) {
			if (kthread_should_stop()) {
				goto logd_exit;
			}

			/* if write_seq is not observed as changed.
			   then we just continue to sleep.

			   But, What if an ISR comes and this core still cannot
			   see the modification made by other cores?

			   Well, we can do nothing about besides keep waiting.
			   The only things is guaranteed:
			   Once write_seq is updated, corresponding content on the ring buffer
			   MUST always be observable by all observers on the same domain.
			*/

			read_seq = tee_buf_vir_ctl->info.tee_read_seq;
			write_seq = tee_buf_vir_ctl->info.tee_write_seq;

			wait_event_interruptible_timeout(log_wqh, log_wakeup != 0,
				msecs_to_jiffies(TEE_LOG_TIMEOUT_MS));
			log_wakeup = 0;
		}

		if (write_seq - read_seq > tee_log_len) {
			printk("[%s][%s] Wrapped around read_seq %u write_seq %u\n",
				tkcore_procfs_tag, __func__, write_seq, read_seq);

			// fix the overflow
			read_seq = write_seq - tee_log_len;

		}

		head = klog_head;
		/* the only read from tail in logd happens here */
		tail = klog_tail;

		/*printk("[%s][%s] head 0x%x tail 0x%x\n",
			tkcore_procfs_tag, __func__, head, tail); */

		new_read_seq = do_klog(read_seq, write_seq, head, tail);

		smp_mb();

		/* The only write to `read_seq` happens here.
		   Avoid caching in register
		*/
		*((volatile uint32_t *) &(tee_buf_vir_ctl->info.tee_read_seq)) = new_read_seq;
	}

logd_exit:
	--tee_buf_vir_ctl->info.tee_reader_alive;

	return 0;
}

int tee_init_procfs(struct tee *tee)
{
	int r = 0;
	struct smc_param param = { 0 };

	if (sizeof(tee_log_ctrl_t) != TEE_LOG_CTL_BUF_SIZE) {
		pr_err("Invalid tee_log_ctrl_t size. Expecting %zu bytes. Get %d bytes\n",
			sizeof(tee_log_ctrl_t), TEE_LOG_CTL_BUF_SIZE);
		return -1;
	}

	mutex_init(&log_mutex);
	mutex_init(&trace_mutex);

	// get os revision
	param.a0 = TEESMC32_CALL_GET_OS_REVISION;
	tee->ops->raw_call_tee(&param);

	tee_version_major = param.a0;
	tee_version_minor = param.a1;

	printk("tkcoreos-rev: 0.%d.%d-gp\n", tee_version_major, tee_version_minor);

	// map control header
	param.a0 = TEESMC32_ST_FASTCALL_GET_LOGM_CONFIG;
	tee->ops->raw_call_tee(&param);

	//wait until
	if (param.a0 != TEESMC_RETURN_OK) {
		pr_err("[%s][%s] log service not available: 0x%x",
				tkcore_procfs_tag, __func__, (uint) param.a0);
		return -1;
	}

#ifdef TKCORE_KDBG
#if defined(CONFIG_ARM64)
	pr_info("[%s][%s] phy_ctl 0x%llx buf_len 0x%llx\n",
			tkcore_procfs_tag, __func__, param.a1, param.a2);
#elif defined(CONFIG_ARM)
	pr_info("[%s][%s] phy_ctl 0x%x buf_len 0x%x\n",
			tkcore_procfs_tag, __func__, param.a1, param.a2);
#endif
#endif

	tee_buf_phy_ctl = param.a1;
	tee_buf_len = (unsigned int) param.a2;

	if (tee_buf_len <= TEE_LOG_CTL_BUF_SIZE) {
		pr_err("Invalid tee_buf_len: %u\n", tee_buf_len);
		return -1;
	}

	if ((tee_buf_len & (PAGE_SIZE - 1)) || (tee_buf_phy_ctl & (PAGE_SIZE - 1))) {
		pr_err("[%s][%s] Invalid tee buf addr: 0x%lx size: 0x%x\n",
				tkcore_procfs_tag, __func__, tee_buf_phy_ctl, tee_buf_len);
		return -1;
	}

	tee_buf_vir_ctl = tee_map_cached_shm(tee_buf_phy_ctl, tee_buf_len);

	tee_log_vir_addr = (uint8_t *) tee_buf_vir_ctl + TEE_LOG_CTL_BUF_SIZE;

	if ((tee_log_len = tee_buf_vir_ctl->info.tee_buf_size) != tee_buf_len - TEE_LOG_CTL_BUF_SIZE) {
		iounmap(tee_buf_vir_ctl);
		pr_err("[%s][%s] Invalid tee log length: %u\n", tkcore_procfs_tag, __func__, tee_log_len);
		return -1;
	}

	// map log buffer
	tee_buf_vir_ctl->info.tee_reader_alive = 0;

	init_waitqueue_head(&log_wqh);
	init_waitqueue_head(&user_log_wqh);

	if ((tee_log_area = (char *) __get_free_pages(GFP_KERNEL,
			LOG_AREA_PAGE_ORDER)) == NULL) {
		iounmap(tee_buf_vir_ctl);
		pr_err("[%s][%s] Failed to allocate 2**4 pages for log area\n",
			tkcore_procfs_tag, __func__);
		return -1;
	}

	if ((tee_log_buf = (char *) __get_free_pages(GFP_KERNEL,
			LOG_AREA_PAGE_ORDER)) == NULL) {
		pr_err("[%s][%s] Failed to allocate 2**4 pages for log area\n",
			tkcore_procfs_tag, __func__);
		goto err0;
	}

	//(16 << 10) == 16KB == 4 Pages
	memset(tee_log_area, 0, LOG_AREA_SIZE);

	klog_head = klog_tail = 0;

	linebuf_last = linebuf;

	if ((log_ts = kthread_run(logd, NULL, "tkcore_logwq")) == NULL) {
		pr_err("[%s][%s] Failed to create kthread log_ts\n", tkcore_procfs_tag, __func__);
		r = -1;
		goto err1;
	}

	{
		int irq_num;
#ifdef CONFIG_OF
		struct device_node *node;
		node = of_find_compatible_node(NULL, NULL, "trustkernel,tkcore");
		if (node)
			irq_num = irq_of_parse_and_map(node, 0);
		else
			irq_num = TEE_LOG_IRQ;
#else
		irq_num = TEE_LOG_IRQ;
#endif

		pr_info("tkcore_log_irq: %d\n", irq_num);

		if (irq_num > 0) {

			if ((r = request_irq(irq_num,(irq_handler_t) tkcore_log_irq_handler, IRQF_TRIGGER_NONE, "tee_log_irq", NULL)) != 0) {
				pr_err("[%s][%s] Failed to request tee_log_irq interrupt\n",
					tkcore_procfs_tag, __func__);
				goto err2;
			}

			log_irq_num = irq_num;
		}
	}

	if (create_entry(tee))
		goto err3;

	return 0;

err3:
	if (log_irq_num > 0)
		free_irq(log_irq_num, NULL);
err2:
	kthread_stop(log_ts);
err1:
	free_pages((unsigned long) tee_log_buf, LOG_AREA_PAGE_ORDER);
err0:
	free_pages((unsigned long) tee_log_area, LOG_AREA_PAGE_ORDER);

	iounmap(tee_buf_vir_ctl);

	return r;
}

void tee_exit_procfs(void)
{
	remove_entry();

	if (log_irq_num > 0)
		free_irq(log_irq_num, NULL);

	free_pages((unsigned long) tee_log_area, LOG_AREA_PAGE_ORDER);
	free_pages((unsigned long) tee_log_buf, LOG_AREA_PAGE_ORDER);
}
