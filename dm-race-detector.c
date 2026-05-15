#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>

#define DM_MSG_PREFIX "race-detector"

struct pending_bio
{
	struct list_head list;
	sector_t start;
	sector_t end;
	bool is_write;
};

struct race_detector_c
{
	struct dm_dev *dev;
	spinlock_t lock;
	struct list_head pending_list;
	unsigned long race_count;
};

static int race_detector_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct race_detector_c *rdc;
	int ret;

	if (argc != 1)
	{
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	rdc = kzalloc(sizeof(*rdc), GFP_KERNEL);
	if (!rdc)
	{
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &rdc->dev);
	if (ret)
	{
		ti->error = "Device lookup failed";
		kfree(rdc);
		return ret;
	}

	spin_lock_init(&rdc->lock);
	INIT_LIST_HEAD(&rdc->pending_list);
	rdc->race_count = 0;

	ti->private = rdc;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->num_secure_erase_bios = 1;
	ti->num_write_zeroes_bios = 1;

	return 0;
}

static void race_detector_dtr(struct dm_target *ti)
{
	struct race_detector_c *rdc = ti->private;
	struct pending_bio *pb, *tmp;

	spin_lock(&rdc->lock);
	list_for_each_entry_safe(pb, tmp, &rdc->pending_list, list)
	{
		list_del(&pb->list);
		kfree(pb);
	}
	spin_unlock(&rdc->lock);

	dm_put_device(ti, rdc->dev);
	kfree(rdc);
}

static int race_detector_map(struct dm_target *ti, struct bio *bio)
{
	struct race_detector_c *rdc = ti->private;
	struct pending_bio *pb, *tmp;
	sector_t start, end;
	bool is_write;

	switch (bio_op(bio)) {
    case REQ_OP_READ:
        is_write = false;
        break;
    case REQ_OP_WRITE:
    case REQ_OP_WRITE_ZEROES:
        is_write = true;
        break;
    default:
        bio_set_dev(bio, rdc->dev->bdev);
        return DM_MAPIO_REMAPPED;
    }

	start = bio->bi_iter.bi_sector;
	end = start + (bio->bi_iter.bi_size >> SECTOR_SHIFT);

	pb = kmalloc(sizeof(*pb), GFP_NOIO);
	if (!pb)
	{
		DMINFO("Cannot allocate pending_bio context");
		return DM_MAPIO_KILL;
	}

	pb->start = start;
	pb->end = end;
	pb->is_write = is_write;

	spin_lock(&rdc->lock);

	// Проверка гонок
	list_for_each_entry(tmp, &rdc->pending_list, list)
	{
		if (start < tmp->end && end > tmp->start)
		{
			bool conflict = false;

			if (tmp->is_write)
			{
				conflict = true;
			}
			else
			{
				if (is_write)
					conflict = true;
			}

			if (conflict)
			{
				rdc->race_count++;
				DMWARN("Data race detected! [%llu,%llu] conflicts with %s [%llu,%llu] (total: %lu)",
					start, end - 1,
					tmp->is_write ? "WRITE" : "READ",
					tmp->start, tmp->end - 1,
					rdc->race_count);
				break;
			}
		}
	}

	list_add_tail(&pb->list, &rdc->pending_list);
	spin_unlock(&rdc->lock);

	bio->bi_private = pb;

	bio_set_dev(bio, rdc->dev->bdev);

	return DM_MAPIO_REMAPPED;
}

static int race_detector_end_io(struct dm_target *ti, struct bio *bio,
								blk_status_t *error)
{
	struct race_detector_c *rdc = ti->private;
	struct pending_bio *pb = bio->bi_private;

	if (!pb)
		return DM_ENDIO_DONE;

	spin_lock(&rdc->lock);
	list_del(&pb->list);
	spin_unlock(&rdc->lock);

	kfree(pb);

	return DM_ENDIO_DONE;
}

static struct target_type race_detector_target = {
	.name = "race-detector",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr = race_detector_ctr,
	.dtr = race_detector_dtr,
	.map = race_detector_map,
	.end_io = race_detector_end_io,
};

static int __init race_detector_init(void)
{
	int ret = dm_register_target(&race_detector_target);
	if (ret < 0)
	{
		DMERR("Failed to register target: %d", ret);
		return ret;
	}
	DMINFO("Target registered successfully");
	return 0;
}

static void __exit race_detector_exit(void)
{
	dm_unregister_target(&race_detector_target);
	DMINFO("Target unregistered");
}

module_init(race_detector_init);
module_exit(race_detector_exit);

MODULE_LICENSE("GPL");