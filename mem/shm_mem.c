/*
 * Shared memory functions
 *
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History:
 * --------
 *  2003-03-12  split shm_mem_init in shm_getmem & shm_mem_init_mallocs
 *               (andrei)
 *  2004-07-27  ANON mmap support, needed on darwin (andrei)
 *  2004-09-19  shm_mem_destroy: destroy first the lock & then unmap (andrei)
 */


#include <stdlib.h>

#include "shm_mem.h"
#include "../config.h"
#include "../globals.h"
#include "../mi/tree.h"

#ifdef  SHM_MMAP

#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h> /*open*/
#include <sys/stat.h>
#include <fcntl.h>

#endif


#ifdef STATISTICS
stat_export_t shm_stats[] = {
	{"total_size" ,     STAT_IS_FUNC,    (stat_var**)shm_get_size  },

#if defined(HP_MALLOC) && !defined(HP_MALLOC_FAST_STATS)
	{"used_size" ,     STAT_NO_RESET,               &shm_used      },
	{"real_used_size" ,STAT_NO_RESET,               &shm_rused     },
#else
	{"used_size" ,      STAT_IS_FUNC,    (stat_var**)shm_get_used  },
	{"real_used_size" , STAT_IS_FUNC,    (stat_var**)shm_get_rused },
#endif

	{"max_used_size" ,  STAT_IS_FUNC,    (stat_var**)shm_get_mused },
	{"free_size" ,      STAT_IS_FUNC,    (stat_var**)shm_get_free  },

#if defined(HP_MALLOC) && !defined(HP_MALLOC_FAST_STATS)
	{"fragments" ,     STAT_NO_RESET,               &shm_frags     },
#else
	{"fragments" ,      STAT_IS_FUNC,    (stat_var**)shm_get_frags },
#endif

	{0,0,0}
};
#endif


#ifndef SHM_MMAP
static int shm_shmid=-1; /*shared memory id*/
#endif

gen_lock_t *mem_lock = NULL;

static void* shm_mempool=(void*)-1;
#ifdef VQ_MALLOC
	struct vqm_block* shm_block;
#elif F_MALLOC
	struct fm_block* shm_block;
#elif HP_MALLOC
	struct hp_block* shm_block;
#else
	struct qm_block* shm_block;
#endif


/*
 * - the memory fragmentation pattern of OpenSIPS
 * - holds the total number of shm_mallocs requested for each
 *   different possible size since daemon startup
 * - allows memory warming (preserving the fragmentation pattern on restarts)
 */
unsigned long long *mem_hash_usage;

#ifdef STATISTICS

#include "../evi/evi_core.h"
#include "../evi/evi_modules.h"

/* events information */
long event_shm_threshold = 0;
long *event_shm_last = 0;
int *event_shm_pending = 0;

static str shm_usage_str = { "usage", 5 };
static str shm_threshold_str = { "threshold", 9 };
static str shm_used_str = { "used", 4 };
static str shm_size_str = { "size", 4 };


void shm_event_raise(long used, long size, long perc)
{
	evi_params_p list = 0;

	*event_shm_pending = 1;
	*event_shm_last = perc;

	// event has to be triggered - check for subscribers
	if (!evi_probe_event(EVI_SHM_THRESHOLD_ID)) {
		goto end;
	}

	if (!(list = evi_get_params()))
		goto end;
	if (evi_param_add_int(list, &shm_usage_str, (int *)&perc)) {
		LM_ERR("unable to add usage parameter\n");
		goto end;
	}
	if (evi_param_add_int(list, &shm_threshold_str, (int *)&event_shm_threshold)) {
		LM_ERR("unable to add threshold parameter\n");
		goto end;
	}
	if (evi_param_add_int(list, &shm_used_str, (int *)&used)) {
		LM_ERR("unable to add used parameter\n");
		goto end;
	}
	if (evi_param_add_int(list, &shm_size_str, (int *)&size)) {
		LM_ERR("unable to add size parameter\n");
		goto end;
	}

	/*
	 * event has to be raised without the lock otherwise a deadlock will be
	 * generated by the transport modules, or by the event_route processing
	 */
#ifdef HP_MALLOC
	shm_unlock(0);
#else
	shm_unlock();
#endif

	if (evi_raise_event(EVI_SHM_THRESHOLD_ID, list)) {
		LM_ERR("unable to send shm threshold event\n");
	}

#ifdef HP_MALLOC
	shm_lock(0);
#else
	shm_lock();
#endif

	list = 0;
end:
	if (list)
		evi_free_params(list);
	*event_shm_pending = 0;
}
#endif



