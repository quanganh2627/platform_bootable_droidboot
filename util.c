/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Google, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/ext3_fs.h>

#include <zlib.h>
#include <diskconfig/diskconfig.h>
#include <cutils/android_reboot.h>

#include "fastboot.h"
#include "droidboot.h"
#include "droidboot_ui.h"
#include "droidboot_util.h"
#include "droidboot_fstab.h"

#define EXT_SUPERBLOCK_OFFSET	1024

/* make_ext4fs.h can't be included along with linux/ext3_fs.h.
 * This is the only item needed out of the former. */
extern int make_ext4fs(const char *filename, int64_t len,
                const char *mountpoint, struct selabel_handle *sehnd);

void die(void)
{
	pr_error("droidboot has encountered an unrecoverable problem, exiting!\n");
	exit(1);
}

int check_ext_superblock(struct part_info *ptn, int *sb_present)
{
	char *device;
	struct ext3_super_block superblock;
	int fd = -1;
	int ret = -1;

	device = find_part_device(disk_info, ptn->name);
	if (!device) {
		pr_error("Coudn't get device node\n");
		goto out;
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		pr_error("could not open device node %s\n", device);
		goto out;
	}
	if (lseek(fd, EXT_SUPERBLOCK_OFFSET, SEEK_SET) !=
			EXT_SUPERBLOCK_OFFSET) {
		pr_perror("lseek");
		goto out;
	}
	if (read(fd, &superblock, sizeof(superblock)) != sizeof(superblock)) {
		pr_perror("read");
		goto out;
	}
	ret = 0;
	*sb_present = (EXT3_SUPER_MAGIC == superblock.s_magic);
out:
	free(device);
	if (fd >= 0)
		close(fd);
	return ret;
}

#define CHUNK 1024 * 256

int named_file_write_decompress_gzip(const char *filename,
	unsigned char *what, size_t sz, off_t offset, int append)
{
	int ret;
	unsigned int have;
	z_stream strm;
	FILE *dest;
	unsigned char out[CHUNK];

	dest = fopen(filename, append ? "a" : "w");
	if (!dest) {
		pr_perror("fopen");
		return -1;
	}

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	ret = inflateInit2(&strm, 15 + 32);
	if (ret != Z_OK) {
		pr_error("zlib inflateInit error");
		fclose(dest);
		return ret;
	}

	if (offset && fseeko(dest, offset, SEEK_SET)) {
		pr_perror("fseek");
		ret = -1;
		goto out;
	}

	do {
		strm.avail_in = (sz > CHUNK) ? CHUNK : sz;
		if (strm.avail_in == 0)
			break;
		strm.next_in = what;

		what += strm.avail_in;
		sz -= strm.avail_in;

		/* run inflate() on input until output buffer not full */
		do {
			strm.avail_out = CHUNK;
			strm.next_out = out;
			ret = inflate(&strm, Z_NO_FLUSH);
			if (ret == Z_STREAM_ERROR) {
				pr_error("zlib state clobbered");
				die();
			}
			switch (ret) {
			case Z_NEED_DICT:
				ret = Z_DATA_ERROR;     /* and fall through */
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				pr_perror("zlib memory/data/corruption error");
				goto out;
			}
			have = CHUNK - strm.avail_out;
			if (fwrite(out, 1, have, dest) != have ||
					ferror(dest)) {
				pr_perror("fwrite");
				ret = -1;
				goto out;
			}
		} while (strm.avail_out == 0);
		/* done when inflate() says it's done */
	} while (ret != Z_STREAM_END);

	if (ret == Z_STREAM_END)
		ret = 0;
	else
		pr_error("zlib data error");
out:
	/* clean up and return */
	(void)inflateEnd(&strm);
	fclose(dest);
	return ret;
}


int named_file_write(const char *filename, const unsigned char *what,
		size_t sz, off_t offset, int append)
{
	int fd, ret, flags;

	flags = O_RDWR | (append ? O_APPEND : O_CREAT);
	fd = open(filename, flags);
	if (fd < 0) {
		pr_error("file_write: Can't open file %s: %s\n",
				filename, strerror(errno));
		return -1;
	}
	if (offset) {
		if (lseek(fd, offset, SEEK_SET) < 0) {
			pr_perror("lseek");
			close(fd);
			return -1;
		}
	}

	while (sz) {
		pr_verbose("write() %zu bytes to %s\n", sz, filename);
		ret = write(fd, what, sz);
		if (ret <= 0 && errno != EINTR) {
			pr_error("file_write: Failed to write to %s: %s\n",
					filename, strerror(errno));
			close(fd);
			return -1;
		}
		what += ret;
		sz -= ret;
	}
	close(fd);
	return 0;
}

