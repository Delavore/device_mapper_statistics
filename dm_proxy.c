#define DEBUG

#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>

#include <linux/kobject.h>
#include <linux/string.h>

#include <linux/kernel.h>

struct dev_statistics {
	unsigned int write_req_cnt;
	unsigned int read_req_cnt; 
	unsigned int req_cnt;
	
	unsigned int total_write_size;
	unsigned int total_read_size;
	unsigned int total_size;
};

/*	Stores information about every underlying device
	Params:
	dev			
*/
struct private_dmp_target {
	struct dm_dev *dev;
	sector_t start;
	struct dev_statistics statistics;

	struct kobject dm_kobj;
};

static int dmp_ctr(struct dm_target *ti, unsigned int argc, char **argv);

static ssize_t dm_read(struct kobject *kobj, struct kobj_attribute *attr, char *buffer) {	
	struct private_dmp_target *pdmp = container_of(kobj, struct private_dmp_target, dm_kobj);
	
	ssize_t len = snprintf(buffer, PAGE_SIZE,
                   "read_req_cnt: %u\n"
                   "total_read_size: %u\n"
                   "write_req_cnt: %u\n"
                   "total_write_size: %u\n",
                   pdmp->statistics.read_req_cnt,
                   pdmp->statistics.total_read_size,
                   pdmp->statistics.write_req_cnt,
                   pdmp->statistics.total_write_size);
	return len;
}

	
/* 
	We dont need to write info to file, so make it 0444 - only for reading and so
	we dont need callback write function so pass NULL instead 
*/
struct kobj_attribute dm_attr = __ATTR(volumes, 0440, dm_read, NULL);

static struct attribute *dm_attrs[] = {
    &dm_attr.attr,
    NULL,
};

static void dm_release(struct kobject *kobj)
{
    /* cleanup */
}

static struct kobj_type dm_ktype = {
	.release = dm_release,
	.default_attrs = dm_attrs,
};

static int dmp_ctr(struct dm_target *ti, unsigned int argc, char **argv) {
	pr_debug("func dmp_ctr: begin of constructor\n");

	struct private_dmp_target *pdmp; 
	unsigned long long start;

	if (argc != 2) {
		pr_err("func dmp_ctr: error argument count\n");
		ti->error = "Invalid argument count";

		return -EINVAL;
	}

	pdmp = kzalloc(sizeof(struct private_dmp_target), GFP_KERNEL);
	if (pdmp == NULL) {
		pr_err("func dmp_ctr: error allocating memory\n");
		ti->error = "Cannot allocate memory";

		return -ENOMEM;
	}

	if (kstrtoull(argv[1], 10, &start)) {
		kfree(pdmp);

		pr_err("func dmp_ctr: error start sector\n");
		ti->error = "Invalid start sector";

		return -EINVAL;
	}
	pdmp->start = (sector_t)start;
	
	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &pdmp->dev)) {
		kfree(pdmp);

		pr_err("func dmp_ctr: error no underlying device\n");
		ti->error = "No such underlying device";

		return -EINVAL;
	}

	pdmp->dm_kobj = kobject_create_and_add("stat", kernel_kobj);  // &THIS_MODULE->mkobj.kobj
	if (pdmp->dm_kobj == NULL) {
		kfree(pdmp);

		pr_err("init_dmp: error in creating kobject\n");
		return -ENOMEM;
	}
	
	int err =  sysfs_create_file(pdmp->dm_kobj, &dm_attr.attr);
	if (err)  {
		kobject_put(pdmp->dm_kobj);

		kfree(pdmp);

		pr_err("init_dmp: cannot create sysfs file\n");
		
		return err;
	}

	pr_info("func dmp_ctr: constructor worked\n");
	ti->private = pdmp;
	return 0;
}

static int dmp_map(struct dm_target *ti, struct bio *bio) {
	pr_debug("func dmp_map: begin of func\n");

	struct private_dmp_target *pdmp = (struct private_dmp_target *)ti->private; 	
	
	bio_set_dev(bio, pdmp->dev->bdev);
	bio->bi_iter.bi_sector += pdmp->start;  // offset 
	
	unsigned int operation_size = bio->bi_iter.bi_size;
	
	pdmp->statistics.total_size += operation_size;
	pdmp->statistics.req_cnt++;

	switch(bio_op(bio)) {
		case REQ_OP_READ:
			pr_debug("func dmp_map: read operation\n"); 

			pdmp->statistics.read_req_cnt++;
			pdmp->statistics.total_read_size += operation_size;	
		break;
		case REQ_OP_WRITE:
			pr_debug("func dmp_map: write operation\n"); 

			pdmp->statistics.write_req_cnt++;
			pdmp->statistics.total_write_size += operation_size;	
		break;
		default:
			pr_debug("func dmp_map: other operation\n"); 

		break;
	}

	pr_info("sector: %llu, size: %u\n",
        (unsigned long long)bio->bi_iter.bi_sector,
        bio->bi_iter.bi_size);
	
	pr_debug("statistics.read_req_cnt: %u\n", pdmp->statistics.read_req_cnt);
	pr_debug("statistics.total_read_size: %u\n", pdmp->statistics.total_read_size);
	pr_debug("statistics.write_req_cnt: %u\n", pdmp->statistics.write_req_cnt);
	pr_debug("statistics.total_write_size: %u\n", pdmp->statistics.total_write_size);
	
	pr_info("func dmp_map: map function worked\n"); 
	return DM_MAPIO_REMAPPED;
}

/*
static void dmp_io_hints(struct dm_target *ti, struct queue_limits *limits) {

}
*/

static void dmp_dtr(struct dm_target *ti) {
	struct private_dmp_target *pdmp = (struct private_dmp_target *)ti->private;

	sysfs_remove_file(pdmp->dm_kobj, &dm_attr.attr);	
	kobject_put(pdmp->dm_kobj);

	dm_put_device(ti, pdmp->dev);

	kfree(pdmp);

	pr_info("func dmp_dtr: destructor worked\n");
}



static struct target_type dmp_target = {
	.name = "dmp",
	.version = {1, 2, 0},
	//.features = ,
	.module = THIS_MODULE, 
	.ctr = dmp_ctr,
	.map = dmp_map,
	.dtr = dmp_dtr,
	// .io_hints = dmp_io_hints,
};

static int __init init_dmp(void) {
	int ret = dm_register_target(&dmp_target);
	if (ret < 0) {
		pr_err("init_dmp: error in registration target\n");	
		return ret;
	}


	pr_info("func init_dmp: target registered succssfully\n");	
	return 0;
}

static void __exit cleanup_dmp(void) {
	dm_unregister_target(&dmp_target);
	
	if (dm_kobj)
		kobject_put(dm_kobj);
	
	pr_info("func cleanum_dmp: target unregistered succssfully\n");	
}


module_init(init_dmp);
module_exit(cleanup_dmp);

MODULE_LICENSE("GPL");
