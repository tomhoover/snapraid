/*
 * Copyright (C) 2011 Andrea Mazzoleni
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "portable.h"

#include "support.h"
#include "elem.h"
#include "state.h"
#include "parity.h"
#include "handle.h"

/****************************************************************************/
/* parity */

block_off_t parity_allocated_size(struct snapraid_state* state)
{
	block_off_t parity_block;
	tommy_node* i;

	/* compute the size of the parity file */
	parity_block = 0;
	for (i = state->disklist; i != 0; i = i->next) {
		struct snapraid_disk* disk = i->data;

		/* start from the declared size */
		block_off_t block = tommy_arrayblk_size(&disk->blockarr);

		/* decrease the block until an allocated one, but part of a file */
		/* we don't stop at deleted blocks, because we want to have them cleared */
		/* if they are at the end of the parity */
		while (block > parity_block && !block_has_file(tommy_arrayblk_get(&disk->blockarr, block - 1)))
			--block;

		/* get the highest value */
		if (block > parity_block)
			parity_block = block;
	}

	return parity_block;
}

block_off_t parity_used_size(struct snapraid_state* state)
{
	block_off_t parity_block;
	tommy_node* i;

	/* compute the size of the parity file */
	parity_block = 0;
	for (i = state->disklist; i != 0; i = i->next) {
		struct snapraid_disk* disk = i->data;

		/* start from the declared size */
		block_off_t block = tommy_arrayblk_size(&disk->blockarr);

		/* decrease the block until an used one */
		while (block > parity_block && !block_has_file_and_valid_parity(tommy_arrayblk_get(&disk->blockarr, block - 1)))
			--block;

		/* get the highest value */
		if (block > parity_block)
			parity_block = block;
	}

	return parity_block;
}

void parity_overflow(struct snapraid_state* state, data_off_t size)
{
	tommy_node* i;
	block_off_t blockalloc;
	int first = 1;

	/* don't report if everything is outside or if the file is not accessible */
	if (size == 0) {
		return;
	}

	blockalloc = size / state->block_size;

	/* for all disks */
	for (i = state->disklist; i != 0; i = i->next) {
		struct snapraid_disk* disk = i->data;
		tommy_node* j;

		/* for all files */
		for (j = disk->filelist; j != 0; j = j->next) {
			struct snapraid_file* file = j->data;

			if (file->blockmax > 0) {
				if (file->blockvec[file->blockmax - 1].parity_pos >= blockalloc) {
					ftag("outofparity:%s:%s\n", disk->name, esc(file->sub));
					fflush_log();
					if (first) {
						first = 0;
						fout("\nYour data requires more parity than the available space.\n");
						fout("Please move the following files to another data disk:\n");
					}
					fout("outofparity %s%s\n", disk->dir, file->sub);
				}
			}
		}
	}
}

int parity_create(struct snapraid_parity_handle* parity, const char* path, data_off_t* out_size, int mode)
{
	int ret;
	int flags;

	pathcpy(parity->path, sizeof(parity->path), path);

	/* opening in sequential mode in Windows */
	/* O_SEQUENTIAL: opening in sequential mode in Windows */
	flags = O_RDWR | O_CREAT | O_BINARY;
	if ((mode & MODE_SEQUENTIAL) != 0)
		flags |= O_SEQUENTIAL;
	parity->f = open(parity->path, flags, 0600);
	if (parity->f == -1) {
		/* LCOV_EXCL_START */
		ferr("Error opening parity file '%s'. %s.\n", parity->path, strerror(errno));
		return -1;
		/* LCOV_EXCL_STOP */
	}

	/* get the stat info */
	ret = fstat(parity->f, &parity->st);
	if (ret != 0) {
		/* LCOV_EXCL_START */
		ferr("Error accessing parity file '%s'. %s.\n", parity->path, strerror(errno));
		goto bail;
		/* LCOV_EXCL_STOP */
	}

#if HAVE_POSIX_FADVISE
	if ((mode & MODE_SEQUENTIAL) != 0) {
		/* advise sequential access */
		ret = posix_fadvise(parity->f, 0, 0, POSIX_FADV_SEQUENTIAL);
		if (ret != 0) {
			/* LCOV_EXCL_START */
			ferr("Error advising parity file '%s'. %s.\n", parity->path, strerror(ret));
			goto bail;
			/* LCOV_EXCL_STOP */
		}
	}
#endif

	/* get the size of the existing data */
	parity->valid_size = parity->st.st_size;
	*out_size = parity->st.st_size;

	return 0;

bail:
	/* LCOV_EXCL_START */
	close(parity->f);
	parity->f = -1;
	parity->valid_size = 0;
	return -1;
	/* LCOV_EXCL_STOP */
}

