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
#include "util.h"
#include "elem.h"
#include "state.h"
#include "parity.h"
#include "handle.h"

/****************************************************************************/
/* dup */

struct snapraid_hash {
	struct snapraid_disk* disk; /**< Disk. */
	struct snapraid_file* file; /**< File. */
	unsigned char hash[HASH_SIZE]; /**< Hash of the whole file. */

	/* nodes for data structures */
	tommy_hashdyn_node node;
};

struct snapraid_hash* hash_alloc(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_file* file)
{
	struct snapraid_hash* hash;
	block_off_t i;
	unsigned char* buf;

	hash = malloc_nofail(sizeof(struct snapraid_hash));
	hash->disk = disk;
	hash->file = file;

	buf = malloc_nofail(file->blockmax * HASH_SIZE);

	/* set the back pointer */
	for (i = 0; i < file->blockmax; ++i) {
		memcpy(buf + i * HASH_SIZE, file->blockvec[i].hash, HASH_SIZE);

		if (!block_has_updated_hash(&file->blockvec[i])) {
			free(buf);
			free(hash);
			return 0;
		}
	}

	memhash(state->besthash, state->hashseed, hash->hash, buf, file->blockmax * HASH_SIZE);

	free(buf);

	return hash;
}

static inline tommy_uint32_t hash_hash(struct snapraid_hash* hash)
{
	return tommy_hash_u32(0, hash->hash, HASH_SIZE);
}

void hash_free(struct snapraid_hash* hash)
{
	free(hash);
}

int hash_compare(const void* void_arg, const void* void_data)
{
	const char* arg = void_arg;
	const struct snapraid_hash* hash = void_data;

	return memcmp(arg, hash->hash, HASH_SIZE);
}

void state_dup(struct snapraid_state* state)
{
	tommy_hashdyn hashset;
	tommy_node* i;
	unsigned count;
	data_off_t size;

	tommy_hashdyn_init(&hashset);

	count = 0;
	size = 0;

	fout("Comparing...\n");

	/* for each disk */
	for (i = state->disklist; i != 0; i = i->next) {
		tommy_node* j;
		struct snapraid_disk* disk = i->data;

		/* for each file */
		for (j = disk->filelist; j != 0; j = j->next) {
			struct snapraid_file* file = j->data;
			struct snapraid_hash* hash;
			tommy_hash_t hash32;

			/* if empty, skip it */
			if (file->size == 0)
				continue;

			hash = hash_alloc(state, disk, file);

			/* if no hash, skip it */
			if (!hash)
				continue;

			hash32 = hash_hash(hash);

			struct snapraid_hash* dup = tommy_hashdyn_search(&hashset, hash_compare, hash->hash, hash32);
			if (dup) {
				++count;
				size += dup->file->size;
				ftag("dup:%s:%s:%s:%s:%" PRIu64 ": dup\n", disk->name, esc(file->sub), dup->disk->name, esc(dup->file->sub), dup->file->size);
				fout("%12" PRIu64 " %s%s = %s%s\n", file->size, disk->dir, file->sub, dup->disk->dir, dup->file->sub);
				hash_free(hash);
			} else {
				tommy_hashdyn_insert(&hashset, &hash->node, hash, hash32);
			}
		}
	}

	tommy_hashdyn_foreach(&hashset, (tommy_foreach_func*)hash_free);
	tommy_hashdyn_done(&hashset);

	fout("\n");
	fout("%8u duplicates, for %" PRIu64 " MiB\n", count, size / (1024 * 1024));
	if (count)
		fout("There are duplicates!\n");
	else
		fout("No duplicates\n");

	ftag("summary:dup_count:%u\n", count);
	ftag("summary:dup_size:%" PRIu64 "\n", size);
	if (count == 0) {
		ftag("summary:exit:unique\n");
	} else {
		ftag("summary:exit:dup\n");
	}
	fflush_log();
}