inline static void* sh_realloc(void* p, unsigned int size)
{
	void *r;

#ifndef HP_MALLOC
	shm_lock(); 
	shm_free_unsafe(p);
	r = shm_malloc_unsafe(size);
#else
	shm_free(p);
	r = shm_malloc(size);
#endif

	shm_threshold_check();

#ifndef HP_MALLOC
	shm_unlock(); 
#endif

	return r;
}

/* look at a buffer if there is perhaps enough space for the new size
   (It is beneficial to do so because vq_malloc is pretty stateful
    and if we ask for a new buffer size, we can still make it happy
    with current buffer); if so, we return current buffer again;
    otherwise, we free it, allocate a new one and return it; no
    guarantee for buffer content; if allocation fails, we return
    NULL
*/

#ifdef DBG_MALLOC
void* _shm_resize( void* p, unsigned int s, const char* file, const char* func,
							int line)
#else
void* _shm_resize( void* p , unsigned int s)
#endif
{
#ifdef VQ_MALLOC
	struct vqm_frag *f;
#endif
	if (p==0) {
		LM_DBG("resize(0) called\n");
		return shm_malloc( s );
	}
#	ifdef DBG_MALLOC
#	ifdef VQ_MALLOC
	f=(struct  vqm_frag*) ((char*)p-sizeof(struct vqm_frag));
	LM_DBG("params (%p, %d), called from %s: %s(%d)\n",
			p, s, file, func, line);
	VQM_DEBUG_FRAG(shm_block, f);
	if (p>(void *)shm_block->core_end || p<(void*)shm_block->init_core){
		LM_CRIT("bad pointer %p (out of memory block!) - aborting\n", p);
		abort();
	}
#endif
#	endif
	return sh_realloc( p, s );
}





