/*
 * Eastape HFS engine: Windows block device backend.
 *
 * Opens a physical disk (\\.\PhysicalDriveN) or an image file with
 * GENERIC_READ only. Windows itself rejects any write through this handle, so
 * no bug in the engine can modify the drive. Only read-only IOCTLs are used:
 * IOCTL_DISK_GET_LENGTH_INFO, IOCTL_STORAGE_QUERY_PROPERTY and
 * IOCTL_DISK_GET_DRIVE_GEOMETRY_EX.
 *
 * The handle is opened for overlapped I/O so several threads can read at
 * different offsets at the same time without sharing a file position.
 */

#ifdef _WIN32

#include "internal.h"

#include <errno.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

static int win32_error_to_errno(DWORD e) {
	switch(e) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:      return -ENOENT;
		case ERROR_ACCESS_DENIED:       return -EACCES;
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:         return -ENOMEM;
		case ERROR_SHARING_VIOLATION:   return -EBUSY;
		case ERROR_INVALID_PARAMETER:   return -EINVAL;
		case ERROR_DEVICE_NOT_CONNECTED:
		case ERROR_NOT_READY:
		case ERROR_NO_SUCH_DEVICE:
		case ERROR_DEV_NOT_EXIST:       return -ENODEV;
		default:                        return -EIO;
	}
}

static int win32_read_aligned(struct ehfs_blockdev* dev, void* buf, uint64_t length, uint64_t offset) {
	HANDLE h = dev->handle;
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if(!event)
		return -ENOMEM;

	int ret = 0;
	char* p = buf;
	while(length) {
		DWORD want = length > (1u << 30) ? (1u << 30) : (DWORD)length;
		OVERLAPPED ov = {0};
		ov.Offset = (DWORD)offset;
		ov.OffsetHigh = (DWORD)(offset >> 32);
		ov.hEvent = event;
		DWORD got = 0;
		if(!ReadFile(h, p, want, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) {
			ret = win32_error_to_errno(GetLastError());
			break;
		}
		if(!GetOverlappedResult(h, &ov, &got, TRUE)) {
			ret = win32_error_to_errno(GetLastError());
			break;
		}
		if(got == 0) {
			ret = -EIO;
			break;
		}
		p += got;
		offset += got;
		length -= got;
	}

	CloseHandle(event);
	return ret;
}

static void win32_close(struct ehfs_blockdev* dev) {
	CloseHandle(dev->handle);
	free(dev);
}

static const struct ehfs_blockdev_ops win32_ops = { win32_read_aligned, win32_close };

static wchar_t* utf8_to_wide(const char* s) {
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if(n <= 0)
		return NULL;
	wchar_t* w = malloc((size_t)n * sizeof(wchar_t));
	if(w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
		free(w);
		return NULL;
	}
	return w;
}

static uint32_t query_sector_size(HANDLE h) {
	STORAGE_PROPERTY_QUERY q = {0};
	q.PropertyId = StorageAccessAlignmentProperty;
	q.QueryType = PropertyStandardQuery;
	STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align = {0};
	DWORD bytes = 0;
	if(DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &align, sizeof(align), &bytes, NULL)
	   && bytes >= sizeof(align) && align.BytesPerLogicalSector)
		return align.BytesPerLogicalSector;

	DISK_GEOMETRY_EX geo = {0};
	if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof(geo), &bytes, NULL)
	   && geo.Geometry.BytesPerSector)
		return geo.Geometry.BytesPerSector;

	return 512;
}

int ehfs_blockdev_open_platform(const char* path, struct ehfs_blockdev** out) {
	wchar_t* wpath = utf8_to_wide(path);
	if(!wpath)
		return -EINVAL;

	/* GENERIC_READ only: the operating system enforces that this handle cannot write */
	HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	free(wpath);
	if(h == INVALID_HANDLE_VALUE)
		return win32_error_to_errno(GetLastError());

	struct ehfs_blockdev* dev = calloc(1, sizeof(*dev));
	if(!dev) {
		CloseHandle(h);
		return -ENOMEM;
	}
	dev->ops = &win32_ops;
	dev->handle = h;
	dev->sector_size = 512;
	dev->align = 1;

	DWORD bytes = 0;
	GET_LENGTH_INFORMATION len = {0};
	LARGE_INTEGER file_size;
	if(GetFileType(h) == FILE_TYPE_DISK &&
	   DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof(len), &bytes, NULL)) {
		dev->is_device = true;
		dev->size = (uint64_t)len.Length.QuadPart;
		dev->sector_size = query_sector_size(h);
		dev->align = dev->sector_size;
	}
	else if(GetFileSizeEx(h, &file_size)) {
		dev->size = (uint64_t)file_size.QuadPart;
	}

	if(!dev->size) {
		win32_close(dev);
		return -EIO;
	}

	*out = dev;
	return 0;
}

#endif