int parity_chsize(struct snapraid_parity_handle* parity, data_off_t size, data_off_t* out_size, int skip_fallocate)
{
	int ret;

	if (parity->st.st_size < size) {
		int f_ret;
		int f_errno;
		const char* call;

#if HAVE_FALLOCATE
		call = "fallocate";
		if (!skip_fallocate) {
			/* allocate real space using the specific Linux fallocate() operation. */
			/* If the underline filesystem doesn't support it, this operation fails, */
			/* instead posix_fallocate() fallbacks to write the whole file. */
			ret = fallocate(parity->f, 0, 0, size);

			/* fallocate() returns the error number as positive integer, */
			/* and in this case it doesn't set errno, just like posix_fallocate() */
			/* Checking the glibc code (2.11.1 and 2.14.1) it seems that ENOSYS */
			/* may be returned in errno, so we support both the return way */
			if (ret > 0) { /* if a positive error is returned, convert it to errno */
				/* LCOV_EXCL_START */
				errno = ret;
				ret = -1;
				/* LCOV_EXCL_STOP */
			}
		} else {
			errno = EOPNOTSUPP;
			ret = -1;
		}

		/* we get EOPNOTSUPP if the operation is not supported, like in ext3/ext2 */
		/* or ENOSYS with kernel before 2.6.23 */
		if (errno == EOPNOTSUPP || errno == ENOSYS) {
			/* fallback using ftruncate() */
			call = "ftruncate";
			ret = ftruncate(parity->f, size);
		}
#else
		(void)skip_fallocate; /* avoid the warning */

		/* allocate using a sparse file */
		call = "ftruncate";
		ret = ftruncate(parity->f, size);
#endif
		/* save the state of the grow operation */
		f_ret = ret;
		f_errno = errno;

		/* get the stat info */
		ret = fstat(parity->f, &parity->st);
		if (ret != 0) {
			/* LCOV_EXCL_START */
			ferr("Error accessing parity file '%s'. %s.\n", parity->path, strerror(errno));
			goto bail;
			/* LCOV_EXCL_STOP */
		}

		/* return the new size */
		*out_size = parity->st.st_size;

		/* now check the error */
		if (f_ret != 0) {
			/* LCOV_EXCL_START */
			if (f_errno == ENOSPC) {
				ferr("Failed to grow parity file '%s' to size %" PRIu64 " using %s due lack of space.\n", parity->path, size, call);
			} else {
				ferr("Error growing parity file '%s' to size %" PRIu64 " using %s. Do you have enough space? %s.\n", parity->path, size, call, strerror(f_errno));
			}
			goto bail;
			/* LCOV_EXCL_STOP */
		}
	} else if (parity->st.st_size > size) {
		int f_ret;
		int f_errno;

		/* truncate the parity file */
		ret = ftruncate(parity->f, size);

		/* save the state of the shrink operation */
		f_ret = ret;
		f_errno = errno;

		/* get the stat info */
		ret = fstat(parity->f, &parity->st);
		if (ret != 0) {
			/* LCOV_EXCL_START */
			ferr("Error accessing parity file '%s'. %s.\n", parity->path, strerror(errno));
			goto bail;
			/* LCOV_EXCL_STOP */
		}

		/* return the new size */
		*out_size = parity->st.st_size;

		/* now check the error */
		if (f_ret != 0) {
			/* LCOV_EXCL_START */
			ferr("Error truncating parity file '%s' to size %" PRIu64 ". %s.\n", parity->path, size, strerror(f_errno));
			goto bail;
			/* LCOV_EXCL_STOP */
		}

		/* adjust the valid to the new size */
		parity->valid_size = size;
	}

	return 0;

bail:
	/* LCOV_EXCL_START */
	parity->valid_size = 0;
	return -1;
	/* LCOV_EXCL_STOP */
}

int parity_open(struct snapraid_parity_handle* parity, const char* path, int mode)
{
	int ret;
	int flags;

	pathcpy(parity->path, sizeof(parity->path), path);

	/* open for read */
	/* O_SEQUENTIAL: opening in sequential mode in Windows */
	/* O_NOATIME: do not change access time */
	flags = O_RDONLY | O_BINARY;
	if ((mode & MODE_SEQUENTIAL) != 0)
		flags |= O_SEQUENTIAL;
	parity->f = open_noatime(parity->path, flags);
	if (parity->f == -1) {
		ferr("Error opening parity file '%s'. %s.\n", parity->path, strerror(errno));
		return -1;
	}

	/* get the stat info */
	ret = fstat(parity->f, &parity->st);
	if (ret != 0) {
		/* LCOV_EXCL_START */
		ferr("Error accessing parity file '%s'. %s.\n", parity->path, strerror(errno));
		goto bail;
		/* LCOV_EXCL_STOP */
	}

	/* get the size of the exising data */
	parity->valid_size = parity->st.st_size;

#if HAVE_POSIX_FADVISE
	if ((mode & MODE_SEQUENTIAL) != 0) {
		/* advise sequential access */
		ret = posix_fadvise(parity->f, 0, 0, POSIX_FADV_SEQUENTIAL);
		if (ret != 0) {
			/* LCOV_EXCL_START */
			ferr("Error advising parity file '%s'. %s.\n", parity->path, strerror(ret));
			goto bail;
			/* LCOV_EXCL_STOP */
		}
	}
#endif

	return 0;

bail:
	/* LCOV_EXCL_START */
	close(parity->f);
	parity->f = -1;
	parity->valid_size = 0;
	return -1;
	/* LCOV_EXCL_STOP */
}

