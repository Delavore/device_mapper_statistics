#define DEBUG

#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/kernel.h>

// Used to create unique name of device
static int counter = 0;

struct dev_statistics {
    unsigned int write_req_cnt;
    unsigned int read_req_cnt;
    unsigned int req_cnt;

    unsigned int total_write_size;
    unsigned int total_read_size;
    unsigned int total_size;
};

/*  
	Wrapper for storing information about every underlying device
    Params:
    dev : out device
	start : start of our sector (Not mb)
	statistics : our structure to storing information 	
	dm_kobj : kernel object, need for sysfs
*/
struct private_dmp_target {
    struct dm_dev *dev;
    sector_t start;
    struct dev_statistics statistics;

    struct kobject dm_kobj;
};


/* SYSFS FUNCTIONS AND STRUCTURES */


// Sysfs show callback for sysfs_ops 
static ssize_t dmp_sysfs_read(struct kobject *kobj, struct attribute *attr, char *buf) {
    struct private_dmp_target *pdmp = container_of(kobj, struct private_dmp_target,
																			 dm_kobj);

    if (strcmp(attr->name, "volumes") == 0) {
        return scnprintf(buf, PAGE_SIZE,
			"read:\n"
			" reqs: %u\n"
			" avg size: %u\n"
			"write:\n"
			" reqs: %u\n"
			" avg size %u\n"
			"total:\n"
			" reqs: %u\n"
			" avg size %u\n",
			pdmp->statistics.read_req_cnt,
			pdmp->statistics.total_read_size / pdmp->statistics.read_req_cnt,
			pdmp->statistics.write_req_cnt,
			pdmp->statistics.total_write_size / pdmp->statistics.write_req_cnt,
			pdmp->statistics.read_req_cnt + pdmp->statistics.write_req_cnt,
			(pdmp->statistics.total_read_size + pdmp->statistics.total_write_size) /
			(pdmp->statistics.read_req_cnt + pdmp->statistics.write_req_cnt)  
		);

    }
    return -EIO;
}

static const struct sysfs_ops dmp_sysfs_ops = {
    .show = dmp_sysfs_read,
    .store = NULL,  // We dont need to write to our device
};

static struct attribute dm_attr_volumes = {
    .name = "volumes",
    .mode = 0444,  // Can only read
};

static struct attribute *dm_attrs[] = {
    &dm_attr_volumes,
    NULL,
};

static const struct attribute_group dm_attr_group = {
    .attrs = dm_attrs,
};

static const struct attribute_group *dm_groups[] = {
    &dm_attr_group,
    NULL,
};

// Cleanup, call when we put device
static void dm_release(struct kobject *kobj)
{
    struct private_dmp_target *pdmp = container_of(kobj, struct private_dmp_target,
																		 dm_kobj);
    kfree(pdmp);
}

static struct kobj_type dm_ktype = {
    .release = dm_release,
    .default_groups = dm_groups,
    .sysfs_ops = &dmp_sysfs_ops,
};


/* DEVICE MAPPER STRUCTURE */


// Constructor, called when we create device
static int dmp_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
    pr_debug("func dmp_ctr: begin of constructor\n");

    struct private_dmp_target *pdmp;
    unsigned long long start;

    if (argc != 2) {
        pr_err("func dmp_ctr: invalid argument count\n");
        ti->error = "Invalid argument count";
        return -EINVAL;
    }

    pdmp = kzalloc(sizeof(*pdmp), GFP_KERNEL);
    if (!pdmp) {
        pr_err("func dmp_ctr: failed to allocate memory\n");
        ti->error = "Cannot allocate memory";
        return -ENOMEM;
    }

    if (kstrtoull(argv[1], 10, &start)) {
        pr_err("func dmp_ctr: invalid start sector\n");
        kfree(pdmp);
        ti->error = "Invalid start sector";
        return -EINVAL;
    }
    pdmp->start = (sector_t)start;

    if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &pdmp->dev)) {
        pr_err("func dmp_ctr: no underlying device\n");
        kfree(pdmp);
        ti->error = "No such underlying device";
        return -EINVAL;
    }

    char name[32];
    snprintf(name, sizeof(name), "dmp-%d", counter++);

    if (kobject_init_and_add(&pdmp->dm_kobj, &dm_ktype, kernel_kobj, name)) {  // &THIS_MODULE->mkobj.kobj
        pr_err("func dmp_ctr: kobject_init_and_add failed\n");
        dm_put_device(ti, pdmp->dev);
        kfree(pdmp);
        return -ENOMEM;
    }

    ti->private = pdmp;
    pr_info("func dmp_ctr: constructor worked\n");
    return 0;
}

// Main device mapper function, called with every operation (read, whire, etc...)
static int dmp_map(struct dm_target *ti, struct bio *bio)
{
	pr_debug("func dmp_map: begin of func\n");

    struct private_dmp_target *pdmp = ti->private;

    bio_set_dev(bio, pdmp->dev->bdev);
    bio->bi_iter.bi_sector += pdmp->start;

    unsigned int op_size = bio->bi_iter.bi_size;

    pdmp->statistics.total_size += op_size;
    pdmp->statistics.req_cnt++;

    switch (bio_op(bio)) {
    case REQ_OP_READ:
		pr_debug("func dmp_map: read operation\n");

        pdmp->statistics.read_req_cnt++;
        pdmp->statistics.total_read_size += op_size;
        break;
    case REQ_OP_WRITE:
		pr_debug("func dmp_map: write operation\n");

        pdmp->statistics.write_req_cnt++;
        pdmp->statistics.total_write_size += op_size;
        break;
    default:
		pr_debug("func dmp_map: default operation\n");

        break;
    }
	
	pr_info("func dmp_map: map function worked\n");
    return DM_MAPIO_REMAPPED;  // Tranfer control to underlying device
}

// Destructor of our device, called when object is destroyed
static void dmp_dtr(struct dm_target *ti)
{
    struct private_dmp_target *pdmp = (struct private_dmp_target *)ti->private;

    kobject_put(&pdmp->dm_kobj);
    dm_put_device(ti, pdmp->dev);

    pr_info("func dmp_dtr: destructor worked\n");
}

static struct target_type dmp_target = {
    .name = "dmp",
    .version = {1, 2, 0},
    .module = THIS_MODULE,
    .ctr = dmp_ctr,
    .map = dmp_map,
    .dtr = dmp_dtr,
};

static int __init init_dmp(void)
{
    int ret = dm_register_target(&dmp_target);
    if (ret < 0) {
        pr_err("init_dmp: failed to register target\n");
        return ret;
    }

    pr_info("init_dmp: target registered successfully\n");
    return 0;
}

static void __exit cleanup_dmp(void)
{
    dm_unregister_target(&dmp_target);
    pr_info("cleanup_dmp: target unregistered successfully\n");
}

module_init(init_dmp);
module_exit(cleanup_dmp);

MODULE_LICENSE("GPL");
