/*
 * Eastape HFS engine: public disk API.
 */

#include "internal.h"

#include <errno.h>
#include <stdlib.h>

int ehfs_disk_open(const char* path, ehfs_disk** out) {
	struct ehfs_disk* disk = calloc(1, sizeof(*disk));
	if(!disk)
		return -ENOMEM;
	int err = ehfs_blockdev_open_platform(path, &disk->dev);
	if(err) {
		free(disk);
		return err;
	}
	*out = disk;
	return 0;
}

void ehfs_disk_close(ehfs_disk* disk) {
	if(!disk)
		return;
	ehfs_blockdev_close(disk->dev);
	free(disk);
}

int ehfs_disk_scan(ehfs_disk* disk, ehfs_disk_layout* out) {
	return ehfs_scan_partitions(disk->dev, out);
}

int ehfs_probe(ehfs_disk* disk, uint64_t offset, uint64_t length, ehfs_volume_info* out) {
	return ehfs_probe_range(disk->dev, offset, length, NULL, true, out);
}