int parity_sync(struct snapraid_parity_handle* parity)
{
#if HAVE_FSYNC
	int ret;

	/* Ensure that data changes are written to disk. */
	/* This is required to ensure that parity is more updated than content */
	/* in case of a system crash. */
	ret = fsync(parity->f);
	if (ret != 0) {
		/* LCOV_EXCL_START */
		ferr("Error synching parity file '%s'. %s.\n", parity->path, strerror(errno));
		return -1;
		/* LCOV_EXCL_STOP */
	}
#endif

	return 0;
}

int parity_close(struct snapraid_parity_handle* parity)
{
	int ret;

	ret = close(parity->f);
	if (ret != 0) {
		/* LCOV_EXCL_START */
		/* invalidate for error */
		parity->f = -1;
		parity->valid_size = 0;

		/* This is a serious error, as it may be the result of a failed write */
		/* identified at later time. */
		/* In a normal filesystem (not NFS) it should never happen */
		ferr("Error closing parity file '%s'. %s.\n", parity->path, strerror(errno));
		return -1;
		/* LCOV_EXCL_STOP */
	}

	/* reset the descriptor */
	parity->f = -1;
	parity->valid_size = 0;

	return 0;
}

int parity_write(struct snapraid_parity_handle* parity, block_off_t pos, unsigned char* block_buffer, unsigned block_size)
{
	ssize_t write_ret;
	data_off_t offset;

	offset = pos * (data_off_t)block_size;

#if HAVE_PWRITE
	write_ret = pwrite(parity->f, block_buffer, block_size, offset);
#else
	if (lseek(parity->f, offset, SEEK_SET) != offset) {
		/* LCOV_EXCL_START */
		if (errno == ENOSPC) {
			ferr("Failed to grow parity file '%s' using lseek due lack of space.\n", parity->path);
		} else {
			ferr("Error seeking file '%s'. %s.\n", parity->path, strerror(errno));
		}
		return -1;
		/* LCOV_EXCL_STOP */
	}

	write_ret = write(parity->f, block_buffer, block_size);
#endif
	if (write_ret != (ssize_t)block_size) { /* conversion is safe because block_size is always small */
		/* LCOV_EXCL_START */
		if (errno == ENOSPC) {
			ferr("Failed to grow parity file '%s' using write due lack of space.\n", parity->path);
		} else {
			ferr("Error writing file '%s'. %s.\n", parity->path, strerror(errno));
		}
		return -1;
		/* LCOV_EXCL_STOP */
	}

	/* adjust the size of the valid data */
	if (parity->valid_size < offset + block_size) {
		parity->valid_size = offset + block_size;
	}

	/* Here doesn't make sense to call posix_fadvise(..., POSIX_FADV_DONTNEED) because */
	/* at this time the data is still in not yet written and it cannot be discharged. */

	return 0;
}

int parity_read(struct snapraid_parity_handle* parity, block_off_t pos, unsigned char* block_buffer, unsigned block_size, fptr* out)
{
	ssize_t read_ret;
	data_off_t offset;
	unsigned count;

	offset = pos * (data_off_t)block_size;

	/* check if we are going to read only not initialized data */
	if (offset >= parity->valid_size) {
		out("Reading missing data from file '%s' at offset %" PRIu64 ".\n", parity->path, offset);
		return -1;
	}

#if !HAVE_PREAD
	if (lseek(parity->f, offset, SEEK_SET) != offset) {
		/* LCOV_EXCL_START */
		out("Error seeking file '%s' at offset %" PRIu64 ". %s.\n", parity->path, offset, strerror(errno));
		return -1;
		/* LCOV_EXCL_STOP */
	}
#endif

	count = 0;
	do {

#if HAVE_PREAD
		read_ret = pread(parity->f, block_buffer + count, block_size - count, offset + count);
#else
		read_ret = read(parity->f, block_buffer + count, block_size - count);
#endif
		if (read_ret < 0) {
			/* LCOV_EXCL_START */
			out("Error reading file '%s' at offset %" PRIu64 " for size %u. %s.\n", parity->path, offset + count, block_size - count, strerror(errno));
			return -1;
			/* LCOV_EXCL_STOP */
		}
		if (read_ret == 0) {
			/* LCOV_EXCL_START */
			out("Unexpected end of file '%s' at offset %" PRIu64 ". %s.\n", parity->path, offset, strerror(errno));
			return -1;
			/* LCOV_EXCL_STOP */
		}

		count += read_ret;
	} while (count < block_size);

	/* Here isn't needed to call posix_fadvise(..., POSIX_FADV_DONTNEED) */
	/* because we already advised sequential access with POSIX_FADV_SEQUENTIAL. */
	/* In Linux 2.6.33 it's enough to ensure that data is not kept in the cache. */
	/* Better to do nothing and save a syscall for each block. */

	return block_size;
}