int shm_getmem(void)
{

#ifdef SHM_MMAP
	int fd;
#else
	struct shmid_ds shm_info;
#endif

#ifdef SHM_MMAP
	if (shm_mempool && (shm_mempool!=(void*)-1)){
#else
	if ((shm_shmid!=-1)||(shm_mempool!=(void*)-1)){
#endif
		LM_CRIT("shm already initialized\n");
		return -1;
	}

#ifdef SHM_MMAP
#ifdef USE_ANON_MMAP
	shm_mempool=mmap(0, shm_mem_size, PROT_READ|PROT_WRITE,
					 MAP_ANON|MAP_SHARED, -1 ,0);
#else
	fd=open("/dev/zero", O_RDWR);
	if (fd==-1){
		LM_CRIT("could not open /dev/zero: %s\n", strerror(errno));
		return -1;
	}
	shm_mempool=mmap(0, shm_mem_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd ,0);
	/* close /dev/zero */
	close(fd);
#endif /* USE_ANON_MMAP */
#else

	shm_shmid=shmget(IPC_PRIVATE, /* SHM_MEM_SIZE */ shm_mem_size , 0700);
	if (shm_shmid==-1){
		LM_CRIT("could not allocate shared memory segment: %s\n",
				strerror(errno));
		return -1;
	}
	shm_mempool=shmat(shm_shmid, 0, 0);
#endif
	if (shm_mempool==(void*)-1){
		LM_CRIT("could not attach shared memory segment: %s\n",
				strerror(errno));
		/* destroy segment*/
		shm_mem_destroy();
		return -1;
	}
	return 0;
}



int shm_mem_init_mallocs(void* mempool, unsigned long pool_size)
{
#ifdef HP_MALLOC
	int i;
#endif

	/* init it for malloc*/
	shm_block = shm_malloc_init(mempool, pool_size, "shm");
	if (!shm_block){
		LM_CRIT("could not initialize shared malloc\n");
		shm_mem_destroy();
		return -1;
	}

#if defined(SHM_EXTRA_STATS) && defined(SHM_SHOW_DEFAULT_GROUP)
	/* we create the the default group statistic where memory alocated untill groups are defined is indexed */

	#ifndef DBG_MALLOC
	memory_mods_stats = MY_MALLOC_UNSAFE(shm_block, sizeof(struct module_info));
	#else
	memory_mods_stats = MY_MALLOC_UNSAFE(shm_block, sizeof(struct module_info), __FILE__, __FUNCTION__, __LINE__ );
	#endif

	if(!memory_mods_stats){
		LM_CRIT("could not alloc shared memory");
		return -1;
	}
	//initialize the new created groups
	memset((void*)&memory_mods_stats[0], 0, sizeof(struct module_info));
	if (init_new_stat((stat_var*)&memory_mods_stats[0].fragments) < 0)
		return -1;
	
	if (init_new_stat((stat_var*)&memory_mods_stats[0].memory_used) < 0)
		return -1;
	
	if (init_new_stat((stat_var*)&memory_mods_stats[0].real_used) < 0)
		return -1;

	update_stat((stat_var*)&memory_mods_stats[0].fragments, 4);
	update_stat((stat_var*)&memory_mods_stats[0].memory_used, sizeof(stat_val) * 3 + sizeof(struct module_info));
	update_stat((stat_var*)&memory_mods_stats[0].real_used, sizeof(stat_val) * 3 + sizeof(struct module_info) 
					+ 4 * FRAG_OVERHEAD);
#endif


#ifdef HP_MALLOC
	/* lock_alloc cannot be used yet! */
	mem_lock = shm_malloc_unsafe(HP_TOTAL_HASH_SIZE * sizeof *mem_lock);
	if (!mem_lock) {
		LM_CRIT("could not allocate the shm lock array\n");
		shm_mem_destroy();
		return -1;
	}

	for (i = 0; i < HP_TOTAL_HASH_SIZE; i++)
		if (!lock_init(&mem_lock[i])) {
			LM_CRIT("could not initialize lock\n");
			shm_mem_destroy();
			return -1;
		}

	mem_hash_usage = shm_malloc_unsafe(HP_TOTAL_HASH_SIZE * sizeof *mem_hash_usage);
	if (!mem_hash_usage) {
		LM_ERR("failed to allocate statistics array\n");
		return -1;
	}

	memset(mem_hash_usage, 0, HP_TOTAL_HASH_SIZE * sizeof *mem_hash_usage);

#else
	mem_lock = shm_malloc_unsafe(sizeof *mem_lock);
	if (!mem_lock) {
		LM_CRIT("could not allocate the shm lock\n");
		shm_mem_destroy();
		return -1;
	}

	if (!lock_init(mem_lock)) {
		LM_CRIT("could not initialize lock\n");
		shm_mem_destroy();
		return -1;
	}
#endif

#ifdef STATISTICS
	if (event_shm_threshold) {
		event_shm_last=shm_malloc_unsafe(sizeof(long));
		if (event_shm_last==0){
			LM_CRIT("could not allocate shm last event indicator\n");
			shm_mem_destroy();
			return -1;
		}
		*event_shm_last=0;
		event_shm_pending=shm_malloc_unsafe(sizeof(int));
		if (event_shm_pending==0){
			LM_CRIT("could not allocate shm pending flags\n");
			shm_mem_destroy();
			return -1;
		}
		*event_shm_pending=0;

	}
#endif /* STATISTICS */

	LM_DBG("success\n");

	return 0;
}


int shm_mem_init(void)
{
	int ret;

	LM_INFO("allocating SHM block\n");

	ret = shm_getmem();
	if (ret < 0)
		return ret;

	return shm_mem_init_mallocs(shm_mempool, shm_mem_size);
}

struct mi_root *mi_shm_check(struct mi_root *cmd, void *param)
{
#if defined(QM_MALLOC) && defined(DBG_MALLOC)
	struct mi_root *root;
	int ret;

	shm_lock();
	ret = qm_mem_check(shm_block);
	shm_unlock();

	/* any return means success; print the number of fragments now */
	root = init_mi_tree(200, MI_SSTR(MI_OK));

	if (!addf_mi_node_child(&root->node, 0, MI_SSTR("total_fragments"), "%d", ret)) {
		LM_ERR("failed to add MI node\n");
		free_mi_tree(root);
		return NULL;
	}

	return root;
#endif

	return NULL;
}

void init_shm_statistics(void)
{
	#ifdef HP_MALLOC
	hp_init_shm_statistics(shm_block);
	#endif

#ifdef SHM_EXTRA_STATS
	struct multi_str *mod_name;
	int i, len;
	char *full_name = NULL;
	stat_var *p;

	if(mem_free_idx != 1){

#ifdef SHM_SHOW_DEFAULT_GROUP
		p = (stat_var *)&memory_mods_stats[0].fragments;
		if (register_stat("shmem_group_default", "fragments", &p, STAT_NO_RESET|STAT_ONLY_REGISTER)!=0 ) {
			LM_CRIT("can't add stat variable");
			return;
		}
		p = (stat_var *)&memory_mods_stats[0].memory_used;
		if (register_stat("shmem_group_default", "memory_used", &p, STAT_NO_RESET|STAT_ONLY_REGISTER)!=0 ) {
			LM_CRIT("can't add stat variable");
			return;
		}

		p = (stat_var *)&memory_mods_stats[0].real_used;
		if (register_stat("shmem_group_default", "real_used", &p, STAT_NO_RESET|STAT_ONLY_REGISTER)!=0 ) {
			LM_CRIT("can't add stat variable");
			return;
		}


		i = mem_free_idx - 1;
#else
		i = mem_free_idx - 2;
#endif
		for(mod_name = mod_names; mod_name != NULL; mod_name = mod_name->next){
			len = strlen(mod_name->s);
			full_name = pkg_malloc((len + STAT_PREFIX_LEN + 1) * sizeof(char));

			strcpy(full_name, STAT_PREFIX);
			strcat(full_name, mod_name->s);
			p = (stat_var *)&memory_mods_stats[i].fragments;
			if (register_stat(full_name, "fragments", &p, STAT_NO_RESET|STAT_ONLY_REGISTER)!=0 ) {
				LM_CRIT("can't add stat variable");
				return;
			}

			p = (stat_var *)&memory_mods_stats[i].memory_used;
			if (register_stat(full_name, "memory_used", &p, STAT_NO_RESET|STAT_ONLY_REGISTER)!=0 ) {
				LM_CRIT("can't add stat variable");
				return;
			}

			p = (stat_var *) &memory_mods_stats[i].real_used;
			if (register_stat(full_name, "real_used", &p, STAT_NO_RESET|STAT_ONLY_REGISTER)!=0 ) {
				LM_CRIT("can't add stat variable");
				return;
			}
			i--;
		}
	}
#endif

}

void shm_mem_destroy(void)
{
#ifndef SHM_MMAP
	struct shmid_ds shm_info;
#endif

#ifdef HP_MALLOC
	update_mem_pattern_file();
#endif

	if (mem_lock){
		LM_DBG("destroying the shared memory lock\n");
		lock_destroy(mem_lock); /* we don't need to dealloc it*/
#ifdef STATISTICS
		if (event_shm_threshold) {
			if (event_shm_last)
				shm_free(event_shm_last);
			if (event_shm_pending)
				shm_free(event_shm_pending);
		}
#endif
	}
	if (shm_mempool && (shm_mempool!=(void*)-1)) {
#ifdef SHM_MMAP
		munmap(shm_mempool, /* SHM_MEM_SIZE */ shm_mem_size );
#else
		shmdt(shm_mempool);
#endif
		shm_mempool=(void*)-1;
	}
#ifndef SHM_MMAP
	if (shm_shmid!=-1) {
		shmctl(shm_shmid, IPC_RMID, &shm_info);
		shm_shmid=-1;
	}
#endif
}

