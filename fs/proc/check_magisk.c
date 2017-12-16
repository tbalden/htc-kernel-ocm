#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>


//file operation+
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/vmalloc.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
//file operation-

static void m_fclose(struct file* file) {
    filp_close(file, NULL);
}

static bool m_fopen_check(const char* path, int flags, int rights) {
    struct file* filp = NULL;
    mm_segment_t oldfs;
    int err = 0;

    oldfs = get_fs();
    set_fs(get_ds());
    filp = filp_open(path, flags, rights);
    set_fs(oldfs);
    if(IS_ERR(filp)) {
        err = PTR_ERR(filp);
        pr_err("[chk_magisk]File Open Error:%s %d\n",path, err);
	if (err==-2) {
		pr_err("[chk_magisk] verity File doesn't exist in root! magisk system\n");
		return true; // file doesnt exist, magisk
	}
        return false; // permission issue, exists, not magisk
    }
    if(!filp->f_op){
	pr_err("[chk_magisk]File Operation Method Error! non-magisk system\n");
        return false;
    }
    m_fclose(filp);
    pr_err("[chk_magisk] success..verity file found - non magisk system\n");
    return false; // successfuly opened, it's not magisk
}

static char *file_name="/verity_key";

bool finished = false;
bool magisk = false;

// work func...
static void check_async(struct work_struct * check_async_work)
{
	if (finished) return;
	magisk = m_fopen_check(file_name, O_RDONLY, 0);
	finished = true;
}
static DECLARE_WORK(check_async_work, check_async);

// work struct
static struct workqueue_struct *magisk_work_queue = NULL;

// sync call for is_magisk. Don't call it from atomic context!
bool is_magisk_sync(void) {
	return m_fopen_check(file_name, O_RDONLY, 0);
}
EXPORT_SYMBOL(is_magisk_sync);

// async might_sleep part moved to work, delay wait for result.
// call this at initramfs mounted, where /init and /verity_key are yet in the root
// like when cmdline_show is shown first
bool is_magisk(void) {
	queue_work(magisk_work_queue, &check_async_work);
	while (!finished) {
		mdelay(1);
	}
	return magisk;
}
EXPORT_SYMBOL(is_magisk);

// call this from a non atomic contet, like init
void init_magisk(void) {
	if (magisk_work_queue == NULL) {
		magisk_work_queue = create_singlethread_workqueue("magisk");
	}
}
EXPORT_SYMBOL(init_magisk);