int mount_partition_device(const char *device, const char *type, char *mountpoint)
{
	int ret;

	ret = mkdir(mountpoint, 0777);
	if (ret && errno != EEXIST) {
		pr_perror("mkdir");
		return -1;
	}

	pr_debug("Mounting %s (%s) --> %s\n", device,
			type, mountpoint);
	ret = mount(device, mountpoint, type, MS_SYNCHRONOUS, "");
	if (ret && errno != EBUSY) {
		pr_debug("mount: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int ext4_filesystem_checks(const char *device, struct part_info *ptn)
{
#ifndef SKIP_FSCK
	int ret;
#endif
	Volume *vol;
	long long length;

	vol = volume_for_device(device);
	if (!vol) {
		pr_error("%s not in recovery.fstab!\n", device);
		return -1;
	}

#ifndef SKIP_FSCK
	/* run fdisk to make sure the partition is OK */
	ret = execute_command("/system/bin/e2fsck -C 0 -fn %s",
				device);
	if (ret) {
		pr_error("fsck of filesystem failed\n");
		return -1;
	}
#endif

	/* Resize the filesystem according to vol->length:
	 * length == 0 -> use all the available size
	 * length < 0 -> make the partition '-length' bytes smaller that the available size
	 * length > 0 -> make the partition 'length' bytes in size
	 * In all cases, 'length' should be a multiple of 1024 */
	if (vol->length == 0)
		length = (long long)ptn->len_kb * 1024;
	else if (vol->length < 0)
		length = (long long)ptn->len_kb * 1024 + vol->length;
	else
		length = vol->length;
	if (execute_command("/system/bin/resize2fs -f -F %s %lldK",
				device, length / 1024)) {
		pr_error("could not resize filesystem to %lldK\n",
				length / 1024);
		return -1;
	}

	/* Set mount count to 1 so that 1st mount on boot doesn't
	 * result in complaints */
	if (execute_command("/system/bin/tune2fs -C 1 %s",
				device)) {
		pr_error("tune2fs failed\n");
		return -1;
	}
	return 0;
}

int mount_partition(struct part_info *ptn)
{
	char *pdevice;
	char *mountpoint = NULL;
	int ret;
	int status = -1;
	Volume *vol;

	pdevice = find_part_device(disk_info, ptn->name);
	if (!pdevice) {
		pr_perror("malloc");
		goto out;
	}
	vol = volume_for_device(pdevice);
	if (!vol) {
		pr_error("%s not in recovery.fstab!\n", pdevice);
		goto out;
	}

	ret = asprintf(&mountpoint, "/mnt/%s", ptn->name);
	if (ret < 0) {
		pr_perror("asprintf");
		goto out;
	}

	status = mount_partition_device(pdevice, vol->fs_type, mountpoint);
out:
	free(mountpoint);
	free(pdevice);

	return status;
}

int unmount_partition(struct part_info *ptn)
{
	int ret;
	char *mountpoint = NULL;

	ret = asprintf(&mountpoint, "/mnt/%s", ptn->name);
	if (ret < 0) {
		pr_perror("asprintf");
		return -1;
	}
	ret = umount(mountpoint);
	free(mountpoint);
	return ret;
}

int erase_partition(struct part_info *ptn)
{
	int ret = -1;
	char *pdevice = NULL;
	Volume *vol;

	pdevice = find_part_device(disk_info, ptn->name);
	if (!pdevice) {
		pr_error("find_part_device failed!\n");
		die();
	}

	if (!is_valid_blkdev(pdevice)) {
		pr_error("invalid destination node. partition disks?\n");
		goto out;
	}

	vol = volume_for_device(pdevice);
	if (!vol) {
		pr_error("%s not in recovery.fstab!\n", pdevice);
		goto out;
	}

	if (!strcmp(vol->fs_type, "ext4")) {
		if (make_ext4fs(vol->device, vol->length, ptn->name, sehandle)) {
		        pr_error("make_ext4fs failed\n");
			goto out;
		}
	} else {
		pr_error("erase_partition: I can't handle fs_type %s\n",
				vol->fs_type);
		goto out;
	}
	ret = 0;
out:
	free(pdevice);
	return ret;
}

int execute_command(const char *fmt, ...)
{
	int ret = -1;
	va_list ap;
	char *cmd;

	va_start(ap, fmt);
	if (vasprintf(&cmd, fmt, ap) < 0) {
		pr_perror("vasprintf");
		return -1;
	}
	va_end(ap);

	pr_debug("Executing: '%s'\n", cmd);
	ret = system(cmd);

	if (ret < 0) {
		pr_error("Error while trying to execute '%s': %s\n",
			cmd, strerror(errno));
		goto out;
	}
	ret = WEXITSTATUS(ret);
	pr_debug("Done executing '%s' (retval=%d)\n", cmd, ret);
out:
	free(cmd);
	return ret;
}

int execute_command_data(void *data, unsigned sz, const char *fmt, ...)
{
	int ret = -1;
	va_list ap;
	char *cmd;
	FILE *fp;
	size_t bytes_written;

	va_start(ap, fmt);
	if (vasprintf(&cmd, fmt, ap) < 0) {
		pr_perror("vasprintf");
		return -1;
	}
	va_end(ap);

	pr_debug("Executing: '%s'\n", cmd);
	fp = popen(cmd, "w");
	free(cmd);
	if (!fp) {
		pr_perror("popen");
		return -1;
	}

	bytes_written = fwrite(data, 1, sz, fp);
	if (bytes_written != sz) {
		pr_perror("fwrite");
		pclose(fp);
		return -1;
	}

	ret = pclose(fp);
	if (ret < 0) {
		pr_perror("pclose");
		return -1;
	}
	ret = WEXITSTATUS(ret);
	pr_debug("Execution complete, retval=%d\n", ret);

	return ret;
}


int is_valid_blkdev(const char *node)
{
	struct stat statbuf;
	if (stat(node, &statbuf)) {
		pr_perror("stat");
		return 0;
	}
	if (!S_ISBLK(statbuf.st_mode)) {
		pr_error("%s is not a block device\n", node);
		return 0;
	}
	return 1;
}


void apply_sw_update(const char *location, int send_fb_ok)
{
	struct part_info *cacheptn;
	char *cmdline;

	if (asprintf(&cmdline, "--update_package=%s", location) < 0) {
		pr_perror("asprintf");
		return;
	}

	cacheptn = find_part(disk_info, "cache");
	if (!cacheptn) {
		pr_error("Couldn't find cache partition. Is your "
				"disk_layout.conf valid?\n");
		goto out;
	}
	if (mount_partition(cacheptn)) {
		pr_error("Couldn't mount cache partition.\n");
		goto out;
	}

	if (mkdir("/mnt/cache/recovery", 0777) && errno != EEXIST) {
		pr_error("Couldn't create /mnt/cache/recovery directory\n");
		goto out;
	}

	if (named_file_write("/mnt/cache/recovery/command", (void *)cmdline,
				strlen(cmdline), 0, 0)) {
		pr_error("Couldn't create recovery console command file\n");
		unlink("/mnt/userdata/droidboot.update.zip");
		goto out;
	}

	pr_info("Rebooting into recovery console to apply update\n");
	if (send_fb_ok)
		fastboot_okay("");
	android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
out:
	if(cacheptn)
		unmount_partition(cacheptn);
	free(cmdline);
}


/* Taken from Android init, which also pulls runtime options
 * out of the kernel command line
 * FIXME: params can't have spaces */
void import_kernel_cmdline(void (*callback)(char *name))
{
	char cmdline[1024];
	char *ptr;
	int fd;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd >= 0) {
		int n = read(fd, cmdline, 1023);
		if (n < 0) n = 0;

		/* get rid of trailing newline, it happens */
		if (n > 0 && cmdline[n-1] == '\n')
			n--;

		cmdline[n] = 0;
		close(fd);
	} else {
		cmdline[0] = 0;
	}

	ptr = cmdline;
	while (ptr && *ptr) {
		char *x = strchr(ptr, ' ');
		if (x != 0)
			*x++ = 0;
		callback(ptr);
		ptr = x;
	}
}

